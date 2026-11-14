#include <intrin.h>
#include "bool.h"
#include "instruction_set.h"
#include "pext.h"


bool _cpu_has_pext;
static bool did_init = init_pext();

/*
 * Return the cpu family from eax of cpuid leaf 1.
 */
int cpu_family(uint32_t cpuid1_eax)
{
	int family = (cpuid1_eax >> 8) & 0xf;

	if (family == 0xf)
		family += (cpuid1_eax >> 20) & 0xff;
	return(family);
}


/*
 * Return true if the cpu's pext is fast enough to use.
 * AMD before Zen 3 (family 0x19) has a very slow microcoded pext.
 */
bool pext_is_fast(const std::string &vendor, int family)
{
	if (vendor == "GenuineIntel")
		return(true);
	if (vendor == "AuthenticAMD" && family >= 0x19)
		return(true);
	return(false);
}


bool init_pext(void)
{
#ifdef _WIN64
	int regs[4];

	__cpuid(regs, 1);
	_cpu_has_pext = InstructionSet::BMI2() && pext_is_fast(InstructionSet::Vendor(), cpu_family((uint32_t)regs[0]));
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


