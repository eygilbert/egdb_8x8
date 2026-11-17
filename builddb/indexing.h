#pragma once
#include <stdint.h>
#include "board.h"
#include "bicoef.h"
#include "bitcount.h"
#include "egdb.h"
#include "pext.h"

#define MAXPIECES_INDEXING 12
#define MAXPIECE_INDEXING 7

#define MAX_SUBSLICE_INDICES (__int64)(0x80000000)
#define DTW_SUBSLICE_INDICES (__int64)(0x40000000)

BITBOARD free_square_bitboard(int logical_square, BITBOARD occupied);
BITBOARD free_square_bitboard_rev(int logical_square, BITBOARD occupied);

BITBOARD index2bitboard(uint32_t index, int num_squares, int num_pieces, int offset);
BITBOARD index2bitboard(uint32_t index, int num_squares, int num_pieces, int offset, BITBOARD occupied);
BITBOARD index2bitboard_rev(uint32_t index, int num_squares, int num_pieces, int offset, BITBOARD occupied);

int64_t getslicesize_gaps(int bm, int bk, int wm, int wk);

/* This next group of functions provides slice indexing by rank0. In this method, the count of
 * black men on rank0 changes the least often during the placement of the pieces with successive indexes.
 * This makes it easy to deal with the interferences between black and white men. The drawback is that
 * it creates a non-monotonic advancement of pieces with increasing indexes. There is
 * not a simple predecessor/successor relationship between the positions of 2 different indexes.
 *
 * Indexing permutation code definitions. 
 * The permuation code is integer with 5 bits defined.
 * The first 3 bits define the 6 possible groupings that describe how the position index is formed from the indexes of the 
 * 3 individual components, men, black kings, and white kings.
 * The order in the groupings determine which piece types are reconfigured most often as the position index is incremented.
 * The notation is {least often, middle, most often}. For example, permuation 0	{men, bk, wk} means that as the position index is incremented, the white kings
 * will go through their range of positions most often, the black king configuration will change less often, and the men 
 * will change the least often.
 * 
 * Bit 3, reverse_men_order, defines how the men index is formed. If 0, white men move most often, then black men. If 1, then the opposite
 * Bit 4, men_index_by_bmindex, determines how the black position changes with increasing indexes. If 1, then the least advanced black man advances first,
 * then the next least advanced black man, etc. This is the 'normal' way to index pieces of the same type.
 * If bit 4 is clear, then the black men are separated into 2 groups, the rank0 group, and the rest. The rank0 group is reconfigured the least often.
 * The number of black men on rank0 decreases as the men index is increased. So for men index 0, the max number of black men are on rank0.
 * 
 *	0	{men, bk, wk}
 *	1	{men, wk, bk}
 *	2	{wk, bk, men}
 *	3	{bk, wk, men}
 *	4	{wk, men, bk}
 *	5	{bk, men, wk}
 */
const int reverse_men_order = 8;		/* bit3 = 0: {bm, wm}. bit3 = 1: {wm, bm}. */
const int men_index_by_bmindex = 16;	/* bit4 = 0 means indexing by count on row0. 1 means the normal progression of advancing the least advanced bm first, etc. */

void init_indexing(void);
int get_num_subslices(int bm, int bk, int wm, int wk, uint32_t subslice_size);
int64_t getdatabasesize_slice(int bm, int bk, int wm, int wk);
int64_t position_to_index_slice(EGDB_ROW_REVERSED_BITBOARD *p, int bm, int bk, int wm, int wk, int permutation = 0);
void indextoposition_slice(int64_t index, EGDB_ROW_REVERSED_BITBOARD *pos, int bm, int bk, int wm, int wk, int permutation = 0);


inline int64_t position_to_index_slice(BOARD *p, int bm, int bk, int wm, int wk, int permutation = 0)
{
	EGDB_ROW_REVERSED_BITBOARD pos;

	pos.black_man = p->black & ~p->king;
	pos.black_king = p->black & p->king;
	pos.white_man = p->white & ~p->king;
	pos.white_king = p->white & p->king;
	return(position_to_index_slice(&pos, bm, bk, wm, wk, permutation));
}


inline int64_t position_to_index_slice(EGDB_ROW_REVERSED_BITBOARD *p, int permutation = 0)
{
	int nbm, nbk, nwm, nwk;

	nbm = bitcount(p->black_man);
	nbk = bitcount(p->black_king);
	nwm = bitcount(p->white_man);
	nwk = bitcount(p->white_king);
	return(position_to_index_slice((EGDB_ROW_REVERSED_BITBOARD *)p, nbm, nbk, nwm, nwk, permutation));
}


