#!/usr/bin/env python3
"""Decrypt Game & Watch (Mario) external flash using OTFDEC params from internal flash."""
import struct, subprocess, sys

INTFLASH = "/home/bsellers/Development/Game-&-Watch-Mods/game-and-watch-backup/backups/internal_flash_backup_mario.bin"
EXTFLASH = "/home/bsellers/Development/Game-&-Watch-Mods/game-and-watch-backup/backups/flash_backup_mario.bin"
OUT      = "flash_decrypted.bin"

ifl = open(INTFLASH,'rb').read()
ext = open(EXTFLASH,'rb').read()

# OTFDEC config (from firmware @ 0x80105e4, verified by disassembly)
VERSION = 0x7123
REGION  = 3            # region index (1-based), verified by decryption
START   = 0x90000000
END     = 0x900FDFFF
key_u   = struct.unpack('<4I', ifl[0x106f4:0x10704])   # key regs
nonce_u = struct.unpack('<2I', ifl[0x106e4:0x106ec])   # nonce regs

# KeyInfo.UIntArrayToBytes: reverse order, big-endian per uint
key   = b''.join(struct.pack('>I', u) for u in reversed(key_u))
iv_hi = b''.join(struct.pack('>I', u) for u in reversed(nonce_u)) + b'\x00\x00' + struct.pack('>H', VERSION)

nblocks = (len(ext) + 15) // 16
ivs = bytearray()
for b in range(nblocks):
    addr = START + b*16
    ctr = addr >> 4
    iv = bytearray(iv_hi)
    iv += bytes([ ((REGION-1)<<4) | ((ctr>>24)&0x0f), (ctr>>16)&0xff, (ctr>>8)&0xff, ctr&0xff ])
    ivs += iv

p = subprocess.run(['openssl','enc','-aes-128-ecb','-K',key.hex(),'-nopad','-nosalt'],
                   input=bytes(ivs), capture_output=True)
if p.returncode: sys.exit(p.stderr.decode())
ks = p.stdout

out = bytearray(ext)
end_blk_start = END & 0xfffff000
for b in range(nblocks):
    addr = START + b*16
    if addr < START or addr > END: continue
    blk = ks[b*16:(b+1)*16][::-1]   # block-reversed XOR, per OtfDecCryptor
    for j in range(16):
        i = b*16+j
        if i < len(out): out[i] ^= blk[j]

open(OUT,'wb').write(out)
print('wrote', OUT, len(out), 'bytes')
