#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "egdb.h"
#include "egdb_internal.h"
#include "bitboard_convert.h"


unsigned short egdb_reverse_nibble_table[0x10000];
unsigned short egdb_reverse_int_table[0x10000];

bool init_reverse_bits_in_row();
bool init_reverse_int();

static bool did_init1 = init_reverse_bits_in_row();
static bool did_init2 = init_reverse_int();

/*
 * Translate a KingsRow board representation into a database board representation.
 *
 *	 Database Board:	     KingsRow Board:
 *	       WHITE		          WHITE
 *	   28  29  30  31	       31  30  29  28
 *	 24  25  26  27  	     27  26  25  24  
 *	   20  21  22  23	       23  22  21  20
 *	 16  17  18  19  	     19  18  17  16
 *	   12  13  14  15	       15  14  13  12
 *	  8   9  10  11  	     11  10  09  08
 *	    4   5   6   7	       07  06  05  04
 *	  0   1   2   3  	     03  02  01  00
 *	       BLACK                   BLACK
 *
 */
bool init_reverse_bits_in_row()
{
	int i;
	static char xlat[] = {
		0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15
	};

	for (i = 0; i < 0x10000; ++i)
		egdb_reverse_nibble_table[i] = xlat[i & 0xf] |
								(xlat[(i >> 4) & 0xf] << 4) |
								(xlat[(i >> 8) & 0xf] << 8) |
								(xlat[(i >> 12) & 0xf] << 12);
	return(true);
}


/*
 * Initialize reverse_int_table, the reverse of a word.
 */
bool init_reverse_int()
{
	int i, j;

	for (i = 0; i < 0x10000; ++i) {
		egdb_reverse_int_table[i] = 0;
		for (j = 0; j < 16; ++j) {
			if (i & (1 << j))
				egdb_reverse_int_table[i] += 1 << (15 - j);
		}
	}
	return(true);
}


