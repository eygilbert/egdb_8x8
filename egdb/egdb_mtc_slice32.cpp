#include <utility>
#include "project.h"
#include "board.h"
#include "egdb.h"
#include "egdb_common.h"
#include "egdb_internal.h"
#include "SpinLock.h"
#include "rle_tables.h"
#include "indexing.h"
#include "bitboard_convert.h"
#include "cache_ref_hashtable.h"
#include "format_with_commas.h"
#include "fileutils.h"
#include "crc.h"
#include "slice.h"
#include "mtc_crcs.h"
#include "fileutils.h"
#include "..\convert_idx\convert_idx.h"

#define USE_HT

namespace kr_mtc {

/* The .idx_mtc index file has indexes for every 4k of data. We throw away 3 of every 4 values.
 * The .bidx_mtc index file has indexes for every 16k of data.
 */
#define IDX_BLOCK_MULT 4
#define FILE_IDX_BLOCKSIZE 4096
#define IDX_BLOCKSIZE (FILE_IDX_BLOCKSIZE * IDX_BLOCK_MULT)
#define CACHE_BLOCKSIZE IDX_BLOCKSIZE

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
	char pieces;				/* number of pieces in this db file. */
	char autoload;				/* statically load the whole file if true. */
	char name[20];				/* db filename prefix. */
	int num_datablocks;			/* number of cacheblock-sized data blocks in this db file. */
	uint8_t *file_cache;		/* if not null the whole db file is here. */
	HANDLE fp;
#ifndef USE_HT
	int *cache_bufferi;			/* An array of indices into ccbs[], indexed by datafile block number. */
#endif
};

// definition of a structure for compressed databases
struct CPRSUBDB {
	int8_t basenode;				/* true if this node has the base pointers for indices[] */
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
	unsigned char *data;	/* data of this block. */
#ifdef USE_HT
	cache_ref_ht::node *ht_node;	/* node containing a pointer back to this block, or nullptr. */
	int ht_index;		/* index into ht for the node with a pointer back to this block, or UNDEFINED_BLOCK_ID. */
#else
	int blocknum;			/* the block number within database file. */
#endif
	DBFILE *dbfile;			/* which database file the block is for. */
};

struct SLICE32_DATA {
	SLICE32_DATA() {
		memset(&lookup_stats, 0, sizeof(lookup_stats));
		db_filepath[0] = 0;
		maxpieces = 0;
		maxpiece = 5;
		cacheblocks = 0;
		same_pieces_one_file = 7;
		log_msg_fn = nullptr;
		ccbs = nullptr;
		ccbs_top = 0;
		idx_extension = "idx_mtc";
		base_extension = "base_mtc";
		mtc_data_extension = "cpr_mtc";
	};
	EGDB_TYPE db_type;				/* to allow for Italian type. */
	char db_filepath[MAXFILENAME];	/* Path to database files. */
	int maxpiece;
	int maxpieces;
	int same_pieces_one_file;
	int cacheblocks;				/* Total number of db cache blocks. */
	int autoload_pieces;			/* permanently load slices with <= this many pieces. */
#ifdef USE_HT
	cache_ref_ht hashtable;
#endif
	void (*log_msg_fn)(const char *);	/* for status and error messages. */
	dbaccess<subdb_list> subdb_table;			/* direct access functions to slices and subslices. */
	CCB *ccbs;
	int ccbs_top;						/* index into ccbs[] of least recently used block. */
#if SMP
	SPINLOCK lock;
#endif
	std::vector<DBFILE> dbfiles;
	EGDB_STATS lookup_stats;
	const char *idx_extension;
	const char *base_extension;
	const char *mtc_data_extension;
};


/* Function prototypes. */
int parseindexfile_binary(SLICE32_DATA *, DBFILE *, int64_t *allocated_bytes);
int parseindexfile_text(SLICE32_DATA *, DBFILE *, int64_t *allocated_bytes);
void build_file_table(SLICE32_DATA *hdat);


void reset_db_stats(EGDB_DRIVER *handle)
{
	SLICE32_DATA *data = (SLICE32_DATA *)handle->internal_data;

	memset(&data->lookup_stats, 0, sizeof(data->lookup_stats));
}


EGDB_STATS *get_db_stats(EGDB_DRIVER *handle)
{
	SLICE32_DATA *data = (SLICE32_DATA *)handle->internal_data;

	return(&data->lookup_stats);
}


/*
 * Return the maximum number of cacheblocks that could be used if 
 * we had unlimited ram.
 */
