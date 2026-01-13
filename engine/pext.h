#pragma once
#include <intrin.h>
#include <cstdint>
#include "instruction_set.h"

extern bool _cpu_has_pext;


inline uint64_t pext64(uint64_t source, uint64_t mask)
{
#ifdef _WIN64
	return(_pext_u64(source, mask));
#else
	return(0);
#endif
}


inline uint32_t pext32(uint32_t source, uint32_t mask)
{
	return(_pext_u32(source, mask));
}


inline bool cpu_has_pext(void)
{
	return(_cpu_has_pext);
}


/* Force pext to be used or not used, for testing. */
inline void force_pext(bool state)
{
	_cpu_has_pext = state;
}


bool init_pext(void);
uint64_t pdep_u64_emul(uint64_t val, uint64_t mask);

