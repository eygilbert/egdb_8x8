#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "egdb.h"
#include "egdb_internal.h"
#include "egdb_common.h"
#include "bitboard_convert.h"



EGDB_API EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type,
								int pieces, 
								int cache_mb,
								const char *directory,
								void (*msg_fn)(const char *))
{
	int stat;
	int max_pieces;
	EGDB_TYPE db_type;
	char msg[MAXMSG];
	EGDB_DRIVER *handle = 0;

	/* This is a C interface, so no C++ exception may escape from it.
	 * The drivers catch exceptions from their own init and clean up. This is a last
	 * resort for anything else; it may leak a partly opened driver, but returns failure.
	 */
	try {
		/* The drivers build filenames from directory in fixed size buffers. */
		if (strlen(directory) > MAX_DB_DIRECTORY_LEN) {
			sprintf(msg, "egdb directory path is too long, limit is %d characters\n", MAX_DB_DIRECTORY_LEN);
			(*msg_fn)(msg);
			return(0);
		}
		stat = egdb_identify(directory, &db_type, &max_pieces);
		if (stat) {
			sprintf(msg, "No egdb found\n");
			(*msg_fn)(msg);
			return(0);
		}
		if (max_pieces < pieces) {
			sprintf(msg, "egdb has only %d pieces\n", max_pieces);
			(*msg_fn)(msg);
			return(0);
		}
		switch (db_type) {

		case EGDB_KINGSROW32_WLD:
		case EGDB_KINGSROW32_ITALIAN_WLD:
			handle = slice32_wld::egdb_open(bitboard_type, pieces, cache_mb, directory, msg_fn, db_type);
			break;

		case EGDB_KINGSROW32_WLD_TUN:
		case EGDB_KINGSROW32_ITALIAN_WLD_TUN:
			handle = tun_v2_wld::egdb_open(bitboard_type, pieces, cache_mb, directory, msg_fn, db_type);
			break;

		case EGDB_KINGSROW32_WLD_TUN_BINIDX:
			handle = tun_v3_wld::egdb_open(bitboard_type, pieces, cache_mb, directory, msg_fn, db_type);
			break;

		case EGDB_KINGSROW32_MTC:
		case EGDB_KINGSROW_MTC_BINIDX:
		case EGDB_KINGSROW32_ITALIAN_MTC:
			handle = kr_mtc::egdb_open(bitboard_type, pieces, cache_mb, directory, msg_fn, db_type);
			break;

		case EGDB_CAKE_WLD:
			handle = cake_wld::egdb_open(bitboard_type, pieces, cache_mb, directory, msg_fn);
			break;

		case EGDB_CHINOOK_WLD:
		case EGDB_CHINOOK_ITALIAN_WLD:
			handle = chinook_wld::egdb_open(bitboard_type, pieces, cache_mb, directory, msg_fn, db_type);
			break;

		case EGDB_KINGSROW_DTW:
		case EGDB_KINGSROW_ITALIAN_DTW:
			handle = kr_dtw::egdb_open(bitboard_type, pieces, cache_mb, directory, msg_fn, db_type);
			break;
		}
	}
	catch (...) {
		(*msg_fn)("egdb_open failed: unexpected exception\n");
		return(0);
	}

	return(handle);
}


