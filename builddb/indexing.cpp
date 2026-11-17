#include <stdio.h>
#include <algorithm>
#include "board.h"
#include "bitcount.h"
#include "bicoef.h"
#include "egdb_common.h"
#include "indexing.h"
#include "pext.h"
#include "reverse.h"


struct Index_pair {
	int64_t index;
	int64_t range;
};


/* base man index[nbm][nwm][nbm_rank0] */
int64_t man_index_by_rank0_base[MAXPIECE_INDEXING + 1][MAXPIECE_INDEXING + 1][ROWSIZE + 1];

void init_man_index_by_rank0_base(void)
{
	int bm, wm, bm0;
	int64_t base, partial;

	for (bm = 0; bm <= MAXPIECE_INDEXING; ++bm) {
		for (wm = 0; wm <= MAXPIECE_INDEXING; ++wm) {
			base = 0;
			for (bm0 = (std::min)(ROWSIZE, bm); bm0 >= 0; --bm0) {
				man_index_by_rank0_base[bm][wm][bm0] = base;

				/* First the number of men configurations excluding black's backrank. */
				partial = choose((NUMSQUARES - 2 * ROWSIZE), bm - bm0) * 
							(int64_t)choose((NUMSQUARES - ROWSIZE) - bm + bm0, wm);

				/* Add the contribution from black's kingrow. */
				partial *= choose(ROWSIZE, bm0);
				base += partial;
			}
		}
	}
}


void init_indexing(void)
{
	static bool did_init = false;

	if (!did_init) {
		init_man_index_by_rank0_base();
		did_init = true;
	}
}


int64_t men_range(int nbm, int nwm)
{
	int64_t range;

	range = man_index_by_rank0_base[nbm][nwm][0];
	range += choose((NUMSQUARES - 2 * ROWSIZE), nbm) * (int64_t)choose((NUMSQUARES - ROWSIZE) - nbm, nwm);
	return(range);
}


int64_t position_to_men_index(EGDB_ROW_REVERSED_BITBOARD *p, int nbm, int nwm, int permutation)
{
	BITBOARD bb_bm, bb_wm;
	uint32_t bmindex, wmindex;
	int64_t index;

	if (permutation & reverse_men_order) {
		bb_bm = reverse_bitboard(p->white_man);
		bb_wm = reverse_bitboard(p->black_man);
		std::swap(nbm, nwm);
	}
	else {
		bb_bm = p->black_man;
		bb_wm = p->white_man;
	}

	/* Set the index for the black men. */
	bmindex = bitboard2index(bb_bm, 0);

	/* Set the index for the white men,
	 * accounting for any interferences from the black men.
	 */
	wmindex = bitboard2index_rev(bb_wm, 0, bb_bm);
	index = wmindex + man_index_monotonic::base(nbm, nwm, bmindex);
	return(index);
}


int64_t position_to_men_index_by_rank0(EGDB_ROW_REVERSED_BITBOARD *p, int nbm, int nwm, int permutation)
{
	BITBOARD bb_bm, bb_wm;
	int nbm0;
	BITBOARD bm0_mask;
	int64_t bmindex, wmindex, bm0index;
	int64_t bmrange, wmrange, bm0range;
	int64_t men_index_base, index;

	if (permutation & reverse_men_order) {
		bb_bm = reverse_bitboard(p->white_man);
		bb_wm = reverse_bitboard(p->black_man);
		std::swap(nbm, nwm);
	}
	else {
		bb_bm = p->black_man;
		bb_wm = p->white_man;
	}

	bm0_mask = bb_bm & ROW0;
	nbm0 = bitcount(bm0_mask);

	/* Set the index for the black men that are not on rank0. */
	bmindex = bitboard2index(bb_bm ^ bm0_mask, ROWSIZE);

	/* Set the index for the black men that are on rank0. */
	bm0index = bitboard2index(bm0_mask, 0);

	/* Set the index for the white men,
	 * accounting for any interferences from the black men.
	 */
	wmindex = bitboard2index_rev(bb_wm, 0, bb_bm);
	bmrange = choose((NUMSQUARES - 2 * ROWSIZE), nbm - nbm0);
	wmrange = choose((NUMSQUARES - ROWSIZE) - nbm, nwm);
	bm0range = choose(ROWSIZE, nbm0);

	/* Indices in this subdb are assigned with index 0 having the highest
	 * number of black men on rank0, ...
	 */
	men_index_base = man_index_by_rank0_base[nbm][nwm][nbm0];
	index = bm0index + men_index_base +
				bmindex * bm0range +
				wmindex * bm0range * bmrange;
	return(index);
}


