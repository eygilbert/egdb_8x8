#include "board.h"
#include "reverse.h"


static uint8_t reverse_byte_tbl[256];


void init_reverse()
{
	int i, j;

	/* reverse_byte_tbl contains the "reverse" of each byte 0..255. For
	 * example, entry 17 (binary 00010001) is a (10001000) - reverse
	 * the bits.
	 */
	for (i = 0; i < 256; i++) {
		reverse_byte_tbl[i] = 0;
		for (j = 0; j < 8; j++)
			if (i & (1L << j))
				reverse_byte_tbl[i] |= (1L << (7 - j));
	}
}


uint8_t reverse_byte(uint8_t byte)
{
	return(reverse_byte_tbl[byte]);

}


/*
 * Reverse a bitboard.
 */
BITBOARD reverse_bitboard(BITBOARD bb)
{
	union {
		unsigned char bytes[4];
		BITBOARD word;
	} conv_out;

	conv_out.bytes[0] = (unsigned char)reverse_byte_tbl[((unsigned char *)(&bb))[3]];
	conv_out.bytes[1] = (unsigned char)reverse_byte_tbl[((unsigned char *)(&bb))[2]];
	conv_out.bytes[2] = (unsigned char)reverse_byte_tbl[((unsigned char *)(&bb))[1]];
	conv_out.bytes[3] = (unsigned char)reverse_byte_tbl[((unsigned char *)(&bb))[0]];
	return(conv_out.word);
}


