#pragma once
#include <algorithm>


namespace rle_compr {

#define SKIPS 58        /* the number of skips, for fully resolved dbs. */
#define MAXSKIP 10000   /* the largest skip value, for fully resolved dbs. */

#define SKIPS_INC 36	/* for incomplete dbs with unresolved positions. */
#define MAXSKIP_INC 500

#define MTC_SKIPS 94
#define MAX_MTCSKIP 1000000000

/* Encode a moves-to-conv number that is >= the threshold for saving. */
#define MTC_ENCODE(mtc) (MTC_SKIPS + (mtc) / 2)

/* Decode a moves-to-conv number that is >= the threshold for saving. */
#define MTC_DECODE(val) (2 * ((val) - MTC_SKIPS))

#define NOT_SINGLEVALUE 127
#define SINGLEVALUE_CODES ".+-=?!"

void init_compression_tables();
void init_runlength16_mtc();

extern int skip[SKIPS];
extern int skip_inc[SKIPS_INC];
extern int mtc_skip[MTC_SKIPS];
extern int skipindex[MAXSKIP + 1];			/* index of the next smaller or equal skip. */
extern int skipindex_inc[MAXSKIP_INC + 1];	/* index of the next smaller or equal skip (for incomplete dbs). */

extern int runlength[256];					/* run length for every byte. */
extern int runlength_inc[256];				/* run length for every byte (for incomplete dbs). */
extern unsigned short runlength16[65536];	/* run length of each 2 bytes. */
extern int compressed_value[256];			/* the db value for every byte. */
extern int compressed_value_inc[256];		/* the db value for every byte (for incomplete dbs). */
extern int runlength_mtc[256];
extern unsigned int *runlength16_mtc;

template <class RUNL_T>
inline size_t get_mtc_runtok(RUNL_T runlength)
{
	auto it = std::upper_bound(std::begin(mtc_skip), std::end(mtc_skip), runlength);
	return(it - std::begin(mtc_skip) - 1);
}


/*
 * Search forward for a target position index in a compressed block.
 * Return the block index containing the target position.
 * entry_and_exit_pos_index has the entry and exit position indexes.
 */
template <class RUNL_T, class COMPR_BLOCK_T, class INDEX_T>
inline int find_compr_val_fwd(RUNL_T *runlength_table, COMPR_BLOCK_T *compr_block, int compr_block_starti, INDEX_T target_pos_index, INDEX_T &entry_and_exit_pos_index)
{
	int blocki = compr_block_starti;
	while (entry_and_exit_pos_index <= target_pos_index) {
		entry_and_exit_pos_index += runlength_table[compr_block[blocki]];
		blocki++;
	}

	blocki--;
	entry_and_exit_pos_index -= runlength_table[compr_block[blocki]];
	return(blocki);
}


/*
 * Search in reverse for a target position index in a compressed block.
 * Return the block index containing the target position.
 * compr_block_lasti has the index into the last entry in compr_block.
 * entry_and_exit_pos_index has the entry and exit position indexes.
 */
template <class RUNL_T, class COMPR_BLOCK_T, class INDEX_T>
inline int find_compr_val_rev(RUNL_T *runlength_table, COMPR_BLOCK_T *compr_block, int compr_block_lasti, INDEX_T target_pos_index, INDEX_T &entry_and_exit_pos_index)
{
	int blocki = compr_block_lasti;
	while (entry_and_exit_pos_index > target_pos_index) {
		blocki--;
		entry_and_exit_pos_index -= runlength_table[compr_block[blocki]];
	}
	return(blocki);
}


/*
 * Extract a WLD value from an RLE compression token.
 */
inline int decode_compr_wld(uint8_t compr_tok, int index)
{
	int wld_value;

	if (compr_tok > 80) {
		/* A runlength code. */
		wld_value = rle_compr::compressed_value[compr_tok];
	}
	else {
		/* an uncompressed byte */
		switch (index) {
		case 0:
			wld_value = compr_tok % 3;
			break;
		case 1:
			wld_value = (compr_tok / 3) % 3;
			break;
		case 2:
			wld_value = (compr_tok / 9) % 3;
			break;
		case 3:
			wld_value = (compr_tok / 27) % 3;
			break;
		}
	}
	++wld_value;
	return(wld_value);
}


} // namespace
