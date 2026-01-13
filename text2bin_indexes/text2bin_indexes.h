#include <stdint.h>


struct Base {
	uint8_t nbm;
	uint8_t nbk;
	uint8_t nwm;
	uint8_t nwk;
	uint8_t color;
	uint8_t singlevalue;		/* EGDB_WIN, etc, if the subdb is singlevalue. */
	uint16_t subslicenum;
	uint32_t num_idx_blocks;		/* 0 means the subdb is singlevalue. */
	uint32_t first_idx_block;
	uint16_t startbyte;
	uint16_t pad;
};
