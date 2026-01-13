#pragma once
#pragma warning(disable:4996)

#define SMP 1

#define MAXMSG 256

struct DBCRC {
	const char *filename;
	unsigned int crc;
};


typedef int (*REV_ROW_LOOKUP_FNPTR)(EGDB_DRIVER *, EGDB_BITBOARD *, int, int);

namespace cake_wld { EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char *directory, void (*msg_fn)(const char *)); }
namespace chinook_wld { EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type); }
namespace slice32_wld { EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type); }
namespace tun_v2_wld { EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type); }
namespace kr_mtc {EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type); }
namespace kr_dtw { EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type); }
namespace tun_v3_wld { EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type); }
