#pragma once
#include "board.h"

void init_reverse();
BITBOARD reverse_bitboard(BITBOARD bb);
uint8_t reverse_byte(uint8_t byte);


inline BOARD reverse(const BOARD &board)
{
	BOARD revboard;

	revboard.black = reverse_bitboard(board.white);
	revboard.white = reverse_bitboard(board.black);
	revboard.king = reverse_bitboard(board.king);
	return(revboard);
}


inline void reverse(BOARD *dest, const BOARD *src)
{
	dest->black = reverse_bitboard(src->white);
	dest->white = reverse_bitboard(src->black);
	dest->king = reverse_bitboard(src->king);
}


