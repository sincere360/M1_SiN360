#!/usr/bin/env python3
"""Append STM32 hardware CRC32 to firmware binary for SD card update."""
import struct
import sys
import os

def stm32_crc32(data):
    """STM32 hardware CRC32: polynomial 0x04C11DB7, init 0xFFFFFFFF, no bit reversal."""
    crc = 0xFFFFFFFF
    for i in range(0, len(data), 4):
        word = struct.unpack('<I', data[i:i+4])[0]
        crc ^= word
        for _ in range(32):
            if crc & 0x80000000:
                crc = (crc << 1) ^ 0x04C11DB7
            else:
                crc = crc << 1
            crc &= 0xFFFFFFFF
    return crc

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} input.bin output.bin")
        sys.exit(1)

    infile = sys.argv[1]
    outfile = sys.argv[2]

    with open(infile, 'rb') as f:
        data = f.read()

    # Size must be 4-byte aligned
    if len(data) % 4 != 0:
        print(f"ERROR: Input size {len(data)} is not 4-byte aligned")
        sys.exit(1)

    crc = stm32_crc32(data)
    output = data + struct.pack('<I', crc)

    with open(outfile, 'wb') as f:
        f.write(output)

    print(f"CRC32: 0x{crc:08X} | {len(data)} + 4 = {len(output)} bytes | {os.path.basename(outfile)}")
