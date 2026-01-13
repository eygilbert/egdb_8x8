#define NOMINMAX	/* exlude min()/max() macros. */
#include <utility>
#include "egdb_common.h"
#include "egdb_internal.h"
#include "rle_tables.h"
#include "bitboard_convert.h"
#include "format_with_commas.h"
#include "crc.h"
#include "SpinLock.h"
#include "indexing.h"


#define MAXPIECES 8
#define MAXPIECE 4

#define USE_RUNLENGTH16 1

#define SAME_PIECES_ONE_FILE 7

#define IDX_BLOCKSIZE 1024
#define IDX_BLOCKS_PER_CACHE_BLOCK 4
#define CACHE_BLOCKSIZE (IDX_BLOCKSIZE * IDX_BLOCKS_PER_CACHE_BLOCK)

#define MAXMSG 256
#define MAXLINE 256
#define MAXFILES 81

#define RANKMAX 6

/* Allocate cache buffers CACHE_ALLOC_COUNT at a time. */
#define CACHE_ALLOC_COUNT 256

/* If he didn't give us enough memory for at least MIN_CACHE_BUF_BYTES of
 * buffers, then use that much.
 */
#define MIN_CACHE_BUF_BYTES (10 * ONE_MB)

/* Use this macro to do the equivalent of multi-dimension array indexing.
 * We need to do this manually because we are dynamically
 * allocating the database table.
 */
#define DBOFFSET(bm, bk, wm, wk, bmrank, wmrank, color) \
	((color) + \
	 ((wmrank) * NUMBEROFCOLORS) + \
	 ((bmrank) * NUMBEROFCOLORS * (RANKMAX + 1)) + \
	 ((wk) * NUMBEROFCOLORS * (RANKMAX + 1) * (RANKMAX + 1)) + \
	 ((wm) * NUMBEROFCOLORS * (RANKMAX + 1) * (RANKMAX + 1) * (MAXPIECE + 1)) + \
	 ((bk) * NUMBEROFCOLORS * (RANKMAX + 1) * (RANKMAX + 1) * (MAXPIECE + 1) * (MAXPIECE + 1)) + \
	 ((bm) * NUMBEROFCOLORS * (RANKMAX + 1) * (RANKMAX + 1) * (MAXPIECE + 1) * (MAXPIECE + 1) * (MAXPIECE + 1)))

/* The number of pointers in the cprsubdatabase array. */
#define DBSIZE ((MAXPIECE + 1) * (MAXPIECE + 1) * (MAXPIECE + 1) * \
			(MAXPIECE + 1) * (RANKMAX + 1) * (RANKMAX + 1) * NUMBEROFCOLORS)