int needed_cache_buffers(SLICE32_DATA *hdat)
{
	size_t i;
	int count;

	count = 0;
	for (i = 0; i < hdat->dbfiles.size(); ++i)
		if (hdat->dbfiles[i].pieces <= hdat->maxpieces && !hdat->dbfiles[i].file_cache)
			count += hdat->dbfiles[i].num_datablocks;

	return(count);
}


#ifdef USE_HT
/*
 * Read block blocknum of dbfiles[filenum] into ccb->data.
 * Return 1 on success, 0 on error.
 */
int read_block_from_file(SLICE32_DATA *hdat, CCB *ccb, unsigned int filenum, unsigned int blocknum)
{
	return(read_cache_block(hdat->dbfiles[filenum].fp, (int64_t)blocknum * CACHE_BLOCKSIZE,
					ccb->data, CACHE_BLOCKSIZE, hdat->log_msg_fn));
}


/*
 * Return a pointer to a cache block.
 * Get the least recently used cache
 * block and load it into that.
 * Update the LRU list to make this block the most recently used.
 * Return NULL if the block cannot be read. In that case the block is not cached.
 */
CCB *load_blocknum_wht(SLICE32_DATA *hdat, CPRSUBDB *subdb, int blocknum, int cache_blocksize, unsigned int ht_index)
{
	CCB *ccbp;
	unsigned int filenum;

	++hdat->lookup_stats.lru_cache_loads;

	/* Not cached, need to load this block from disk. */
	ccbp = hdat->ccbs + hdat->ccbs_top;
	if (ccbp->ht_node) {

		/* The LRU block is in use.
		 * Mark its cache hashtable node as unused.
		 */
		hdat->hashtable.remove_node(ccbp->ht_node);
		ccbp->ht_node = nullptr;
	}

	/* Read this block from disk into ccbs_top's ccb.
	 * If the read fails, the ccb stays unused at the top of the lru list, so the next load reuses it.
	 */
	filenum = (unsigned int)(subdb->file - &hdat->dbfiles[0]);
	if (!read_block_from_file(hdat, ccbp, filenum, blocknum))
		return(NULL);

	/* The read succeeded, so now it is safe to map the block to this ccb. */
	ccbp->ht_node = hdat->hashtable.add_node(filenum, blocknum, hdat->ccbs_top, ht_index);

	/* Fix linked list to point to the next oldest entry */
	hdat->ccbs_top = hdat->ccbs[hdat->ccbs_top].next;
	return(ccbp);
}
#endif

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
int dblookup(EGDB_DRIVER *handle, EGDB_ROW_REVERSED_BITBOARD *p, int color, int cl)
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

#ifdef USE_HT
	int filenum;
	CCB *ccbp;
	unsigned int ht_index;
	cache_ref_ht::node *ht_node;

	/* We know the index and the database, so look in 
	 * the indices array to find the right index block.
	 */
	indices = dbpointer->indices;
	idx_blocknum = find_block(0, dbpointer->num_idx_blocks, indices, index);

	/* See if blocknumber is already in cache. */
	blocknum = dbpointer->first_idx_block + idx_blocknum;
    filenum = (int)(dbpointer->file - &hdat->dbfiles[0]);

    /* Is this block already cached? Look it up in the cache reference hashtable. */
#if SMP
	AcquireSpinLock(hdat->lock);
#endif
	ht_node = hdat->hashtable.lookup(filenum, blocknum, ht_index);
	if (ht_node != nullptr) {
 
		/* Already cached.  Update the lru list to make this cache block the most recently used. */
		ccbp = update_lru<CCB>(hdat, ht_node->cacheblock_index);
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
		ccbp = load_blocknum_wht(hdat, dbpointer, blocknum, CACHE_BLOCKSIZE, ht_index);
		if (ccbp == NULL) {
#if SMP
			ReleaseSpinLock(hdat->lock);
#endif
			return(MTC_UNKNOWN);
		}
    }
	diskblock = ccbp->data;

#else

	/* We know the index and the database, so look in 
	 * the indices array to find the right index block.
	 */
	indices = dbpointer->indices;
	idx_blocknum = find_block(0, dbpointer->num_idx_blocks, indices, index);

#if SMP
	AcquireSpinLock(hdat->lock);
