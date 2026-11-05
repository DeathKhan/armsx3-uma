# Remaining SPU retry waits: writer and lock audit

Inspected the current `codex/cell` source. This is a conversion proposal, not a
production change or a claim that every spin is budgeted. The phase-one APK was
already sealed; no SPU retry loops were changed by this audit. Device game gates
and AArch64 execution remain required. Do not advance fibers/local-store runtime
phases on the strength of a host predicate fixture.

## Four requested loops

| Retry loop | Predicate / writer | Locks held at retry | Safe conversion requirements |
| --- | --- | --- | --- |
| Accurate DMA PUT byte ownership, `spu_thread::do_dma_transfer`, originally ~3440 | `atomic_t<u128>` byte mask at `g_reservations + ((eal & 0xff80)/2 + 16)` intersects `wmask`. Only this function claims it and clears `v &= ~wmask` (~3559). The matching masked wait/notify calls are commented out. | Byte ownership **not acquired** at failed CAS; shared reservation reference and VM range lock acquired **after** success. Outer `rsx::reservation_lock<false,1>` can hold shared IO-map locks when strict rendering is enabled. | Replace the unconditional `true || ++i < 10` with a wall-clock hot budget. Add an actual wake after byte-mask release. Release/reacquire outer RSX shared lock before a long park. Full 128-bit CAS remains the admission authority. |
| Accurate DMA GET, `do_dma_transfer`, originally ~3274 | Requires reservation low 7 bits zero, then data copy bracketed by timestamp equality; 128-byte reads also compare data. Writers span C++, LLVM-generated code, PPU stores and DMA. | The outer RSX guard is disabled for GET (`!is_get` false). This reader does not own a reservation reference, byte-mask or range lock in this loop. CPU passive VM ownership/state still needs the existing pause/check protocol. | A raw reservation wait with no timeout is unsafe: failure unlocks and some ordinary data writes do not notify. Require a finite fallback or a new completely covered release-generation channel; snapshot failures with unchanged timestamp need a timed retry independent of notification. |
| GETLLAR snapshot acquisition, `spu_thread::process_mfc_cmd`, originally ~5963 | Same reservation low-bit / stable snapshot condition. Current path hot-spins 24 times then repeatedly kernel-yields. | `rsx::reservation_lock(addr,128)` holds shared IO-map locks when accurate RSX reservations are enabled. No reservation lock or byte mask is owned by the reader yet. | Release/reacquire the RSX guard before parking. Preserve the acquire fence between copy and timestamp re-read. Do not turn a compare failure with stable timestamp into an infinite epoch wait. Keep `if_suspended` snapshot fallback and state semantics. |
| SNR event serialization, `spu_thread::get_events`, originally ~6746 | `ch_events.locks != 0` while reading S1/S2. Only `push_snr` and `push_in_mbox` increment/decrement this field (~2332/2364 and ~2379/2427). | Reader owns no host mutex here; `ch_events.fetch_op` has completed. `push_in_mbox` releases its group mutex before its final decrement. | Add wake after final `locks:1→0` on both producers; bounded retry plus a low-word or generation wait. Preserve existing count/read/retry logic. Include state/abort wake and finite safety timeout. |

Source line numbers move with concurrent replay instrumentation. The named
functions and unique predicates above are the durable anchors.

## Accurate PUT: width and notification alternatives

`atomic_t<T>::wait` currently supports only four and eight bytes. The historical
`bits->wait(old,wmask)` / `notify_all(wmask)` API no longer exists. Restoring those
commented calls verbatim will not compile. `thread_ctrl::wait_on` also builds a
list of **32-bit** predicate records; it cannot accept a 128-bit `old` value.

Two viable designs, neither yet enabled in production:

1. **Narrow predicate views, no extra layout.** After a failed full-width claim,
   choose a 32-bit word containing at least one bit of `old & wmask`, and wait on
   that word's exact old value. Clearing that conflicting bit necessarily changes
   the predicate; notification before registration is caught by its recheck. A
   new writer restoring the same bits before registration is a legitimate ABA:
   the bytes are owned again and its later release must notify. After clearing
   the 128-bit mask, notify each 32-bit word touched by `wmask` (at most four).
   Every wake still retries the **full-width** CAS, so unrelated word changes
   cannot authorize overlapping stores. Existing eight-byte atomic waits already
   use 32-bit views, but mixed 128/32 access must be executed and stress-tested on
   AArch64 before accepting this extension.
