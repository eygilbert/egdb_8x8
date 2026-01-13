#include <utility>
#include "project.h"
#include "board.h"
#include "egdb.h"
#include "egdb_common.h"
#include "egdb_internal.h"
#include "rle_tables.h"
#include "indexing.h"
#include "bitboard_convert.h"
#include "format_with_commas.h"
#include "crc.h"
#include "slice.h"
#include "mtc_crcs.h"

namespace kr_mtc {

#undef MTC_THRESHOLD
#undef MTC_DECODE

#define MTC_THRESHOLD 10
#define MTC_DECODE(val) (2 * ((val) - MTC_SKIPS))

#define SAME_PIECES_ONE_FILE 7

#define IDX_BLOCK_MULT 4
#define FILE_IDX_BLOCKSIZE 4096
#define IDX_BLOCKSIZE (FILE_IDX_BLOCKSIZE * IDX_BLOCK_MULT)
#define IDX_BLOCKS_PER_CACHE_BLOCK 1
#define CACHE_BLOCKSIZE (IDX_BLOCKSIZE * IDX_BLOCKS_PER_CACHE_BLOCK)

#define MAXLINE 256

/* Allocate cache buffers CACHE_ALLOC_COUNT at a time. */
#define CACHE_ALLOC_COUNT 256

/* If he didn't give us enough memory for at least MIN_CACHE_BUF_BYTES of
 * buffers, then use that much.
 */
#define MIN_CACHE_BUF_BYTES (4 * CACHE_BLOCKSIZE)


typedef unsigned int INDEX;

struct DBFILE {
	char is_present;
	char name[20];			/* db filename prefix. */
	int pieces;				/* number of pieces in this db file. */
	int num_idx_blocks;		/* number of index blocks in this db file. */
	int num_datablocks;		/* number of cacheblock-sized data blocks in this db file. */
	unsigned char *file_cache;/* if not null the whole db file is here. */
	HANDLE fp;
	int *cache_bufferi;		/* An array of indices into ccbs[]. */
};

// definition of a structure for compressed databases
struct CPRSUBDB {
	char value;				// WIN/LOSS/DRAW if single value, UNKNOWN == 0 else
	unsigned short int startbyte;	// at this byte
	int num_idx_blocks;		// how many index blocks does this db need?
	int first_idx_block;	// where is the first block in it's db?
	INDEX *indices;			// array of first index number in each index block 
	DBFILE *file;			// file info for this subdb
};

struct subdb_list {
	CPRSUBDB *subdb;		/* an array of subdb pointers indexed by subslicenum. */
	int num_subslices;
};

/* cache control block type. */
struct CCB {
	int next;			/* index of next node */
	int prev;			/* index of previous node */
	int blocknum;			/* the block number within database file. */
	DBFILE *dbfile;			/* which database file the block is for. */
	unsigned char *data;	/* data of this block. */
};

struct SLICE32_DATA {
	SLICE32_DATA() {
		db_filepath[0] = 0;
		maxpieces = 0;
		maxpiece = 5;
		cacheblocks = 0;
		same_pieces_one_file = 7;
		log_msg_fn = nullptr;
		ccbs = nullptr;
		ccbs_top = 0;
		idx_extension = "idx_mtc";
		mtc_data_extension = "cpr_mtc";
	};
	EGDB_TYPE db_type;				/* to allow for Italian type. */
	char db_filepath[MAXFILENAME];	/* Path to database files. */
	int maxpiece;
	int maxpieces;
	int same_pieces_one_file;
	int cacheblocks;				/* Total number of db cache blocks. */
	int autoload_pieces;			/* permanently load slices with <= this many pieces. */
	void (*log_msg_fn)(const char *);	/* for status and error messages. */
	dbaccess<subdb_list> subdb_table;			/* direct access functions to slices and subslices. */
	CCB *ccbs;
	int ccbs_top;						/* index into ccbs[] of least recently used block. */
	std::vector<DBFILE> dbfiles;
	EGDB_STATS lookup_stats;
	const char *idx_extension;
	const char *mtc_data_extension;
};


/* Function prototypes. */
int parseindexfile(SLICE32_DATA *, DBFILE *, int *allocated_bytes);
void build_file_table(SLICE32_DATA *hdat);


static void reset_db_stats(EGDB_DRIVER *handle)
{
	SLICE32_DATA *data = (SLICE32_DATA *)handle->internal_data;

	memset(&data->lookup_stats, 0, sizeof(data->lookup_stats));
}


static EGDB_STATS *get_db_stats(EGDB_DRIVER *handle)
{
	SLICE32_DATA *data = (SLICE32_DATA *)handle->internal_data;

	return(&data->lookup_stats);
}


/*
 * Return the maximum number of cacheblocks that could be used if 
 * we had unlimited ram.
 */
static int needed_cache_buffers(SLICE32_DATA *hdat)
{
	size_t i;
	int count;

	count = 0;
	for (i = 0; i < hdat->dbfiles.size(); ++i)
		if (hdat->dbfiles[i].pieces <= hdat->maxpieces && !hdat->dbfiles[i].file_cache)
			count += hdat->dbfiles[i].num_datablocks;

	return(count);
}


/*
 * Returns DB_WIN, DB_LOSS, DB_DRAW, DB_UNKNOWN, or DB_NOT_IN_CACHE
 * for a position in a database lookup from a compressed database. 
 * First this function computes the index of the current position. 
 * Then it searches the index file of that database to determine in which
 * index block that index resides. 
 * Finally it reads and decompresses that block to find
 * the value of the position.
 * If the value is not already in a cache buffer, the action depends on
 * the argument cl.  If cl is true, DB_NOT_IN_CACHE is returned, 
 * otherwise the cache block is read and cached and the value is obtained.
 */
static int dblookup(EGDB_DRIVER *handle, EGDB_ROW_REVERSED_BITBOARD *p, int color, int cl)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
	UINT32 index;
	int64_t index64;
	int bm, bk, wm, wk;
	int i, subslicenum;
	int idx_blocknum;	
	int blocknum;
	int returnvalue;
	unsigned char *diskblock;
	INDEX n_idx;
	INDEX *indices;
	EGDB_ROW_REVERSED_BITBOARD revpos;
	subdb_list *dbp;
	CPRSUBDB *dbpointer;

