#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "project.h"
#include "egdb.h"
#include "egdb_internal.h"
#include "egdb_common.h"
#include "crc.h"


typedef struct {
	EGDB_TYPE egdb_type;
	const char *name;
	int pieces;
	unsigned int crc;
} EGDB_FIND_INFO;

EGDB_FIND_INFO egdb_find_table[] = {

	/* Chinook db8 */
	{EGDB_CHINOOK_WLD, "db8.00.idx", 8, 0x392b283f},
	{EGDB_CHINOOK_WLD, "db7.0.idx", 7, 0xb4d33402},
	{EGDB_CHINOOK_WLD, "db6.idx", 6, 0x8bcc7c29},
	{EGDB_CHINOOK_WLD, "db6.idx", 6, 0x31d857e7},

	/* Cake db8 set. */
	{EGDB_CAKE_WLD, "db8_0404.idx", 8, 0x597d7db4},
	{EGDB_CAKE_WLD, "db7.idx", 7, 0xa82bf62d},
	{EGDB_CAKE_WLD, "db6.idx", 6, 0x28a4a819},
	{EGDB_CAKE_WLD, "db5.idx", 5, 0x9fb18a70},
	{EGDB_CAKE_WLD, "db4.idx", 4, 0x1d4d4e66},

	/* Cake db6 built from cb menu. */
	{EGDB_CAKE_WLD, "db6.idx", 6, 0xFF8BC11B},
	{EGDB_CAKE_WLD, "db5.idx", 5, 0xB4C0E2E8},


	/* Slice32 Tunstall 12-piece. */
	{EGDB_KINGSROW32_WLD_TUN_12PC, "db12-1524.base2", 12, 0xb163eafb},
	{EGDB_KINGSROW32_WLD_TUN_12PC, "db11-5114.base2", 11, 0xaf5e697d},
	{EGDB_KINGSROW32_WLD_TUN_12PC, "db10-6040.base2", 10, 0x88ce576b},
	{EGDB_KINGSROW32_WLD_TUN_12PC, "db9-5040.base2", 9, 0xb1de71aa},
	{EGDB_KINGSROW32_WLD_TUN_12PC, "db8.base2", 8, 0x8d99c99f},

	/* Slice32 Tunstall. */
	{EGDB_KINGSROW32_WLD_TUN, "db10-5050.idx1", 10, 0xf00cbd12},
	{EGDB_KINGSROW32_WLD_TUN, "db9-5040.idx1", 9, 0xd47a058c},
	{EGDB_KINGSROW32_WLD_TUN, "db8-4040.idx1", 8, 0x367157d8},
	{EGDB_KINGSROW32_WLD_TUN, "db7.idx1", 7, 0x3c650a14},
	{EGDB_KINGSROW32_WLD_TUN, "db6.idx1", 6, 0xdfbd7a60},
	{EGDB_KINGSROW32_WLD_TUN, "db5.idx1", 5, 0x64f545e1},
	{EGDB_KINGSROW32_WLD_TUN, "db4.idx1", 4, 0x3ca29c17},
	{EGDB_KINGSROW32_WLD_TUN, "db3.idx1", 3, 0x821172ad},
	{EGDB_KINGSROW32_WLD_TUN, "db2.idx1", 2, 0x78a29015},

	/* Slice32. */
	{EGDB_KINGSROW32_WLD, "db10-5050.idx", 10, 0x52bbb51e},
	{EGDB_KINGSROW32_WLD, "db9-5040.idx", 9, 0x552f9e17},
	{EGDB_KINGSROW32_WLD, "db8-4040.idx", 8, 0xd500a692},
	{EGDB_KINGSROW32_WLD, "db7.idx", 7, 0xdaf15ed1},
	{EGDB_KINGSROW32_WLD, "db6.idx", 6, 0x7b1458c8},
	{EGDB_KINGSROW32_WLD, "db5.idx", 5, 0x5ec9db61},
	{EGDB_KINGSROW32_WLD, "db4.idx", 4, 0x11039de4},
	{EGDB_KINGSROW32_WLD, "db3.idx", 3, 0xcfff1221},
	{EGDB_KINGSROW32_WLD, "db2.idx", 2, 0x75d75851},

	/* MTC Slice32 - threshold 10. */
	{EGDB_KINGSROW32_MTC, "db10-0505.idx_mtc", 10, 0x2dc89460},
	{EGDB_KINGSROW32_MTC, "db9-0504.idx_mtc", 9, 0x27f5f046},
	{EGDB_KINGSROW32_MTC, "db8-2222.idx_mtc", 8, 0x56efc8e8},
	{EGDB_KINGSROW32_MTC, "db7.idx_mtc", 7, 0xd44ac494},
	{EGDB_KINGSROW32_MTC, "db6.idx_mtc", 6, 0x312df022},
	{EGDB_KINGSROW32_MTC, "db5.idx_mtc", 5, 0x9d1ade84},
	{EGDB_KINGSROW32_MTC, "db4.idx_mtc", 4, 0x06447ee0},
	{EGDB_KINGSROW32_MTC, "db3.idx_mtc", 3, 0x70382098},
	{EGDB_KINGSROW32_MTC, "db2.idx_mtc", 2, 0x0e707590},

	/* Chinook Italian WLD. */
	{EGDB_CHINOOK_ITALIAN_WLD, "idb6.idx", 6, 0x6c3deb60},

	/* Slice32 Italian Tunstall WLD. */
	{EGDB_KINGSROW32_ITALIAN_WLD_TUN, "db10-5050.idx1", 10, 0x8d0fc3f2},
	{EGDB_KINGSROW32_ITALIAN_WLD_TUN, "db9-5040.idx1", 9, 0x933b0f16},
	{EGDB_KINGSROW32_ITALIAN_WLD_TUN, "db8-4040.idx1", 8, 0x68ac1c67},
	{EGDB_KINGSROW32_ITALIAN_WLD_TUN, "db7.idx1", 7, 0x1c9d057b},
	{EGDB_KINGSROW32_ITALIAN_WLD_TUN, "db6.idx1", 6, 0x465aa342},
	{EGDB_KINGSROW32_ITALIAN_WLD_TUN, "db5.idx1", 5, 0xcb8c94b8},
	{EGDB_KINGSROW32_ITALIAN_WLD_TUN, "db4.idx1", 4, 0x1f5edf9c},
	{EGDB_KINGSROW32_ITALIAN_WLD_TUN, "db3.idx1", 3, 0x22f61933},
	{EGDB_KINGSROW32_ITALIAN_WLD_TUN, "db2.idx1", 2, 0xfc15d0f9},

	/* Slice32 Italian WLD. */
	{EGDB_KINGSROW32_ITALIAN_WLD, "db10-5050.idx", 10, 0x8d1534d7},
	{EGDB_KINGSROW32_ITALIAN_WLD, "db9-5040.idx", 9, 0x204e0f32},
	{EGDB_KINGSROW32_ITALIAN_WLD, "db8-4040.idx", 8, 0x322c0b3d},
	{EGDB_KINGSROW32_ITALIAN_WLD, "db7.idx", 7, 0xe0dfa806},
	{EGDB_KINGSROW32_ITALIAN_WLD, "db7.idx", 7, 0x920c92de},	/* Old, buggy move generator. */
	{EGDB_KINGSROW32_ITALIAN_WLD, "db6.idx", 6, 0xfe2092b8},
	{EGDB_KINGSROW32_ITALIAN_WLD, "db5.idx", 5, 0xa851984a},
	{EGDB_KINGSROW32_ITALIAN_WLD, "db4.idx", 4, 0xc881d3df},
	{EGDB_KINGSROW32_ITALIAN_WLD, "db3.idx", 3, 0xf273000a},
	{EGDB_KINGSROW32_ITALIAN_WLD, "db2.idx", 2, 0xcf78e282},

	/* Slice32 Italian MTC. */
	{EGDB_KINGSROW32_ITALIAN_MTC, "db10-0505.idx_mtc", 10, 0xceb74d0f},
	{EGDB_KINGSROW32_ITALIAN_MTC, "db9-0504.idx_mtc", 9, 0xcc8566b7},
	{EGDB_KINGSROW32_ITALIAN_MTC, "db8-0404.idx_mtc", 8, 0xdff47970},
	{EGDB_KINGSROW32_ITALIAN_MTC, "db7.idx_mtc", 7, 0xae444ac4},
	{EGDB_KINGSROW32_ITALIAN_MTC, "db6.idx_mtc", 6, 0xbd7dba3a},
	{EGDB_KINGSROW32_ITALIAN_MTC, "db5.idx_mtc", 5, 0x91485e19},
	{EGDB_KINGSROW32_ITALIAN_MTC, "db4.idx_mtc", 4, 0x83c375b6},
	{EGDB_KINGSROW32_ITALIAN_MTC, "db3.idx_mtc", 3, 0xc8cd5e2b},
	{EGDB_KINGSROW32_ITALIAN_MTC, "db2.idx_mtc", 2, 0x0e707590},

	{EGDB_KINGSROW_ITALIAN_DTW, "db8-4040.idx_dtw", 8, 0x8ce1f62b},
	{EGDB_KINGSROW_ITALIAN_DTW, "db7-4030.idx_dtw", 7, 0xd3f72de9},
	{EGDB_KINGSROW_ITALIAN_DTW, "db6-3030.idx_dtw", 6, 0xa99fbe12},

	{EGDB_KINGSROW_DTW, "db8-4040.idx_dtw", 8, 0xd61de72a},
	{EGDB_KINGSROW_DTW, "db7-4030.idx_dtw", 7, 0x7305ce04},
	{EGDB_KINGSROW_DTW, "db6-3030.idx_dtw", 6, 0x0bc91ddb},

};


EGDB_API int egdb_identify(const char *directory, EGDB_TYPE *egdb_type, int *max_pieces)
{
	int i, len, pieces;
	unsigned int crc;
	FILE *fp;
	const char *sep;
	EGDB_FIND_INFO *tablep;
	char name[MAXFILENAME];

	len = (int)strlen(directory);
	if (len && (directory[len - 1] != '\\'))
		sep = "\\";
	else
		sep = "";

	for (pieces = 12; pieces >= 2; --pieces) {
		for (i = 0; i < ARRAY_SIZE(egdb_find_table); ++i) {
			tablep = egdb_find_table + i;
			if (tablep->pieces != pieces)
				continue;
			sprintf(name, "%s%s%s", directory, sep, tablep->name);
			fp = fopen(name, "rb");
			if (fp) {
				crc = file_crc_calc(fp);
				fclose(fp);
				if (crc == egdb_find_table[i].crc) {
					*egdb_type = tablep->egdb_type;
					*max_pieces = tablep->pieces;
					return(0);
				}
			}
		}
	}

	/* Nothing found. */
	return(1);
}


