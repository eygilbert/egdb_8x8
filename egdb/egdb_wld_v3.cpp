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
#include "wld_v3_crcs.h"

#define USE_HT
#define BINARY_IDX_FILES

#ifdef BINARY_IDX_FILES
#include "fileutils.h"
#include "..\convert_idx\convert_idx.h"
#endif

#define MIN_AUTOLOAD_PIECES 6

namespace tun_v3_wld {

#include "egdb_tunstall.h"

EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces,
				int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type)
{
	int status;
	EGDB_DRIVER *handle;
	SLICE32_DATA *hdat;

	if (db_type != EGDB_KINGSROW32_WLD_TUN_BINIDX) {
		(*msg_fn)("Invalid db type in egdb_open().\n");
		return(0);
	}
	handle = (EGDB_DRIVER *)calloc(1, sizeof(EGDB_DRIVER));
	if (!handle) {
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
#pragma warning(suppress : 4316)		// Can't do various aligns with "new".
	handle->internal_data = new (std::nothrow) SLICE32_DATA;
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
	hdat->base_extension = "base2";
#ifdef BINARY_IDX_FILES
	hdat->idx_extension = "bidx2";
#else
	hdat->idx_extension = "idx2";
#endif
	hdat->wld_data_extension = "cpr2";
	hdat->maxpieces = pieces;
	hdat->maxpiece = 6;
	hdat->same_pieces_one_file = 8;
	hdat->log_msg_fn = msg_fn;

	try {
		status = initdblookup(hdat, pieces, cache_mb, directory, msg_fn);
	}
	catch (const std::exception &e) {
		char msg[MAXMSG];

		/* Out of memory, or another C++ exception. Don't let it escape the C interface. */
		sprintf(msg, "egdb_open failed: %.100s\n", e.what());
		(*msg_fn)(msg);
		status = 1;
	}
	if (status) {
		egdb_close_slice32(handle);
		return(0);
	}
	if (bitboard_type == EGDB_NORMAL)
		/* This is a kingsrow bitboard. */
		handle->lookup = normal_lookup;
	else
		handle->lookup = (REV_ROW_LOOKUP_FNPTR)dblookup;
	
	return(handle);
}

}	// namespace