	/* Start tracking db stats here. */
	++hdat->lookup_stats.db_requests;

	/* set bm, bk, wm, wk. */
	bm = bitcount(p->black_man);
	wm = bitcount(p->white_man);
	bk = bitcount(p->black_king);
	wk = bitcount(p->white_king);
	
	/* if one side has nothing, return appropriate value
	 * this means you can call lookupcompressed with positions
	 * where one side has no material - unlike chinook db.
	 */
	if ((bm + bk) == 0) {
		++hdat->lookup_stats.db_returns;
		return(MTC_LESS_THAN_THRESHOLD);
	}
	if ((wm + wk) == 0) {
		++hdat->lookup_stats.db_returns;
		return(MTC_LESS_THAN_THRESHOLD);
	}

	if ((bm + wm + wk + bk > hdat->maxpieces) || (bm + bk > hdat->maxpiece) || (wm + wk > hdat->maxpiece)) {
		++hdat->lookup_stats.db_not_present_requests;
		return(MTC_LESS_THAN_THRESHOLD);
	}

	/* If white is dominating this position, change colors. */
	if (needs_reversal(bm, bk, wm, wk, color)) {
		reverse(&revpos, p);
		p = &revpos;
		color = OTHER_COLOR(color);
		using std::swap;
		swap(bm, wm);
		swap(bk, wk);
	}

	index64 = position_to_index_slice(p, bm, bk, wm, wk);
	get_subindices(index64, subslicenum, index, MAX_SUBSLICE_INDICES);

	/* get pointer to db. */
	dbp = hdat->subdb_table.subslices(bm, bk, wm, wk, color);
	dbpointer = dbp->subdb;

	/* check presence. */
	if (dbpointer == 0) {
		++hdat->lookup_stats.db_not_present_requests;
		return(MTC_LESS_THAN_THRESHOLD);
	}

	dbpointer += subslicenum;

	if (!dbpointer->file) {
		++hdat->lookup_stats.db_returns;
		return(MTC_LESS_THAN_THRESHOLD);
	}

	/* check if the db contains only a single value. */
	if (dbpointer->value != EGDB_UNKNOWN) {
		++hdat->lookup_stats.db_returns;
		return(dbpointer->value);
	}

