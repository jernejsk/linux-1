// SPDX-License-Identifier: GPL-2.0-only

#include <crypto/sha2.h>
#include <crypto/utils.h>
#include <linux/string.h>
#include <linux/types.h>

#include "core.h"

static u8 uwe5622_choose(u8 x, u8 y, u8 z)
{
	return (x & y) ^ (~x & z);
}

static u8 uwe5622_majority(u8 x, u8 y, u8 z)
{
	return (x & y) ^ (x & z) ^ (y & z);
}

static void uwe5622_confuse(const u8 input[4], u8 output[16])
{
	u8 digest[SHA256_DIGEST_SIZE];
	u8 mixed[36];
	u8 majority[4];
	int i;

	sha256(input, 4, digest);
	for (i = 0; i < 4; i++) {
		memcpy(mixed + i * 9, digest + i * 8, 8);
		mixed[i * 9 + 8] = input[i];
	}

	for (i = 0; i < 12; i++)
		output[i] = uwe5622_choose(mixed[i * 3],
					   mixed[i * 3 + 1], mixed[i * 3 + 2]);

	for (i = 0; i < 4; i++) {
		majority[i] = uwe5622_majority(output[i * 3],
					       output[i * 3 + 1],
						output[i * 3 + 2]);
		output[12 + i] = majority[i] ^ input[i];
	}

	memzero_explicit(digest, sizeof(digest));
	memzero_explicit(mixed, sizeof(mixed));
}

static int uwe5622_deconfuse(const u8 input[16], u8 output[4])
{
	u8 check[16];
	int i;

	for (i = 0; i < 4; i++)
		output[i] = uwe5622_majority(input[i * 3], input[i * 3 + 1],
					     input[i * 3 + 2]) ^ input[12 + i];

	uwe5622_confuse(output, check);
	return crypto_memneq(input, check, sizeof(check)) ? -EBADMSG : 0;
}

static void uwe5622_encrypt(const u8 input[4], u8 output[4])
{
	u8 digest[SHA256_DIGEST_SIZE];
	u8 mixed[36];
	u8 majority[12];
	int i;

	sha256(input, 4, digest);
	for (i = 0; i < 4; i++) {
		memcpy(mixed + i * 9, digest + i * 8, 8);
		mixed[i * 9 + 8] = input[i];
	}

	for (i = 0; i < 12; i++)
		majority[i] = uwe5622_majority(mixed[i * 3],
					       mixed[i * 3 + 1], mixed[i * 3 + 2]);
	for (i = 0; i < 4; i++)
		output[i] = uwe5622_choose(majority[i * 3],
					   majority[i * 3 + 1], majority[i * 3 + 2]);

	memzero_explicit(digest, sizeof(digest));
	memzero_explicit(mixed, sizeof(mixed));
	memzero_explicit(majority, sizeof(majority));
}

int uwe5622_bind_verify(const u8 challenge[16], u8 response[16])
{
	u8 random[4];
	u8 encrypted[4];
	int ret;

	ret = uwe5622_deconfuse(challenge, random);
	if (ret)
		return ret;

	uwe5622_encrypt(random, encrypted);
	uwe5622_confuse(encrypted, response);
	memzero_explicit(random, sizeof(random));
	memzero_explicit(encrypted, sizeof(encrypted));

	return 0;
}
