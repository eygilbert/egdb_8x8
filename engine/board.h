#pragma once
#include <stdint.h>
#include "project.h"
#include "bool.h"

#define BLACK 0
#define WHITE 1
#define OTHER_COLOR(color) ((color) ^ 1)

#define MAXMOVES 64		/* more than any position with at most 12 pieces per side (8 open kings already have 32 moves) */
#define UNDEFINED_MOVE 31	/* "no move" in 5-bit move fields (opening book); not tied to MAXMOVES */
#define MAXPIECES_JUMPED 12
#define NUMSQUARES 32
#define MAXSQUARE NUMSQUARES
#define ROWSIZE 4
#define NUMROWS 8
#define NUM_BITBOARD_BITS 32

/*                English    
 *   Bit positions       Square numbers
 *
 *       white                white     
 *
 *    31  30  29  28      32  31  30  29
 *  27  26  25  24      28  27  26  25  
 *    23  22  21  20      24  23  22  21
 *  19  18  17  16      20  19  18  17  
 *    15  14  13  12      16  15  14  13
 *  11  10  09  08      12  11  10  09  
 *    07  06  05  04      08  07  06  05
 *  03  02  01  00      04  03  02  01  
 *
 *       black                black
 *
 *                Italian
 *   Bit positions       Square numbers
 *
 *       white                white     
 *
 *  28  29  30  31      32  31  30  29
 *    24  25  26  27      28  27  26  25  
 *  20  21  22  23      24  23  22  21
 *    16  17  18  19      20  19  18  17  
 *  12  13  14  15      16  15  14  13
 *    08  09  10  11      12  11  10  09  
 *  04  05  06  07      08  07  06  05
 *    00  01  02  03      04  03  02  01  
 *
 *       black                black
 */

#define S1 1
#define S2 2
#define S3 4
#define S4 8
#define S5 0x10
#define S6 0x20
#define S7 0x40
#define S8 0x80
#define S9 0x100
#define S10 0x200
#define S11 0x400
#define S12 0x800
#define S13 0x1000
#define S14 0x2000
#define S15 0x4000
#define S16 0x8000
#define S17 0x10000
#define S18 0x20000
#define S19 0x40000
#define S20 0x80000
#define S21 0x100000 
#define S22 0x200000
#define S23 0x400000
#define S24 0x800000
#define S25 0x1000000
#define S26 0x2000000
#define S27 0x4000000
#define S28 0x8000000
#define S29 0x10000000
#define S30 0x20000000
#define S31 0x40000000
#define S32 0x80000000

const int s1_ital = 8;
const int s2_ital = 4;
const int s3_ital = 2;
const int s4_ital = 1;
const int s5_ital = 0x80;
const int s6_ital = 0x40;
const int s7_ital = 0x20;
const int s8_ital = 0x10;
const int s9_ital = 0x800;
const int s10_ital = 0x400;
const int s11_ital = 0x200;
const int s12_ital = 0x100;
const int s13_ital = 0x8000;
const int s14_ital = 0x4000;
const int s15_ital = 0x2000;
const int s16_ital = 0x1000;
const int s17_ital = 0x80000;
const int s18_ital = 0x40000;
const int s19_ital = 0x20000;
const int s20_ital = 0x10000;
const int s21_ital = 0x800000;
const int s22_ital = 0x400000;
const int s23_ital = 0x200000;
const int s24_ital = 0x100000;
const int s25_ital = 0x8000000;
const int s26_ital = 0x4000000;
const int s27_ital = 0x2000000;
const int s28_ital = 0x1000000;
const int s29_ital = 0x80000000;
const int s30_ital = 0x40000000;
const int s31_ital = 0x20000000;
const int s32_ital = 0x10000000;


#define S1_bit 0
#define S2_bit 1
#define S3_bit 2
#define S4_bit 3
#define S5_bit 4
#define S6_bit 5
#define S7_bit 6
#define S8_bit 7
#define S9_bit 8
#define S10_bit 9
#define S11_bit 10
#define S12_bit 11
#define S13_bit 12
#define S14_bit 13
#define S15_bit 14
#define S16_bit 15
#define S17_bit 16
#define S18_bit 17
#define S19_bit 18
#define S20_bit 19
#define S21_bit 20
#define S22_bit 21
#define S23_bit 22
#define S24_bit 23
#define S25_bit 24
#define S26_bit 25
#define S27_bit 26
#define S28_bit 27
#define S29_bit 28
#define S30_bit 29
#define S31_bit 30
#define S32_bit 31