	/* We know the index and the database, so look in 
	 * the indices array to find the right index block.
	 */
	indices = dbpointer->indices;
	idx_blocknum = find_block(0, dbpointer->num_idx_blocks, indices, index);

	/* See if this is an autoloaded block. */
	if (dbpointer->file->file_cache) {
		++hdat->lookup_stats.autoload_hits;
		diskblock = dbpointer->file->file_cache + IDX_BLOCKSIZE *
							(dbpointer->first_idx_block + idx_blocknum);
	}
	else {
		int ccbi;
		CCB *ccbp;

		/* See if blocknumber is already in cache. */
		blocknum = (dbpointer->first_idx_block + idx_blocknum) / IDX_BLOCKS_PER_CACHE_BLOCK;

		/* Is this block already cached? */
		ccbi = dbpointer->file->cache_bufferi[blocknum];
		if (ccbi != UNDEFINED_BLOCK_ID) {

			/* Already cached.  Update the lru list. */
			ccbp = update_lru<CCB>(hdat, ccbi);
		}
		else {

			/* we must load it.
			 * if the lookup was a "conditional lookup", we don't load the block.
			 */
			if (cl)
				return(MTC_LESS_THAN_THRESHOLD);

			/* If necessary load this block from disk, update lru list. */
			ccbp = load_blocknum_v2<CCB>(hdat, dbpointer->file, blocknum, CACHE_BLOCKSIZE);
		}

		diskblock = ccbp->data + ((dbpointer->first_idx_block + idx_blocknum) %
						IDX_BLOCKS_PER_CACHE_BLOCK) * IDX_BLOCKSIZE;
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
			reverse = 1;
	}
	if (reverse) {
		n_idx = indices[idx_blocknum + 1];
		i = IDX_BLOCKSIZE - 1;
		while (n_idx > index) {
			n_idx -= rle_compr::runlength_mtc[diskblock[i]];
			i--;
		}
		i++;
	}
	else {
		n_idx = indices[idx_blocknum];

		/* and move through the bytes until we overshoot index
		 * set the start byte in this block: if it's block 0 of a db,
		 * this can be !=0.
		 */
		i = 0;
		if (idx_blocknum == 0)
			i = dbpointer->startbyte;

		while (n_idx <= index) {
			n_idx += rle_compr::runlength_mtc[diskblock[i]];
			i++;
		}

		/* once we are out here, n>index
		 * assert(n>index);
		 * we overshot again, move back:
		 */
		i--;
		n_idx -= rle_compr::runlength_mtc[diskblock[i]];
	}

	/* Do some simple error checking. */
	if (i < 0 || i >= IDX_BLOCKSIZE) {
		char msg[MAXMSG];

		sprintf(msg, "db block array index outside block bounds: %d\n", i);
		(*hdat->log_msg_fn)(msg);
		return MTC_LESS_THAN_THRESHOLD;
	}

	/* The target position is in diskblock[i]. */
	if (diskblock[i] < MTC_SKIPS) {

		/* It is a compressed byte.  Return 1, the value which means
		 * "less than MTC_THRESHOLD moves to conversion". 
		 */
		returnvalue = MTC_LESS_THAN_THRESHOLD;
	}
	else {
		returnvalue = MTC_DECODE(diskblock[i]);
	}

	++hdat->lookup_stats.db_returns;
	return(returnvalue);
}


/*
 * Open the endgame db driver for the slice32 database.
 * 
 * pieces is the maximum number of pieces to do lookups for.
 * cache_mb is the amount of ram to use for the driver, in bytes * 1E6.
 * filepath is the path to the database files.
 * msg_fn is a pointer to a function which will log status and error messages.
 * A non-zero return value means some kind of error occurred.  The nature of
 * any errors are communicated through the msg_fn.
 */