2. **Generation channel.** Add an independent 32-bit release generation per
   reservation hash bucket, not a field in `spu_thread`. Read the generation
   **before** testing/claiming ownership, register with that old generation, then
   recheck before park. Writer clears its mask, increments generation, and
   notifies. Collisions cause harmless retries. This avoids mixed-width atomic
   views but adds storage, RMW and hash collisions. Counter wrap is handled by
   bounded waits, not by assuming a 32-bit value can never repeat.

A master-gated policy can keep a small wall-clock hot budget (a provisional
10 us, to be judged in the combined build), then use the existing registered Cell
wait so its event phase is bounded at 3 ms before native futex. A safety timeout
must remain. Repeated short event-only waits must **not** reset the total idle
budget forever; that would bypass the promised long-idle futex fallback.

The exact predicate record should include CPU state as well as the byte-word or
generation. `thread_ctrl::wait_on_custom<2>` can combine that predicate and
`cpu.state`, while its own base synchronization/task records supply abort/task
notifications. Check pending state before registration and after return. Preserve
`cpu_flag::temp` meaning: it prevents `check_state()` from abandoning an in-flight
host operation. Explicitly decide how process stop completes/abandons DMA rather
than adding an ignored boolean check which can retry forever on an exited CPU.

## Reservation timestamp writers: existing notifications are incomplete

The general reservation word is **not** an exhaustive release notification
channel. Source inventory:

- `SPUThread.cpp::do_putllc`: no-change CAS advances epoch; the SPURS shortcut,
  success and failure release unique bits with `+64` / `-64`. A failure can leave
  the same epoch and has no matching reservation-notifier event.
- `SPUThread.cpp::do_cell_atomic_128_store`: the combined 128-bit metadata includes
  unique/PUTUNC flags and shared-memory identity; stores clear those flags with
  `reservation_acquire(addr) += 32`. Duplicate-store abandonment loops observe
  the full 128-bit metadata. `do_putlluc` later notifies previous epochs, not every
  intermediate lock transition.
- Accurate DMA PUT releases the shared reference with `res += 127`; byte ownership
  is then cleared separately. No general reservation notifier is emitted there.
- `PPUThread.cpp::ppu_store_reservation`: full and narrow success paths advance
  epoch; failure paths subtract unique bits (`-64`) without epoch advancement.
  Successful notifications may be postponed by `res_notify` policy. A lock-release
  waiter cannot assume an immediate wake from that policy.
- `vm.cpp::try_reservation_update` performs a CAS without notifying; the enclosing
  `reservation_update` notifies. `reservation_op_internal` uses `+127` / `-1`.
  `vm_reservation.h::{reservation_op,light_op}` have notification conditional on
  template `Ack` and result, so failure/unacknowledged paths need separate care.
- `SPULLVMRecompiler.cpp::putllc16_pattern`: C++ RSX helper releases `old_res` on
  failure and `old_res+128` on success; normal notification is conditional. The
  generated fast path stores `rval+128` on success and `rval` on failure; its
  notification is conditional on `info.no_notify` and actual data change.
  `putllc0_pattern` includes another successful CAS with its notify commented out.
  A new release-generation design must update generated paths and invalidate the
  relevant object cache; C++-only instrumentation misses them.
- Ordinary guest writes can invalidate a data snapshot without changing the
  reservation epoch at the point the reader observes it. Data-compare retry must
  retain a bounded timed path even with a perfect timestamp-lock notifier.

`reservation_notifier_begin_wait(addr,epoch)` validates only `res & -128`, and its
notification hashing includes epoch parity. It is appropriate for reservation
loss/epoch change, not a promise that every temporary low-bit lock release emits
an event. Never replace these loops with an infinite wait on that notifier.