#define COLUMN0 (S5 | S13 | S21 | S29)
#define COLUMN1 (S1 | S9 | S17 | S25)
#define COLUMN2 (S6 | S14 | S22 | S30)
#define COLUMN3 (S2 | S10 | S18 | S26)
#define COLUMN4 (S7 | S15 | S23 | S31)
#define COLUMN5 (S3 | S11 | S19 | S27)
#define COLUMN6 (S8 | S16 | S24 | S32)
#define COLUMN7 (S4 | S12 | S20 | S28)

#define ROW0 (S1 | S2 | S3 | S4)
#define ROW1 (S5 | S6 | S7 | S8)
#define ROW2 (S9 | S10 | S11 | S12)
#define ROW3 (S13 | S14 | S15 | S16)
#define ROW4 (S17 | S18 | S19 | S20)
#define ROW5 (S21 | S22 | S23 | S24)
#define ROW6 (S25 | S26 | S27 | S28)
#define ROW7 (S29 | S30 | S31 | S32)

#define BLACK_KING_RANK_MASK ROW7
#define WHITE_KING_RANK_MASK ROW0


typedef uint32_t BITBOARD;

typedef union {
	struct {
		BITBOARD pieces[2];		/* Indexed by BLACK or WHITE. */
		BITBOARD king;
	};
	struct {
		BITBOARD black;
		BITBOARD white;
		BITBOARD king;
	};
} BOARD;


inline bool is_legal(BOARD *board)
{
	if ((board->black & ~board->king) & ROW7)
		return(false);
	if ((board->white & ~board->king) & ROW0)
		return(false);
	if (board->black & board->white)
		return(false);
	return(true);
}


inline void get_start_pos(BOARD *board, int *color)
{
#ifdef ITALIAN_RULES
	*color = WHITE;
#else
	*color = BLACK;
#endif
	board->black = ROW0 | ROW1 | ROW2;
	board->white = ROW5 | ROW6 | ROW7;
	board->king = 0;
}


inline int english_to_ital_bitnum(int bitnum)
{
	return(4 * ((bitnum) / 4) + 3 - ((bitnum) & 3));
}


inline int ital_to_english_bitnum(int bitnum)
{
	return(english_to_ital_bitnum(bitnum));
}


/* Convert a 0-based English square number to its Italian equivalent. */
inline int english_to_ital_sq0(int sq0)
{
	return(english_to_ital_bitnum(sq0));
}


inline int bitboard_to_square0(BITBOARD bitmask)
{
	int bitnum;

	bitnum = LSB(bitmask);
#ifdef ITALIAN_RULES
	return(english_to_ital_bitnum(bitnum));
#else
	return(bitnum);
#endif
}


/*
 * Return a board square number (1 - 32) of a board bitboard containing a
 * single bit set.
 */
inline int square_num(BITBOARD mask)
{
	return(bitboard_to_square0(mask) + 1);
}


inline BITBOARD square0_to_bitboard(int sq0)
{
#ifdef ITALIAN_RULES
	return(1 << english_to_ital_sq0(sq0));
#else
	return(1 << (sq0));
#endif
}


inline int bitnum_to_square0(int bitnum)
{
#ifdef ITALIAN_RULES
	return(english_to_ital_bitnum(bitnum));
#else
	return(bitnum);
#endif
}


/* 
 * Returns the rank (row) of the highest set bit in bb.
 * Row numbers start at 0 (squares 1 through 4).
 */
inline int maxrank(BITBOARD bb)
{
	return(MSB(bb) / ROWSIZE);
}


inline int minrank(BITBOARD bb)
{
	return(LSB(bb) / ROWSIZE);
}


/*
 * Returns the mirror of the rank number.
 */
inline int rank_mirror(int rank)
{
	return((NUMROWS - 1) - rank);
}


inline int square_mirror(int sq0)
{
	return((NUMSQUARES - 1) - sq0);
}


/*
 * Return a bitboard with all bits set from rank0 through maxrank.
 */
inline BITBOARD inclusive_ranks_bb(int maxrank)
{
	int64_t bb = (int64_t)1 << (ROWSIZE * (maxrank + 1));
	bb -= 1;
	return((BITBOARD)bb);
}


inline BITBOARD rank_bb(int rank)
{
	return(ROW0 << (rank * ROWSIZE));
}