#endif
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
		blocknum = (dbpointer->first_idx_block + idx_blocknum);

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
			if (cl) {
#if SMP
				ReleaseSpinLock(hdat->lock);
#endif
				return(MTC_LESS_THAN_THRESHOLD);
			}

			/* If necessary load this block from disk, update lru list. */
			ccbp = load_blocknum_v2<CCB>(hdat, dbpointer->file, blocknum, CACHE_BLOCKSIZE);
			if (ccbp == NULL) {
#if SMP
				ReleaseSpinLock(hdat->lock);
#endif
				return(MTC_UNKNOWN);
			}
		}

		diskblock = ccbp->data;
	}
#endif

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
#if SMP
		ReleaseSpinLock(hdat->lock);
#endif
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

	/* Done with the cache block; another thread can now evict it. */
#if SMP
	ReleaseSpinLock(hdat->lock);
#endif

	++hdat->lookup_stats.db_returns;
	return(returnvalue);
}


void log_indexing(SLICE32_DATA *hdat, DBFILE *file)
{
	char msg[250];

	/* Find each subdb in the file. */
	sprintf(msg, "File %s: %d blocks\n", file->name, file->num_datablocks);
	hdat->log_msg_fn(msg);

	for (int i = 0; i < hdat->subdb_table.size(); ++i) {
		auto p = hdat->subdb_table.subslices_flat_view(i);
		if (p && p->subdb != NULL) {
			if (p->subdb->file == file) {
				sprintf(msg, "basenode %d, num_blocks %d, first_block %d, start_byte %d\n", p->subdb->basenode, p->subdb->num_idx_blocks, p->subdb->first_idx_block, p->subdb->startbyte);
				hdat->log_msg_fn(msg);

				/* Print first 5 and last. */
				sprintf(msg, "indexes:  ");
				hdat->log_msg_fn(msg);
				for (int k = 0; k <= std::min(5, p->subdb->num_idx_blocks - 1); ++k) {
					sprintf(msg, "%d  ", p->subdb->indices[k]);
					hdat->log_msg_fn(msg);
				}
				if (p->subdb->num_idx_blocks > 5) {
					sprintf(msg, "  [%d]: %d", p->subdb->num_idx_blocks - 1, p->subdb->indices[p->subdb->num_idx_blocks - 1]);
					hdat->log_msg_fn(msg);
				}
				hdat->log_msg_fn("\n\n");
			}
		}
	}
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
int initdblookup(SLICE32_DATA *hdat, int pieces, int cache_mb, const char *filepath, void (*msg_fn)(const char *))
{
	int i, j;
	int t0, t1;
	char dbname[MAXFILENAME];
	char msg[MAXMSG];
	int64_t allocated_bytes;	/* keep track of heap allocations in bytes. */
	int autoload_bytes;			/* keep track of autoload allocations in bytes. */
	int size;
	int count, stat;
	unsigned char *blockp;		/* Base address of an allocate group of cache buffers. */

	hdat->base_extension = "base_mtc";
	if (hdat->db_type == EGDB_KINGSROW_MTC_BINIDX) {
		hdat->idx_extension = "bidx_mtc";
		hdat->maxpiece = 6;
		hdat->same_pieces_one_file = 8;
	}
	else {
		hdat->idx_extension = "idx_mtc";
		hdat->maxpiece = 5;
		hdat->same_pieces_one_file = 7;
	}

	hdat->mtc_data_extension = "cpr_mtc";
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

#if SMP
	ResetSpinLock(hdat->lock);
#endif

	/* Build table of db filenames. */
	build_file_table(hdat);

	/* Parse index files. */
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {

		/* Dont do more pieces than he asked for. */
		if (hdat->dbfiles[i].pieces > pieces)
			break;
		if (hdat->db_type == EGDB_KINGSROW_MTC_BINIDX)
			stat = parseindexfile_binary(hdat, &hdat->dbfiles[i], &allocated_bytes);
		else
			stat = parseindexfile_text(hdat, &hdat->dbfiles[i], &allocated_bytes);

		if (stat)
			return(1);

//		if (hdat->dbfiles[i].pieces <= 5)
//			log_indexing(hdat, &(hdat->dbfiles[i]));

		/* Calculate the number of cache blocks. */
		if (hdat->dbfiles[i].is_present)
			hdat->dbfiles[i].num_datablocks = hdat->dbfiles[i].num_datablocks;
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

			if (!read_file(hdat->dbfiles[i].fp, hdat->dbfiles[i].file_cache, size, get_page_size())) {
				sprintf(msg, "ERROR: autoload read failed for %s, error %lu\n", hdat->dbfiles[i].name, GetLastError());
				(*hdat->log_msg_fn)(msg);
				CloseHandle(hdat->dbfiles[i].fp);
				hdat->dbfiles[i].fp = INVALID_HANDLE_VALUE;
				return(1);
			}

			/* Close the db file, we are done with it. */
			CloseHandle(hdat->dbfiles[i].fp);
			hdat->dbfiles[i].fp = INVALID_HANDLE_VALUE;
		}
#ifndef USE_HT
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
#endif
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
			else {
#ifdef USE_HT
				int64_t bytes_left = (int64_t)cache_mb * (int64_t)ONE_MB - (int64_t)allocated_bytes;
				int64_t bytes_per_cache_block = CACHE_BLOCKSIZE + sizeof(CCB) + hdat->hashtable.bytes_per_cacheblock();

				/* Calculate the number of cache blocks and hashtable nodes that can be allocated
				 * from the bytes remaining.  Use 50% more hashtable nodes than cache blocks.
				 */
				hdat->cacheblocks = (int)(bytes_left / bytes_per_cache_block);

#else
				hdat->cacheblocks = (int)((cache_mb * ONE_MB - allocated_bytes) / (CACHE_BLOCKSIZE + sizeof(CCB)));
#endif
			}
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
#ifdef USE_HT
				hdat->ccbs[i].ht_node = nullptr;
#else
				hdat->ccbs[i].blocknum = UNDEFINED_BLOCK_ID;
#endif
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

#ifdef USE_HT
			/* Allocate the cache hashtable. */
			if (hdat->cacheblocks > 0) {
				if (hdat->hashtable.open(hdat->cacheblocks)) {
					(*hdat->log_msg_fn)("cannot open hashtable\n");
					return(1);
				}
			}
#endif
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
int parseindexfile_binary(SLICE32_DATA *hdat, DBFILE *f, int64_t *allocated_bytes)
{
	char name[MAXFILENAME], basename[MAXFILENAME];
	char msg[MAXMSG];
	FILE *fp, *base_fp;
	uint32_t *indices;
	CPRSUBDB *dbpointer;
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
	f->num_datablocks = (int)(filesize / IDX_BLOCKSIZE);
	if (filesize % IDX_BLOCKSIZE)
		++f->num_datablocks;

	/* Close the file. */
	if (!CloseHandle(cprfp)) {
		sprintf(msg, "Error from CloseHandle\n");
		(*hdat->log_msg_fn)(msg);
		return(1);
	}

	sprintf(basename, "%s%s.%s", hdat->db_filepath, f->name, hdat->base_extension);
	base_fp = fopen(basename, "rb");
	if (base_fp == 0) {
		sprintf(msg, "cannot open base file %s\n", basename);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}

	f->is_present = 1;
	int64_t fsize;
	bool status = file_size(basename, fsize);
	if (status) {
		sprintf(msg, "Cannot get size of %s\n", basename);
		(*hdat->log_msg_fn)(msg);
		fclose(base_fp);
		return(1);
	}
	Base *basep = (Base *)malloc((size_t)fsize);
	if (basep == nullptr) {
		sprintf(msg, "Cannot allocate memory for %s\n", basename);
		(*hdat->log_msg_fn)(msg);
		fclose(base_fp);
		return(1);
	}
	*allocated_bytes += fsize;

	size_t nsubdbs;
	if (get_nrecords(fsize, sizeof(Base), nsubdbs)) {
		sprintf(msg, "Fractional record read from %s\n", basename);
		(*hdat->log_msg_fn)(msg);
		fclose(base_fp);
		free(basep);
		return(1);
	}

	size_t nread = fread(basep, sizeof(Base), nsubdbs, base_fp);
	if (nread != nsubdbs) {
		sprintf(msg, "Read %zu records from %s, expected %zu\n", nread, basename, nsubdbs);
		(*hdat->log_msg_fn)(msg);
		fclose(base_fp);
		free(basep);
		return(1);
	}
	fclose(base_fp);
	sprintf(name, "%s%s.%s", hdat->db_filepath, f->name, hdat->idx_extension);
	fp = fopen(name, "rb");
	if (fp == 0) {
		sprintf(msg, "cannot open index file %s\n", name);
		(*hdat->log_msg_fn)(msg);
		free(basep);
		return(1);
	}
	status = file_size(name, fsize);
	if (status) {
		sprintf(msg, "Cannot get size of %s\n", name);
		(*hdat->log_msg_fn)(msg);
		fclose(fp);
		free(basep);
		return(1);
	}
	size_t indexsize = sizeof(uint32_t);
	size_t num_index_records;
	if (get_nrecords(fsize, indexsize, num_index_records)) {
		sprintf(msg, "Fractional record in %s\n", name);
		(*hdat->log_msg_fn)(msg);
		fclose(fp);
		free(basep);
		return(1);
	}
	indices = (uint32_t *)malloc(indexsize * num_index_records);
	if (indices == nullptr) {
		sprintf(msg, "Cannot allocate memory for index array for %s\n", name);
		(*hdat->log_msg_fn)(msg);
		fclose(fp);
		free(basep);
		return(1);
	}
	*allocated_bytes += indexsize * num_index_records;

	nread = fread(indices, indexsize, num_index_records, fp);
	if (nread != num_index_records) {
		sprintf(msg, "Read %zu records from %s, expected %zu\n", nread, name, num_index_records);
		(*hdat->log_msg_fn)(msg);
		fclose(fp);
		free(basep);
		free(indices);
		return(1);
	}
	fclose(fp);

	int indices_offset = 0;
	for (size_t i = 0; i < nsubdbs; ++i) {

		/* Get the subdb node. */
		dbp = hdat->subdb_table.subslices(basep[i].nbm, basep[i].nbk, basep[i].nwm, basep[i].nwk, basep[i].color);

		/* If we have not allocated a subslice table for this subdb, then
		 * do it now.
		 */
		if (!dbp->subdb) {
			dbp->num_subslices = get_num_subslices(basep[i].nbm, basep[i].nbk, basep[i].nwm, basep[i].nwk, MAX_SUBSLICE_INDICES);
			dbp->subdb = (CPRSUBDB *)calloc(dbp->num_subslices, sizeof(CPRSUBDB));
			*allocated_bytes += dbp->num_subslices * sizeof(CPRSUBDB);
			if (!dbp->subdb) {
				(*hdat->log_msg_fn)("Cannot allocate subslice subdb\n");
				free(basep);
				return(1);
			}
		}
		dbpointer = dbp->subdb + basep[i].subslicenum;
		dbpointer->num_idx_blocks = basep[i].num_idx_blocks;
		dbpointer->file = f;
		dbpointer->startbyte = basep[i].startbyte;
		dbpointer->indices = indices + indices_offset;
		dbpointer->first_idx_block = basep[i].first_idx_block;

		/* Only one subdb owns the indices[] allocation and frees it at close.
		 * Leading subdbs with no index blocks also have indices_offset == 0,
		 * so they must not be marked, or the array is freed more than once.
		 */
		if (indices_offset == 0 && basep[i].num_idx_blocks > 0)
			dbpointer->basenode = 1;
		indices_offset += basep[i].num_idx_blocks;
	}
	
	free(basep);
	sprintf(msg, "%10d index blocks: %s\n", f->num_datablocks, name);
	(*hdat->log_msg_fn)(msg);

	return(0);
}


int parseindexfile_text(SLICE32_DATA *hdat, DBFILE *f, int64_t *allocated_bytes)
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
	f->num_datablocks = (int)(filesize / IDX_BLOCKSIZE);
	if (filesize % IDX_BLOCKSIZE)
		++f->num_datablocks;

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


void build_file_table(SLICE32_DATA *hdat)
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


int egdb_close_slice32_mtc(EGDB_DRIVER *handle)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
	subdb_list *p;

	/* Free the cache buffers in groups of CACHE_ALLOC_COUNT at a time.
	 * After a failed open, ccbs can be null, or only some groups allocated.
	 */
	if (hdat->ccbs) {
		for (int i = 0; i < hdat->cacheblocks; i += CACHE_ALLOC_COUNT) {
			if (hdat->ccbs[i].data)
				VirtualFree(hdat->ccbs[i].data, 0, MEM_RELEASE);
		}

		/* Free the cache control blocks. */
		free(hdat->ccbs);
	}

	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;

		if (!hdat->dbfiles[i].is_present)
			continue;
	
		if (hdat->dbfiles[i].file_cache) {
			VirtualFree(hdat->dbfiles[i].file_cache, 0, MEM_RELEASE);
			hdat->dbfiles[i].file_cache = 0;
		}
#ifndef USE_HT
		else {
			if (hdat->dbfiles[i].cache_bufferi) {
				free(hdat->dbfiles[i].cache_bufferi);
			}
		}
#endif

		if (hdat->dbfiles[i].fp != INVALID_HANDLE_VALUE)
			CloseHandle(hdat->dbfiles[i].fp);

		hdat->dbfiles[i].num_datablocks = 0;
		hdat->dbfiles[i].fp = INVALID_HANDLE_VALUE;
	}
#ifdef USE_HT
	hdat->hashtable.close();
#endif

	for (int i = 0; i < hdat->subdb_table.size(); ++i) {
		p = hdat->subdb_table.subslices_flat_view(i);
		if (p && p->subdb != NULL) {
			for (int k = 0; k < p->num_subslices; ++k) {
				if (hdat->db_type == EGDB_KINGSROW_MTC_BINIDX) {
					if (p->subdb[k].basenode && p->subdb[k].indices) {
						free(p->subdb[k].indices);
						p->subdb[k].indices = nullptr;
					}
				}
				else {
					if (p->subdb[k].indices) {
						free(p->subdb[k].indices);
						p->subdb[k].indices = nullptr;
					}
				}
			}
			free(p->subdb);
			p->subdb = nullptr;
		}
	}
	hdat->subdb_table.close();
	delete hdat;
	free(handle);
	return(0);
}