inline void indextoposition_slice(int64_t index, BOARD *p, int bm, int bk, int wm, int wk, int permutation = 0)
{
	EGDB_ROW_REVERSED_BITBOARD pos;

	indextoposition_slice(index, &pos, bm, bk, wm, wk, permutation);
	p->black = pos.black_man | pos.black_king;
	p->white = pos.white_man | pos.white_king;
	p->king = pos.black_king | pos.white_king;
}


inline void get_subindices(int64_t index64, int &subslicenum, uint32_t &index32, uint32_t max_indices)
{
	subslicenum = (int)(index64 / max_indices);
	index32 = (unsigned int)(index64 - (int64_t)subslicenum * max_indices);
}


inline uint32_t bitboard2index(BITBOARD bb, int offset)
{
	int piece, bitnum;
	uint32_t index;

	for (index = 0, piece = 1; bb; ++piece) {

		/* Get square of next piece. */
		bitnum = LSB(bb);

		/* Remove him from the bitboard. */
		bb = clear_lsb(bb);

		/* Add to his index. */
		index += choose(bitnum - offset, piece);
	}
	return(index);
}


inline uint32_t bitboard2index(BITBOARD bb, int offset, BITBOARD interfering)
{
	int piece, bitnum, square0;
	uint32_t index;
	BITBOARD interfering_mask;

	if (cpu_has_pext())
		bb = _pext_u32(bb, ~interfering);
	for (piece = 1, index = 0; bb; ++piece) {

		/* Get square of the next piece. */
		bitnum = LSB(bb);
		bb = clear_lsb(bb);

		/* Subtract from his square the count of pieces on 
		 * squares 0....square-1.
		 */
		if (cpu_has_pext()) {
			index += choose(bitnum - offset, piece);
		}
		else {
			square0 = bitnum - offset;
			interfering_mask = (((BITBOARD)1 << bitnum) - 1);
			square0 -= bitcount(interfering & interfering_mask); 

			/* Add to his index. */
			index += choose(square0, piece);
		}
	}
	return(index);
}


inline uint32_t bitboard2index_rev(BITBOARD bb, int sq_offset, BITBOARD interfering)
{
	int piece, bitnum, square0;
	uint32_t index;

	for (index = 0, piece = 1; bb; ++piece) {
		
		/* Get square of most advanced piece. */
		bitnum = MSB(bb);

		/* Remove him from the bitboard. */
		bb = bb ^ ((BITBOARD)1 << bitnum);

		/* Get his 'logical' square number, or how far advance he is,
		 * accounting for interfering pieces.
		 */
		square0 = square_mirror(bitnum);
		square0 -= bitcount(interfering & ~(((BITBOARD)1 << bitnum) - 1));

		/* Add to his index. */
		index += choose(square0 - sq_offset, piece);
	}
	return(index);
}


/*
 * The man_index_monotonic namespace has functions for creating an index of black and white checkers men (no kings) that
 * follow a monotonic advancement of pieces with increasing indexes. In other words, if index1 < index2, then index1
 * cannot be a successor position of index2 by the rules of checkers. This is not the case with man indexing by rank0. 
 */
namespace man_index_monotonic {

	/* man_index_base[nbm][nwm][bm index] gives the base checker index for a given
	 * configuration of nbm, nwm, and bmindex. This table uses 86mb of memory,
	 * so it's only initialized if an access function is called, or by init().
	 * The access functions build it on first use without a lock, so a program that uses
	 * them from several threads must call init() first. The egdb DTW driver does this
	 * when it opens a database that needs the table.
	 */
	extern int64_t *man_index_base[MAXPIECE_INDEXING + 1][MAXPIECE_INDEXING + 1];
	extern bool did_init;
	extern int64_t init_man_index_base(void);

	/* Build the table if it has not been built. Return the number of bytes allocated, 0 if none. */
	inline int64_t init(void)
	{
		int64_t bytes = 0;

		if (!did_init) {
			bytes = init_man_index_base();
			did_init = true;
		}
		return(bytes);
	}

	inline int64_t base(int nbm, int nwm, uint32_t bmindex)
	{
		init();
		return(man_index_base[nbm][nwm][bmindex]);
	}

	inline uint32_t find_bmindex(int nbm, int nwm, uint32_t bmrange, int64_t menindex) 
	{
		init();
		int64_t *first = man_index_base[nbm][nwm];
		auto next_blockp = std::upper_bound(first, first + bmrange, menindex);
		uint32_t bmindex = (uint32_t)(next_blockp - first - 1);
		return(bmindex);
	}

}	// namespace man_index_monotonic

