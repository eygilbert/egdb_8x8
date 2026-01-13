#pragma once
#include <cstdint>
#include "tunstall_decompress.h"

namespace tun_v2 {
	extern DECOMPRESS_CATALOG decompress_catalog_v2[30];
	extern unsigned char value_runs_v2[];
}

class tun_v2_decompress {
private:
	char virtual_to_real[256][4];

	void init_virtual_to_real(void)
	{
		int vr0, vr1, vr2, vr3, index;

		for (vr0 = 0; vr0 < 4; ++vr0) {
			for (vr1 = 0; vr1 < 4; ++vr1) {
				if (vr1 == vr0)
					continue;
				for (vr2 = 0; vr2 < 4; ++vr2) {
					if (vr2 == vr1 || vr2 == vr0)
						continue;
					for (vr3 = 0; vr3 < 4; ++vr3) {
						if (vr3 == vr0 || vr3 == vr1 || vr3 == vr2)
							continue;
						index = vr0 + vr1 * 4 + vr2 * 16 + vr3 * 64;
						virtual_to_real[index][0] = vr0;
						virtual_to_real[index][1] = vr1;
						virtual_to_real[index][2] = vr2;
						virtual_to_real[index][3] = vr3;
					}
				}
			}
		}
	}

public:
	void init(void) {
		init_virtual_to_real();
	}

	uint16_t *get_runlengths(uint8_t catalognum) {
		return(tun_v2::decompress_catalog_v2[catalognum].runlength_table);
	}

	char decode(int value_index, uint8_t encoded_val, uint8_t vmap_code, uint8_t catalognum) {
		uint16_t value_runs_offset = tun_v2::decompress_catalog_v2[catalognum].value_runs[encoded_val];
		int i = 0;

		while (1) {
			i += tun_v2::value_runs_v2[value_runs_offset + 1] + (tun_v2::value_runs_v2[value_runs_offset + 2] << 8);
			if (i > value_index)
				break;
			value_runs_offset += 3;
		}
		int virtual_value = tun_v2::value_runs_v2[value_runs_offset];
		char returnvalue = virtual_to_real[vmap_code][virtual_value];
		return(returnvalue);
	}
};

