#define NOMINMAX	/* exlude min()/max() macros. */
#include <algorithm>
#include "egdb.h"
#include "egdb_internal.h"
#include "egdb_chinook.h"
#include "egdb_common.h"
#include "bitcount.h"
#include "rle_tables.h"
#include "bicoef.h"
#include "bitboard_convert.h"
#include "format_with_commas.h"
#include "crc.h"
#include "indexing.h"
#include "SpinLock.h"


#define USE_RUNLENGTH16 0

#if USE_RUNLENGTH16
	#define RUNLENGTH16_TABLE runlength16_chinook
#endif

#define OMIT_LOPSIZED_SLICES 0

#define FAST_PROBE3_READ 1

#define MAX_PIECES 8
#define MAX_RANK 7

/* bk X wk X bp X wp */
#define DBTABLESIZE (MAX_PIECES * MAX_PIECES * MAX_PIECES * MAX_PIECES)

/* Given the number w/b kings and checkers (pawns), DBINDEX gives us
 * the index into DBTable.	Speeds up finding things.
 */
#define DBINDEX(bk, wk, bp, wp) \
	((bk * (MAX_PIECES * MAX_PIECES * MAX_PIECES)) + \
	(wk * (MAX_PIECES * MAX_PIECES)) + (bp * MAX_PIECES) + wp)

#define CI_TABLESIZE (MAX_PIECES * MAX_RANK * MAX_PIECES * MAX_RANK)
#define CI_INDEX(bm, bmr, wm, wmr) \
	((bm * (MAX_RANK * MAX_PIECES * MAX_RANK)) + \
	(bmr * (MAX_PIECES * MAX_RANK)) + \
	(wm * MAX_RANK) + wmr)

/* The size of a block as defined by the db8 index files.
 * Don't try to change this unless you re-index the database.
 */
#define IDX_BLOCK_SIZE 1024

/* Cache blocks are read N at a time for efficiency. */
#define CACHE_BLOCKS_PER_PROBE3 1
#define PROBE3_BLOCK_SIZE (CACHE_BLOCK_SIZE * CACHE_BLOCKS_PER_PROBE3)

/* The size of cache blocks that are read on demand. */
#define IDX_BLOCKS_PER_CACHE_BLOCK 4
#define CACHE_BLOCK_SIZE (IDX_BLOCK_SIZE * IDX_BLOCKS_PER_CACHE_BLOCK)

/* Allocate cache block buffers CACHEBLOCK_ALLOC_COUNT at a time. */
#define CACHEBLOCK_ALLOC_COUNT 256

/* Convert an index block number into a cache block number. */
#define IDX_2_CACHE_BLOCK(b) ((b) / IDX_BLOCKS_PER_CACHE_BLOCK)

/* Convert a cache block number into a PROBE3 block number. */
#define CACHE_2_PROBE3_BLOCK(b) ((b) / CACHE_BLOCKS_PER_PROBE3)

/* Return a pointer of type (char *) to the base of a cache block. */
#define CACHE_DATAP(block) (hdat->ccbs[block].data)

/* If he didn't give us enough memory for at least MIN_CACHE_BUF_BYTES of
 * buffers, then use that much.
 */
#define MIN_CACHE_BUF_BYTES (10 * ONE_MB)

namespace chinook_wld {

/* Information about each database file. */
struct DB_FILE {
	const char *filename;	/* the prefix part of this db's filename. */
	int pieces; 			/* number of checkers pieces in this db. */
	int lopsided;			/* if true then this is an optional db, file might be missing. */
	unsigned char *autoload;/* if not NULL then points to the whole db in ram. */ 
	uint32_t *indices;		/* An array of database index numbers, indexed by idx block number. */
	int *cache_bufferi;		/* An array of indices into cache_buffers[], indexed by block number. */
	int bytes;				/* size in bytes. */
	int num_idx_blocks;		/* size in index blocks. */
	int num_cache_blocks;	/* size in cache blocks. */
	HANDLE fp; 				/* handle to the open database file. */
};

struct INDEX_DATA {
	int rangeBK;			/* number of Black king configurations */
	int rangeWK;			/* number of White king configurations */
	int first_bmindex;
	int *checker_index_base;
};

/* Entry for each sub-database. */
struct DBENTRY {
	DB_FILE *file;					/* file info for this db. */
	unsigned char dominant_val;		/* Dominant value in the db, TIE, WIN, or LOSS. */
	unsigned char all_same_val;		/* If TIE, WIN, or LOSS then all are this value, else UNKNOWN. */
	unsigned short startbyte;		/* Byte where subdatabase starts. */
	int start_idx_block;			/* Index block number of the start byte */
	int end_idx_block;				/* Index block number of the last byte */
	INDEX_DATA *index_data;			/* Pointer to index info */
};

/* cache control block type. */
struct CCB {
	int next;					/* index of next node */
	int prev;					/* index of previous node */
	int blocknum;					/* the block number within sub_database expressed in block units */
	DB_FILE *dbfile;				/* which database file the block is for. */
	unsigned char *data;			/* data of this block. */
};

struct CHINOOK_DATA {
	EGDB_TYPE db_type;				/* to allow for Italian type. */
	char db_filepath[MAXFILENAME];	/* Path to database files. */
	int maxpieces;					/* the max number of pieces to lookup. */
	int autoload_pieces;			/* permanently load slices with <= this many pieces. */
	void (*log_msg_fn)(const char *msg);	/* pointer to function to call for logging status and error msgs. */
	unsigned short *runlength16_chinook;
	DB_FILE *dbfiles;
	int numdbfiles;
#if SMP
	SPINLOCK lock;
#endif

	/* DBTable is used to speedup looking up an entry.	There is one
	 * entry for each possible database.  Note that some of the
	 * posible databases do not really exist (e.g. 6 against 0)
	 */
	DBENTRY *DBTable[DBTABLESIZE];
#if CACHE_BLOCKS_PER_PROBE3 > 1
	unsigned char probe3_buffer[PROBE3_BLOCK_SIZE];
#endif

	/* Used for storing parts of the database in memory.  ccbs_top is an index to the
	 * head of a linked list of 1024-byte blocks of the database.  The list
	 * is organized so that the oldest parts of the database are the first
	 * removed to make room for new parts (LRU replacement strategy).
	 */
	int ccbs_top;				/* index into cache_buffers[] of least recently used block. */
	int num_cache_buffers;		/* number of cache buffers to allocate. */
	CCB *ccbs;					/* array of cache control blocks. */
	EGDB_STATS lookup_stats;

