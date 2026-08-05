import struct, subprocess, math, collections, itertools

ifl = open("/home/bsellers/Development/Game-&-Watch-Mods/game-and-watch-backup/backups/internal_flash_backup_mario.bin",'rb').read()
ext = open("/home/bsellers/Development/Game-&-Watch-Mods/game-and-watch-backup/backups/flash_backup_mario.bin",'rb').read()[:0x2000]

key_u   = struct.unpack('<4I', ifl[0x106f4:0x10704])
nonce_u = struct.unpack('<2I', ifl[0x106e4:0x106ec])

def conv(uints, reverse, be):
    us = list(reversed(uints)) if reverse else list(uints)
    return b''.join(struct.pack('>I' if be else '<I', u) for u in us)

def entropy(b):
    c = collections.Counter(b)
    return -sum((n/len(b))*math.log2(n/len(b)) for n in c.values())

best = []
for krev, kbe in itertools.product([0,1],[0,1]):
    key = conv(key_u, krev, kbe)
    for nrev, nbe in itertools.product([0,1],[0,1]):
        nonce = conv(nonce_u, nrev, nbe)
        for ver_be in [0,1]:
            ver = struct.pack('>H' if ver_be else '<H', 0x7123)
            for region in (1,2,3,4):
                ivs = bytearray()
                for b in range(len(ext)//16):
                    ctr = (0x90000000 + b*16) >> 4
                    ivs += nonce + b'\x00\x00' + ver + bytes([((region-1)<<4)|((ctr>>24)&0xf),(ctr>>16)&0xff,(ctr>>8)&0xff,ctr&0xff])
                p = subprocess.run(['openssl','enc','-aes-128-ecb','-K',key.hex(),'-nopad','-nosalt'],input=bytes(ivs),capture_output=True)
                ks = p.stdout
                out = bytearray(ext)
                for b in range(len(ext)//16):
                    blk = ks[b*16:(b+1)*16][::-1]
                    for j in range(16): out[b*16+j] ^= blk[j]
                e = entropy(bytes(out[:0x1000]))
                if e < 7.8:
                    print('HIT ent=%.2f krev=%d kbe=%d nrev=%d nbe=%d ver_be=%d region=%d' % (e,krev,kbe,nrev,nbe,ver_be,region))
                    print(out[:64].hex())
                    best.append((e,krev,kbe,nrev,nbe,ver_be,region))
print('done,', len(best), 'candidates')
