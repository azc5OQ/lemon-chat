/*
 * sha256.h
 *
 *  Created on: May 9, 2018
 *      Author: ITH
 */

#ifndef SHA256_H_

#define SHA256_H_

#include <stdio.h>
#include <string.h>

#define uchar unsigned char // 8-bit byte
#define uint unsigned int // 32-bit word

// DBL_INT_ADD treats two unsigned ints a and b as one 64-bit integer and adds c to it
#define DBL_INT_ADD(a, b, c)      \
	if (a > 0xffffffff - (c)) \
		++b;              \
	a += c;
#define ROTLEFT(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define ROTRIGHT(a, b) (((a) >> (b)) | ((a) << (32 - (b))))

#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x, 2) ^ ROTRIGHT(x, 13) ^ ROTRIGHT(x, 22))
#define EP1(x) (ROTRIGHT(x, 6) ^ ROTRIGHT(x, 11) ^ ROTRIGHT(x, 25))
#define SIG0(x) (ROTRIGHT(x, 7) ^ ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x, 17) ^ ROTRIGHT(x, 19) ^ ((x) >> 10))

typedef struct ITH_SHA256_CTX
{
	uchar data[64];
	uint datalen;
	uint bitlen[2];
	uint state[8];
} ITH_SHA256_CTX;

void ith_sha256_transform(ITH_SHA256_CTX *ctx, uchar data[]);

void ith_sha256_init(ITH_SHA256_CTX *ctx);

void ith_sha256_update(ITH_SHA256_CTX *ctx, uchar data[], uint len);

void ith_sha256_final(ITH_SHA256_CTX *ctx, uchar hash[]);

#endif
