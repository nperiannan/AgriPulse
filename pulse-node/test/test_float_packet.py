"""
Unit tests for FloatPacket encoding.

Verifies that the 4-byte packet sent by the transmitter matches
the expected byte layout for all tank states:

    FULL  -> [0x02, 1, 1, 1]   (sw_full=1, sw_half=1, sw_low=1)
    HALF  -> [0x02, 0, 1, 1]   (sw_full=0, sw_half=1, sw_low=1)
    LOW   -> [0x02, 0, 0, 1]   (sw_full=0, sw_half=0, sw_low=1)
    EMPTY -> [0x02, 0, 0, 0]   (sw_full=0, sw_half=0, sw_low=0)

Byte order: [type, sw_full, sw_half, sw_low]
"""

import struct
import unittest

# Matches the #pragma pack(push, 1) struct in main.cpp
# B = uint8_t, 4 fields = 4 bytes
PACKET_FORMAT = "<4B"  # little-endian, 4 unsigned bytes
PACKET_TYPE = 0x02


def build_packet(low: int, half: int, full: int) -> bytes:
    """Build a FloatPacket exactly as the transmitter firmware does."""
    return struct.pack(PACKET_FORMAT, PACKET_TYPE, full, half, low)


class TestFloatPacketSize(unittest.TestCase):
    def test_packet_is_4_bytes(self):
        pkt = build_packet(0, 0, 0)
        self.assertEqual(len(pkt), 4)

    def test_struct_calcsize(self):
        self.assertEqual(struct.calcsize(PACKET_FORMAT), 4)


class TestPacketType(unittest.TestCase):
    def test_type_byte_is_0x02(self):
        pkt = build_packet(0, 0, 0)
        self.assertEqual(pkt[0], 0x02)


class TestFullState(unittest.TestCase):
    """FULL -> all three switches triggered: sw_full=1, sw_half=1, sw_low=1"""

    def test_raw_bytes(self):
        pkt = build_packet(low=1, half=1, full=1)
        self.assertEqual(pkt, bytes([0x02, 1, 1, 1]))

    def test_fields(self):
        pkt = build_packet(low=1, half=1, full=1)
        t, sw_full, sw_half, sw_low = struct.unpack(PACKET_FORMAT, pkt)
        self.assertEqual(t, 0x02)
        self.assertEqual(sw_full, 1)
        self.assertEqual(sw_half, 1)
        self.assertEqual(sw_low, 1)


class TestHalfState(unittest.TestCase):
    """HALF -> sw_full=0, sw_half=1, sw_low=1"""

    def test_raw_bytes(self):
        pkt = build_packet(low=1, half=1, full=0)
        self.assertEqual(pkt, bytes([0x02, 0, 1, 1]))

    def test_fields(self):
        pkt = build_packet(low=1, half=1, full=0)
        t, sw_full, sw_half, sw_low = struct.unpack(PACKET_FORMAT, pkt)
        self.assertEqual(sw_full, 0)
        self.assertEqual(sw_half, 1)
        self.assertEqual(sw_low, 1)


class TestLowState(unittest.TestCase):
    """LOW -> sw_full=0, sw_half=0, sw_low=1"""

    def test_raw_bytes(self):
        pkt = build_packet(low=1, half=0, full=0)
        self.assertEqual(pkt, bytes([0x02, 0, 0, 1]))

    def test_fields(self):
        pkt = build_packet(low=1, half=0, full=0)
        t, sw_full, sw_half, sw_low = struct.unpack(PACKET_FORMAT, pkt)
        self.assertEqual(sw_full, 0)
        self.assertEqual(sw_half, 0)
        self.assertEqual(sw_low, 1)


class TestEmptyState(unittest.TestCase):
    """EMPTY -> sw_full=0, sw_half=0, sw_low=0"""

    def test_raw_bytes(self):
        pkt = build_packet(low=0, half=0, full=0)
        self.assertEqual(pkt, bytes([0x02, 0, 0, 0]))

    def test_fields(self):
        pkt = build_packet(low=0, half=0, full=0)
        t, sw_full, sw_half, sw_low = struct.unpack(PACKET_FORMAT, pkt)
        self.assertEqual(sw_full, 0)
        self.assertEqual(sw_half, 0)
        self.assertEqual(sw_low, 0)


class TestByteOrder(unittest.TestCase):
    """Verify the exact memory layout: byte[0]=type, [1]=sw_full, [2]=sw_half, [3]=sw_low"""

    def test_byte_positions(self):
        pkt = build_packet(low=1, half=1, full=1)
        self.assertEqual(pkt[0], 0x02, "byte[0] must be type")
        self.assertEqual(pkt[1], 1,    "byte[1] must be sw_full")
        self.assertEqual(pkt[2], 1,    "byte[2] must be sw_half")
        self.assertEqual(pkt[3], 1,    "byte[3] must be sw_low")

    def test_distinguishable_fields(self):
        """Each field at a unique position — use distinct values to confirm."""
        # Manually craft: type=0x02, sw_full=0xAA, sw_half=0xBB, sw_low=0xCC
        pkt = struct.pack(PACKET_FORMAT, 0x02, 0xAA, 0xBB, 0xCC)
        self.assertEqual(pkt[0], 0x02)
        self.assertEqual(pkt[1], 0xAA)
        self.assertEqual(pkt[2], 0xBB)
        self.assertEqual(pkt[3], 0xCC)


class TestInvalidCombinations(unittest.TestCase):
    """Physically impossible states — packet should still encode correctly."""

    def test_full_without_lower_switches(self):
        # Full=1 but half=0, low=0 — can't happen in real tank, but encoding must be correct
        pkt = build_packet(low=0, half=0, full=1)
        self.assertEqual(pkt, bytes([0x02, 1, 0, 0]))

    def test_half_without_low(self):
        pkt = build_packet(low=0, half=1, full=0)
        self.assertEqual(pkt, bytes([0x02, 0, 1, 0]))


if __name__ == "__main__":
    unittest.main()