int verify_crc_stem(SLICE32_DATA *hdat, const char *stem, const char *ext, const DBCRC *crctbl, int crcsize) 
{
	FILE *fp;
	char filename[MAXFILENAME];
	char msg[MAXMSG];
	const DBCRC *pcrc;
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


int verify_crc(EGDB_DRIVER *handle)
{
	int status;
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;

	status = 0;
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;
		if (!hdat->dbfiles[i].is_present)
			continue;
		if (hdat->db_type == EGDB_KINGSROW32_MTC) {
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->idx_extension, english_mtc_crcs, ARRAY_SIZE(english_mtc_crcs));
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->mtc_data_extension, english_mtc_crcs, ARRAY_SIZE(english_mtc_crcs));
		}
		else if (hdat->db_type == EGDB_KINGSROW_MTC_BINIDX) {
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->base_extension, english_mtc_12pc_crcs, ARRAY_SIZE(english_mtc_12pc_crcs));
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->idx_extension, english_mtc_12pc_crcs, ARRAY_SIZE(english_mtc_12pc_crcs));
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->mtc_data_extension, english_mtc_12pc_crcs, ARRAY_SIZE(english_mtc_12pc_crcs));
		}
		else if (hdat->db_type == EGDB_KINGSROW32_ITALIAN_MTC) {
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->idx_extension, italian_mtc_crcs, ARRAY_SIZE(italian_mtc_crcs));
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->mtc_data_extension, italian_mtc_crcs, ARRAY_SIZE(italian_mtc_crcs));
		}
	}
	return(status);
}


