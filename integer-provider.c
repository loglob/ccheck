/* number-provider:
	provides 30 values centered around 0
	and randomized values using at most 3/4 of available bits
*/
#include <stdio.h>
#include <stdlib.h>
#include "interface.h"
#include <inttypes.h>
#include <sys/random.h>

PROVIDER(uint16_t, fixedU16)
PROVIDER(uint16_t, randomizedU16)
PROVIDER(int16_t, fixedI16)
PROVIDER(int16_t, randomizedI16)
PROVIDER(uint32_t, fixedU32)
PROVIDER(uint32_t, randomizedU32)
PROVIDER(int32_t, fixedI32)
PROVIDER(int32_t, randomizedI32)
PROVIDER(uint64_t, fixedU64)
PROVIDER(uint64_t, randomizedU64)
PROVIDER(int64_t, fixedI64)
PROVIDER(int64_t, randomizedI64)

// number of integers to generate in fixed providers
#define N 30

#define FIXED_UNSIGNED {\
	if(cap < N) return N; \
	for(int i = 0; i < N; ++i) data[i] = i; \
	return N; \
 }

void xGetRandom(size_t n, void *buffer)
{
	ssize_t r = getrandom(buffer, n, 0);

	if(r < 0)
	{
		perror("getrandom");
		exit(EXIT_FAILURE);
	}
	else if(r != (ssize_t)n)
	{
		fprintf(stderr, "getrandom failed to produce enough output");
		exit(EXIT_FAILURE);
	}
}

size_t fixedU16(size_t cap, uint16_t data[restrict static cap])
FIXED_UNSIGNED

size_t fixedU32(size_t cap, uint32_t data[restrict static cap])
FIXED_UNSIGNED

size_t fixedU64(size_t cap, uint64_t data[restrict static cap])
FIXED_UNSIGNED


#define FIXED_SIGNED {\
	if(cap < N) return N; \
	for(int i = 0; i < N; ++i) data[i] = i - (N/2) + 1; \
	return N; \
 }

size_t fixedI16(size_t cap, int16_t data[restrict static cap])
FIXED_SIGNED

size_t fixedI32(size_t cap, int32_t data[restrict static cap])
FIXED_SIGNED

size_t fixedI64(size_t cap, int64_t data[restrict static cap])
FIXED_SIGNED


#define RANDOM_UNSIGNED(mask) { \
	xGetRandom(sizeof(*data) * cap, data); \
	for(size_t i = 0; i < cap; ++i) data[i] &= mask; \
	return cap; \
}

size_t randomizedU16(size_t cap, uint16_t data[restrict static cap])
RANDOM_UNSIGNED(0xFFF)

size_t randomizedU32(size_t cap, uint32_t data[restrict static cap])
RANDOM_UNSIGNED(0xFFFFFF)

size_t randomizedU64(size_t cap, uint64_t data[restrict static cap])
RANDOM_UNSIGNED(0xFFFFFFFFFFFFL)


#define RANDOM_SIGNED(mask, sign) { \
	xGetRandom(sizeof(*data) * cap, data); \
	for(size_t i = 0; i < cap; ++i) \
	{ \
		bool neg = data[i] & (sign); \
		data[i] &= (mask); \
		if(neg) data[i] = -data[i]; \
	} \
	return cap; \
}

size_t randomizedI16(size_t cap, int16_t data[restrict static cap])
RANDOM_SIGNED(0xFFF, 1 << 15)

size_t randomizedI32(size_t cap, int32_t data[restrict static cap])
RANDOM_SIGNED(0xFFFFFF, 1 << 31)

size_t randomizedI64(size_t cap, int64_t data[restrict static cap])
RANDOM_SIGNED(0xFFFFFFFFFFFFL, 1L << 63)


#define FMT(str) { return snprintf(to, n, str, *data); }

size_t format_uint16_t(char *to, size_t n, const uint16_t data[restrict static 1])
FMT("%" PRIu16)

size_t format_int16_t(char *to, size_t n, const int16_t data[restrict static 1])
FMT("%" PRId16)

size_t format_uint32_t(char *to, size_t n, const uint32_t data[restrict static 1])
FMT("%" PRIu32)

size_t format_int32_t(char *to, size_t n, const int32_t data[restrict static 1])
FMT("%" PRId32)

size_t format_uint64_t(char *to, size_t n, const uint64_t data[restrict static 1])
FMT("%" PRIu64)

size_t format_int64_t(char *to, size_t n, const int64_t data[restrict static 1])
FMT("%" PRId64)
