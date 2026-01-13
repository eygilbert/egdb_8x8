#pragma once
#include "egdb.h"

/* Functions to convert kr bitboards to endgame db bitboards. */

extern unsigned short egdb_reverse_nibble_table[0x10000];
extern unsigned short egdb_reverse_int_table[0x10000];


/*
 * In each bitboard row swap the first bit with the 4th, and 2nd bit with the 3rd.
 * This converts between a kingsrow bitboard type and a Cake/Chinook type.
 */
inline unsigned int reverse_bits_in_row(unsigned int n)
{
	unsigned int result;

	result = egdb_reverse_nibble_table[n & 0xffff];
	result |= (egdb_reverse_nibble_table[n >> 16] << 16);
	return(result);
}


inline void reverse_bits_in_row(EGDB_NORMAL_BITBOARD &dest, EGDB_NORMAL_BITBOARD &src)
{
	dest.black = reverse_bits_in_row(src.black);
	dest.white = reverse_bits_in_row(src.white);
	dest.king = reverse_bits_in_row(src.king);
}


inline void reverse_bits_in_row(EGDB_ROW_REVERSED_BITBOARD &dest, EGDB_ROW_REVERSED_BITBOARD &src)
{
	dest.black_man = reverse_bits_in_row(src.black_man);
	dest.black_king = reverse_bits_in_row(src.black_king);
	dest.white_man = reverse_bits_in_row(src.white_man);
	dest.white_king = reverse_bits_in_row(src.white_king);
}


inline void reverse_bits_in_row(EGDB_ROW_REVERSED_BITBOARD &dest, EGDB_NORMAL_BITBOARD &src)
{
	unsigned int black, white, king;

	black = reverse_bits_in_row(src.black);
	white = reverse_bits_in_row(src.white);
	king = reverse_bits_in_row(src.king);
	dest.black_man = black & ~king;
	dest.black_king = black & king;
	dest.white_man = white & ~king;
	dest.white_king = white & king;
}


/*
 * Reverses the bits in an integer.
 * Swap bit0 with bit31, bit1 with bit30, bit2 with bit29, ...
 * This is used to create the color-reversed image of the board.
 */
inline unsigned int reverse_int(unsigned int n)
{
	return(egdb_reverse_int_table[n >> 16] +
				(egdb_reverse_int_table[n & 0xffff] << 16));
}


inline void reverse(EGDB_ROW_REVERSED_BITBOARD *dest, EGDB_ROW_REVERSED_BITBOARD *src)
{
	dest->black_man = reverse_int(src->white_man);
	dest->white_man = reverse_int(src->black_man);
	dest->black_king = reverse_int(src->white_king);
	dest->white_king = reverse_int(src->black_king);
}