static int initdblookup(SLICE32_DATA *hdat, int pieces, int cache_mb, const char *filepath, void (*msg_fn)(const char *))
{
	int i, j;
	int t0, t1;
	char dbname[MAXFILENAME];
	char msg[MAXMSG];
	int allocated_bytes;		/* keep track of heap allocations in bytes. */
	int autoload_bytes;			/* keep track of autoload allocations in bytes. */
	int size;
	int count, stat;
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
	if (hdat->subdb_table.open(hdat->maxpiece)) {
		(*hdat->log_msg_fn)("Out of memory allocating subdb table.\n");
		return(1);
	}
	allocated_bytes += hdat->subdb_table.size() * sizeof(subdb_list);

	/* initialize binomial coefficients. */
	initbicoef();

	/* initialize runlength and lookup array. */
	rle_compr::init_compression_tables();

	/* initialize man index base table. */
	init_indexing();


	/* Build table of db filenames. */
	build_file_table(hdat);

	/* Parse index files. */
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {

		/* Dont do more pieces than he asked for. */
		if (hdat->dbfiles[i].pieces > pieces)
			break;

		stat = parseindexfile(hdat, &hdat->dbfiles[i], &allocated_bytes);
		if (stat)
			return(1);

		/* Calculate the number of cache blocks. */
		if (hdat->dbfiles[i].is_present) {
			hdat->dbfiles[i].num_datablocks = hdat->dbfiles[i].num_idx_blocks / IDX_BLOCKS_PER_CACHE_BLOCK;
			if (hdat->dbfiles[i].num_idx_blocks > hdat->dbfiles[i].num_datablocks * IDX_BLOCKS_PER_CACHE_BLOCK)
				++hdat->dbfiles[i].num_datablocks;
		}
	}
	
	/* End of reading index files, start of autoload. */
	t1 = GetTickCount();
	sprintf(msg, "Reading index files took %d secs\n", (t1 - t0) / 1000);
	(*hdat->log_msg_fn)(msg);

	/* Open file pointers for each db: keep open all the time. */
	for (i = 0; i < (int)hdat->dbfiles.size(); ++i) {

		/* Dont do more pieces than he asked for. */
		if (hdat->dbfiles[i].pieces > pieces)
			break;

		if (!hdat->dbfiles[i].is_present)
			continue;

		sprintf(dbname, "%s%s.%s", hdat->db_filepath, hdat->dbfiles[i].name, hdat->mtc_data_extension);
		hdat->dbfiles[i].fp = CreateFile((LPCSTR)dbname, GENERIC_READ, FILE_SHARE_READ,
						NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY,
						NULL);
		if (hdat->dbfiles[i].fp == INVALID_HANDLE_VALUE) {
			sprintf(msg, "Cannot open %s\n", dbname);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		/* Allocate buffers and read files for autoloaded dbs. */
		if (hdat->dbfiles[i].pieces <= hdat->autoload_pieces) {
			size = hdat->dbfiles[i].num_datablocks * CACHE_BLOCKSIZE;
			hdat->dbfiles[i].file_cache = (unsigned char *)VirtualAlloc(0, size, MEM_COMMIT, PAGE_READWRITE);
			allocated_bytes += size;
			autoload_bytes += size;
			if (hdat->dbfiles[i].file_cache == NULL) {
				(*hdat->log_msg_fn)("Cannot allocate memory for file_cache array\n");
				return(1);
			}

			/* Seek to the beginning of the file. */
			if (SetFilePointer(hdat->dbfiles[i].fp, 0, NULL, FILE_BEGIN) == -1) {
				(*hdat->log_msg_fn)("ERROR: file_cache seek failed\n");
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
			 * Each array entry is either an index into ccbs or -1 if that data block is not loaded.
			 */
			size = hdat->dbfiles[i].num_datablocks * sizeof(hdat->dbfiles[i].cache_bufferi[0]);
			hdat->dbfiles[i].cache_bufferi = (int *)malloc(size);
			allocated_bytes += size;
			if (hdat->dbfiles[i].cache_bufferi == NULL) {
				(*hdat->log_msg_fn)("Cannot allocate memory for cache_bufferi array\n");
				return(-1);
			}

			/* Init these buffer indices to UNDEFINED_BLOCK_ID, means that data block is not loaded. */
			for (j = 0; j < hdat->dbfiles[i].num_datablocks; ++j)
				hdat->dbfiles[i].cache_bufferi[j] = UNDEFINED_BLOCK_ID;
		}
	}

	sprintf(msg, "Allocated %dkb for indexing\n", (int)((allocated_bytes - autoload_bytes) / 1024));
	(*hdat->log_msg_fn)(msg);
	sprintf(msg, "Allocated %dkb for permanent slice caches\n", (int)(autoload_bytes / 1024));
	(*hdat->log_msg_fn)(msg);

	/* Figure out how much ram is left for lru cache buffers.
	 * If we autoloaded everything then no need for lru cache buffers.
	 */
	if (hdat->autoload_pieces >= pieces)
		hdat->cacheblocks = 0;
	else {

		int needed = needed_cache_buffers(hdat);
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
				hdat->cacheblocks = (cache_mb * ONE_MB - allocated_bytes) / 
						(CACHE_BLOCKSIZE + sizeof(CCB));
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
			for (i = 0, count = 0; i < (int)hdat->dbfiles.size(); ++i) {
				if (hdat->dbfiles[i].pieces <= hdat->autoload_pieces)
					continue;

				if (!hdat->dbfiles[i].is_present)
					continue;

				if (hdat->dbfiles[i].fp == INVALID_HANDLE_VALUE)
					continue;

				for (j = 0; j < hdat->dbfiles[i].num_datablocks && count < hdat->cacheblocks; ++count, ++j) {
					load_blocknum_v2<CCB>(hdat, &hdat->dbfiles[i], j, CACHE_BLOCKSIZE);
				}
				if (count >= hdat->cacheblocks)
					break;
			}
			t1 = GetTickCount();
			sprintf(msg, "Read %d buffers in %d sec, %.3f msec/buffer\n", 
						hdat->cacheblocks, (t1 - t0) / 1000, 
						(double)(t1 - t0) / (double)hdat->cacheblocks);
			(*hdat->log_msg_fn)(msg);
		}
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
static int parseindexfile(SLICE32_DATA *hdat, DBFILE *f, int *allocated_bytes)
{
	int stat0, stat;
	char name[MAXFILENAME];
	char msg[MAXMSG];
	FILE *fp;
	char c, colorchar;
	int bm, bk, wm, wk, subslicenum, color;
	int startbyte;
	adjustable_array<INDEX> indices;
	INDEX block_index_start;
	int linecount;
	int first_idx_block;
	CPRSUBDB *dbpointer;
	int size;
	int64_t filesize;
	HANDLE cprfp;
	subdb_list *dbp;

	/* Open the compressed data file. */
	sprintf(name, "%s%s.%s", hdat->db_filepath, f->name, hdat->mtc_data_extension);
	cprfp = CreateFile(name, GENERIC_READ, FILE_SHARE_READ,
					NULL, OPEN_EXISTING, 0, NULL);

	/* No problem if we cant open a file.  Many slices dont have any
	 * useful mtc data.
	 */
	if (cprfp == INVALID_HANDLE_VALUE)
		return(0);

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

	sprintf(name, "%s%s.%s", hdat->db_filepath, f->name, hdat->idx_extension);
	fp = fopen(name, "r");
	if (fp == 0) {
		sprintf(msg, "cannot open index file %s\n", name);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}

	f->is_present = 1;
	for ( ; ; ) {

		/* At this point it has to be a BASE line or else end of file. */
		stat = fscanf(fp, " BASE%i,%i,%i,%i,%i,%c:",
				&bm, &bk, &wm, &wk, &subslicenum, &colorchar);

		/* Done if end-of-file reached. */
		if (stat <= 0)
			break;
		else if (stat < 6) {
			sprintf(msg, "Error parsing %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		/* Decode the color. */
		if (colorchar == 'b')
			color = EGDB_BLACK;
		else
			color = EGDB_WHITE;

		/* Get the subdb node. */
		dbp = hdat->subdb_table.subslices(bm, bk, wm, wk, color);

		/* If we have not allocated a subslice table for this subdb, then
		 * do it now.
		 */
		if (!dbp->subdb) {
			dbp->num_subslices = get_num_subslices(bm, bk, wm, wk, MAX_SUBSLICE_INDICES);
			dbp->subdb = (CPRSUBDB *)calloc(dbp->num_subslices, sizeof(CPRSUBDB));
			*allocated_bytes += dbp->num_subslices * sizeof(CPRSUBDB);
			if (!dbp->subdb) {
				(*hdat->log_msg_fn)("Cannot allocate subslice subdb\n");
				return(1);
			}
		}
		dbpointer = dbp->subdb + subslicenum;

		/* Get the rest of the line.  For mtc index files it has to be n/n. */
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
            dbpointer->first_idx_block = first_idx_block / IDX_BLOCK_MULT;       /* which block in db. */
			dbpointer->value = 0;
			dbpointer->startbyte = startbyte + (first_idx_block % IDX_BLOCK_MULT) * FILE_IDX_BLOCKSIZE;
			dbpointer->file = f;

			/* now, we got the first line, maybe there are more... 
			 * count the number of blocks in the db.
			 */
			linecount = first_idx_block % IDX_BLOCK_MULT;
			indices.push_back(0);		/* first block is index 0. */
			while (fscanf(fp, "%d", &block_index_start) == 1) {
				if (++linecount >= IDX_BLOCK_MULT) {
					linecount = 0;
					indices.push_back(block_index_start);
				}
			}

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
			(*hdat->log_msg_fn)("Error parsing mtc index file.\n");
			return(1);
		}
	}
	fclose(fp);

	sprintf(msg, "%13s index blocks: %s\n", format_with_commas(f->num_datablocks).c_str(), name);
	(*hdat->log_msg_fn)(msg);

	return(0);
}

static void build_file_table(SLICE32_DATA *hdat)
{
	Slice slice(2);
	DBFILE file;

	memset(&file, 0, sizeof(file));
	file.fp = INVALID_HANDLE_VALUE;
	for (slice = Slice(2); slice.npieces() <= hdat->maxpieces; ) {
		if (slice.npieces() <= hdat->same_pieces_one_file) {
			sprintf(file.name, "db%d", slice.npieces());
			file.pieces = slice.npieces();
			hdat->dbfiles.push_back(file);
			slice = Slice(slice.npieces() + 1);
			continue;
		}
		if (slice.nb() > hdat->maxpiece) {
			slice.increment();
			continue;
		}

		sprintf(file.name, "db%d-%d%d%d%d", slice.npieces(), slice.nbm(), slice.nbk(), slice.nwm(), slice.nwk());
		file.pieces = slice.npieces();
		hdat->dbfiles.push_back(file);
		slice.increment();
	}
}


static int egdb_close_slice32_mtc(EGDB_DRIVER *handle)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
	int i, k, count, size;
	subdb_list *p;

	/* Free the cache buffers in groups of CACHE_ALLOC_COUNT at a time. */
	for (i = 0; i < hdat->cacheblocks; i += CACHE_ALLOC_COUNT) {
		count = std::min(CACHE_ALLOC_COUNT, hdat->cacheblocks - i);
		size = count * CACHE_BLOCKSIZE * sizeof(unsigned char);
		VirtualFree(hdat->ccbs[i].data, 0, MEM_RELEASE);
	}

	/* Free the cache control blocks. */
	free(hdat->ccbs);

	for (i = 0; i < (int)hdat->dbfiles.size(); ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;

		if (!hdat->dbfiles[i].is_present)
			continue;

		if (hdat->dbfiles[i].file_cache) {
			VirtualFree(hdat->dbfiles[i].file_cache, 0, MEM_RELEASE);
			hdat->dbfiles[i].file_cache = 0;
		}
		else {
			free(hdat->dbfiles[i].cache_bufferi);
			hdat->dbfiles[i].cache_bufferi = 0;
		}

		if (hdat->dbfiles[i].fp != INVALID_HANDLE_VALUE)
			CloseHandle(hdat->dbfiles[i].fp);

		hdat->dbfiles[i].num_idx_blocks = 0;
		hdat->dbfiles[i].num_datablocks = 0;
		hdat->dbfiles[i].fp = INVALID_HANDLE_VALUE;
	}

	for (i = 0; i < hdat->subdb_table.size(); ++i) {
		p = hdat->subdb_table.subslices_flat_view(i);
		if (p->subdb != NULL) {
			for (k = 0; k < p->num_subslices; ++k) {
				if (p->subdb[k].indices)
					free(p->subdb[k].indices);
			}
			free(p->subdb);
		}
	}
	hdat->subdb_table.close();
	delete hdat;
	free(handle);
	return(0);
}


static int verify_crc_stem(SLICE32_DATA *hdat, const char *stem, const char *ext, DBCRC *crctbl, int crcsize) 
{
	FILE *fp;
	char filename[MAXFILENAME];
	char msg[MAXMSG];
	DBCRC *pcrc;
	unsigned int crc;

	sprintf(filename, "%s.%s", stem, ext);
	pcrc = find_file_crc(filename, crctbl, crcsize);
	if (!pcrc) {
		sprintf(msg, "No crc value known for file %s\n", filename);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	sprintf(filename, "%s%s.%s", hdat->db_filepath, stem, ext);
	fp = fopen(filename, "rb");
	if (!fp) {
		sprintf(msg, "Cannot open file %s for crc check.\n", filename);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	sprintf(msg, "checking crc %s\n", filename);
	(*hdat->log_msg_fn)(msg);

	crc = file_crc_calc(fp);
	fclose(fp);
	if (crc != pcrc->crc) {
		sprintf(msg, "file %s failed crc check, expected %08x, got %08x\n",
			filename, pcrc->crc, crc);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	return(0);
}


static int verify_crc(EGDB_DRIVER *handle)
{
	int i;
	int status;
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;

	status = 0;
	for (i = 0; i < (int)hdat->dbfiles.size(); ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;
		if (!hdat->dbfiles[i].is_present)
			continue;
		if (hdat->db_type == EGDB_KINGSROW32_MTC) {
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->idx_extension, english_mtc_crcs, ARRAY_SIZE(english_mtc_crcs));
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->mtc_data_extension, english_mtc_crcs, ARRAY_SIZE(english_mtc_crcs));
		}
		else {
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->idx_extension, italian_mtc_crcs, ARRAY_SIZE(italian_mtc_crcs));
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->mtc_data_extension, italian_mtc_crcs, ARRAY_SIZE(italian_mtc_crcs));
		}
	}
	return(status);
}


static int normal_lookup(EGDB_DRIVER *handle, EGDB_BITBOARD *krpos, int color, int cl)
{
	int result;
	EGDB_ROW_REVERSED_BITBOARD dbpos;

	/* Convert to the chinook/cake endgame board bitboard. */
	reverse_bits_in_row(dbpos, *(EGDB_NORMAL_BITBOARD *)krpos);

	result = dblookup(handle, &dbpos, color, cl);
	return(result);
}


static int normal_lookup_italian(EGDB_DRIVER *handle, EGDB_BITBOARD *krpos, int color, int cl)
{
	int result;
	EGDB_BITBOARD dbpos;

	/* Convert to the cake struct format. */
	dbpos.row_reversed.black_man = krpos->normal.black & ~krpos->normal.king;
	dbpos.row_reversed.black_king = krpos->normal.black & krpos->normal.king;
	dbpos.row_reversed.white_man = krpos->normal.white & ~krpos->normal.king;
	dbpos.row_reversed.white_king = krpos->normal.white & krpos->normal.king;

	result = dblookup(handle, (EGDB_ROW_REVERSED_BITBOARD *)&dbpos, color, cl);
	return(result);
}


static int reversed_lookup_italian(EGDB_DRIVER *handle, EGDB_BITBOARD *cakepos, int color, int cl)
{
	int result;
	EGDB_ROW_REVERSED_BITBOARD dbpos;

	/* Convert to the cake struct format. */
	reverse_bits_in_row(dbpos, *(EGDB_ROW_REVERSED_BITBOARD *)cakepos);

	result = dblookup(handle, &dbpos, color, cl);
	return(result);
}


EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces,
					int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type)
{
	int status;
	EGDB_DRIVER *handle;

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
	((SLICE32_DATA *)(handle->internal_data))->db_type = db_type;
	status = initdblookup((SLICE32_DATA *)handle->internal_data, pieces, cache_mb, directory, msg_fn);
	if (status) {
		egdb_close_slice32_mtc(handle);
		return(0);
	}
	if (bitboard_type == EGDB_NORMAL) {

		if (db_type == EGDB_KINGSROW32_MTC) {
			/* This is a kingsrow bitboard. */
			handle->lookup = normal_lookup;
		}
		else
			handle->lookup = normal_lookup_italian;
	}
	else {
		if (db_type == EGDB_KINGSROW32_MTC)
			handle->lookup = (REV_ROW_LOOKUP_FNPTR)dblookup;
		else {
			handle->lookup = reversed_lookup_italian;
		}
	}
	
	handle->get_stats = get_db_stats;
	handle->reset_stats = reset_db_stats;
	handle->verify = verify_crc;
	handle->close = egdb_close_slice32_mtc;
	return(handle);
}

}	// namespace