int64_t position_to_index_slice(EGDB_ROW_REVERSED_BITBOARD *p, int bm, int bk, int wm, int wk, int permutation)
{
	int64_t index64;
	Index_pair bkpair, wkpair, menpair;
	Index_pair *slowest, *middle, *fastest;

	/* Set the index for the black kings, accounting for any interferences
	 * from the black men and white men.
	 */
	bkpair.index = bitboard2index(p->black_king, 0, p->black_man | p->white_man);

	/* Set the index for the white kings, accounting for any interferences
	 * from the black men, white men, and black kings.
	 */
	wkpair.index = bitboard2index(p->white_king, 0, p->black_man | p->white_man | p->black_king);

	bkpair.range = choose(NUMSQUARES - bm - wm, bk);
	wkpair.range = choose(NUMSQUARES - bm - wm - bk, wk);
	if (permutation & men_index_by_bmindex)
		menpair.index = position_to_men_index(p, bm, wm, permutation);
	else
		menpair.index = position_to_men_index_by_rank0(p, bm, wm, permutation);

	permutation &= 7;
	if (permutation == 0) {
		slowest = &wkpair;
		middle = &bkpair;
		fastest = &menpair;
	}
	else if (permutation == 1) {
		slowest = &bkpair;
		middle = &wkpair;
		fastest = &menpair;
	}
	else if (permutation == 2) {
		slowest = &menpair;
		middle = &bkpair;
		fastest = &wkpair;
		slowest->range = men_range(bm, wm);
	}
	else if (permutation == 3) {
		slowest = &menpair;
		middle = &wkpair;
		fastest = &bkpair;
		slowest->range = men_range(bm, wm);
	}
	else if (permutation == 4) {
		slowest = &bkpair;
		middle = &menpair;
		fastest = &wkpair;
		middle->range = men_range(bm, wm);
	}
	else if (permutation == 5) {
		slowest = &wkpair;
		middle = &menpair;
		fastest = &bkpair;
		middle->range = men_range(bm, wm);
	}
	index64 = slowest->index +
				middle->index * slowest->range +
				fastest->index * middle->range * slowest->range;
	
	return(index64);
}


void indextoposition_men(int64_t menindex, EGDB_ROW_REVERSED_BITBOARD *pos, int nbm, int nwm, int permutation)
{
	int64_t bmrange;
	uint32_t bmindex, wmindex;

	/* Find bmindex. */
	bmrange = choose(MAXSQUARE - ROWSIZE, nbm);
	bmindex = man_index_monotonic::find_bmindex(nbm, nwm, (uint32_t)bmrange, menindex);
	wmindex = (uint32_t)(menindex - man_index_monotonic::base(nbm, nwm, bmindex));

	/* Place black men. */
	pos->black_man = index2bitboard((uint32_t)bmindex, MAXSQUARE - ROWSIZE, nbm, 0);

	/* Place white men. */
	pos->white_man = index2bitboard_rev((uint32_t)wmindex, MAXSQUARE - ROWSIZE, nwm, 0, pos->black_man);

	if (permutation & reverse_men_order) {
		BITBOARD blackman, whiteman;

		whiteman = reverse_bitboard(pos->black_man);
		blackman = reverse_bitboard(pos->white_man);
		pos->black_man = blackman;
		pos->white_man = whiteman;
	}
}


