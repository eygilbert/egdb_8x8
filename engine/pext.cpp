#include <intrin.h>
#include "bool.h"
#include "instruction_set.h"
#include "pext.h"


bool _cpu_has_pext;
static bool did_init = init_pext();

bool init_pext(void)
{
#ifdef _WIN64
	_cpu_has_pext = InstructionSet::BMI2() && InstructionSet::Vendor() == "GenuineIntel";
	return(_cpu_has_pext);
#else
	return(false);
#endif
}


uint64_t pdep_u64_emul(uint64_t val, uint64_t mask)
{
	uint64_t res = 0;
	for (uint64_t bb = 1; mask; bb += bb) {
		if (val & bb)
			res |= get_lsb(mask);
		mask = clear_lsb(mask);
	}
	return res;
}