namespace cake_wld {

typedef unsigned int INDEX;

struct DBFILE {
	char name[20];			/* db filename prefix. */
	int pieces;				/* number of pieces in this db file. */
	int num_idx_blocks;		/* number of index blocks in this db file. */
	int num_datablocks;		/* number of cacheblock-sized data blocks in this db file. */
	unsigned char *file_cache;/* if not null the whole db file is here. */
	HANDLE fp;
	int *cache_bufferi;		/* An array of indices into ccbs[], indexed by data block number. */
};

// definition of a structure for compressed databases
struct CPRSUBDB {
	char value;				// WIN/LOSS/DRAW if single value, UNKNOWN == 0 else
	short int startbyte;	// at this byte
	int num_idx_blocks;		// how many index blocks does this db need?
	int first_idx_block;	// where is the first block in it's db?
	INDEX *indices;			// array of first index number in each index block 
							// allocated dynamically, to size n
	DBFILE *file;			// file info for this subdb
};

/* Cache control block type. */
struct CCB {
	int next;				/* index of next node */
	int prev;				/* index of previous node */
	int blocknum;			/* the block number within database file. */
	DBFILE *dbfile;			/* which database file the block is for. */
	unsigned char *data;	/* data of this block. */
};

struct CAKE_DATA {
	char db_filepath[MAXFILENAME];	/* Path to database files. */
	int maxpieces;
	int cacheblocks;				/* Total number of db cache blocks. */
	int autoload_pieces;			/* permanently load slices with <= this many pieces. */
	void (*log_msg_fn)(const char *);	/* for status and error messages. */
	CPRSUBDB **cprsubdatabase;
	CCB *ccbs;
	int ccbs_top;						/* index into ccbs[] of least recently used block. */
	int numdbfiles;
#if SMP
	SPINLOCK lock;
#endif
	DBFILE dbfiles[MAXFILES];
	EGDB_STATS lookup_stats;
};

/* A table of crc values for each database file. */
static DBCRC dbcrc[] = {
	{"db2.cpr", 0xadb68b06},
	{"db2.idx", 0x06139b08},
	{"db3.cpr", 0xb51fe30b},
	{"db3.idx", 0xfbdabd1a},
	{"db4.cpr", 0x9d87eb5c},
	{"db4.idx", 0x1d4d4e66},
	{"db5.cpr", 0xbff8cc4e},
	{"db5.idx", 0x9fb18a70},
	{"db6.cpr", 0x1dc1cfa6},
	{"db6.idx", 0x28a4a819},
	{"db7.cpr", 0xf9ba2e4e},
	{"db7.idx", 0xa82bf62d},
	{"db8_0404.cpr", 0xd8cffde7},
	{"db8_0404.idx", 0x597d7db4},
	{"db8_0413.cpr", 0x04986ae0},
	{"db8_0413.idx", 0xed64a249},
	{"db8_0422.cpr", 0xc4730d19},
	{"db8_0422.idx", 0x76ea19cd},
	{"db8_0431.cpr", 0xaf6b70ba},
	{"db8_0431.idx", 0x7b38a40d},
	{"db8_0440.cpr", 0x66204009},
	{"db8_0440.idx", 0xf921a9b5},
	{"db8_1313.cpr", 0x74d8a4b8},
	{"db8_1313.idx", 0xa549df6a},
	{"db8_1322.cpr", 0x5025b382},
	{"db8_1322.idx", 0x33e92395},
	{"db8_1331.cpr", 0x8a31c086},
	{"db8_1331.idx", 0xcca718bc},
	{"db8_1340.cpr", 0xd42289be},
	{"db8_1340.idx", 0x0b9b4bc4},
	{"db8_2222.cpr", 0x247dfcf3},
	{"db8_2222.idx", 0x1900530c},
	{"db8_2231.cpr", 0x51e02cce},
	{"db8_2231.idx", 0x4416ed3a},
	{"db8_2240.cpr", 0x04349c97},
	{"db8_2240.idx", 0xfe23c572},
	{"db8_3131.cpr", 0x284dad0d},
	{"db8_3131.idx", 0x6df1d300},
	{"db8_3140.cpr", 0xfa7a3a67},
	{"db8_3140.idx", 0x271a0728},
	{"db8_4040.cpr", 0x96aca6aa},
	{"db8_4040.idx", 0xb9029ca5},
};


/* Function prototypes. */
int parseindexfile(CAKE_DATA *hdat, DBFILE *, int64_t *allocated_bytes);
void build_file_table(CAKE_DATA *hdat);


static void reset_db_stats(EGDB_DRIVER *handle)
{
	CAKE_DATA *hdat = (CAKE_DATA *)handle->internal_data;

	memset(&hdat->lookup_stats, 0, sizeof(hdat->lookup_stats));
}


static EGDB_STATS *get_db_stats(EGDB_DRIVER *handle)
{
	CAKE_DATA *hdat = (CAKE_DATA *)handle->internal_data;

	return(&hdat->lookup_stats);
}


INDEX position2index(EGDB_ROW_REVERSED_BITBOARD *pos, int nbm, int nbk, int nwm, int nwk, int bmrank, int wmrank)
{
	uint32_t bmindex, bkindex, wmindex, wkindex;
	uint32_t bmrange, wmrange, bkrange;
	INDEX index;

	/* Set the index of black men. */
	bmindex = bitboard2index(pos->black_man, 0);

	/* Set the index of white men, disregarding black men. */
	wmindex = bitboard2index_rev(pos->white_man, 0, 0);

	/* Set the index of black kings, including interfering black and white men. */
	bkindex = bitboard2index(pos->black_king, 0, pos->black_man | pos->white_man);

	/* Set the index of white kings, with interfering other pieces. */
	wkindex = bitboard2index(pos->white_king, 0, pos->black_man | pos->black_king | pos->white_man);

	bmrange = 1;
	if (nbm)
		bmrange = choose(ROWSIZE * (bmrank + 1), nbm) - choose(ROWSIZE * bmrank, nbm);

	wmrange = 1;
	if (nwm)
		wmrange = choose(ROWSIZE * (wmrank + 1), nwm) - choose(ROWSIZE * wmrank, nwm);

	bkrange = choose(NUMSQUARES - nbm - nwm, nbk);

	if (bmrank)
		bmindex -= choose(ROWSIZE * bmrank, nbm);
	if (wmrank)
		wmindex -= choose(ROWSIZE * wmrank, nwm);

	index = bmindex + 
			(INDEX)wmindex * (INDEX)bmrange + 
			(INDEX)bkindex * (INDEX)bmrange * (INDEX)wmrange +
			(INDEX)wkindex * (INDEX)bmrange * (INDEX)wmrange * (INDEX)bkrange;
	return(index);
}


/*
 * Returns DB_WIN, DB_LOSS, DB_DRAW, DB_UNKNOWN, or DB_NOT_IN_CACHE
 * for a position in a database lookup from a compressed database. 
 * First this function computes the index of the current position. 
 * Then it loads the index file of that database to determine in which
 * 1K-block that index resides. 
 * Finally it reads and decompresses that block to find
 * the value of the position.
 * If the value is not already in a cache buffer, the action depends on
 * the argument cl.  If cl is true, DB_NOT_IN_CACHE is returned, 
 * otherwise the disk block is read and cached and the value is obtained.
 */
static int dblookup(EGDB_DRIVER *handle, EGDB_ROW_REVERSED_BITBOARD *pos, int color, int cl)
{
	CAKE_DATA *hdat = (CAKE_DATA *)handle->internal_data;
	INDEX index;
	int bm, bk, wm, wk, bmrank, wmrank;
	int i;
	int idx_blocknum;	
	int dbfile_blocknum;
	int returnvalue;
	unsigned char *diskblock;
#if USE_RUNLENGTH16
	unsigned short *diskblock16;
#endif
	INDEX n_idx;
	INDEX *indices;
	CPRSUBDB *dbpointer;

	/* Set bm, bk, wm, wk, and ranks. */
	bm = bitcount(pos->black_man);
	bmrank = 0;
	if (bm)
		bmrank = MSB(pos->black_man) / 4;
	bk = bitcount(pos->black_king);
	wm = bitcount(pos->white_man);
	wmrank = 0;
	if (wm)
		wmrank = (31 - LSB(pos->white_man)) / 4;	
	wk = bitcount(pos->white_king);
	
	/* Start tracking db stats here. */
	++hdat->lookup_stats.db_requests;

	/* If one side has nothing, return appropriate value. */
	if ((bm + bk) == 0) {
		++hdat->lookup_stats.db_returns;
		return(color == EGDB_BLACK ? EGDB_LOSS : EGDB_WIN);
	}
	if ((wm + wk) == 0) {
		++hdat->lookup_stats.db_returns;
		return(color == EGDB_WHITE ? EGDB_LOSS : EGDB_WIN);
	}

	if ((bm + wm + wk + bk > MAXPIECES) || (bm + bk > MAXPIECE) || (wm + wk > MAXPIECE)) {
		++hdat->lookup_stats.db_not_present_requests;
		return EGDB_UNKNOWN;
	}

	/* If white is dominating this position, change colors. */
	if (needs_reversal(bm, bk, wm, wk, bmrank, wmrank, color)) {
		EGDB_ROW_REVERSED_BITBOARD revpos;
		reverse(&revpos, pos);
		pos = &revpos;
		color = OTHER_COLOR(color);
		using std::swap;
		swap(bm, wm);
		swap(bk, wk);
		swap(bmrank, wmrank);
	}

	/* get pointer to db. */
	dbpointer = hdat->cprsubdatabase[DBOFFSET(bm, bk, wm, wk, bmrank, wmrank, color)];

	/* check presence. */
	if (dbpointer == 0) {
		++hdat->lookup_stats.db_not_present_requests;
		return(EGDB_UNKNOWN);
	}

	/* check if the db contains only a single value. */
	if (dbpointer->value != EGDB_UNKNOWN) {
		++hdat->lookup_stats.db_returns;
		return(dbpointer->value);
	}

	index = position2index(pos, bm, bk, wm, wk, bmrank, wmrank);

	/* We know the index and the database, so look in 
	 * the indices array to find the right index block.
	 */
	indices = dbpointer->indices;
	idx_blocknum = find_block(0, dbpointer->num_idx_blocks, indices, index);

	/* See if this is an autoloaded block. */
	if (dbpointer->file->file_cache) {
		++hdat->lookup_stats.autoload_hits;
		diskblock = dbpointer->file->file_cache + IDX_BLOCKSIZE *
							(SIZE_T)(dbpointer->first_idx_block + idx_blocknum);
	}
	else {
		int ccbi;
		CCB *ccbp;

		/* See if blocknumber is already in cache. */
		dbfile_blocknum = (dbpointer->first_idx_block + idx_blocknum) / IDX_BLOCKS_PER_CACHE_BLOCK;

		/* Is this block already cached? */
#if SMP
		AcquireSpinLock(hdat->lock);
#endif
		ccbi = dbpointer->file->cache_bufferi[dbfile_blocknum];
		if (ccbi != UNDEFINED_BLOCK_ID) {

			/* Already cached.  Update the lru list. */
			ccbp = update_lru<CCB>(hdat, ccbi);
		}
		else {

			/* we must load it.
			 * if the lookup was a "conditional lookup", we don't load the block.
			 */
			if (cl) {
#if SMP
				ReleaseSpinLock(hdat->lock);
#endif
				return(EGDB_NOT_IN_CACHE);
			}

			/* If necessary load this block from disk, update lru list. */
			ccbp = load_blocknum_v2<CCB>(hdat, dbpointer->file, dbfile_blocknum, CACHE_BLOCKSIZE);
		}

		diskblock = ccbp->data + ((dbpointer->first_idx_block + idx_blocknum) %
						IDX_BLOCKS_PER_CACHE_BLOCK) * IDX_BLOCKSIZE;
#if SMP
		ReleaseSpinLock(hdat->lock);
#endif
	}

	/* The block we were looking for is now pointed to by diskblock.
	 * now we decompress the memory block
	 * Do a linear search for the byte within the block.
	 * Search from the end backwards if closer to the end.
	 */
	int reverse = 0;
	if (dbpointer->num_idx_blocks > idx_blocknum + 1) {

		/* If we are not at the last block. */
		if (indices[idx_blocknum + 1] - index < index - indices[idx_blocknum])
#if USE_RUNLENGTH16
			/* If we are not at the first index block with an odd startbyte. */
			if (idx_blocknum > 0 || !(dbpointer->startbyte & 1))
#endif
				reverse = 1;
	}
#if USE_RUNLENGTH16
	diskblock16 = (unsigned short *)diskblock;
	if (reverse) {
		n_idx = indices[idx_blocknum + 1];
		i = rle_compr::find_compr_val_rev(rle_compr::runlength16, diskblock16, IDX_BLOCKSIZE / 2, index, n_idx);
	}
	else {
		n_idx = indices[idx_blocknum];

		/* and move through the bytes until we overshoot index
		 * set the start byte in this block: if it's block 0 of a db,
		 * this can be !=0.
		 */
		i = 0;
		if (idx_blocknum == 0) {
			i = dbpointer->startbyte;
			if (i & 1)
				n_idx += rle_compr::runlength[diskblock[i]];
			i = (dbpointer->startbyte + 1) / 2;
		}
		i = rle_compr::find_compr_val_fwd(rle_compr::runlength16, diskblock16, i, index, n_idx);

	}

	/* Change i from a 16-bit index to a byte index. */
	i *= 2;

	/* Now see if we are 1 byte off. */
	if (n_idx + rle_compr::runlength[diskblock[i]] <= index) {
		n_idx += rle_compr::runlength[diskblock[i]];
		++i;
	}
#else
	if (reverse) {
		n_idx = indices[idx_blocknum + 1];
		i = rle_compr::find_compr_val_rev(rle_compr::runlength, diskblock, IDX_BLOCKSIZE, index, n_idx);
	}
	else {
		n_idx = indices[idx_blocknum];
		i = 0;
		if (idx_blocknum == 0)
			i = dbpointer->startbyte;
		i = rle_compr::find_compr_val_fwd(rle_compr::runlength, diskblock, i, index, n_idx);
	}
#endif

	/* Do some simple error checking. */
	if (i < 0 || i >= IDX_BLOCKSIZE) {
		char msg[MAXMSG];

		sprintf(msg, "db block array index outside block bounds: %d\n", i);
		(*hdat->log_msg_fn)(msg);
		return EGDB_UNKNOWN;
	}
	
	/* The target position is in diskblock[i]. */
	returnvalue = rle_compr::decode_compr_wld(diskblock[i], (int)(index - n_idx));
	++hdat->lookup_stats.db_returns;
	return(returnvalue);
}


/*
 * Open the endgame db driver for the cake database.
 * 
 * pieces is the maximum number of pieces to do lookups for.
 * cache_mb is the amount of ram to use for the driver, in bytes * 1E6.
 * filepath is the path to the database files.
 * msg_fn is a pointer to a function which will log status and error messages.
 * A non-zero return value means some kind of error occurred.  The nature of
 * any errors are communicated through the msg_fn.
 */
static int initdblookup(CAKE_DATA *hdat, int pieces, int cache_mb, const char *filepath, void (*msg_fn)(const char *))
{
	int i, j;
	int t0, t1, t2, t3;
	char dbname[MAXFILENAME];
	char msg[MAXMSG];
	int64_t allocated_bytes;		/* keep track of heap allocations in bytes. */
	int64_t autoload_bytes;			/* keep track of autoload allocations in bytes. */
	SIZE_T size;
	int count;
	unsigned char *blockp;		/* Base address of an allocate group of cache buffers. */

	t0 = GetTickCount();

	/* Save off some global data. */
	strcpy(hdat->db_filepath, filepath);

	/* Add trailing backslash if needed. */
	size = (int)strlen(hdat->db_filepath);
	if (size && (hdat->db_filepath[size - 1] != '\\'))
		strcat(hdat->db_filepath, "\\");

	hdat->maxpieces = pieces;
	hdat->log_msg_fn = msg_fn;
	allocated_bytes = 0;
	autoload_bytes = 0;

	std::sprintf(msg, "Available RAM: %smb\n", format_with_commas(get_mem_available_mb()).c_str());
	(*hdat->log_msg_fn)(msg);

	/* Decide how much db to autoload. */
	hdat->autoload_pieces = 0;
	if (pieces == 8) {
		if (cache_mb >= 900)
			hdat->autoload_pieces = 7;
		else if (cache_mb >= 120)
			hdat->autoload_pieces = 6;
		else if (cache_mb >= 10)
			hdat->autoload_pieces = 5;
	}
	else if (pieces == 7) {
		if (cache_mb >= 370)
			hdat->autoload_pieces = 7;
		else if (cache_mb >= 110)
			hdat->autoload_pieces = 6;
		else if (cache_mb >= 10)
			hdat->autoload_pieces = 5;
	}
	else if (pieces == 6) {
		if (cache_mb >= 40)
			hdat->autoload_pieces = 6;
		else if (cache_mb >= 10)
			hdat->autoload_pieces = 5;
	}
	else
		if (cache_mb >= 3)
			hdat->autoload_pieces = pieces;

	/* Build table of db filenames. */
	build_file_table(hdat);

	/* initialize binomial coefficients. */
	initbicoef();

	/* initialize runlength and lookup array. */
	rle_compr::init_compression_tables();

#if SMP
	ResetSpinLock(hdat->lock);
#endif

	/* Allocate the cprsubdatabase array. */
	hdat->cprsubdatabase = (CPRSUBDB **)calloc(DBSIZE, sizeof(CPRSUBDB *));
	if (!hdat->cprsubdatabase) {
		(*hdat->log_msg_fn)("Out of memory allocating cprsubdatabase.\n");
		return(1);
	}
	allocated_bytes += DBSIZE * sizeof(CPRSUBDB *);

	/* Parse index files. */
	for (i = 0; i < hdat->numdbfiles; ++i) {
		int stat;

		/* Dont do more pieces than he asked for. */
		if (hdat->dbfiles[i].pieces > pieces)
			break;

		stat = parseindexfile(hdat, hdat->dbfiles + i, &allocated_bytes);
		if (stat)
			return(1);

		/* Calculate the number of cache blocks. */
		hdat->dbfiles[i].num_datablocks = hdat->dbfiles[i].num_idx_blocks / IDX_BLOCKS_PER_CACHE_BLOCK;
		if (hdat->dbfiles[i].num_idx_blocks > hdat->dbfiles[i].num_datablocks * IDX_BLOCKS_PER_CACHE_BLOCK)
			++hdat->dbfiles[i].num_datablocks;
	}

	t1 = GetTickCount();
	sprintf(msg, "Reading index files took %d secs\n", (t1 - t0) / 1000);
	(*hdat->log_msg_fn)(msg);

	/* Open file pointers for each db: keep open all the time. */
	for (i = 0; i < hdat->numdbfiles; ++i) {

		/* Dont do more pieces than he asked for. */
		if (hdat->dbfiles[i].pieces > pieces)
			break;

		sprintf(dbname, "%s%s.cpr", hdat->db_filepath,hdat-> dbfiles[i].name);
		hdat->dbfiles[i].fp = CreateFile(dbname, GENERIC_READ, FILE_SHARE_READ,
						NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
		if (hdat->dbfiles[i].fp == INVALID_HANDLE_VALUE) {
			sprintf(msg, "Cannot open %s\n", dbname);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		/* Allocate buffers and read files for autoloaded dbs. */
		if (hdat->dbfiles[i].pieces <= hdat->autoload_pieces) {
			size = hdat->dbfiles[i].num_datablocks * (SIZE_T)CACHE_BLOCKSIZE;
			hdat->dbfiles[i].file_cache = (unsigned char *)VirtualAlloc(0, size, MEM_COMMIT, PAGE_READWRITE);
			allocated_bytes += size;
			autoload_bytes += size;
			if (hdat->dbfiles[i].file_cache == NULL) {
				(*hdat->log_msg_fn)("Cannot allocate memory for autoload array\n");
				return(1);
			}

			/* Seek to the beginning of the file. */
			if (SetFilePointer(hdat->dbfiles[i].fp, 0, NULL, FILE_BEGIN) == -1) {
				(*hdat->log_msg_fn)("ERROR: autoload seek failed\n");
				return(-1);
			}

			read_file(hdat->dbfiles[i].fp, hdat->dbfiles[i].file_cache, size, get_page_size());

			/* Close the db file, we are done with it. */
			CloseHandle(hdat->dbfiles[i].fp);
			hdat->dbfiles[i].fp = INVALID_HANDLE_VALUE;
		}
		else {
			/* These slices are not autoloaded.
			 * Allocate the array of indices into ccbs[].
			 * Each array entry is either an index into ccbs or -1 if that block is not loaded.
			 */
			size = hdat->dbfiles[i].num_datablocks * sizeof(hdat->dbfiles[i].cache_bufferi[0]);
			hdat->dbfiles[i].cache_bufferi = (int *)malloc(size);
			allocated_bytes += size;
			if (hdat->dbfiles[i].cache_bufferi == NULL) {
				(*hdat->log_msg_fn)("Cannot allocate memory for cache_bufferi array\n");
				return(-1);
			}

			/* Init these buffer indices to UNDEFINED_BLOCK_ID, means that cache block is not loaded. */
			for (j = 0; j < hdat->dbfiles[i].num_datablocks; ++j)
				hdat->dbfiles[i].cache_bufferi[j] = UNDEFINED_BLOCK_ID;
		}
	}

	sprintf(msg, "Allocated %dkb for indexing\n", (int)((allocated_bytes - autoload_bytes) / 1024));
	(*hdat->log_msg_fn)(msg);
	sprintf(msg, "Allocated %dkb for permanent slice caches\n", (int)(autoload_bytes / 1024));
	(*hdat->log_msg_fn)(msg);

	t2 = GetTickCount();
	sprintf(msg, "Autoload took %d secs\n", (t2 - t1) / 1000);
	(*hdat->log_msg_fn)(msg);

	/* Figure out how much ram is left for lru cache buffers.
	 * If we autoloaded everything then no need for lru cache buffers.
	 */
	if (hdat->autoload_pieces >= pieces)
		hdat->cacheblocks = 0;
	else {

		int needed = needed_cache_buffers<CAKE_DATA>(hdat);
		if (needed > 0) {
			if ((allocated_bytes + MIN_CACHE_BUF_BYTES) / ONE_MB >= cache_mb) {

				/* We need more memory than he gave us, allocate 10mb of cache
				 * buffers if we can use that many.
				 */
				hdat->cacheblocks = std::min(MIN_CACHE_BUF_BYTES / CACHE_BLOCKSIZE, needed);
				sprintf(msg, "Allocating the minimum %d cache buffers\n",
								hdat->cacheblocks);
				(*hdat->log_msg_fn)(msg);
			}
			else
				hdat->cacheblocks = (int)(((int64_t)cache_mb * (int64_t)ONE_MB - allocated_bytes) / 
						(int64_t)(CACHE_BLOCKSIZE + sizeof(CCB)));
		}
		else
			hdat->cacheblocks = 0;		/* Should never get here. */

		/* Allocate the CCB array. */
		if (hdat->cacheblocks) {
			hdat->ccbs = (CCB *)malloc(hdat->cacheblocks * sizeof(CCB));
			if (!hdat->ccbs) {
				(*hdat->log_msg_fn)("Cannot allocate memory for ccbs\n");
				return(1);
			}

			/* Clear so we know which ones got used if we have to bail early. */
			memset(hdat->ccbs, 0, hdat->cacheblocks * sizeof(CCB));

			/* Init the lru list. */
			for (i = 0; i < hdat->cacheblocks; ++i) {
				hdat->ccbs[i].next = i + 1;
				hdat->ccbs[i].prev = i - 1;
				hdat->ccbs[i].blocknum = UNDEFINED_BLOCK_ID;
			}
			hdat->ccbs[hdat->cacheblocks - 1].next = 0;
			hdat->ccbs[0].prev = hdat->cacheblocks - 1;
			hdat->ccbs_top = 0;

			if (hdat->cacheblocks > 0) {
				sprintf(msg, "Allocating %d cache buffers of size %d\n",
							hdat->cacheblocks, CACHE_BLOCKSIZE);
				(*hdat->log_msg_fn)(msg);
			}

			/* Allocate the cache buffers in groups of CACHE_ALLOC_COUNT at a time. */
			for (i = 0; i < hdat->cacheblocks; i += CACHE_ALLOC_COUNT) {
				count = std::min(CACHE_ALLOC_COUNT, hdat->cacheblocks - i);
				size = count * CACHE_BLOCKSIZE * sizeof(unsigned char);
				blockp = (unsigned char *)VirtualAlloc(0, size, MEM_COMMIT, PAGE_READWRITE);
				if (blockp == NULL) {
					(*hdat->log_msg_fn)("ERROR: VirtualAlloc failure on cache buffers\n");
					return(-1);
				}

				/* Assign the blockinfo[] data pointers. */
				for (j = 0; j < count; ++j)
					hdat->ccbs[i + j].data = blockp + j * CACHE_BLOCKSIZE * sizeof(unsigned char);
			}

			/* Preload the cache blocks with data. */
			t0 = GetTickCount();
			for (i = 0, count = 0; i < hdat->numdbfiles; ++i) {
				if (hdat->dbfiles[i].pieces <= hdat->autoload_pieces)
					continue;

				for (j = 0; j < hdat->dbfiles[i].num_datablocks && count < hdat->cacheblocks; ++count, ++j) {
					load_blocknum_v2<CCB>(hdat, hdat->dbfiles + i, j, CACHE_BLOCKSIZE);
				}
				if (count >= hdat->cacheblocks)
					break;
			}
		}
		t3 = GetTickCount();
		sprintf(msg, "Read %d buffers in %d sec, %.3f msec/buffer\n", 
					hdat->cacheblocks, (t3 - t2) / 1000, 
					(double)(t3 - t2) / (double)hdat->cacheblocks);
		(*hdat->log_msg_fn)(msg);
		sprintf(msg, "Egdb init took %d sec total\n", (t3 - t0) / 1000);
		(*hdat->log_msg_fn)(msg);
	}

	std::sprintf(msg, "Available RAM: %smb\n", format_with_commas(get_mem_available_mb()).c_str());
	(*hdat->log_msg_fn)(msg);

	return(0);
}


/*
 * Parse an index file and write all information in cprsubdatabase[].
 * A nonzero return value means some kind of error occurred.
 *
 * Some sample lines from an index file:
 *
 * BASE2,2,0,2,1,0,b:37859/350
 * BASE2,2,0,2,1,0,w:37859/727
 * 188142
 * 2187878
 * BASE2,2,0,2,0,0,b:+
 *
 */
static int parseindexfile(CAKE_DATA *hdat, DBFILE *f, int64_t *allocated_bytes)
{
	int stat0, stat;
	char name[MAXFILENAME];
	char msg[MAXMSG];
	FILE *fp;
	char c, colorchar;
	int bm, bk, wm, wk, bmrank, wmrank, color;
	int singlevalue, startbyte;
	adjustable_array<INDEX> indices;
	INDEX index;
	int first_idx_block;
	CPRSUBDB *dbpointer;
	int size;
	int64_t filesize;
	HANDLE cprfp;

	/* Open the compressed data file. */
	sprintf(name, "%s%s.cpr", hdat->db_filepath, f->name);
	cprfp = CreateFile(name, GENERIC_READ, FILE_SHARE_READ,
					NULL, OPEN_EXISTING, 0, NULL);
	if (cprfp == INVALID_HANDLE_VALUE) {
		sprintf(msg, "\nCannot open %s\n", name);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}

	/* Get the size in bytes and index blocks. */
	filesize = get_file_size(cprfp);
	f->num_idx_blocks = (int)(filesize / IDX_BLOCKSIZE);
	if (filesize % IDX_BLOCKSIZE)
		++f->num_idx_blocks;

	/* Close the file. */
	if (!CloseHandle(cprfp)) {
		sprintf(msg, "\nError from CloseHandle\n");
		(*hdat->log_msg_fn)(msg);
		return(1);
	}

	sprintf(name, "%s%s.idx", hdat->db_filepath, f->name);
	fp = fopen(name, "r");
	if (fp == 0) {
		sprintf(msg, "cannot open index file %s\n", name);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}

	for ( ; ; ) {

		/* At this point it has to be a BASE line or else end of file. */
		stat = fscanf(fp, " BASE%i,%i,%i,%i,%i,%i,%c:",
				&bm, &bk, &wm, &wk, &bmrank, &wmrank, &colorchar);

		/* Done if end-of-file reached. */
		if (stat <= 0)
			break;
		else if (stat < 7) {
			sprintf(msg, "Error parsing %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		/* Decode the color. */
		if (colorchar == 'b')
			color = EGDB_BLACK;
		else
			color = EGDB_WHITE;

		/* Allocate a subdb node. */
		dbpointer = (CPRSUBDB *)calloc(1, sizeof(CPRSUBDB));
		if (!dbpointer) {
			(*hdat->log_msg_fn)("Out of memory\n");
			return(1);
		}
		*allocated_bytes += ROUND_UP(sizeof(CPRSUBDB), MALLOC_ALIGNSIZE);
		hdat->cprsubdatabase[DBOFFSET(bm, bk, wm, wk, bmrank, wmrank, color)] = dbpointer;

		/* Get the rest of the line.  It could be a n/n, or it could just
		 * be a single character that is '+', '=', or '-'.
		 */
		stat = fgetc(fp);
		if (isdigit(stat)) {
			ungetc(stat, fp);
			stat0 = fscanf(fp, "%d/%d", &first_idx_block, &startbyte);
			if (stat0 < 2) {
				stat = fscanf(fp, "%c", &c);
				if (stat < 1) {
					sprintf(msg, "Bad line in %s\n", name);
					(*hdat->log_msg_fn)(msg);
					return(1);
				}
			}
            dbpointer->first_idx_block = first_idx_block;       /* which block in db. */
			dbpointer->value = 0;
			dbpointer->startbyte = startbyte;
			dbpointer->file = f;
			indices.push_back(0);		/* first block is index 0. */
			while (fscanf(fp, "%d", &index) == 1)
				indices.push_back(index);

			dbpointer->indices = indices.shrink_to_fit();

			dbpointer->num_idx_blocks = indices.size;
			size = indices.size * sizeof(dbpointer->indices[0]);
			*allocated_bytes += ROUND_UP(size, MALLOC_ALIGNSIZE);

			/* Reset the adjustable array class. This doesn't touch the allocated buffers, as they will be used
			 * by the driver and freed when the driver closes. After reset() the size field of the class object is zero. 
			 * The next push_back() will allocate a new array.
			 */
			indices.reset();
		}
		else {
			switch (stat) {
			case '+':
				singlevalue = EGDB_WIN;
				break;
			case '=':
				singlevalue = EGDB_DRAW;
				break;
			case '-':
				singlevalue = EGDB_LOSS;
				break;
			default:
				sprintf(msg, "Bad singlevalue line in %s\n", name);
				(*hdat->log_msg_fn)(msg);
				return(1);
			}
			dbpointer->first_idx_block = 0;
			dbpointer->indices = nullptr;
			dbpointer->num_idx_blocks = 0;
			dbpointer->value = singlevalue;
			dbpointer->file = f;
		}
	}
	fclose(fp);


	sprintf(msg, "%13s index blocks: %s\n", format_with_commas(f->num_idx_blocks).c_str(), name);
	(*hdat->log_msg_fn)(msg);

	return(0);
}


/*
 * Build the table of db filenames.
 */
static void build_file_table(CAKE_DATA *hdat)
{
	int count;
	int pieces;
	int nb, nw;
	int bm, bk, wm, wk;

	count = 0;
	for (pieces = 2; pieces <= MAXPIECES; ++pieces) {
		if (pieces <= SAME_PIECES_ONE_FILE) {
			sprintf(hdat->dbfiles[count].name, "db%d", pieces);
			hdat->dbfiles[count].pieces = pieces;
			hdat->dbfiles[count].fp = INVALID_HANDLE_VALUE;
			++count;
		}
		else {
			for (nb = MAXPIECES - MAXPIECE; nb <= MAXPIECE; ++nb) {
				nw = pieces - nb;
				if (nw > nb)
					continue;
				for (bk = 0; bk <= nb; ++bk) {
					bm = nb - bk;
					for (wk = 0; wk <= nw; ++wk) {
						wm = nw - wk;
						if (nb == nw && wk > bk)
							continue;
						sprintf(hdat->dbfiles[count].name, "db%d_%d%d%d%d",
									pieces, bm, bk, wm, wk);
						hdat->dbfiles[count].pieces = pieces;
						hdat->dbfiles[count].fp = INVALID_HANDLE_VALUE;
						++count;
					}
				}
			}
		}
	}
	hdat->numdbfiles = count;
}


static int egdb_close_cake(EGDB_DRIVER *handle)
{
	CAKE_DATA *hdat = (CAKE_DATA *)handle->internal_data;
	int i;
	CPRSUBDB *p;

	/* Free the cache buffers in groups of CACHE_ALLOC_COUNT at a time. */
	if (hdat->cacheblocks) {
		for (i = 0; i < hdat->cacheblocks; i += CACHE_ALLOC_COUNT) {
			if (hdat->ccbs[i].data)
				VirtualFree(hdat->ccbs[i].data, 0, MEM_RELEASE);
		}

		/* Free the cache control blocks. */
		free(hdat->ccbs);
	}

	for (i = 0; i < sizeof(hdat->dbfiles) / sizeof(hdat->dbfiles[0]); ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;

		if (hdat->dbfiles[i].file_cache) {
			VirtualFree(hdat->dbfiles[i].file_cache, 0, MEM_RELEASE);
			hdat->dbfiles[i].file_cache = 0;
		}
		else {
			if (hdat->dbfiles[i].cache_bufferi) {
				free(hdat->dbfiles[i].cache_bufferi);
				hdat->dbfiles[i].cache_bufferi = 0;
			}
		}

		if (hdat->dbfiles[i].fp != INVALID_HANDLE_VALUE)
			CloseHandle(hdat->dbfiles[i].fp);

		hdat->dbfiles[i].num_idx_blocks = 0;
		hdat->dbfiles[i].num_datablocks = 0;
		hdat->dbfiles[i].fp = INVALID_HANDLE_VALUE;
	}

	for (i = 0; i < DBSIZE; ++i) {
		p = hdat->cprsubdatabase[i];
		if (p != NULL) {
			if (p->indices)
				free(p->indices);
			free(p);
		}
	}
	free(hdat->cprsubdatabase);
	free(hdat);
	free(handle);
	return(0);
}


static int verify_crc(EGDB_DRIVER *handle)
{
	int i;
	int status;
	unsigned int crc;
	DBCRC *pcrc;
	FILE *fp;
	char filename[MAXFILENAME];
	char msg[MAXMSG];
	CAKE_DATA *hdat = (CAKE_DATA *)handle->internal_data;

	status = 0;
	for (i = 0; i < hdat->numdbfiles; ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;

		/* Build the index filename. */
		sprintf(filename, "%s.idx", hdat->dbfiles[i].name);
		pcrc = find_file_crc(filename, dbcrc, ARRAY_SIZE(dbcrc));
		if (!pcrc) {
			sprintf(msg, "No crc value known for file %s\n", filename);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}
		sprintf(filename, "%s%s.idx", hdat->db_filepath, hdat->dbfiles[i].name);
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
		if (crc != pcrc->crc) {
			sprintf(msg, "file %s failed crc check, expected %08x, got %08x\n",
				filename, pcrc->crc, crc);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}
		fclose(fp);

		/* Build the data filename. */
		sprintf(filename, "%s.cpr", hdat->dbfiles[i].name);
		pcrc = find_file_crc(filename, dbcrc, ARRAY_SIZE(dbcrc));
		if (!pcrc) {
			sprintf(msg, "No crc value known for file %s\n", filename);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}
		sprintf(filename, "%s%s.cpr", hdat->db_filepath, hdat->dbfiles[i].name);
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
		if (crc != pcrc->crc) {
			sprintf(msg, "file %s failed crc check, expected %08x, got %08x\n",
				filename, pcrc->crc, crc);
			(*hdat->log_msg_fn)(msg);
			status = 1;
			continue;
		}
		fclose(fp);
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


EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char *directory, void (*msg_fn)(const char *))
{
	int status;
	EGDB_DRIVER *handle;

	handle = (EGDB_DRIVER *)calloc(1, sizeof(EGDB_DRIVER));
	if (!handle) {
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
	handle->internal_data = calloc(1, sizeof(CAKE_DATA));
	if (!handle->internal_data) {
		free(handle);
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
	status = initdblookup((CAKE_DATA *)handle->internal_data, pieces, cache_mb, directory, msg_fn);
	if (status) {
		egdb_close_cake(handle);
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
	handle->close = egdb_close_cake;
	return(handle);
}


}	// namespace