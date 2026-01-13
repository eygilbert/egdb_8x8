#include <utility>
#include <algorithm>
#include "project.h"
#include "board.h"
#include "egdb_common.h"
#include "egdb_internal.h"
#include "rle_tables.h"
#include "bitboard_convert.h"
#include "format_with_commas.h"
#include "crc.h"
#include "indexing.h"
#include "SpinLock.h"
#include "slice.h"
#include "timer.h"
#include "tun_v2_decompress.h"
#include "cache_ref_hashtable.h"
#include "wld_v2_crcs.h"

//#define USE_HT
//#define BINARY_IDX_FILES

#ifdef BINARY_IDX_FILES
#include "fileutils.h"
#include "..\convert2db12\convert2db12.h"
#endif


#define MIN_AUTOLOAD_PIECES 5

namespace tun_v2_wld {
	#include "egdb_tunstall.h"

EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces,
				int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type)
{
	int status;
	EGDB_DRIVER *handle;
	SLICE32_DATA *hdat;

	handle = (EGDB_DRIVER *)calloc(1, sizeof(EGDB_DRIVER));
	if (!handle) {
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
#pragma warning(suppress : 4316)		// Can't do various aligns with "new".
	handle->internal_data = new SLICE32_DATA;
	if (!handle->internal_data) {
		free(handle);
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
	handle->get_stats = get_db_stats;
	handle->reset_stats = reset_db_stats;
	handle->verify = verify_crc;
	handle->close = egdb_close_slice32;
	hdat = (SLICE32_DATA *)handle->internal_data;
	hdat->db_type = db_type;
	hdat->idx_extension = "idx1";
	hdat->wld_data_extension = "cpr1";
	hdat->maxpieces = pieces;
	hdat->maxpiece = 5;
	hdat->same_pieces_one_file = 7;
	hdat->log_msg_fn = msg_fn;

	status = initdblookup(hdat, pieces, cache_mb, directory, msg_fn);
	if (status) {
		egdb_close_slice32(handle);
		return(0);
	}
	if (bitboard_type == EGDB_NORMAL) {

		if (db_type == EGDB_KINGSROW32_WLD_TUN) {
			/* This is a kingsrow bitboard. */
			handle->lookup = normal_lookup;
		}
		else
			handle->lookup = normal_lookup_italian;
	}
	else {
		if (db_type == EGDB_KINGSROW32_WLD_TUN)
			handle->lookup = (REV_ROW_LOOKUP_FNPTR)dblookup;
		else {
			handle->lookup = reversed_lookup_italian;
		}
	}
	
	return(handle);
}

} // namespace
