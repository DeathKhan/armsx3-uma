#!/usr/bin/env python3
"""Executable LS representation/invalidation contracts, not an emulator replay test.

The model deliberately does not import production code. It establishes requirements
that a future implementation must satisfy, including a counterexample to clearing
one global dirty bit after validating only the current block.
"""
import random
import unittest

LS_SIZE = 256 * 1024
PAGE_SIZE = 4096
PAGE_COUNT = LS_SIZE // PAGE_SIZE

class Store:
    def __init__(self, host_order):
        self.host_order = host_order
        self.physical = bytearray(LS_SIZE)

    def location(self, address):
        address %= LS_SIZE
        return address ^ 15 if self.host_order else address

    def read(self, address, count):
        return bytes(self.physical[self.location(address + i)] for i in range(count))

    def write(self, address, data):
        for i, byte in enumerate(data):
            self.physical[self.location(address + i)] = byte

    def copy_from(self, source, address, source_address, count):
        # Snapshot semantics also cover overlapping ranges in the same store.
        self.write(address, source.read(source_address, count))

def page_mask(address, count):
    return sum(1 << page for page in {(address + i) % LS_SIZE // PAGE_SIZE for i in range(count)})

class InvalidationGraph:
    def __init__(self, blocks):
        self.blocks = blocks  # name -> complete dependency-page mask
        self.valid = set(blocks)
        self.dirty = 0

    def write(self, address, count):
        self.dirty |= page_mask(address, count)

    def drain_dirty(self, concurrent_write=None):
        # Equivalent sequencing requirement: snapshot/exchange, invalidate EVERY
        # dependent block, preserve writes published after the exchange.
        pending, self.dirty = self.dirty, 0
        if concurrent_write:
            self.write(*concurrent_write)
        self.valid.difference_update(name for name, mask in self.blocks.items() if mask & pending)

    def validate(self, name):
        self.drain_dirty()
        self.valid.add(name)  # Represents an actual byte comparison/recompile.

class LSContracts(unittest.TestCase):
    def test_quadword_native_load_has_register_order(self):
        store = Store(True)
        for address in (0, 16, PAGE_SIZE - 16, LS_SIZE - 16):
            data = bytes(range(16))
            store.write(address, data)
            self.assertEqual(store.physical[address:address + 16], data[::-1])
            self.assertEqual(int.from_bytes(store.physical[address:address + 16], 'little'),
                             int.from_bytes(data, 'big'))

    def test_scalar_and_instruction_accesses_need_address_permutation(self):
        store = Store(True)
        data = bytes(range(16))
        store.write(0, data)
        for size in (1, 2, 4, 8):
            for address in range(0, 16, size):
                native_address = address ^ (16 - size)
                self.assertEqual(int.from_bytes(store.physical[native_address:native_address + size], 'little'),
                                 int.from_bytes(data[address:address + size], 'big'))
        # A BE instruction array cast at the original PC is demonstrably wrong.
        self.assertNotEqual(int.from_bytes(store.physical[0:4], 'big'), int.from_bytes(data[0:4], 'big'))

    def test_partial_cross_quadword_page_and_wrap_transfers_all_modes(self):
        rng = random.Random(0xCE1103)
        boundaries = (0, 1, 7, 8, 12, 15, 16, PAGE_SIZE - 8, PAGE_SIZE - 1, LS_SIZE - 8, LS_SIZE - 1)
        for source_host in (False, True):
            for target_host in (False, True):
                source, target = Store(source_host), Store(target_host)
                for iteration in range(120):
                    a = boundaries[iteration % len(boundaries)] if iteration < 2 * len(boundaries) else rng.randrange(LS_SIZE)
                    b = boundaries[-1 - iteration % len(boundaries)] if iteration < 2 * len(boundaries) else rng.randrange(LS_SIZE)
                    n = (1, 2, 4, 8, 16, 31, 128, 4097)[iteration % 8]
                    data = rng.randbytes(n)
                    source.write(a, data)
                    target.copy_from(source, b, a, n)
                    self.assertEqual(target.read(b, n), data, (source_host, target_host, a, b, n))

    def test_overlap_uses_logical_snapshot(self):
        for host in (False, True):
            store = Store(host)
            store.write(0, bytes(range(64)))
            store.copy_from(store, 1, 0, 63)
            self.assertEqual(store.read(0, 64), bytes([0]) + bytes(range(63)))

    def test_full_bitmap_and_page63_wrap(self):
        self.assertEqual(page_mask(0, LS_SIZE), (1 << 64) - 1)
        self.assertEqual(page_mask(LS_SIZE - 1, 2), (1 << 63) | 1)
        self.assertEqual(page_mask(4095, 2), 3)
        self.assertEqual(page_mask(4096, 0), 0)

    def test_clearing_shared_bit_after_only_current_block_is_unsound(self):
        # A and B share page0. B changes; A's byte comparison still succeeds.
        dirty = 1
        validated_a = True
        if validated_a:
            dirty &= ~1
        # A naive B entry would pass; a correct dependency graph rejects B.
        self.assertFalse(dirty & 1)
        graph = InvalidationGraph({'A': 1, 'B': 1, 'C': 2})
        graph.write(128, 4)
        graph.validate('A')
        self.assertNotIn('B', graph.valid)
        self.assertIn('C', graph.valid)

    def test_all_blocks_intersecting_dirty_pages_are_invalidated(self):
        blocks = {'A': 1, 'B': 3, 'C': 2, 'D': 1 << 63, 'E': (1 << 63) | 1}
        for page in range(PAGE_COUNT):
            graph = InvalidationGraph(blocks)
            graph.write(page * PAGE_SIZE, 1)
            graph.drain_dirty()
            self.assertEqual(graph.valid, {name for name, mask in blocks.items() if not mask & (1 << page)})

    def test_writer_after_dirty_exchange_is_not_lost(self):
        graph = InvalidationGraph({'A': 1, 'B': 2})
        graph.write(0, 4)
        graph.drain_dirty(concurrent_write=(PAGE_SIZE, 4))
        self.assertEqual(graph.dirty, 2)
        self.assertNotIn('A', graph.valid)
        graph.drain_dirty()
        self.assertFalse(graph.valid)

if __name__ == '__main__':
    unittest.main()
