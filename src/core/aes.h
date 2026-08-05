/* aes.h - AES-128, encryption only
 *
 * Both users need the forward cipher and nothing else: OTFDEC generates a
 * keystream with it, and GCM is built entirely out of encryptions.
 */
#ifndef GW_AES_H
#define GW_AES_H

#include "gw.h"

typedef struct { u8 rk[11][16]; } AES128;

void aes128_expand(AES128 *a, const u8 key[16]);
void aes128_encrypt(const AES128 *a, const u8 in[16], u8 out[16]);

#endif /* GW_AES_H */