void indextoposition_men_by_rank0(int64_t index, EGDB_ROW_REVERSED_BITBOARD *pos, int nbm, int nwm, int permutation)
{
	int bm0;
	int64_t bmrange, bm0range, multiplier;
	int64_t bmindex, wmindex, bm0index;

	/* Find bm0. */
	for (bm0 = (std::min)(nbm, ROWSIZE); bm0 > 0; --bm0)
		if (man_index_by_rank0_base[nbm][nwm][bm0 - 1] > index)
			break;

	index -= man_index_by_rank0_base[nbm][nwm][bm0];

	/* Compute the range of indices for each piece type. */
	bmrange = choose(MAXSQUARE - 2 * ROWSIZE, nbm - bm0);
	bm0range = choose(ROWSIZE, bm0);

	/* Extract the wmindex, bmindex, and bm0index. */
	multiplier = bmrange * bm0range;
	wmindex = index / multiplier;
	index -= wmindex * multiplier;

	multiplier = bm0range;
	bmindex = index / multiplier;
	index -= bmindex * multiplier;
	bm0index = index;

	/* Place black men, except those on rank0. */
	pos->black_man = index2bitboard((uint32_t)bmindex, MAXSQUARE - 2 * ROWSIZE, nbm - bm0, ROWSIZE);

	/* Place black men on rank0. */
	pos->black_man |= index2bitboard((uint32_t)bm0index, ROWSIZE, bm0, 0);

	/* Place white men. */
	pos->white_man = index2bitboard_rev((uint32_t)wmindex, MAXSQUARE - ROWSIZE, nwm, 0, pos->black_man);

	if (permutation & reverse_men_order) {
		BITBOARD blackman, whiteman;

		whiteman = reverse_bitboard(pos->black_man);
		blackman = reverse_bitboard(pos->white_man);
		pos->black_man = blackman;
		pos->white_man = whiteman;
	}
}


void indextoposition_slice(int64_t index, EGDB_ROW_REVERSED_BITBOARD *pos, int bm, int bk, int wm, int wk, int permutation)
{
	Index_pair bkpair, wkpair, menpair;
	Index_pair *slowest, *middle, *fastest;
	int64_t multiplier;

	/* Initialize the board. */
	memset(pos, 0, sizeof(*pos));

	if (permutation & reverse_men_order)
		std::swap(bm, wm);

	bkpair.range = choose(MAXSQUARE - bm - wm, bk);
	wkpair.range = choose(MAXSQUARE - bm - wm - bk, wk);

	if ((permutation & 7) == 0) {
		slowest = &wkpair;
		middle = &bkpair;
		fastest = &menpair;
	}
	else if ((permutation & 7) == 1) {
		slowest = &bkpair;
		middle = &wkpair;
		fastest = &menpair;
	}
	else if ((permutation & 7) == 2) {
		slowest = &menpair;
		middle = &bkpair;
		fastest = &wkpair;
		slowest->range = men_range(bm, wm);
	}
	else if ((permutation & 7) == 3) {
		slowest = &menpair;
		middle = &wkpair;
		fastest = &bkpair;
		slowest->range = men_range(bm, wm);
	}
	else if ((permutation & 7) == 4) {
		slowest = &bkpair;
		middle = &menpair;
		fastest = &wkpair;
		middle->range = men_range(bm, wm);
	}
	else if ((permutation & 7) == 5) {
		slowest = &wkpair;
		middle = &menpair;
		fastest = &bkpair;
		middle->range = men_range(bm, wm);
	}
	multiplier = middle->range * slowest->range;
	fastest->index = index / multiplier;
	index -= fastest->index * multiplier;

	middle->index = index / slowest->range;
	index -= middle->index * slowest->range;

	slowest->index = index;

	if (permutation & men_index_by_bmindex)
		indextoposition_men(menpair.index, pos, bm, wm, permutation);
	else
		indextoposition_men_by_rank0(menpair.index, pos, bm, wm, permutation);

	/* Place black kings. */
	pos->black_king = index2bitboard((uint32_t)bkpair.index, MAXSQUARE, bk, 0, pos->black_man | pos->white_man);

	/* Place white kings. */
	pos->white_king = index2bitboard((uint32_t)wkpair.index, MAXSQUARE, wk, 0, pos->black_man | pos->white_man | pos->black_king);
}