int normal_lookup(EGDB_DRIVER *handle, EGDB_BITBOARD *krpos, int color, int cl)
{
	int result;
	EGDB_ROW_REVERSED_BITBOARD dbpos;

	/* Convert to the chinook/cake endgame board bitboard. */
	reverse_bits_in_row(dbpos, *(EGDB_NORMAL_BITBOARD *)krpos);

	result = dblookup(handle, &dbpos, color, cl);
	return(result);
}


int normal_lookup_italian(EGDB_DRIVER *handle, EGDB_BITBOARD *krpos, int color, int cl)
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


int reversed_lookup_italian(EGDB_DRIVER *handle, EGDB_BITBOARD *cakepos, int color, int cl)
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
	handle->internal_data = new (std::nothrow) SLICE32_DATA;
	if (!handle->internal_data) {
		free(handle);
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
	((SLICE32_DATA *)(handle->internal_data))->db_type = db_type;
	try {
		status = initdblookup((SLICE32_DATA *)handle->internal_data, pieces, cache_mb, directory, msg_fn);
	}
	catch (const std::exception &e) {
		char msg[MAXMSG];

		/* Out of memory, or another C++ exception. Don't let it escape the C interface. */
		sprintf(msg, "egdb_open failed: %.100s\n", e.what());
		(*msg_fn)(msg);
		status = 1;
	}
	if (status) {
		egdb_close_slice32_mtc(handle);
		return(0);
	}
	if (bitboard_type == EGDB_NORMAL) {

		if (db_type == EGDB_KINGSROW32_MTC || db_type == EGDB_KINGSROW_MTC_BINIDX) {
			/* This is a kingsrow bitboard. */
			handle->lookup = normal_lookup;
		}
		else
			handle->lookup = normal_lookup_italian;
	}
	else {
		if (db_type == EGDB_KINGSROW32_MTC || db_type == EGDB_KINGSROW_MTC_BINIDX)
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