A conservative finite native wait can guarantee eventual recheck even when a
writer does not notify, but its added latency is a performance risk and needs the
same-game gate. Do not claim complete syscall elimination from such a fallback.

## RSX and VM lock graph

`rsx::reservation_lock<false,...>` in `RSXReservationLock.hpp` acquires **shared**
`rsx_iomap_table::rs` mutexes; `IsFullLock=true` takes exclusive locks. Sleeping with
shared ownership can stall RSX exclusive access and any writer needing that
exclusive operation before completing the condition the SPU awaits.

- Accurate GET's explicit RSX setting is false.
- Accurate PUT can acquire the shared lock under strict rendering; the FIFO arm
  additionally requires inaccurate DMA, but that does **not** disable the strict
  rendering arm.
- GETLLAR's guard is controlled by accurate RSX reservations and can be active.

At a failed claim/read, release the shared RSX guard before any long wait and
reacquire with the original transfer range before retrying. Preserve lock order:
RSX shared guard -> byte ownership -> shared reservation reference -> VM range
lock -> copy -> range release -> reservation release -> byte release. Do not
reacquire RSX while already owning bytes, which would invert the current order.
The current guard exposes `unlock`, but no public reacquire; use an explicit
scoped/resettable guard or a narrowly reviewed relock API, not assignment/copy of
the RAII guard (it is not a unique lock token type).

VM CPU passive ownership and suspend-all participation are separate. An arbitrary
sleep is not a substitute for the existing `state/check_state` and
`if_suspended` protocols. Avoid freeing/decommitting source memory while retaining
an unprotected borrowed pointer; revalidate or reacquire through the VM path.

## SNR-specific details

`ch_events` is eight bytes: locks live in low-word bits 16..23; mask lives in the
high word. Existing `set_events()` can notify the target **before** the producer
releases its event lock. That notification cannot replace a final-decrement wake.
Both producers can deliberately yield for up to **500 us** while holding
`ch_events.locks`, waiting for target-PC progress. A reader waiting for that lock
can therefore spin throughout this head-start window. The producer's deadline is
finite, preventing an infinite cycle, but this is real scheduler work.

A narrow wait uses the low 32-bit view of `ch_events.raw().all`, not a new pointer
inside a bitfield and not `wait_on(ch_events, full64old)` (list conversion is
32-bit). If the read fetch-op changed count/waiting bits, re-read the low word;
otherwise the pre-update value can cause a harmless immediate return and burn
retries. On each producer's final decrement, capture the prior locks value; wake
when it was one. Concurrent increments/new writers remain protected by rechecking
locks after every wake. Alternatively a per-SPU-independent sidecar generation
avoids mixed-width views but needs lifetime protection for raw/threaded SPUs.
No new field should casually move the JIT-visible SPU layout.

## Required validation before applying retry conversions

1. Actual atomic engine fixture: 128-bit ownership + selected 32-bit predicates;
   all four word positions, crossing masks, overlapping/disjoint writers,
   release-before-registration, ABA re-ownership, multiple waiters and complete
   registry cleanup. Host success is not an AArch64 proof.
2. SNR nested-producer fixture: final-decrement wake, early `set_events` wake,
   mailbox group mutex release, unchanged count/mask semantics, state/abort while
   parked. Verify a producer cannot require a mutex retained by the waiter.
3. Reservation failure-unlock test with no epoch change and intentionally missing
   notification must complete through finite fallback. Snapshot comparison
   mismatch with stable timestamp must also retain progress.
4. Android AArch64 execution of these fixtures, then LBP/Ratchet boot/play,
   pause/stop during each wait, savestate round-trip and ten boot loops. Exercise
   strict rendering/accurate RSX lock modes, not only their default-off case.
5. Only the completed architecture gets the combined release comparison. Record
   watts, syscall counts and late frames; none is inferred from fixture pass.

No device commands were issued for this audit while Eden was open. No production
retry patch is attached because the cross-architecture and game checkpoints have
not yet established its safety. The existing Cell wait primitive is ready for
registered waits; that does not make the remaining retry loops bounded.