	/* An array of pointers to individual checker_index_base tables.
	 * It is indexed by [bm][bmrank][wm][wmrank].
	 */
	int **ci_base_table;
};


/* Information about the db fileset. */
static const DB_FILE dbfile_info[] = {
	{"db6", 6, 0},
	{"db7.0", 7, 0},
	{"db7.1", 7, 0},
	{"db7.2", 7, 0},
	{"db7.3", 7, 0},
	{"db7.4", 7, 0},
	{"db7.5", 7, 1},
	{"db7.6", 7, 1},
	{"db8.00", 8, 0},
	{"db8.10", 8, 0},
	{"db8.11", 8, 0},
	{"db8.20", 8, 0},
	{"db8.21", 8, 0},
	{"db8.22", 8, 0},
	{"db8.30", 8, 0},
	{"db8.31", 8, 0},
	{"db8.32", 8, 0},
	{"db8.33", 8, 0},
	{"db8.40", 8, 0},
	{"db8.41", 8, 0},
	{"db8.42", 8, 0},
	{"db8.43", 8, 0},
	{"db8.44", 8, 0},
	{"db8.53", 8, 1},
	{"db8.62", 8, 1},
	{"db8.71", 8, 1},
};

/* A table of crc values for each database file. */
static const DBCRC dbcrc[] = {
	{"db6", 0xf9f97079},
	{"db6.idx", 0x8bcc7c29},
	{"db7.0", 0x866d00e0},
	{"db7.0.idx", 0xb4d33402},
	{"db7.1", 0x9a67e872},
	{"db7.1.idx", 0x403c46dc},
	{"db7.2",    0x532f0},
	{"db7.2.idx", 0xaa01a8a8},
	{"db7.3", 0xbfad6d06},
	{"db7.3.idx", 0xaa27aa19},
	{"db7.4", 0x4315d21b},
	{"db7.4.idx", 0xb2a05bae},
	{"db7.5", 0x99f649bb},
	{"db7.5.idx", 0xe751ed3b},
	{"db7.6", 0x8f93f098},
	{"db7.6.idx", 0x73523e11},
	{"db8.00", 0xbe5128fa},
	{"db8.00.idx", 0x392b283f},
	{"db8.10", 0x1d5ee251},
	{"db8.10.idx", 0xac75a74b},
	{"db8.11", 0x55fee46a},
	{"db8.11.idx", 0xbc5c78aa},
	{"db8.20", 0xeffaf2af},
	{"db8.20.idx", 0xd74b29fd},
	{"db8.21", 0x273f465e},
	{"db8.21.idx", 0x6ac9896f},
	{"db8.22", 0xd28046e1},
	{"db8.22.idx", 0xc64b05c7},
	{"db8.30", 0x805d17cb},
	{"db8.30.idx", 0x256185f0},
	{"db8.31", 0xc2e0197e},
	{"db8.31.idx", 0xd776793a},
	{"db8.32", 0xb50b33eb},
	{"db8.32.idx", 0x1f6b84f9},
	{"db8.33", 0x74ac926e},
	{"db8.33.idx", 0x7fb69dd7},
	{"db8.40", 0xafbd8275},
	{"db8.40.idx", 0xac5fcb0d},
	{"db8.41", 0xf1ede586},
	{"db8.41.idx", 0x30575f6a},
	{"db8.42", 0x148fc220},
	{"db8.42.idx", 0xcdd9f212},
	{"db8.43", 0xc45b8382},
	{"db8.43.idx", 0xe5349645},
	{"db8.44", 0x65984beb},
	{"db8.44.idx", 0x60fb2607},
	{"db8.53", 0x429691a7},
	{"db8.53.idx", 0x30e03bb7},
	{"db8.62",  0xaef67e9},
	{"db8.62.idx", 0x9c8f9af9},
	{"db8.71", 0x6895eece},
	{"db8.71.idx", 0x27f561fb},
};

/* Information about the db fileset. */
static const DB_FILE dbfile_info_italian[] = {
	{"idb6", 6, 0},
};

/* A table of crc values for each database file. */
static const DBCRC dbcrc_italian[] = {
	{"idb6", 0xac336760},
	{"idb6.idx", 0x6c3deb60},
};


/* Each byte in the database contains either 5 values (3**5=243),
 * accounting for values 0..242 in a byte.	The following table tells
 * values 243..255.  These values the number of consecutive positions
 * whose value is equal to the "dominant" value of the database.  For
 * example, Table[13] corresponds to a byte value of 255 (242+13) and
 * says that the next 1600 values in the db are all the same as the
 * dominant value.
 */
static int Skip[14] = {
	0, 10, 15, 20, 25, 30, 40, 50, 60, 100, 200, 400, 800, 1600
};

static int runlength[256]; /* holds the run length for every byte. */

/* Divisors to extract values from a byte.	Byte contains 5 position
 * values, each which can be W/L/D.  Div is the number to do a divide/
 * mod to extract a value.
 * The values are stored as ( v1 ) + ( v2 * 3 ) + ( v3 * 3*3 ) +
 *		( v4 * 3*3*3 ) + ( v5 * 3*3*3*3 )
 */
static int Div[7] = {0, 1, 3, 9, 27, 81, 243};

/* An array to map Chinook values into egdb values. */
static const int chinook_to_egdb[] = {
	EGDB_DRAW,
	EGDB_WIN,
	EGDB_LOSS,
	EGDB_UNKNOWN
};


/* Function prototypes. */
int *create_bm_indices(int nbm, int nwm, int bmrank, int wmrank, int *num_base_indices);
int DBindex(int *pos, int k);
int read_block(CHINOOK_DATA *hdat, DB_FILE *dbfile, int block);


static void reset_db_stats(EGDB_DRIVER *handle)
{
	CHINOOK_DATA *hdat = (CHINOOK_DATA *)handle->internal_data;

	memset(&hdat->lookup_stats, 0, sizeof(hdat->lookup_stats));
}


static EGDB_STATS *get_db_stats(EGDB_DRIVER *handle)
{
	CHINOOK_DATA *hdat = (CHINOOK_DATA *)handle->internal_data;

	return(&hdat->lookup_stats);
}


/*
 * Read and parse an index file.
 *
 * Index file contains all the sub-database - file is in text
 * each db is in there, and this loop reads in that info.  For
 * example, here is the head of the file:
 * BASE1100.00 =	DB 1bk, 1wk, 0bp, 0wp - mostly draws
 * S	  0 	  0/0	Start position 0 at block 0, byte 0
 * E	995 	  0/189 End at position 995, at block 0 byte 189
 *			By defualt, block size is 1024 bytes
 *
 *			This says this database of 995 positions
 *			has been compressed to 189 bytes.
 *
 * BASE1001.00 +	DB 1bk, 0wk, 0bp, 1wp on rank 0. Wins
 * S	  0 	  0/189
 * E	125 	  0/214
 *			125 positions in 214-189=25 bytes
 *			Most  positions in DB are black wins
 *
 * BASE1001.01 =	DB 1bk, 0wk, 0bp, 1wp on rank 1.  Draws
 * S	  0 	  0/214
 * etc. 		Interesting.  With the checker on the
 *			1st rank most pos are draws, on the 0th,
 *			wins.  Must be because of the move.
 *
 * BASE0011.51 ==	DB 0 kings, 1 bp on 5th, 1 wp on 1st
 *			== means entire db is drawn.  ++ means
 *			all db is win; -- all is loss.
 *
 * Here is a bigger db:
 * BASE1311.26 -
 * S	  0    2024/783 Position 0 at block 2024, byte 783
 *			i.e. starts at addr=2024*1024+783
 * . 327765    2025 Next block (2025), 1st pos is 327,765
 * .1625795    2026 Next block (2026), 1st pos is 1,625,795
 * E1753920    2026/143 Ending addr=2026*1024+143
 *			Num positions in db = 1,753,920
 *			Bytes used: end_idx_block-start_idx_block=1408
 *			Nice compression ratio!!
 */
int parse_index_file(CHINOOK_DATA *hdat, DB_FILE *p, char *filepath, SIZE_T *allocated_bytes)
{
	int wk, bk, bp, wp, wr, br, stat;
	int pieces, last_pieces;
	int dbidx, num_base_indices;
	int count, index, addr, byte;
	char dt, dtt, next;
	FILE *dbifp;
	DBENTRY *subdb;
	char msg[MAXMSG];
	char filename[MAXFILENAME];

	/* Read index file and build table */
	sprintf(filename, "%s%s.idx", filepath, p->filename);
	dbidx = -1;
	dbifp = fopen(filename, "r");
	if (dbifp == NULL) {
		sprintf(msg, "ERROR: cannot open %s\n", filename);
		(*hdat->log_msg_fn)(msg);
		return(-1);
	}

	/* Log some stats about this file. */
	sprintf(msg, "%10d index blocks: %s\n", p->num_idx_blocks, filename);
	(*hdat->log_msg_fn)(msg);

	/* Read the entire index file */
	for (last_pieces = 0; ; last_pieces = pieces) {

		/* Parse the header line of each sub-database */
		stat = fscanf(dbifp, "BASE%1d%1d%1d%1d.%1d%1d %c%c",
					&bk, &wk, &bp, &wp, &br, &wr, &dt, &dtt);

		/* Done if end-of-file reached. */
		if (stat <= 0)
			break;

		if (stat < 8) {
			sprintf(msg, "ERROR: initdblookup scanf failure %d\n", stat);
			(*hdat->log_msg_fn)(msg);
			return(-1);
		}

		pieces = bk + wk + bp + wp;

		/* What is the database? */
		index = DBINDEX(bk, wk, bp, wp);
		subdb = hdat->DBTable[index];
		if (subdb == NULL) {
			sprintf(msg, "ERROR: %d %d %d %d %d %d %c dbtab\n",
						bk, wk, bp, wp, br, wr, dt);
			(*hdat->log_msg_fn)(msg);
			return(-1);
		}

		 /* Based on who has checkers, index into the 1/7/49 entries. */
		if (wp == 0)
			count = br;
		else
			count = br * 7 + wr;
		subdb += count;

		/* If we are not at end of line, we have a db
		 * that is all one value.
		 */
		if (dtt != '\n') {
			if (dtt == '+')
				subdb->all_same_val = CHINOOK_WIN;
			else if (dtt == '-')
				subdb->all_same_val = CHINOOK_LOSS;
			else if (dtt == '=')
				subdb->all_same_val = CHINOOK_TIE;
			else {
				sprintf(msg, "ERROR: illegal result %c\n", dtt);
				(*hdat->log_msg_fn)(msg);
			}
			fscanf(dbifp, "\n");
			continue;
		}

		*allocated_bytes += ROUND_UP(sizeof(INDEX_DATA), MALLOC_ALIGNSIZE);
		subdb->index_data = (INDEX_DATA *)malloc(sizeof(INDEX_DATA));
		if (!subdb->index_data) {
			(*hdat->log_msg_fn)("Cannot allocate memory for index data\n");
			return(-1);
		}

		/*
		 * Compute the number of black and white king configurations for this
		 * database slice
		 */
		subdb->index_data->rangeBK = choose(32 - bp - wp, bk);
		subdb->index_data->rangeWK = choose(32 - bp - wp - bk, wk);

		/* Determine the number of black positions which have one
		 * or more pawns on bmrank.  This number is obtained by
		 * (4*(rank+1) choose nbm) - (4*rank choose nbm).
		 */
		if (bp == 0)
			subdb->index_data->first_bmindex = 0;
		else
			subdb->index_data->first_bmindex = choose(br * 4, bp);

		index = CI_INDEX(bp, br, wp, wr);
		if (!hdat->ci_base_table[index]) {

			/* Create the table of base indicies for checkers. */
			subdb->index_data->checker_index_base =
								create_bm_indices(bp, wp, br, wr, &num_base_indices);

			if (!subdb->index_data->checker_index_base) {
				(*hdat->log_msg_fn)("Cannot allocate memory for checker index base table\n");
				return(-1);
			}

			hdat->ci_base_table[index] = subdb->index_data->checker_index_base;
			*allocated_bytes += ROUND_UP(num_base_indices * sizeof(int), MALLOC_ALIGNSIZE);
		}
		else {

			/* We already have an entry for this configuration. */
			subdb->index_data->checker_index_base = hdat->ci_base_table[index];
		}
		
		/* Save the file handle for this database file. */
		subdb->file = p;

		/* Set the default type */
		if (dt == '=')
			subdb->dominant_val = CHINOOK_TIE;
		else if (dt == '+')
			subdb->dominant_val = CHINOOK_WIN;
		else if (dt == '-')
			subdb->dominant_val = CHINOOK_LOSS;
		else {
			sprintf(msg, "ERROR: illegal type %c\n", dt);
			(*hdat->log_msg_fn)(msg);
			return(-1);
		}

		/* Read in the starting address */
		next = getc(dbifp);
		if (next != 'S') {
			sprintf(msg, "ERROR: illegal start %c\n", next);
			(*hdat->log_msg_fn)(msg);
			return(-1);
		}
		stat = fscanf(dbifp, "%ld%ld/%ld\n", &index, &addr, &byte);
		if (stat != 3) {
			sprintf(msg, "ERROR: illegal start read %d\n", stat);
			(*hdat->log_msg_fn)(msg);
			return(-1);
		}
		subdb->start_idx_block = addr;
		subdb->startbyte = (unsigned short)byte;

		/* If starting address takes us into a new block,
		 * update table of indexes.
		 */
		if (addr != dbidx)
			p->indices[++dbidx] = index;

		/* Read each line of the database - block number
		 * and position number.
		 */
		for ( ; ; ) {
			stat = fscanf(dbifp, "%c", &dt);
			if (stat != 1) {
				sprintf(msg, "ERROR: db read1 %d\n", stat);
				(*hdat->log_msg_fn)(msg);
				return(-1);
			}
			if (dt == 'E')
				break;
			stat = fscanf(dbifp, "%ld%ld\n", &index, &addr);
			if (stat != 2) {
				sprintf(msg, "ERROR: db read2 %d\n", stat);
				(*hdat->log_msg_fn)(msg);
				return(-1);
			}
			if (addr != dbidx)
				p->indices[++dbidx] = index;
		}
		stat = fscanf(dbifp, "%ld%ld/%ld\n", &index, &addr, &byte);
		if (stat != 3) {
			sprintf(msg, "ERROR: db read3 %d\n", stat);
			(*hdat->log_msg_fn)(msg);
			return(-1);
		}
		subdb->end_idx_block = addr;
#if LOG_SLICE_INFO
		sprintf(msg, "bk %d, wk %d, bp %d, wp %d, br %d, wr %d, idx blocks %d\n",
			bk, wk, bp, wp, br, wr, (1 + subdb->end_idx_block - subdb->start_idx_block));
		(*hdat->log_msg_fn)(msg);
#endif
	}
	fclose(dbifp);
	return(0);
}


/*
 * Return the maximum number of cache blocks that could be used if we had unlimited ram.
 */
int needed_cache_buffers(CHINOOK_DATA *hdat)
{
	int i;
	int count;

	count = 0;
	for (i = 0; i < hdat->numdbfiles; ++i)
		if (hdat->dbfiles[i].pieces <= hdat->maxpieces && hdat->dbfiles[i].bytes && !hdat->dbfiles[i].autoload)
			count += hdat->dbfiles[i].num_cache_blocks;

	return(count);
}


/* Initialize the database for the number of pieces requested.
 *
 * cache_mb is the amount of ram in mb to use for cache buffers.
 * filepath is the path where the database files are located.
 * msg_fn is a pointer to a function that will log status and error messages.
 *
 * Return non-zero on error.
 */
static int initdblookup(CHINOOK_DATA *hdat, int pieces, int cache_mb, const char *filepath, void (*msg_fn)(const char *))
{
	int i, j, p, w, b, wk, bk, bp, wp;
	int count, index;
	int64_t allocated_bytes;			/* keep track of heap allocations in bytes. */
	int64_t autoload_bytes;				/* keep track of autoload allocations in bytes. */
	SIZE_T index_bytes;				/* keep track of index alloc overhead. */
	int stat;
	SIZE_T size;
	int t0, t1;
	unsigned char *blockp;
	DBENTRY *subdb;
	char msg[MAXMSG];
	char filename[MAXFILENAME];

	/* Save off some global data. */
	strcpy(hdat->db_filepath, filepath);

	/* Add trailing backslash if needed. */
	size = (int)strlen(hdat->db_filepath);
	if (size && (hdat->db_filepath[size - 1] != '\\'))
		strcat(hdat->db_filepath, "\\");

	if (pieces < 6)
		pieces = 6;
	hdat->maxpieces = pieces;
	allocated_bytes = 0;
	autoload_bytes = 0;
	index_bytes = 0;
	hdat->log_msg_fn = msg_fn;

	std::sprintf(msg, "Available RAM: %smb\n", format_with_commas(get_mem_available_mb()).c_str());
	(*hdat->log_msg_fn)(msg);

	hdat->autoload_pieces = 0;
	if (pieces == 8) {
		if (cache_mb >= 5500)
			hdat->autoload_pieces = 8;
		else if (cache_mb >= 900)
			hdat->autoload_pieces = 7;
		else if (cache_mb >= 120)
			hdat->autoload_pieces = 6;
	}
	else if (pieces == 7) {
		if (cache_mb >= 430)
			hdat->autoload_pieces = 7;
		else if (cache_mb >= 120)
			hdat->autoload_pieces = 6;
	}
	else
		if (cache_mb >= 50)
			hdat->autoload_pieces = 6;

	/* Compute binomial coeficients */
	initbicoef();

	/* Initialize the runlength array. */
	for (i = 0; i < 256; i++) {
		if (i < 243)
			runlength[i] = 5;
		else
			runlength[i] = Skip[i - 242];
	}
#if USE_RUNLENGTH16
	hdat->runlength16_chinook = (unsigned short *)malloc(65536 * sizeof(unsigned short));
	for (i = 0; i < 65536; ++i)
		hdat->runlength16_chinook[i] = runlength[i & 0xff] + runlength[i >> 8];
#endif

#if SMP
	ResetSpinLock(hdat->lock);
#endif

	/* Allocate the ci_base_table. */
	hdat->ci_base_table = (int **)calloc(CI_TABLESIZE, sizeof(int *));
	if (!hdat->ci_base_table) {
		(*hdat->log_msg_fn)("Cannot allocate memory for ci_base_table.\n");
		return(-1);
	}
	allocated_bytes += CI_TABLESIZE * sizeof(int *);
	index_bytes += CI_TABLESIZE * sizeof(int *);

	/* Allocate the dbfile[] array. */
	if (hdat->db_type == EGDB_CHINOOK_WLD)
		hdat->numdbfiles = ARRAY_SIZE(dbfile_info);
	else
		hdat->numdbfiles = ARRAY_SIZE(dbfile_info_italian);
	hdat->dbfiles = (DB_FILE *)calloc(hdat->numdbfiles, sizeof(DB_FILE));
	if (!hdat->dbfiles) {
		(*hdat->log_msg_fn)("Cannot allocate memory for dbfiles.\n");
		return(-1);
	}
	if (hdat->db_type == EGDB_CHINOOK_WLD)
		memcpy(hdat->dbfiles, dbfile_info, sizeof(dbfile_info));
	else
		memcpy(hdat->dbfiles, dbfile_info_italian, sizeof(dbfile_info_italian));

	/* DBTable is used to speedup looking up an entry.	There is one
	 * entry for each possible database.  Note that some of the
	 * posible databases do not really exist (e.g. 6 against 0)
	 */
	for (i = 0; i < DBTABLESIZE; i++)
		hdat->DBTable[i] = 0;

	/* Open each database file, get the number of disk blocks, store the file handle.
	 * Allocate arrays.
	 */
	for (i = 0; i < hdat->numdbfiles; ++i) {
		if (hdat->dbfiles[i].pieces > pieces)
			continue;

#if OMIT_LOPSIZED_SLICES
		if (hdat->dbfiles[i].lopsided)
			continue;
#endif

		/* Open database file */
		sprintf(filename, "%s%s", hdat->db_filepath, hdat->dbfiles[i].filename);
		hdat->dbfiles[i].fp = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ,
						NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
		if (hdat->dbfiles[i].fp == INVALID_HANDLE_VALUE) {
			if (hdat->dbfiles[i].lopsided)
				continue;

			sprintf(msg, "Cannot open %s\n", filename);
			(*hdat->log_msg_fn)(msg);
			return(-1);
		}

		/* Get the size in bytes and index blocks. */
		hdat->dbfiles[i].bytes = GetFileSize(hdat->dbfiles[i].fp, NULL);
		hdat->dbfiles[i].num_idx_blocks = 1 + (hdat->dbfiles[i].bytes / IDX_BLOCK_SIZE);

		/* Allocate the array of indices, one for each index block. */
		size = hdat->dbfiles[i].num_idx_blocks * sizeof(hdat->dbfiles[i].indices[0]);
		hdat->dbfiles[i].indices = (uint32_t *)malloc(size);
		allocated_bytes += size;
		index_bytes += size;
		if (hdat->dbfiles[i].indices == NULL) {
			(*hdat->log_msg_fn)("Cannot allocate memory for indices array\n");
			return(-1);
		}

		/* If this file is to be autoloaded, allocate space for it and load it now. */
		if (hdat->dbfiles[i].pieces <= hdat->autoload_pieces) {
			size = hdat->dbfiles[i].bytes;
			hdat->dbfiles[i].autoload = (unsigned char *)VirtualAlloc(0, size, MEM_COMMIT, PAGE_READWRITE);;
			allocated_bytes += size;
			autoload_bytes += size;
			if (hdat->dbfiles[i].autoload == NULL) {
				(*hdat->log_msg_fn)("Cannot allocate memory for autoload array\n");
				return(-1);
			}

			/* Seek to the beginning of the file. */
			if (SetFilePointer(hdat->dbfiles[i].fp, 0, NULL, FILE_BEGIN) == -1) {
				(*hdat->log_msg_fn)("ERROR: autoload seek failed\n");
				return(-1);
			}

			sprintf(msg, "autoload %s\n", hdat->dbfiles[i].filename);
			(*hdat->log_msg_fn)(msg);
			if (!read_file(hdat->dbfiles[i].fp, hdat->dbfiles[i].autoload, hdat->dbfiles[i].bytes, get_page_size())) {
				sprintf(msg, "ERROR: autoload read failed for %s, error %lu\n", hdat->dbfiles[i].filename, GetLastError());
				(*hdat->log_msg_fn)(msg);
				CloseHandle(hdat->dbfiles[i].fp);
				hdat->dbfiles[i].fp = INVALID_HANDLE_VALUE;
				return(-1);
			}

			/* Close this database file, we're done with it. */
			CloseHandle(hdat->dbfiles[i].fp);
			hdat->dbfiles[i].fp = INVALID_HANDLE_VALUE;
		}
		else {
			/* These slices are not autoloaded.
			 * Allocate the array of indices into cache_buffers[].
			 * Each array entry is either an index into cache_buffers or -1 if that block is not loaded.
			 */
			hdat->dbfiles[i].num_cache_blocks = hdat->dbfiles[i].num_idx_blocks / IDX_BLOCKS_PER_CACHE_BLOCK;
			if (hdat->dbfiles[i].num_idx_blocks > hdat->dbfiles[i].num_cache_blocks * IDX_BLOCKS_PER_CACHE_BLOCK)
				++hdat->dbfiles[i].num_cache_blocks;
			size = hdat->dbfiles[i].num_cache_blocks * sizeof(hdat->dbfiles[i].cache_bufferi[0]);
			hdat->dbfiles[i].cache_bufferi = (int *)malloc(size);
			allocated_bytes += size;
			if (hdat->dbfiles[i].cache_bufferi == NULL) {
				(*hdat->log_msg_fn)("Cannot allocate memory for cache_bufferi array\n");
				return(-1);
			}

			/* Init these cache buffer indices to UNDEFINED_BLOCK_ID, means that cache block is not loaded. */
			for (j = 0; j < hdat->dbfiles[i].num_cache_blocks; ++j)
				hdat->dbfiles[i].cache_bufferi[j] = UNDEFINED_BLOCK_ID;
		}
	}

	/* Initialize data structures for each size of database
	 * For each combination of wk, bk, wp, bp totalling 6 pieces
	 * have an entry in DBTable.  Each entry is a pointer to storage
	 * For a pure king endgame, need only 1 entry.	For a db with
	 * only 1 side having checkers, need 7 entries (checker can be
	 * on one of 7 ranks).	Other dbs need 49 entries - each side
	 * has a checker on 1 of 7 ranks.
	 */
	for (p = 2; p <= hdat->maxpieces; p++) {
		for (b = 1; b <= p; b++) {
			w = p - b;
			if (w == 0)
				continue;
			for (bk = b; bk >= 0; bk--) {
				for (wk = w; wk >= 0; wk--) {
					bp = b - bk;
					wp = w - wk;
					if (bp == 0 && wp == 0)
						count = 1;
					else if (bp == 0 || wp == 0)
						count = 7;
					else
						count = 49;
					index = DBINDEX(bk, wk, bp, wp);

					/* Allocate storage of 1, 7 or 49 entries */
					size = sizeof(DBENTRY) * count;
					subdb = (DBENTRY *)malloc(size);
					allocated_bytes += size;
					index_bytes += size;
					if (subdb == NULL) {
						(*hdat->log_msg_fn)("Cannot malloc memory for DBENTRY in initdblookup\n");
						return(-1);
					}

					/* Initialize each entry */
					hdat->DBTable[index] = subdb;
					for (i = 0; i < count; i++) {
						subdb->all_same_val = CHINOOK_UNKNOWN;
						subdb->dominant_val = CHINOOK_UNKNOWN;
						subdb->start_idx_block = 0;
						subdb->startbyte = 0;
						subdb->end_idx_block = 0;
						subdb->index_data = 0;
						subdb++;
					}
				}
			}
		}
	}

	/* Read the index files. */
	for (i = 0; i < hdat->numdbfiles; ++i) {
		if (hdat->dbfiles[i].pieces <= hdat->maxpieces) {

			/* If its a lopsided file that isn't present, skip it. */
			if (hdat->dbfiles[i].bytes != 0) {
				size = 0;
				stat = parse_index_file(hdat, hdat->dbfiles + i, hdat->db_filepath, &size);
				allocated_bytes += size;
				index_bytes += size;
				if (stat)
					return(stat);
			}
		}
	}

	sprintf(msg, "Allocated %dkb for indexing\n", (int)((allocated_bytes - autoload_bytes) / 1024));
	(*hdat->log_msg_fn)(msg);
	sprintf(msg, "Allocated %dkb for permanent slice caches\n", (int)(autoload_bytes / 1024));
	(*hdat->log_msg_fn)(msg);

	/* Allocate the cache buffers - these are fixed-size blocks that
	 * cache portions of the database as they are needed.
	 * First compute how many buffers can actually be use.  For example, if
	 * we are only using 6pieces and we autoloaded everything then we dont
	 * need any buffers.
	 */
	int needed = needed_cache_buffers(hdat);
	if (needed > 0) {
		if ((allocated_bytes + MIN_CACHE_BUF_BYTES) / ONE_MB >= cache_mb) {

			/* We need more memory than he gave us, allocate 10mb of cache
			 * buffers if we can use that many.
			 */
			hdat->num_cache_buffers = std::min(MIN_CACHE_BUF_BYTES / CACHE_BLOCK_SIZE, needed);
			sprintf(msg, "Allocating the minimum %d cache buffers\n",
							hdat->num_cache_buffers);
			(*hdat->log_msg_fn)(msg);
		}
		else
			hdat->num_cache_buffers = (int)(((int64_t)cache_mb * (int64_t)ONE_MB - allocated_bytes) / (int64_t)(CACHE_BLOCK_SIZE + sizeof(CCB)));
	}
	else
		hdat->num_cache_buffers = 0;

	hdat->num_cache_buffers = std::min(hdat->num_cache_buffers, needed);
	
	if (hdat->num_cache_buffers > 0) {
		sprintf(msg, "allocating %d cache buffers of size %d\n",
					hdat->num_cache_buffers, CACHE_BLOCK_SIZE);
		(*hdat->log_msg_fn)(msg);

		hdat->ccbs = (CCB *)malloc(hdat->num_cache_buffers * sizeof(CCB));
		if (hdat->ccbs == NULL) {
			(*hdat->log_msg_fn)("ERROR: malloc failure on cache control blocks\n");
			return(-1);
		}

		/* Clear so we know which ones got used if we have to bail early. */
		memset(hdat->ccbs, 0, hdat->num_cache_buffers * sizeof(CCB));

		for (i = 0; i < hdat->num_cache_buffers; i++) {
			hdat->ccbs[i].next = i + 1;
			hdat->ccbs[i].prev = i - 1;
			hdat->ccbs[i].blocknum = UNDEFINED_BLOCK_ID;
		}

		hdat->ccbs[hdat->num_cache_buffers - 1].next = 0;
		hdat->ccbs[0].prev = i - 1;
		hdat->ccbs_top = 0;

		/* Allocate the cache buffers in groups of CACHEBLOCK_ALLOC_COUNT at a time. */
		for (i = 0; i < hdat->num_cache_buffers; i += CACHEBLOCK_ALLOC_COUNT) {
			count = std::min(CACHEBLOCK_ALLOC_COUNT, hdat->num_cache_buffers - i);
			size = count * CACHE_BLOCK_SIZE * sizeof(unsigned char);
			blockp = (unsigned char *)VirtualAlloc(0, size, MEM_COMMIT, PAGE_READWRITE);
			if (blockp == NULL) {
				(*hdat->log_msg_fn)("ERROR: VirtualAlloc failure on cache buffers\n");
				return(-1);
			}

			/* Assign the ccbs[] data pointers. */
			for (j = 0; j < count; ++j)
				hdat->ccbs[i + j].data = blockp + j * CACHE_BLOCK_SIZE * sizeof(unsigned char);
		}

		/* Pre-load the cache with database slices. */
		count = 0;
		t0 = GetTickCount();
		for (i = 0; i < hdat->numdbfiles && count < hdat->num_cache_buffers; ++i) {
			if (!hdat->dbfiles[i].autoload && hdat->dbfiles[i].bytes != 0) {
				for (j = 0; j < hdat->dbfiles[i].num_cache_blocks && count < hdat->num_cache_buffers; ++j) {
					read_block(hdat, hdat->dbfiles + i, j);
					++count;
				}
			}
		}
		t1 = GetTickCount();
		sprintf(msg, "Read %d buffers in %d sec, %.3f msec/buffer\n", 
					count, (t1 - t0) / 1000, (double)(t1 - t0) / (double)count);
		(*hdat->log_msg_fn)(msg);
	}

	std::sprintf(msg, "Available RAM: %smb\n", format_with_commas(get_mem_available_mb()).c_str());
	(*hdat->log_msg_fn)(msg);

	return(0);
}


/*
 * Read a cache block identified by its block number within the sub-database
 * defined by dbentry.
 * If the block is already in the cache then do nothing and exit.
 * If the block is not in cache then read it and link it into the end of the LRU list.
 * Return non-zero on a read error.
 */
int read_block(CHINOOK_DATA *hdat, DB_FILE *dbfile, int block)
{
	int use;

	/* If this block is already cached, then exit. */
	if (dbfile->cache_bufferi[block] != UNDEFINED_BLOCK_ID)
		return(0);

	++hdat->lookup_stats.lru_cache_loads;

	/*
	 * Need a cache buffer to read into.  Look at the top one (ccbs_top)
	 * If has some data, mark it as invalid.
	 */
	if (hdat->ccbs[hdat->ccbs_top].blocknum >= 0)
		hdat->ccbs[hdat->ccbs_top].dbfile->cache_bufferi[hdat->ccbs[hdat->ccbs_top].blocknum] = UNDEFINED_BLOCK_ID;

	use = hdat->ccbs_top;

	/* This ccb is unused until the read succeeds. If the read fails it stays
	 * at the top of the lru list, so the next load reuses it.
	 */
	hdat->ccbs[use].blocknum = UNDEFINED_BLOCK_ID;
	if (!read_cache_block(dbfile->fp, (int64_t)block * CACHE_BLOCK_SIZE, CACHE_DATAP(use), CACHE_BLOCK_SIZE, hdat->log_msg_fn))
		return(1);

	/* Mark this buffer as in use */
	dbfile->cache_bufferi[block] = use;

	/* Fix linked list to point to the next oldest entry */
	hdat->ccbs_top = hdat->ccbs[hdat->ccbs_top].next;
	hdat->ccbs[use].blocknum = block;
	hdat->ccbs[use].dbfile = dbfile;
	return(0);
}


#if CACHE_BLOCKS_PER_PROBE3 > 1
/*
 * Read some number of cache blocks (called a probe3_block) in one
 * disk I/O operation, then copy them into cache block buffers in
 * the lru list.  This is slightly faster than doing n separate I/O operations.
 */
int read_probe3_block(CHINOOK_DATA *hdat, DB_FILE *dbfile, int probe3_block)
{
	int i;
	int first_block, last_block;
	int read_count;
	int top;

	/* Get the first block to read. */
	first_block = probe3_block * CACHE_BLOCKS_PER_PROBE3;

	/* Get the last block to read. */
	last_block = first_block + CACHE_BLOCKS_PER_PROBE3 - 1;
	last_block = MIN(last_block, dbfile->num_cache_blocks - 1);

	/* Seek to the probe3 block - probe3_block# * PROBE3_BLOCK_SIZE. */
	if (SetFilePointer(dbfile->fp, (probe3_block * PROBE3_BLOCK_SIZE), NULL, FILE_BEGIN) == -1) {
		(*hdat->log_msg_fn)("ERROR: dblookup seek failed\n");
		return(CHINOOK_UNKNOWN);
	}

	/* Read in the PROBE3_BLOCK_SIZE bytes */
	ReadFile(dbfile->fp, hdat->probe3_buffer, PROBE3_BLOCK_SIZE, (unsigned long *)&read_count, NULL);

	/* Copy pieces into cache buffers. */
	top = hdat->ccbs_top;
	for (i = first_block; i <= last_block; ++i) {

		/* Dont cache this block if it is already cached. */
		if (dbfile->cache_bufferi[i] != UNDEFINED_BLOCK_ID)
			continue;

		++hdat->lookup_stats.lru_cache_loads;

		/* Copy this block into top. */
		memcpy(CACHE_DATAP(top), hdat->probe3_buffer + (i - first_block) * CACHE_BLOCK_SIZE, CACHE_BLOCK_SIZE);

		/* If the top buffer had data in it, mark that block as invalid. */
		if (hdat->ccbs[top].blocknum >= 0)
			hdat->ccbs[top].dbfile->cache_bufferi[hdat->ccbs[top].blocknum] = UNDEFINED_BLOCK_ID;

		/* Mark this buffer as in use */
		dbfile->cache_bufferi[i] = top;
		hdat->ccbs[top].blocknum = i;
		hdat->ccbs[top].dbfile = dbfile;

		/* Move top down to the next cache block. */
		top = hdat->ccbs[top].next;
	}

	/* Adjust top of lru list. */
	hdat->ccbs_top = top;
	return(0);
}
#endif

/*
 * Look up a position in the database, return one of 
 * these results: CHINOOK_TIE, CHINOOK_WIN, 
 * CHINOOK_LOSS, or CHINOOK_UNKNOWN.
 *
 * Do not call with one side having 0 pieces.
 *
 * Do not call with the side to move having a 
 * capture move, or with the other side having
 * a capture move if it was that side's turn.
 *
 * board is an array of bitboards describing the board position.
 * The macros CHINOOK_WHITE, CHINOOK_BLACK, and CHINOOK_KINGS
 * can be used to index into this array.
 *		board[0] is the white pieces
 *		board[1] is the black pieces
 *		board[2] is for king attributes
 *
 * turn is the side to move, CHINOOK_WHITE or CHINOOK_BLACK.
 *
 * The board bitboard must be in this weird format:
 *
 *	  square numbers	  db bit positions
 *
 *		   WHITE				WHITE
 *	   29  30  31  32		31	30	29	28
 *	 25  26  27  28 	  27  26  25  24  
 *	   21  22  23  24		23	22	21	20
 *	 17  18  19  20 	  19  18  17  16
 *	   13  14  15  16		15	14	13	12
 *	  9  10  11  12 	  11  10  09  08
 *		5	6	7	8		07	06	05	04
 *	  1   2   3   4 	  03  02  01  00
 *		   BLACK				BLACK
 *
 * dblookup works like this:
 * The color of the side to move is tested.  If its white's turn,
 * then the board is reversed.  The db only contains positions where it
 * is black's turn to move.
 *
 * The number of black men, black kings, white men and white kings are counted,
 * and these are used to index into DBTable[], and array of info about each 
 * configuration.  Some configurations are all the same result for every position,
 * and if DBTable has this result if its one of these cases.
 *
 * The position is converted into an index.  The database is contained in
 * a handful of files, and the indices begin at 0 for positions in each file.
 *
 * For each of these db files there is an array of indices that give the index
 * of the first position in each 1024-byte block.  The index of the position
 * in question is located to within a particular block by doing a binary search
 * in the array of indices.
 * 
 * Some db files are "autoloaded" into permanent ram buffers.  If the position is
 * in one of these files then a pointer is assigned to the base of the
 * 1024-byte block (I call them index blocks) containing the position.
 * If the position is not in an autoloaded buffer, then the cache buffers
 * are checked.  Each buffer is some multiple of 1024 bytes.  If the position
 * is already in a buffer then a pointer is assigned to the base of
 * the index block containing the position.
 * If the position is not in a buffer then the action depends on the 
 * cl (conditional lookup) argument.  If cl is true, dblookup returns the
 * value CHINOOK_UNKNOWN.  If cl is false, some number of cache buffers are read
 * from disk (including the one with the requested position) and the
 * index block pointer is assigned.
 *
 * A linear search is then done within the index block for the index of the
 * requested position.  If the target index is closer to the end of the block
 * than the beginning, the search starts at the end and goes in reverse.  When
 * the byte containing the target index is found, the result is extracted and
 * returned.
 */
static int dblookup(EGDB_DRIVER *handle, EGDB_ROW_REVERSED_BITBOARD *board, int color, int cl)
{
	CHINOOK_DATA *hdat = (CHINOOK_DATA *)handle->internal_data;
	int result, diff, back, fwd, use;
	int index, cindex;
	DBENTRY *dbentry;
	int start, end, middle, i;
	unsigned char *buffer;
	uint16_t blockcopy[IDX_BLOCK_SIZE / 2];		/* private copy of a cached index block, 16-bit aligned. */
	int *dbbufptr;
	uint32_t *dbindex;
	char msg[MAXMSG];
#if USE_RUNLENGTH16
	unsigned short *diskblock16;
	unsigned short *rltp;
#endif
#if CACHE_BLOCKS_PER_PROBE3 > 1
	int probe3_block;
#if !FAST_PROBE3_READ
	int last_block;
#endif
#endif
	unsigned int blackmen, whitemen, blackkings, whitekings;
	int nbm, nbk, nwm, nwk, bmrank, wmrank;
	int bmindex, wmindex, bkindex, wkindex, firstidx;
	INDEX_DATA *db;

	++hdat->lookup_stats.db_requests;

	/* If it's white to move, reverse the board & look up black to move. */
	if (color == EGDB_WHITE) {
		blackmen = reverse_int(board->white_man);
		whitemen = reverse_int(board->black_man);
		blackkings = reverse_int(board->white_king);
		whitekings = reverse_int(board->black_king);
	}
	else {
		blackmen = board->black_man;
		whitemen = board->white_man;
		blackkings = board->black_king;
		whitekings = board->white_king;
	}

	/* Get the number of men, kings, and the ranks of the leading
	 * black and white men.
	 */
	nbm = bitcount(blackmen);
	bmrank = 0;
	if (nbm)
		bmrank = MSB(blackmen) / 4;
	nbk = bitcount(blackkings);
	nwm = bitcount(whitemen);
	wmrank = 0;
	if (nwm)
		wmrank = (31 - LSB(whitemen)) / 4;	
	nwk = bitcount(whitekings);

	if ((nbm + nwm + nwk + nbk > MAX_PIECES)) {
		++hdat->lookup_stats.db_not_present_requests;
		return(EGDB_UNKNOWN);
	}

	/* If one side has nothing, return appropriate value.
	 * This must come before the DBTable access, because 8 pieces against
	 * nothing gives DBINDEX == DBTABLESIZE, one past the end of the table.
	 */
	if ((nbm + nbk) == 0) {
		++hdat->lookup_stats.db_returns;
		return(EGDB_LOSS);
	}
	if ((nwm + nwk) == 0) {
		++hdat->lookup_stats.db_returns;
		return(EGDB_WIN);
	}

	/* DBTable only has entries for the slices that were opened. */
	if (nbm + nwm + nwk + nbk > hdat->maxpieces) {
		++hdat->lookup_stats.db_not_present_requests;
		return(EGDB_UNKNOWN);
	}

	/* Lookup info for this slice. */
	dbentry = hdat->DBTable[DBINDEX(nbk, nwk, nbm, nwm)];
	if (dbentry == 0) {
		(*hdat->log_msg_fn)("ERROR: NULL DBTable entry for this pos!\n");
		return(EGDB_UNKNOWN);
	}
	if (nwm == 0)
		dbentry += bmrank;
	else
		dbentry += (bmrank * 7 + wmrank);
	if (dbentry == NULL) {
		(*hdat->log_msg_fn)("ERROR: NULL DBTable entry for this pos!\n");
		return(EGDB_UNKNOWN);
	}

	if (dbentry->all_same_val != CHINOOK_UNKNOWN) {
		++hdat->lookup_stats.db_returns;
		return(chinook_to_egdb[dbentry->all_same_val]);
	}
		
	if (dbentry->index_data == NULL) {

		/* This can happen if it is one of the lopsided slices that
		 * we are not using.
		 */
		++hdat->lookup_stats.db_not_present_requests;
		return(EGDB_UNKNOWN);
	}

	/* Determine bmindex. */
	bmindex = bitboard2index(blackmen, 0);

	/* Determine wmindex, accounting for interfering black men. */
	wmindex = bitboard2index_rev(whitemen, 0, blackmen);

	/* Determine bkindex, accounting for interfering black and white men. */
	bkindex = bitboard2index(blackkings, 0, blackmen | whitemen);

	/* Determine wkindex, accounting for interfering other pieces. */
	wkindex = bitboard2index(whitekings, 0, blackmen | whitemen | blackkings);

	/* Compute the total index. */
	db = dbentry->index_data;
	bmindex -= db->first_bmindex;	/* get matching index number */

	/* Calculate the number of white squares behind the white leading
	 * rank that are availble for white checkers.
	 */
	i = 0;
	firstidx = 0;
	if (nwm) {
		if (wmrank) {
			i = 1 << (8 - wmrank) * 4;

			/* Instead of bitcount(~blackmen & ~(i-1)), use the
			 * simpler equivalent.
			 */
			i = bitcount(~(blackmen | (i - 1)));
		}
		firstidx = choose(i, nwm);
	}

	/* cindex is the index for the black and white men combined. */
	cindex = db->checker_index_base[bmindex] + wmindex - firstidx;
	index = (((cindex * db->rangeBK) + bkindex) * db->rangeWK) + wkindex;

	/* Have to find the value */
	start = dbentry->start_idx_block;
	end = dbentry->end_idx_block;
	dbindex = dbentry->file->indices;
	dbbufptr = dbentry->file->cache_bufferi;
	start = find_block(start, end + 1, dbindex, (uint32_t)index);

	/* If this database file was autoloaded, then skip the cache probe. */
	if (dbentry->file->autoload) {
		buffer = dbentry->file->autoload + (start * IDX_BLOCK_SIZE);
		++hdat->lookup_stats.autoload_hits;
	}
	else {
		middle = IDX_2_CACHE_BLOCK(start);
#if SMP
		AcquireSpinLock(hdat->lock);
#endif
		if (dbbufptr[middle] == UNDEFINED_BLOCK_ID) {

			/* This idx block is not in memory. */
			if (cl) {
#if SMP
				ReleaseSpinLock(hdat->lock);
#endif
				return(EGDB_NOT_IN_CACHE);
			}

#if CACHE_BLOCKS_PER_PROBE3 > 1

			/* Get the PROBE3 block to read. */
			probe3_block = CACHE_2_PROBE3_BLOCK(middle);

#if FAST_PROBE3_READ
			if (read_probe3_block(hdat, dbentry->file, probe3_block))
				return(EGDB_UNKNOWN);
#else
			
			/* Get the first block to read. */
			i = probe3_block * CACHE_BLOCKS_PER_PROBE3;

			/* Get the last block to read. */
			last_block = i + CACHE_BLOCKS_PER_PROBE3 - 1;
			last_block = MIN(last_block, dbentry->file->num_cache_blocks - 1);

			/* Read each block into the cache. */
			for ( ; i <= last_block; ++i)
				if (read_block(hdat, dbentry->file, i))
					return(EGDB_UNKNOWN);
#endif
			/* Setup the use variable for decompression. */
			use = dbbufptr[middle];
	
#else
			/*
			 * Need a cache buffer to read into.  Look at the top one (ccbs_top)
			 * If has some data, mark it as invalid.
			 */
			++hdat->lookup_stats.lru_cache_loads;
			if (hdat->ccbs[hdat->ccbs_top].blocknum >= 0)
				hdat->ccbs[hdat->ccbs_top].dbfile->cache_bufferi[hdat->ccbs[hdat->ccbs_top].blocknum] = UNDEFINED_BLOCK_ID;

			use = hdat->ccbs_top;

			/* This ccb is unused until the read succeeds. If the read fails it stays
			 * at the top of the lru list, so the next load reuses it.
			 */
			hdat->ccbs[use].blocknum = UNDEFINED_BLOCK_ID;
			if (!read_cache_block(dbentry->file->fp, (int64_t)middle * CACHE_BLOCK_SIZE, CACHE_DATAP(use), CACHE_BLOCK_SIZE, hdat->log_msg_fn)) {
#if SMP
				ReleaseSpinLock(hdat->lock);
#endif
				return(EGDB_UNKNOWN);
			}

			/* Mark this buffer as in use */
			dbbufptr[middle] = use;

			/* Fix linked list to point to the next oldest entry */
			hdat->ccbs_top = hdat->ccbs[hdat->ccbs_top].next;
			hdat->ccbs[use].blocknum = middle;
			hdat->ccbs[use].dbfile = dbentry->file;
#endif
		}
		else {

			/* In memory - no i/o necessary.  Adjust linked list so this
			 * block is moved to the end of the list, since it is the
			 * most recently used.
			 */
			++hdat->lookup_stats.lru_cache_hits;
			use = dbbufptr[middle];
			if (hdat->ccbs_top == use)
				hdat->ccbs_top = hdat->ccbs[hdat->ccbs_top].next;
			else {
				back = hdat->ccbs[use].prev;
				fwd = hdat->ccbs[use].next;
				hdat->ccbs[back].next = fwd; 
				hdat->ccbs[fwd].prev = back;

				back = hdat->ccbs[hdat->ccbs_top].prev;
				hdat->ccbs[back].next = use;
				hdat->ccbs[use].prev = back;
				hdat->ccbs[use].next = hdat->ccbs_top;
				hdat->ccbs[hdat->ccbs_top].prev = use;
			}
		}

		/* Get a pointer to the 1K index block within the cache buffer. */
		buffer = CACHE_DATAP(use) + (start % IDX_BLOCKS_PER_CACHE_BLOCK) * IDX_BLOCK_SIZE;

		/* Copy the index block while we still hold the lock. After the lock is
		 * released, another thread can evict this cache block and overwrite its data.
		 */
		memcpy(blockcopy, buffer, IDX_BLOCK_SIZE);
		buffer = (unsigned char *)blockcopy;
#if SMP
		ReleaseSpinLock(hdat->lock);
#endif
	}

	/* If we are closer to the end of the block, search in reverse order. */
	back = 0;
	if (dbentry->end_idx_block > start) {

		/* we are not at the last block. */
		if (dbindex[start + 1] - index < index - dbindex[start])
#if USE_RUNLENGTH16
			/* If we are not at the first index block with an odd startbyte. */
			if (start > dbentry->start_idx_block || !(dbentry->startbyte & 1))
#endif
				back = 1;
	}

#if USE_RUNLENGTH16
	diskblock16 = (unsigned short *)buffer;
	rltp = hdat->runlength16_chinook;
	if (back) {
		cindex = dbindex[start + 1];
		i = IDX_BLOCK_SIZE / 2 - 1;
		while (cindex > index) {
			cindex -= rltp[diskblock16[i]];
			i--;
		}
		i++;
	}
	else {

		/* and move through the bytes until we overshoot index
		 * set the start byte in this block: if it's block 0 of a db,
		 * this can be !=0.
		 */
		if (start == dbentry->start_idx_block) {
			cindex = 0;
			i = dbentry->startbyte;
			if (i & 1)
				cindex += runlength[buffer[i]];
			i = (dbentry->startbyte + 1) / 2;
		}
		else {
			cindex = dbindex[start];
			i = 0;
		}

		/* Search forward through the compressed data until we
		 * overshoot index, then backup one word.
		 */
		while (cindex <= index) {
			cindex += rltp[diskblock16[i]];
			i++;
		}

		/* once we are out here, n>index
		 * assert(n>index);
		 * we overshot again, move back:
		 */
		i--;
		cindex -= rltp[diskblock16[i]];
	}

	/* Change i from a 16-bit index to a byte index. */
	i *= 2;

	/* Now see if we are 1 byte off. */
	if (cindex + runlength[buffer[i]] <= index) {
		cindex += runlength[buffer[i]];
		++i;
	}
#else
	if (back) {
		cindex = dbindex[start + 1];
		for (i = IDX_BLOCK_SIZE - 1; cindex > index && i >= 0; --i)
			cindex -= runlength[buffer[i]];

		i++;

		/* If cindex > index we ran off the start of the block, or the loop never ran. */
		if (cindex > index || i == IDX_BLOCK_SIZE)
			i = -1;
	}
	else {

		/* We have a pointer to the idx block, now need to find the position value. */
		if (start == dbentry->start_idx_block) {
			i = dbentry->startbyte;
			cindex = 0;
		}
		else {
			i = 0;
			cindex = dbindex[start];
		}

		for ( ; cindex <= index && i < IDX_BLOCK_SIZE; ++i)
			cindex += runlength[buffer[i]];

		/* Now cindex > index, go back one byte.
		 * If cindex <= index we ran off the end of the block.
		 */
		if (cindex <= index || i == 0)
			i = -1;
		else {
			--i;
			cindex -= runlength[buffer[i]];
		}
	}
#endif

	/* Safety check, before buffer[i] is used. */
	if (i < 0 || i >= IDX_BLOCK_SIZE) {
		(*hdat->log_msg_fn)("ERROR: scanned block and did not find\n");
		sprintf(msg, "INDEX %d CINDEX %d\n", index, cindex);
		(*hdat->log_msg_fn)(msg);
		sprintf(msg, "Start %d end %d byte %d = middle %d\n",
			dbentry->start_idx_block,
			dbentry->end_idx_block,
			dbentry->startbyte, middle);
		(*hdat->log_msg_fn)(msg);
		return(EGDB_UNKNOWN);
	}

	if (buffer[i] >= 243)

		/* Its a run length encoded value equal to the dominant value. */
		result = dbentry->dominant_val;
	else {

		/* Found - extract the value */
		diff = index - cindex;
		result = buffer[i] % Div[5 - diff + 1];
		result = result / Div[5 - diff];
	}

	++hdat->lookup_stats.db_returns;
	return(chinook_to_egdb[result]);
}


/*
 * Initialize the bm_squares[] array for the starting configuration
 * of nbm black checkers with highest square number highest_bm.
 * Square numbers are base 0, one less than conventional square 
 * numbers that start at 1.
 * The lowest square number is stored in bm_squares[0].
 * The highest square number is stored in bm_squares[nbm - 1].
 */
static void init_bm_config(int bm_squares[], int nbm, int highest_bm)
{
	int i;

	for (i = 0; i < nbm - 1; ++i)
		bm_squares[i] = i;

	bm_squares[nbm - 1] = highest_bm;
}


/*
 * next_bm_config - get the next position for a subset of pieces
 *
 * This routine sets the next position for a subset of pieces.	For example,
 * suppose three black checkers occupy database squares 0,1,2 (which correspond
 * to squares 1,2, and 3 in checkers literature).  Successive calls to this
 * routine sets the following checker configurations:
 * (0,1,2) (0,1,3) (0,2,3) (1,2,3) (0,1,4) etc.
 *
 * The number of times next_bm_config is called is known in advance so no overflow
 * checking is necessary.
 */
static void next_bm_config(int *bm_squares, int nbm)
{
	int i;

	for (i = 0; i < nbm - 1 && bm_squares[i] == bm_squares[i + 1] - 1; ++i)
		bm_squares[i] = i;
	++bm_squares[i];
}


/*
 * Nsq - return the number of squares available for placement of the white
 * checkers.
 *
 * Paramters:
 *	rank	leading white checker rank
 *	Ppos	an array describing the square number of the black checkers
 *	nbm	is the number of black checkers.
 *
 * Although this routine is generalized, it is used only for determining white
 * checker squares.
 */
static int Nsq(int rank, int *bm_squares, int nbm)
{
	int pawnhit = ((7 - rank) * 4);
	int available_squares = ((rank + 1) * 4);

	while (nbm--) {
		if (*bm_squares++ >= pawnhit)
			available_squares--;
	}
	return(available_squares);
}


/*
 * A database slice is determined by the leading rank of the black and white
 * checkers.  A position index within the slice is computed by the position
 * of the black checkers, then adding the white checkers, followed by the
 * black kings, and finally the white kings.
 *
 * The following "|" notation refers to "choose";
 *	i.e. | n | = n!/((n-k)!k!).
 *		 | k |
 *
 * The number of positions with nbm black checkers with leading rank rbp
 * (where 0 <= rbp <= 7) is MaxBP = | 4 x (rbp+1) | - | 4 x rbp |.
 *									|	  nbm	  |   |  nbm	|
 *
 * Next the white checkers are added.  This is discussed in more detail below.
 *
 * The number of positions created by adding nbk black kings is
 *		MaxBK = | 32 - nbm - nwm |
 *				|	   nbk		 |
 *
 * and the number of positions created by adding nwk white kings is
 *		MaxWK = | 32 - nbm - nwm - nbk |.
 *				|		  nwk		   |
 *
 *
 * Computing the number of positions with nbm black checkers having leading
 * rank rbp and nwm white checkers having leading rank rwp is difficult because
 * some of the black checker configurations may have one or more checkers
 * within the first rwp checker squares.  Thus the simple formula applied to
 * computing MaxBP cannot be applied to MaxWP because different black checker
 * configurations may have a different number of white checker configurations.
 * MaxWP is computed by creating a set of subindexes for every black checker
 * configuration.  The number of white checker positions for each black checker
 * configuration is computed and stored, as well as a cumulative total.  The
 * amount of storage required can be quite large for a large number of black
 * checkers.
 *
 * See Appendix B in the Technical Report TR93-13 for more details.
 */
static int *create_bm_indices(int nbm, int nwm, int bmrank, int wmrank,
					   int *num_base_indices)
{
	int base;			/* first man index of this black checker configuration. */
	int num_man_indices;
	int i;
	int nsqr1, nsqr2;
	int *table;
	int board_squares[MAX_PIECES];

	if (nbm)
		*num_base_indices = choose((bmrank + 1) * ROWSIZE, nbm) - choose(bmrank * ROWSIZE, nbm);
	else
		*num_base_indices = 1;

	/*
	 * Allocate the secondary index array.	This array holds the
	 * cumulative number of white checker positions for each black
	 * checker configuration.
	 */
	table = (int *)malloc((*num_base_indices) * sizeof(int));
	if (table == NULL)
		return(NULL);		/* the caller reports the error. */
	memset(table, 0, (*num_base_indices) * sizeof(int));

	/* initialize the array with FIRST & BASE */
	base = 0;
	if (nbm) {
		init_bm_config(board_squares, nbm, std::max(bmrank * ROWSIZE, nbm - 1));
	}

	/* For each black checker configuration... */
	for (i = 0; i < *num_base_indices; ++i) {
		if (nwm) {
			/* Compute available squares for white checkers */
			if (nbm) {

				/* nsqr1 is the number of non leading rank squares available
				 * for white men.
				 */
				nsqr1 = Nsq(wmrank - 1, board_squares, nbm);

				/*
				 * nsqr2 is the total number of squares available for white men.
				 */
				nsqr2 = Nsq(wmrank, board_squares, nbm);
			}
			else {
				nsqr1 = wmrank * ROWSIZE;
				nsqr2 = (wmrank + 1) * ROWSIZE;
			}

			/* Calculate the number of checker configurations for this
			 * particular arrangement of black men.
			 */
			num_man_indices = choose(nsqr2, nwm) - choose(nsqr1, nwm);
		}
		else
			num_man_indices = 1;

		/* store these values for future reference */
		table[i] = base;

		/*
		 * Add the number of checker positions to the cumulative
		 * amount
		 */
		base += num_man_indices;

		/* Get the next black checker configuration */
		if (nbm)
			next_bm_config(board_squares, nbm);
	}
	return(table);
}


static int egdb_close_chinook(EGDB_DRIVER *handle)
{
	CHINOOK_DATA *hdat = (CHINOOK_DATA *)handle->internal_data;
	int i, j, size;
	int bm, wm, bk, wk;
	int n;
	DBENTRY *p;

	/* Free the cache buffers in groups of CACHEBLOCK_ALLOC_COUNT at a time. */
	if (hdat->ccbs) {
		for (i = 0; i < hdat->num_cache_buffers; i += CACHEBLOCK_ALLOC_COUNT) {
			if (hdat->ccbs[i].data)
				VirtualFree(hdat->ccbs[i].data, 0, MEM_RELEASE);
		}

		/* Free the cache control blocks. */
		free(hdat->ccbs);
	}

	if (hdat->dbfiles) {
		for (i = 0; i < hdat->numdbfiles; ++i) {
			if (hdat->dbfiles[i].pieces > hdat->maxpieces)
				continue;

#if OMIT_LOPSIZED_SLICES
			if (hdat->dbfiles[i].lopsided)
				continue;
#endif
			if (hdat->dbfiles[i].indices)
				free(hdat->dbfiles[i].indices);

			if (hdat->dbfiles[i].autoload) {
				VirtualFree(hdat->dbfiles[i].autoload, 0, MEM_RELEASE);
				hdat->dbfiles[i].autoload = 0;
			}
			else {
				if (hdat->dbfiles[i].cache_bufferi)
					free(hdat->dbfiles[i].cache_bufferi);
				if (hdat->dbfiles[i].fp != INVALID_HANDLE_VALUE) {
					CloseHandle(hdat->dbfiles[i].fp);
					hdat->dbfiles[i].fp = INVALID_HANDLE_VALUE;
				}
			}
		}
	}

	for (i = 0; i < DBTABLESIZE; i++) {
		if (hdat->DBTable[i]) {
			n = i;
			bk = n / (MAX_PIECES * MAX_PIECES * MAX_PIECES);
			n -= bk * (MAX_PIECES * MAX_PIECES * MAX_PIECES);
			wk = n / (MAX_PIECES * MAX_PIECES);
			n -= wk * (MAX_PIECES * MAX_PIECES);
			bm = n / MAX_PIECES;
			n -= bm * MAX_PIECES;
			wm = n;

			if (i != DBINDEX(bk, wk, bm, wm))
				break;	/* shouldn't happen. */

			p = hdat->DBTable[i];
			if (p == NULL)
				break;	/* this shouldn't happen either. */

			if (bm == 0 && wm == 0)
				size = 1;
			else if (bm == 0 || wm == 0)
				size = 7;
			else
				size = 49;

			for (j = 0; j < size; ++j) {
				if (p[j].index_data) {
					free(p[j].index_data);
				}
			}
			free(p);
			hdat->DBTable[i] = 0;
		}
	}

	/* Free the ci_base_table entries. */
	if (hdat->ci_base_table) {
		for (i = 0; i < CI_TABLESIZE; ++i)
			if (hdat->ci_base_table[i])
				free(hdat->ci_base_table[i]);

		/* Free the ci_base_table. */
		free(hdat->ci_base_table);
	}

	if (hdat->dbfiles)
		free(hdat->dbfiles);
#if USE_RUNLENGTH16
	free(hdat->runlength16_chinook);
#endif
	free(hdat);
	free(handle);

	return(0);
}


static int verify_crc(EGDB_DRIVER *handle)
{
	int i;
	int status;
	unsigned int crc;
	const DBCRC *pcrc;
	FILE *fp;
	char filename[MAXFILENAME];
	char msg[MAXMSG];
	CHINOOK_DATA *hdat = (CHINOOK_DATA *)handle->internal_data;

	status = 0;
	for (i = 0; i < hdat->numdbfiles; ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;

		if (!hdat->dbfiles[i].autoload && hdat->dbfiles[i].fp == INVALID_HANDLE_VALUE)
			continue;

		/* Build the index filename. */
		sprintf(filename, "%s.idx", hdat->dbfiles[i].filename);
		if (hdat->db_type == EGDB_CHINOOK_WLD)
			pcrc = find_file_crc(filename, dbcrc, ARRAY_SIZE(dbcrc));
		else
			pcrc = find_file_crc(filename, dbcrc_italian, ARRAY_SIZE(dbcrc_italian));
		if (!pcrc) {
			sprintf(msg, "No crc value known for file %s\n", filename);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}
		sprintf(filename, "%s%s.idx", hdat->db_filepath, hdat->dbfiles[i].filename);
		fp = fopen(filename, "rb");
		if (!fp) {
			sprintf(msg, "Cannot open file %s for crc check.\n", filename);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}

		sprintf(msg, "checking crc %s\n", filename);
		(*hdat->log_msg_fn)(msg);

		crc = file_crc_calc(fp);
		fclose(fp);
		if (crc != pcrc->crc) {
			sprintf(msg, "file %s failed crc check, expected %08x, got %08x\n",
				filename, pcrc->crc, crc);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}

		/* Build the data filename. */
		sprintf(filename, "%s", hdat->dbfiles[i].filename);
		if (hdat->db_type == EGDB_CHINOOK_WLD)
			pcrc = find_file_crc(filename, dbcrc, ARRAY_SIZE(dbcrc));
		else
			pcrc = find_file_crc(filename, dbcrc_italian, ARRAY_SIZE(dbcrc_italian));
		if (!pcrc) {
			sprintf(msg, "No crc value known for file %s\n", filename);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}
		sprintf(filename, "%s%s", hdat->db_filepath, hdat->dbfiles[i].filename);
		fp = fopen(filename, "rb");
		if (!fp) {
			sprintf(msg, "Cannot open file %s for crc check.\n", filename);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}

		sprintf(msg, "checking crc %s\n", filename);
		(*hdat->log_msg_fn)(msg);

		crc = file_crc_calc(fp);
		fclose(fp);
		if (crc != pcrc->crc) {
			sprintf(msg, "file %s failed crc check, expected %08x, got %08x\n",
				filename, pcrc->crc, crc);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}
	}
	return(status);
}


static int kr_lookup(EGDB_DRIVER *handle, EGDB_BITBOARD *krpos, int color, int cl)
{
	int result;
	EGDB_ROW_REVERSED_BITBOARD dbpos;

	/* Convert to the chinook/cake endgame board bitboard. */
	reverse_bits_in_row(dbpos, *(EGDB_NORMAL_BITBOARD *)krpos);

	result = dblookup(handle, &dbpos, color, cl);
	return(result);
}


EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type)
{
	int status;
	EGDB_DRIVER *handle;

	handle = (EGDB_DRIVER *)calloc(1, sizeof(EGDB_DRIVER));
	if (!handle) {
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
	handle->internal_data = calloc(1, sizeof(CHINOOK_DATA));
	if (!handle->internal_data) {
		free(handle);
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
	((CHINOOK_DATA *)(handle->internal_data))->db_type = db_type;
	status = initdblookup((CHINOOK_DATA *)handle->internal_data, pieces, cache_mb, directory, msg_fn);
	if (status) {
		egdb_close_chinook(handle);
		return(0);
	}
	if (bitboard_type == EGDB_NORMAL) {

		/* This is a kingsrow bitboard. */
		handle->lookup = kr_lookup;
	}
	else
		handle->lookup = (REV_ROW_LOOKUP_FNPTR)dblookup;
	
	handle->get_stats = get_db_stats;
	handle->reset_stats = reset_db_stats;
	handle->verify = verify_crc;
	handle->close = egdb_close_chinook;
	return(handle);
}

}	// namespace