/* return the number of database indices for this subdb. */
int64_t getdatabasesize_slice(int bm, int bk, int wm, int wk)
{
	int64_t size;

	size = men_range(bm, wm);

	/* Add the black kings. */
	size *= choose(NUMSQUARES - bm - wm, bk);

	/* Add the white kings. */
	size *= choose(NUMSQUARES - bm - wm - bk, wk);

	return(size);
}


int get_num_subslices(int bm, int bk, int wm, int wk, uint32_t subslice_size)
{
	int64_t size, last;
	int num_subslices;

	size = getdatabasesize_slice(bm, bk, wm, wk);
	if (size > subslice_size) {
		num_subslices = (int)(size / (int64_t)subslice_size);
		last = size - (int64_t)num_subslices * (int64_t)subslice_size;
		if (last)
			++num_subslices;
	}
	else
		num_subslices = 1;
	return(num_subslices);
}


/*
 * Returns a bitboard with one bit set, the bit that corresponds to 
 * nth unoccupied square, which is given by logical_square.
 * Logical_square numbers start at 0, and skip over occupied squares.
 * For example, if logical_square is 3, then free_square_bitboard returns a bitboard 
 * with a bit set at the 4th unoccupied square starting the bit that represents square 1.
 */
BITBOARD free_square_bitboard(int logical_square, BITBOARD occupied)
{
	int skipcount;
	BITBOARD bitboard, behind, empty_ahead;

	bitboard = (BITBOARD)1 << logical_square;
	behind = bitboard - 1;
	empty_ahead = ~(occupied | behind);
	skipcount = bitcount(behind & occupied);

	while (skipcount) {
		--skipcount;
		empty_ahead &= (empty_ahead - 1);
	}

	return((BITBOARD)1 << LSB(empty_ahead));
}


/*
 * Similar to free_square_bitboard(), but from white's perspective
 * For example, if logical_square is 3, then free_square_bitboard returns a bitboard 
 * with a bit set at the 4th unoccupied square starting from the bit that represents square 32.
 */
BITBOARD free_square_bitboard_rev(int logical_square, BITBOARD occupied)
{
	int skipcount;
	BITBOARD bitboard, ahead, behind, empty_ahead;

	bitboard = (BITBOARD)1 << square_mirror(logical_square);
	ahead = bitboard | (bitboard - 1);
	empty_ahead = ~occupied & ahead;
	behind = ~ahead;
	skipcount = bitcount(behind & occupied);

	while (skipcount) {
		--skipcount;
		empty_ahead ^= ((BITBOARD)1 << MSB(empty_ahead));
	}

	return((BITBOARD)1 << MSB(empty_ahead));
}


/*
 * Place num_pieces on a bitboard of num_squares unoccupied squares. 
 * The lowest square is square 1, unless a square offset is given.
 */
BITBOARD index2bitboard(uint32_t index, int num_squares, int num_pieces, int offset)
{
	int piece, logical_square;
	BITBOARD bitboard;

	logical_square = num_squares - 1;
	bitboard = 0;
	for (piece = num_pieces; piece > 0; --piece, --logical_square) {
		while (choose(logical_square, piece) > index)
			logical_square--;
		index -= choose(logical_square, piece);
		bitboard |= (1 << (logical_square + offset));
	}
	return(bitboard);
}


/*
 * Place num_pieces on a bitboard of num_squares unoccupied squares. 
 * The lowest square is square 1, unless a square offset is given.
 * Any set bits in occupied will be avoided.
 */
