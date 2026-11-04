# ARMSX3 UMA

An unofficial performance fork of [ARMSX3](https://github.com/ARMSX2/ARMSX3) (the Android port of
RPCS3) for handhelds with a Snapdragon 8 Elite and an Adreno 8xx GPU, developed and measured on an
AYN Odin 3. The changes make the emulator stop burning CPU while it waits, keep more of the SPU code
fast, and let the GPU read the emulated PS3 memory in place (unified memory) under the Turnip driver.

Everything here is based on ARMSX3 1.6.1 and is GPLv2 like RPCS3 and ARMSX3. It installs as its own
application (`com.armsx3.uma`, "ARMSX3 UMA") next to the official app, with its own settings and data.

## Results

LittleBigPlanet (v02.00, the first stage, 30 fps locked), AYN Odin 3, measured as CPU work per frame
with `simpleperf` and the in-game temperature from the thermal sensors:

| | Official ARMSX3 1.6.1 | ARMSX3 UMA 1.6.1-uma.1 |
|---|---|---|
| CPU cycles per frame | ~300 million | ~165–172 million (−44%) |
| CPU temperature in game | 85–86 °C | 62–70 °C |
| GPU temperature in game | ~62 °C | ~45–50 °C |
| SPU code build at boot (second boot on) | ~33 s | ~7 s |

One game, one device. Other games should get the wait-loop and SPU savings too, since those are not
game-specific, but they have not been measured here. The frame rate is the same (the game is locked
to 30 fps); what you get is less heat, longer battery, and more headroom for games that are not.

## Install

**Obtainium** (gets updates automatically): add this repository as an app,
[**tap here on the device**](https://apps.obtainium.imranr.dev/redirect?r=obtainium://add/https://github.com/DeathKhan/armsx3-uma)
or paste `https://github.com/DeathKhan/armsx3-uma` into Obtainium's *Add App*. The in-app updater
also checks this fork's releases.

Manual:

1. Install `armsx3-uma-<version>-a15-armv8.2-sdk35.apk` from the latest release. Needs Android 15 or
   newer on a 64-bit ARMv8.2 device (the a15 variant only; there is no Android 13 build).
2. Install the PS3 firmware and set up games as in official ARMSX3. The official app's data is not
   shared; this is a separate app.
3. GPU driver: for the unified-memory paths, install `turnip-uma-1.1.zip` through the GPU driver
   manager in settings and select it. It is the same Turnip build that ships with
   [Eden UMA](https://github.com/DeathKhan/eden-uma): Mesa Turnip for Adreno 8xx with host-memory
   import on KGSL. With the system driver, the wait-loop and SPU improvements still apply; the GPU
   reads vertex data through copies as before.

The in-app updater checks this fork's releases, not the official ones.

## What changed

- **Waiting no longer spins.** The RSX thread parked the core on WFE while waiting on the game, the
  emulated `usleep(0)` polling loops returned at once and spun through the kernel (~114,000 calls a
  second in LittleBigPlanet), and flush-request waits spun too. They sleep now. This is most of the
  CPU and heat reduction.
- **SPU byte swaps stay one instruction.** LLVM 22 rewrote the SPU recompiler's `tbl` byte swap back
  into a slower sequence; the swap is kept.
- **Compiled SPU code is cached on ARM64.** The SPU object cache works on this platform, so the SPU
  runtime is rebuilt in seconds instead of half a minute of full-clock compiling on every boot.
- **No VRAM spill on unified memory.** Device-local and host memory are the same RAM on Adreno;
  moving data between them freed nothing and cost a copy each way.
- **Unified memory under Turnip.** Emulated PS3 memory is imported into Vulkan
  (`VK_EXT_external_memory_host`), and draws read their vertex arrays straight from it, including draws
  whose arrays span several memory blocks. Small win in LittleBigPlanet, which moves little vertex
  data; more in games that move a lot.
- **SPU reservation waits back off.** The wait for a lost reservation woke every 50 µs
  (~74,000 sleeps a second); it now backs off adaptively (~14,000).
- **JIT code on 2 MiB pages.** Compiled code is collapsed onto huge pages in the background, cutting
  the CPU's instruction page-table walks by about a third.
- **A boot hang fixed.** `select()`/`accept()` on emulated sockets could miss a wakeup; LittleBigPlanet
  hung on its first notice about one boot in ten.
- Builds on Linux and case-sensitive file systems.

**Disable MLAA (Settings → Performance, on by default).** MLAA is the post-process anti-aliasing
many PS3 games run on the SPUs; on this hardware it costs far more CPU than the smoothing is worth.
RPCS3 has no switch for it, only patches, so at every boot the setting switches on every
MLAA-disabling patch: RPCS3's universal "Disable SPU MLAA" patches (keyed by the hash of Sony's Edge
MLAA SPU module, so they catch any game that loads it: Journey, Tokyo Jungle, Portal 2, the Yakuza
games, SoulCalibur V, Shadow of the Colossus, ICO, MotorStorm Apocalypse, Sly Cooper: Thieves in
Time, LittleBigPlanet 2 and 3, Metro Last Light, God of War: Ascension, Twisted Metal, DmC and
dozens more) plus the game's own entries (God of War III, Killzone 3, Puppeteer, Dragon Age II,
GT6…). Turn it off, globally or per game, to keep a game's MLAA. Games with no such patch are
unaffected.

Tuning knobs, for experiments only, go in `Android/data/com.armsx3.uma/files/driver_env.txt` (one
`NAME=value` per line): `ARMSX3_LR_WAIT_CAP_US` (reservation-wait cap, default 800) and
`ARMSX3_HUGE_CODE=0` (turn the huge-page collapse off).

## Not included

A custom SPU-to-AArch64 compiler was written and measured during this work (its code is 1.2–1.9×
faster than LLVM's on LittleBigPlanet's long SPU loops, and exact on 1,200 captured test segments),
but it is not faster in the emulator yet and has an unresolved rendering bug, so it is not part of this
release. Textures in place and readback avoidance (the rest of the unified-memory design) are future
work.

## Building

As official ARMSX3 (see the README), on Linux too. The release APK is signed with this fork's own
key, so it cannot be installed over an official sideload build, and official releases cannot be
installed over it; they are different applications anyway.

## Support

If this is useful to you: [ko-fi.com/jamesfo](https://ko-fi.com/jamesfo).
