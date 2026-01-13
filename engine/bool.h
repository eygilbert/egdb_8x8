#pragma once
#include <intrin.h>
#include <stdint.h>
#include "project.h"


inline uint32_t get_lsb(uint32_t x)
{
	return(x & -(int32_t)x);
}


inline uint64_t get_lsb(uint64_t x)
{
	return(x & -(int64_t)x);
}


inline uint32_t clear_lsb(uint32_t x)
{
	return(x & (x - 1));
}


inline uint64_t clear_lsb(uint64_t x)
{
	return(x & (x - 1));
}

inline int LSB(uint32_t x)
{
	unsigned long bitpos;

	if (_BitScanForward(&bitpos, x))
		return(bitpos);
	else
		return(0);
}


inline int MSB(uint32_t x)
{
	unsigned long bitpos;

	if (_BitScanReverse(&bitpos, x))
		return(bitpos);
	else
		return(0);
}


#ifdef _WIN64
inline int LSB64(uint64_t x)
{
	unsigned long bitpos;

	if (_BitScanForward64(&bitpos, x))
		return(bitpos);
	else
		return(0);
}

inline int MSB64(uint64_t x)
{
	unsigned long bitpos;

	if (_BitScanReverse64(&bitpos, x))
		return(bitpos);
	else
		return(0);
}

#else

inline int LSB64(uint64_t x)
{
	if (x & 0xffffffff)
		return(LSB((uint32_t)(x & 0xffffffff)));
	else
		return(32 + LSB((uint32_t)(x >> 32)));
}

inline int MSB64(uint64_t x)
{
	if (x & 0xffffffff00000000)
		return(32 + MSB((uint32_t)(x >> 32)));
	else
		return(MSB((uint32_t)(x & 0xffffffff)));
}
#endif