BITBOARD index2bitboard(uint32_t index, int num_squares, int num_pieces, int sq_offset, BITBOARD occupied)
{
    int piece, logical_square;
    BITBOARD bitboard;

    logical_square = num_squares - 1;
    bitboard = 0;
    for (piece = num_pieces; piece > 0; --piece, --logical_square) {
        while (choose(logical_square, piece) > index)
            logical_square--;
        index -= choose(logical_square, piece);
		if (cpu_has_pext())
			bitboard |= 1 << (logical_square + sq_offset);
		else
	        bitboard |= free_square_bitboard(logical_square + sq_offset, occupied);
    }
	if (cpu_has_pext())
		return(_pdep_u32(bitboard, ~occupied));
	else
	    return(bitboard);
}


/*
 * This is similar to index2bitboard() except from white's perspective. The lowest indexes correspond
 * to square placement starting at the top of the board (square 32).
 */
BITBOARD index2bitboard_rev(uint32_t index, int num_squares, int num_pieces, int sq_offset, BITBOARD occupied)
{
	int piece, logical_square;
	BITBOARD bitboard;

	logical_square = num_squares - 1;
	bitboard = 0;
	for (piece = num_pieces; piece > 0; --piece, --logical_square) {
		while (choose(logical_square, piece) > index)
			logical_square--;
		index -= choose(logical_square, piece);
		bitboard |= free_square_bitboard_rev(logical_square + sq_offset, occupied);
	}
	return(bitboard);
}


/*
 * Return the size with gaps (ignore interferences between men) of a major slice.
 */
int64_t getslicesize_gaps(int bm, int bk, int wm, int wk)
{
	int64_t size;

	/* Black men. */
	size = choose(NUMSQUARES - ROWSIZE, bm);

	/* White men. */
	size *= choose(NUMSQUARES - ROWSIZE, wm);

	/* Add the black kings. */
	size *= choose(NUMSQUARES - bm - wm, bk);

	/* Add the white kings. */
	size *= choose(NUMSQUARES - bm - wm - bk, wk);

	return(size);
}


namespace man_index_monotonic {

	/* man_index_base[nbm][nwm][bm index] gives the base checker index for a given
	 * configuration of nbm, nwm, and bmindex. This table uses 86mb of memory,
	 * so it's only initialized if an access function is called, or by init().
	 */
	int64_t *man_index_base[MAXPIECE_INDEXING + 1][MAXPIECE_INDEXING + 1];
	bool did_init;

	/* Build the table. Each array is filled before its pointer is stored. Return the bytes allocated. */
	int64_t init_man_index_base(void)
	{
		const bool print = false;
		int nbm, nwm, nbm_interfering;
		int64_t base, bmrange, wmrange, index, size;
		int64_t *table;
		BITBOARD bmmask;

		size = 0;
		for (nbm = 0; nbm <= MAXPIECE_INDEXING; ++nbm) {
			for (nwm = 0; nwm <= MAXPIECE_INDEXING; ++nwm) {
				if (nbm + nwm > MAXPIECES_INDEXING)
					continue;
				base = 0;
				bmrange = choose(NUMSQUARES - ROWSIZE, nbm);
				if (print)
					printf("\nsize tble[%d][%d] = %lld entries", nbm, nwm, bmrange);
				table = (int64_t *)malloc((size_t)bmrange * sizeof(int64_t));
				size += bmrange * sizeof(int64_t);
				for (index = 0; index < bmrange; ++index) {
					table[index] = base;

					/* Place the black men. */
					bmmask = index2bitboard((uint32_t)index, NUMSQUARES - ROWSIZE, nbm, 0);

					/* Find the number of black men on rows 1 through 6. These men will interfere
					 * with the placement of white men.
					 */
					nbm_interfering = nbm - bitcount(bmmask & ROW0);

					/* Find the number of white men configurations. */
					wmrange = choose(NUMSQUARES - ROWSIZE - nbm_interfering, nwm);
					base += wmrange;
				}
				man_index_base[nbm][nwm] = table;
			}
		}
		if (print)
			printf("\nman_index_base table size %lld", size);
		return(size);
	}

}

