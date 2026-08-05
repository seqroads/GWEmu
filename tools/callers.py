#!/usr/bin/env python3
"""Find every BL/B.W in the internal flash image that targets a given address."""
import struct, sys
BASE = 0x08000000
img = open("/home/bsellers/Development/Game-&-Watch-Mods/game-and-watch-backup/backups/internal_flash_backup_mario.bin","rb").read()

def decode(addr):
    hw1 = struct.unpack_from('<H', img, addr - BASE)[0]
    hw2 = struct.unpack_from('<H', img, addr - BASE + 2)[0]
    if (hw1 & 0xF800) != 0xF000: return None
    if (hw2 & 0xD000) not in (0xD000, 0xC000, 0x9000, 0x8000): return None
    S = (hw1 >> 10) & 1
    imm10 = hw1 & 0x3FF
    j1 = (hw2 >> 13) & 1
    j2 = (hw2 >> 11) & 1
    imm11 = hw2 & 0x7FF
    i1 = 1 - (j1 ^ S)
    i2 = 1 - (j2 ^ S)
    imm = (S << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1)
    if imm & (1 << 24): imm -= (1 << 25)
    kind = 'BL' if (hw2 & 0x4000) else 'B.W'
    return kind, addr + 4 + imm

targets = [int(a, 0) for a in sys.argv[1:]]
for off in range(0, len(img) - 4, 2):
    r = decode(BASE + off)
    if r and r[1] in targets:
        print("%08x  %-4s -> %08x" % (BASE + off, r[0], r[1]))
