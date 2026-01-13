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

#define USE_HT
#define BINARY_IDX_FILES

#ifdef BINARY_IDX_FILES
#include "fileutils.h"
#include "..\convert2db12\convert2db12.h"
#endif

/* Having types with the same name as types in other files confuses the debugger. */
#define SLICE32_DATA SLICE32_DATA_TUN12
#define DBFILE DBFILE_TUN12
#define CPRSUBDB CPRSUBDB_TUN12
#define subdb_list subdb_list_TUN12

#define LOG_HITS 1

#define MIN_AUTOLOAD_PIECES 6

#define NUM_SUBINDICES 64
#define IDX_BLOCKSIZE 4096
#define CACHE_BLOCKSIZE IDX_BLOCKSIZE
#define SUBINDEX_BLOCKSIZE (IDX_BLOCKSIZE / NUM_SUBINDICES)

#define MAXLINE 256

/* Allocate cache buffers CACHE_ALLOC_COUNT at a time. */
#define CACHE_ALLOC_COUNT 512

/* If he didn't give us enough memory for at least MIN_CACHE_BUF_BYTES of
 * buffers, then use that much.
 */
#define MIN_CACHE_BUF_BYTES (10 * ONE_MB)

namespace tun_v3 {

typedef unsigned int INDEX;

struct subdb_list;

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
	std::vector<subdb_list *> *slices;	/* An ordered list of the slices in this file. */
#if LOG_HITS
	int hits;
#endif
};

// definition of a structure for compressed databases
struct CPRSUBDB {
	int8_t singlevalue;				/* WIN/LOSS/DRAW if single value, else NOT_SINGLEVALUE. */
	int8_t basenode;				/* true if this node has the base pointers for indices[], catalogidx[], and vmap[]. */
	uint8_t first_subidx_block;		/* modulus NUM_SUBINDICES */
	uint8_t single_subidx_block;	/* true if only one subidx blocks in this subdb. */
	uint8_t last_subidx_block;		/* modulus NUM_SUBINDICES */
	short int startbyte;			/* offset into first index block of first data. */
	int num_idx_blocks;				/* number of index blocks in this subdb. */
	int first_idx_block;			/* index block containing the first data value. */
	INDEX *indices;					/* array of first index number in each index block */
									/* allocated dynamically, to size [num_idx_blocks] */
	uint8_t *catalogidx;			/* array of catalog indices, one for each block. */
	uint8_t *vmap;					/* array of vmap descriptors, one for each block. */
	INDEX *autoload_subindices;		/* subindices for autoloaded files. */
	DBFILE *file;					/* file info for this subdb */
	CPRSUBDB *next;					/* previous subdb in the same file that is not all one value. */
	CPRSUBDB *prev;					/* next subdb in the same file that is not all one value. */
#if LOG_HITS
	int hits;
#endif
};

struct subdb_list {
	CPRSUBDB *subdb;		/* an array of subdb pointers indexed by subslicenum. */
	int num_subslices;
};

/* Cache control block type. */
struct CCB {
	int next;			/* index of next node */
	int prev;			/* index of previous node */
	unsigned char *data;	/* data of this block. */
#ifdef USE_HT
	cache_ref_ht::node *ht_node;	/* node containing a pointer back to this block, or nullptr. */
	int ht_index;		/* index into ht for the node with a pointer back to this block, or UNDEFINED_BLOCK_ID. */
#else
	int blocknum;			/* the block number within database file. */
	CPRSUBDB *subdb;		/* which subdb the block is for; there may be more than 1. */
#endif
	INDEX subindices[NUM_SUBINDICES];
};

struct SLICE32_DATA {
	SLICE32_DATA() {
		db_filepath[0] = 0;
		maxpiece = 0;
		maxpieces = 0;
		cacheblocks = 0;
		same_pieces_one_file = 0;
		log_msg_fn = nullptr;
		ccbs = nullptr;
		ccbs_top = 0;
	};
	EGDB_TYPE db_type;				/* to allow for Italian type. */
	char db_filepath[MAXFILENAME];	/* Path to database files. */
	int maxpiece;
	int maxpieces;
	int same_pieces_one_file;
	int cacheblocks;				/* Total number of db cache blocks. */
#ifdef USE_HT
	cache_ref_ht hashtable;
#endif
	void (*log_msg_fn)(const char *);		/* for status and error messages. */
	CCB *ccbs;
	int ccbs_top;					/* index into ccbs[] of least recently used block. */
#if SMP
	SPINLOCK lock;
#endif
	std::vector<DBFILE> dbfiles;
	std::vector<DBFILE *> files_autoload_order;
	EGDB_STATS lookup_stats;
	tun_v2_decompress decompress;
	dbaccess<subdb_list> subdb_table;
	const char *idx_extension;
	const char *base_extension;
	const char *wld_data_extension;
};


/* Function prototypes. */
int parseindexfile(SLICE32_DATA *, DBFILE *, int64_t *allocated_bytes);
void build_file_table(SLICE32_DATA *hdat);
void assign_subindices(SLICE32_DATA *hdat, CPRSUBDB *subdb, CCB *ccbp);


static void reset_db_stats(EGDB_DRIVER *handle)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
#if LOG_HITS
	int i, k;
	subdb_list *p;

	for (i = 0; i < hdat->dbfiles.size(); ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;

		if (!hdat->dbfiles[i].is_present)
			continue;
		hdat->dbfiles[i].hits = 0;
	}
	for (i = 0; i < hdat->subdb_table.size(); ++i) {
		p = hdat->subdb_table.subslices_flat_view(i);
		if (p->subdb != NULL) {
			for (k = 0; k < p->num_subslices; ++k) {
				p->subdb[k].hits = 0;
			}
		}
	}

#endif
	memset(&hdat->lookup_stats, 0, sizeof(hdat->lookup_stats));
}


static EGDB_STATS *get_db_stats(EGDB_DRIVER *handle)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
#if LOG_HITS
	int i, k, count;
	int nb, nw, bk, wk, bm, wm, pieces, color;
	DBFILE *f;
	subdb_list *p;
	double hitrate;
	char msg[100];

	count = 0;
	for (i = 0; i < hdat->dbfiles.size(); ++i) {
		f = &hdat->dbfiles[i];
		if (f->pieces > hdat->maxpieces)
			continue;

		if (!f->is_present)
			continue;
		count += f->hits;
	}

	for (i = 0; i < hdat->dbfiles.size(); ++i) {
		f = &hdat->dbfiles[i];
		if (f->hits) {
			hitrate = 10000.0 * (double)f->hits / ((double)count * (double)f->num_datablocks);
			sprintf(msg, "%s %d hits, %f hits/iblock\n",
					f->name,
					f->hits,
					hitrate);
			(*hdat->log_msg_fn)(msg);
		}
	}
	(*hdat->log_msg_fn)("\n");
	for (pieces = hdat->same_pieces_one_file; pieces <= hdat->maxpieces; ++pieces) {
		for (nb = 1; nb <= hdat->maxpiece; ++nb) {
			nw = pieces - nb;
			if (nw > nb)
				continue;
			for (bk = 0; bk <= nb; ++bk) {
				bm = nb - bk;
				for (wk = 0; wk <= nw; ++wk) {
					wm = nw - wk;
					if (bm + bk == wm + wk && wk > bk)
						continue;
					for (color = 0; color < 2; ++color) {
						p = hdat->subdb_table.subslices(bm, bk, wm, wk, color);
						if (p->subdb != NULL) {
							for (k = 0; k < p->num_subslices; ++k) {
								hitrate = 10000.0 * (double)p->subdb[k].hits / ((double)count * (double)p->subdb[k].num_idx_blocks);
								if (hitrate > .001) {
									sprintf(msg, "%d-%d%d%d%d.%d%c  %d hits, %f hits/iblock, %.1fmb\n",
											nb + nw,
											bm, bk, wm, wk, k,
											color == EGDB_BLACK ? 'b' : 'w',
											p->subdb[k].hits,
											hitrate,
											(double)p->subdb[k].num_idx_blocks * (double)IDX_BLOCKSIZE / (double)ONE_MB);
									(*hdat->log_msg_fn)(msg);
								}
							}
						}
					}
				}
			}
		}
	}
#endif
	return(&hdat->lookup_stats);
}


#ifdef USE_HT
void read_block_from_file(SLICE32_DATA *hdat, CCB *ccb)
{
	I64_HIGH_LOW filepos;
	DWORD bytes_read;
	BOOL stat;
	HANDLE hfile;

	filepos.word64 = (int64_t)ccb->ht_node->blocknum * CACHE_BLOCKSIZE;
	hfile = hdat->dbfiles[ccb->ht_node->filenum].fp;

	/* Seek the start position. */
	if (SetFilePointer(hfile, filepos.words32.low32, (long*)&filepos.words32.high32, FILE_BEGIN) == -1) {
		(*hdat->log_msg_fn)("\nseek failed\n");
		return;
	}
	stat = ReadFile(hfile, ccb->data, CACHE_BLOCKSIZE, &bytes_read, NULL);
	if (!stat)
		(*hdat->log_msg_fn)("\nError reading file\n");
}


/*
 * Return a pointer to a cache block.
 * Get the least recently used cache
 * block and load it into that.
 * Update the LRU list to make this block the most recently used.
 */
CCB *load_blocknum_wht(SLICE32_DATA *hdat, CPRSUBDB *subdb, int blocknum, int cache_blocksize, unsigned int ht_index)
{
	CCB *ccbp;

	++hdat->lookup_stats.lru_cache_loads;

	/* Not cached, need to load this block from disk. */
	ccbp = hdat->ccbs + hdat->ccbs_top;
	if (ccbp->ht_node) {

		/* The LRU block is in use.
		 * Mark its cache hashtable node as unused.
		 */
		hdat->hashtable.remove_node(ccbp->ht_node);
	}

	hdat->ccbs[hdat->ccbs_top].ht_node = hdat->hashtable.add_node((unsigned int)(subdb->file - &hdat->dbfiles[0]), blocknum, hdat->ccbs_top, ht_index);

	/* Read this block from disk into ccbs_top's ccb. */
	read_block_from_file(hdat, hdat->ccbs + hdat->ccbs_top);

	assign_subindices(hdat, subdb, ccbp);

	/* Fix linked list to point to the next oldest entry */
	hdat->ccbs_top = hdat->ccbs[hdat->ccbs_top].next;
	return(ccbp);
}
#endif

/*
 * Returns EGDB_WIN, EGDB_LOSS, EGDB_DRAW, EGDB_UNKNOWN, or EGDB_NOT_IN_CACHE
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
static int dblookup(EGDB_DRIVER *handle, EGDB_ROW_REVERSED_BITBOARD *p, int color, int cl)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
	UINT32 index;
	unsigned short *runlength;
	int64_t index64;
	int bm, bk, wm, wk;
	int i, subslicenum;
	int idx_blocknum, subidx_blocknum;	
	int dbfile_blocknum;
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
		return(color == EGDB_BLACK ? EGDB_LOSS : EGDB_WIN);
	}
	if ((wm + wk) == 0) {
		++hdat->lookup_stats.db_returns;
		return(color == EGDB_WHITE ? EGDB_LOSS : EGDB_WIN);
	}

	if ((bm + wm + wk + bk > hdat->maxpieces) || (bm + bk > hdat->maxpiece) || (wm + wk > hdat->maxpiece)) {
		++hdat->lookup_stats.db_not_present_requests;
		return EGDB_SUBDB_UNAVAILABLE;
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

		/* If not a conditional lookup, then determine if both side-to-move colors are unavailable. */
		dbp = hdat->subdb_table.subslices(bm, bk, wm, wk, OTHER_COLOR(color));
		if (dbp->subdb)
			return(EGDB_UNKNOWN);
		else
			return(EGDB_SUBDB_UNAVAILABLE);
	}

	dbpointer += subslicenum;
#if LOG_HITS
	++dbpointer->file->hits;
	++dbpointer->hits;
#endif

	/* check if the db contains only a single value. */
	if (dbpointer->singlevalue != NOT_SINGLEVALUE) {
		++hdat->lookup_stats.db_returns;
		return(dbpointer->singlevalue);
	}

	/* See if this is an autoloaded block. */
	if (dbpointer->file->file_cache) {
		++hdat->lookup_stats.autoload_hits;

		/* Do a binary search to find the exact subindex. */
		indices = dbpointer->autoload_subindices;
		subidx_blocknum = find_block(dbpointer->first_subidx_block, 
									dbpointer->num_idx_blocks * NUM_SUBINDICES - (NUM_SUBINDICES - 1 - dbpointer->last_subidx_block),
									indices, index);
		dbfile_blocknum = subidx_blocknum / NUM_SUBINDICES + dbpointer->first_idx_block;
		diskblock = dbpointer->file->file_cache + (subidx_blocknum * (SIZE_T)SUBINDEX_BLOCKSIZE) +
					dbpointer->first_idx_block * (SIZE_T)IDX_BLOCKSIZE;
		n_idx = indices[subidx_blocknum];
		i = 0;
		if (subidx_blocknum == dbpointer->first_subidx_block)
			i = dbpointer->startbyte - subidx_blocknum * SUBINDEX_BLOCKSIZE;
	}
	else {		/* Not an autoloaded block. */
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
		dbfile_blocknum = dbpointer->first_idx_block + idx_blocknum;
        filenum = (int)(dbpointer->file - &hdat->dbfiles[0]);

#if SMP
		AcquireSpinLock(hdat->lock);
#endif
        /* Is this block already cached? Look it up in the cache reference hashtable. */
		ht_node = hdat->hashtable.lookup(filenum, dbfile_blocknum, ht_index);
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
			ccbp = load_blocknum_wht(hdat, dbpointer, dbfile_blocknum, CACHE_BLOCKSIZE, ht_index);
        }

#else
		int ccbi;
		CCB *ccbp;

		/* We know the index and the database, so look in 
		 * the indices array to find the right index block.
		 */
		indices = dbpointer->indices;
		idx_blocknum = find_block(0, dbpointer->num_idx_blocks, indices, index);

		/* See if blocknumber is already in cache. */
		dbfile_blocknum = dbpointer->first_idx_block + idx_blocknum;

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
			ccbp = load_blocknum<CCB>(hdat, dbpointer, dbfile_blocknum, CACHE_BLOCKSIZE);
		}
#endif

		/* Do a binary search to find the exact subindex.  This is complicated a bit by the
		 * problem that there may be a boundary between the end of one subdb and the start of
		 * the next in this block.  For any subindex block that contains one of these
		 * boundaries, the subindex stored is the ending block (0 is implied for the
		 * starting block).  Therefore the binary search cannot use the first subindex of
		 * a subdb.  We check for this separately.
		 */
		indices = ccbp->subindices;
		if (idx_blocknum == 0 && (dbpointer->single_subidx_block ||
						dbpointer->first_subidx_block == NUM_SUBINDICES - 1 ||
						indices[dbpointer->first_subidx_block + 1] > index)) {
			subidx_blocknum = dbpointer->first_subidx_block;
			n_idx = 0;
			i = dbpointer->startbyte - subidx_blocknum * SUBINDEX_BLOCKSIZE;
		}
		else {
            int first, last;
            if (idx_blocknum == 0)
                    first = dbpointer->first_subidx_block + 1;
            else
                    first = 0;
            if (idx_blocknum == dbpointer->num_idx_blocks - 1)
                    last = dbpointer->last_subidx_block + 1;
            else
                    last = NUM_SUBINDICES;
            subidx_blocknum = find_block(first, last, indices, index);

			n_idx = indices[subidx_blocknum];
			i = 0;
		}
		diskblock = ccbp->data + subidx_blocknum * SUBINDEX_BLOCKSIZE;
#if SMP
		ReleaseSpinLock(hdat->lock);
#endif
	}

	/* The subindex block we were looking for is now pointed to by diskblock.
	 * now we decompress the memory block
	 * Do a linear search for the byte within the block.
	 */

	/* and move through the bytes until we overshoot index
	 * set the start byte in this block: if it's block 0 of a db,
	 * this can be !=0.
	 */
	uint8_t catalognum = dbpointer->catalogidx[dbfile_blocknum - dbpointer->first_idx_block];
	runlength = hdat->decompress.get_runlengths(catalognum);
	while (n_idx <= index) {
		n_idx += runlength[diskblock[i]];
		i++;
	}

	/* once we are out here, n>index
	 * assert(n>index);
	 * we overshot again, move back:
	 */
	i--;
	n_idx -= runlength[diskblock[i]];

	/* Do some simple error checking. */
	if (i < 0 || i >= SUBINDEX_BLOCKSIZE) {
		char msg[MAXMSG];

		sprintf(msg, "db block array index outside block bounds: %d\nFile %s\n",
					 i, dbpointer->file->name);
		(*hdat->log_msg_fn)(msg);
		return EGDB_UNKNOWN;
	}

	/* The target position is in diskblock[i]. */
	returnvalue = hdat->decompress.decode(index - n_idx, diskblock[i], dbpointer->vmap[dbfile_blocknum - dbpointer->first_idx_block], catalognum);
	++hdat->lookup_stats.db_returns;

	return(returnvalue);
}


static int init_autoload_subindices(SLICE32_DATA *hdat, DBFILE *file, SIZE_T *allocated_bytes)
{
	int i, k, m, size;
	int first_subi, num_subi, subi, cache_blocknum;
	subdb_list *p;
	INDEX index;
	uint8_t catalognum;
	unsigned char *datap;
	unsigned short *runlen_table;

	*allocated_bytes = 0;
	for (i = 0; i < hdat->subdb_table.size(); ++i) {
		p = hdat->subdb_table.subslices_flat_view(i);
		if (p->subdb != NULL) {
			for (k = 0; k < p->num_subslices; ++k) {
				if (p->subdb[k].file == file && p->subdb[k].singlevalue == NOT_SINGLEVALUE) {

					/* This subdb is autoloaded. */
					first_subi = p->subdb[k].first_subidx_block;
					num_subi = p->subdb[k].num_idx_blocks * NUM_SUBINDICES - 
								(NUM_SUBINDICES - 1 - p->subdb[k].last_subidx_block);

					size = num_subi * sizeof(INDEX);
					p->subdb[k].autoload_subindices = (INDEX *)malloc(size);
					if (p->subdb[k].autoload_subindices == NULL) {
						(*hdat->log_msg_fn)("Cannot allocate memory for autoload subindices array\n");
						return(1);
					}
					*allocated_bytes += size;

					/* Zero all subindices up to first_subi. */
					for (subi = 0; subi <= first_subi; ++subi)
						p->subdb[k].autoload_subindices[subi] = 0;

					datap = p->subdb[k].file->file_cache + IDX_BLOCKSIZE * (SIZE_T)p->subdb[k].first_idx_block;

					index = 0;
					subi = first_subi;
					cache_blocknum = p->subdb[k].startbyte / CACHE_BLOCKSIZE;
					catalognum = p->subdb[k].catalogidx[cache_blocknum];
					runlen_table = hdat->decompress.get_runlengths(catalognum);
					for (m = p->subdb[k].startbyte; subi < num_subi; ++m) {

						if ((m % SUBINDEX_BLOCKSIZE) == 0) {
							subi = m / SUBINDEX_BLOCKSIZE;
							if (subi >= num_subi)
								break;
							p->subdb[k].autoload_subindices[subi] = index;

							cache_blocknum = m / CACHE_BLOCKSIZE;
							catalognum = p->subdb[k].catalogidx[cache_blocknum];
							runlen_table = hdat->decompress.get_runlengths(catalognum);
						}
						index += runlen_table[datap[m]];
					}
				}
			}
		}
	}
	return(0);
}


/*
 * We just loaded a block of data into a cacheblock.  Assign the subindices
 * for each subdb that has data in the block.
 */
static void assign_subindices(SLICE32_DATA *hdat, CPRSUBDB *subdb, CCB *ccbp)
{
	int subi, end_subi, first_idx_block, idx_blocknum;
	unsigned int m;
	INDEX index;
	unsigned short *runlen_table;
#ifdef USE_HT
	int ccb_blocknum = ccbp->ht_node->blocknum;
#else
	int ccb_blocknum = ccbp->blocknum;
#endif

	/* Find the first subdb that has some data in this block.  Use the linked list of 
	 * subdbs in this file.
	 */
	while (subdb->first_idx_block == ccb_blocknum && subdb->startbyte > 0)
		subdb = subdb->prev;

	/* For each subdb that has data in this block. */
	do {
		first_idx_block = subdb->first_idx_block;
		if (first_idx_block == ccb_blocknum) {
			subi = subdb->first_subidx_block;
			m = subdb->startbyte;
			index = 0;
			runlen_table = hdat->decompress.get_runlengths(subdb->catalogidx[0]);
		}
		else {
			subi = 0;
			m = 0;
			idx_blocknum = ccb_blocknum - subdb->first_idx_block;
			runlen_table = hdat->decompress.get_runlengths(subdb->catalogidx[idx_blocknum]);
			index = subdb->indices[idx_blocknum];
		}

		if (subdb->first_idx_block + subdb->num_idx_blocks - 1 > ccb_blocknum)
			end_subi = NUM_SUBINDICES - 1;
		else
			end_subi = subdb->last_subidx_block;

		for ( ; ; ++m) {
			if ((m % SUBINDEX_BLOCKSIZE) == 0) {
				subi = m / SUBINDEX_BLOCKSIZE;
				if (subi > end_subi)
					break;
				ccbp->subindices[subi] = index;
			}
			index += runlen_table[ccbp->data[m]];

		}
		subdb = subdb->next;
	}
	while (subdb && (subdb->first_idx_block == ccb_blocknum));
}

		
/*
 * Preload the cache blocks with data most likely to be use.
 */
static void preload_cache_blocks(SLICE32_DATA *hdat)
{
	int count;				/* keep count of cacheblocks that are preloaded. */
	int t0, t1;
	char msg[200];

	/* Preload any of the files that we did not have space for in the autoload file table. */
	t0 = GetTickCount();
	count = 0;
	for (size_t i = 0; i < hdat->dbfiles.size() && count < hdat->cacheblocks; ++i) {
		if (count >= hdat->cacheblocks)
			break;

		DBFILE *file = hdat->files_autoload_order[i];
		if (!file || !file->is_present || file->autoload)
			continue;

		sprintf(msg, "preload %s\n", file->name);
		(*hdat->log_msg_fn)(msg);

		/* Preload every slice in the file. */
		for (size_t j = 0; j < file->slices->size(); ++j) {
			if (count >= hdat->cacheblocks)
				break;

			/* Every subslice in the slice. */
			subdb_list *slice = (*file->slices)[j];
			for (int k = 0; k < slice->num_subslices; ++k) {

				CPRSUBDB *subdb = slice->subdb + k;
				if (subdb->singlevalue != NOT_SINGLEVALUE)
					continue;

				/* Every block in the subslice. */
				for (int b = 0; b < subdb->num_idx_blocks; ++b) {
					if (count >= hdat->cacheblocks)
						break;

					int blocknum = subdb->first_idx_block + b;

					/* It might already be cached. */
#ifdef USE_HT
					unsigned int filenum;
					unsigned int ht_index;
					cache_ref_ht::node *ht_node;

					filenum = (int)(file - &hdat->dbfiles[0]);
					ht_node = hdat->hashtable.lookup(filenum, blocknum, ht_index);
					if (ht_node == nullptr) {
						load_blocknum_wht(hdat, subdb, blocknum, CACHE_BLOCKSIZE, ht_index);
						++count;
					}
#else
					if (file->cache_bufferi[blocknum] == UNDEFINED_BLOCK_ID) {
						load_blocknum<CCB>(hdat, subdb, blocknum, CACHE_BLOCKSIZE);
						++count;
					}
#endif
				}
			}
		}
	}
	t1 = GetTickCount();
	sprintf(msg, "Read %d buffers in %d sec, %.3f msec/buffer\n", 
				hdat->cacheblocks, (t1 - t0) / 1000, 
				(double)(t1 - t0) / (double)hdat->cacheblocks);
	(*hdat->log_msg_fn)(msg);
}


static int needed_cache_buffers(SLICE32_DATA *hdat)
{
	int count;

	count = 0;
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i)
		if (hdat->dbfiles[i].pieces <= hdat->maxpieces && !hdat->dbfiles[i].file_cache)
			count += hdat->dbfiles[i].num_datablocks;

	return(count);
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
	int j, stat;
	int t0, t1, t2, t3;
	char dbname[MAXFILENAME];
	char msg[MAXMSG];
	int64_t allocated_bytes;		/* keep track of heap allocations in bytes. */
	int64_t autoload_bytes;			/* keep track of autoload allocations in bytes. */
	int cache_mb_avail;
	int max_autoload;
	int64_t total_dbsize;
	SIZE_T size;
	int count;
	DBFILE *f;
	unsigned char *blockp;		/* Base address of an allocate group of cache buffers. */

	hdat->base_extension = "base2";
#if BINARY_IDX_FILES
	hdat->idx_extension = "bidx2";
#else
	hdat->idx_extension = "idx2";
#endif
	hdat->wld_data_extension = "cpr2";
	t0 = GetTickCount();

	/* Save off some global data. */
	strcpy(hdat->db_filepath, filepath);

	/* Add trailing backslash if needed. */
	size = (int)strlen(hdat->db_filepath);
	if (size && (hdat->db_filepath[size - 1] != '\\'))
		strcat(hdat->db_filepath, "\\");

	hdat->maxpieces = pieces;
	hdat->maxpiece = 6;
	hdat->same_pieces_one_file = 8;
	hdat->log_msg_fn = msg_fn;
	allocated_bytes = 0;
	autoload_bytes = 0;

	std::sprintf(msg, "Available RAM: %smb\n", format_with_commas(get_mem_available_mb()).c_str());
	(*hdat->log_msg_fn)(msg);

#if EGDB_LIB
	init_bitcount();
#endif

	if (hdat->subdb_table.open(hdat->maxpiece)) {
		(*hdat->log_msg_fn)("Out of memory allocating subdb table.\n");
		return(1);
	}
	allocated_bytes += hdat->subdb_table.size() * sizeof(subdb_list);

	/* initialize binomial coefficients. */
	initbicoef();

	/* initialize man index base table. */
	init_indexing();

	hdat->decompress.init();

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

		stat = parseindexfile(hdat, &hdat->dbfiles[i], &allocated_bytes);
		if (stat)
			return(1);
	}

	/* End of reading index files, start of autoload. */
	t1 = GetTickCount();
	sprintf(msg, "Reading index files took %d secs\n", (t1 - t0) / 1000);
	(*hdat->log_msg_fn)(msg);

	/* Find the total size of all the files that will be used. */
	total_dbsize = 0;
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {
		if (hdat->dbfiles[i].is_present)
			total_dbsize += (int64_t)hdat->dbfiles[i].num_datablocks * CACHE_BLOCKSIZE;
	}

#define MIN_AUTOLOAD_RATIO .18
#define MAX_AUTOLOAD_RATIO .35

	/* Calculate how much cache mb to autoload. */
	cache_mb_avail = (int)(cache_mb - allocated_bytes / ONE_MB);
	if (total_dbsize / ONE_MB - cache_mb_avail < 20)
		max_autoload = 1 + (int)(total_dbsize / ONE_MB);		/* Autoload everything. */
	else if (cache_mb_avail < 15) {
		cache_mb_avail = 15;
		max_autoload = (int)(cache_mb_avail * MIN_AUTOLOAD_RATIO);
	}
	else if (cache_mb_avail > 1000)
		max_autoload = (int)(cache_mb_avail * MAX_AUTOLOAD_RATIO);
	else
		max_autoload = (int)((float)cache_mb_avail * (float)(MIN_AUTOLOAD_RATIO + cache_mb_avail * (MAX_AUTOLOAD_RATIO - MIN_AUTOLOAD_RATIO) / 1000.0));

	autoload_order_list_builder<SLICE32_DATA, DBFILE>::build(hdat);

	size = 0;

	for (size_t i = 0; i < hdat->files_autoload_order.size(); ++i) {
		f = hdat->files_autoload_order[i];
		if (!f || !f->is_present || f->autoload)
			continue;

		size += f->num_datablocks;
		if (f->pieces <= MIN_AUTOLOAD_PIECES || (int)((int64_t)((int64_t)size * (int64_t)CACHE_BLOCKSIZE) / ONE_MB) <= max_autoload) {
			f->autoload = 1;
			sprintf(msg, "autoload %s\n", f->name);
			(*hdat->log_msg_fn)(msg);
		}
	}

	/* Open handles for each wld data file. Keep open all the time. */
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {

		/* Dont do more pieces than he asked for. */
		if (hdat->dbfiles[i].pieces > pieces)
			break;

		if (!hdat->dbfiles[i].is_present)
			continue;

		sprintf(dbname, "%s%s.%s", hdat->db_filepath, hdat->dbfiles[i].name, hdat->wld_data_extension);
		hdat->dbfiles[i].fp = CreateFile((LPCSTR)dbname, GENERIC_READ, FILE_SHARE_READ,
						NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY,
						NULL);
		if (hdat->dbfiles[i].fp == INVALID_HANDLE_VALUE) {
			sprintf(msg, "Cannot open %s\n", dbname);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		/* Allocate buffers and read files for autoloaded dbs. */
		if (hdat->dbfiles[i].autoload) {
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

			/* Allocate the subindices. */
			stat = init_autoload_subindices(hdat, &hdat->dbfiles[i], &size);
			if (stat)
				return(1);

			allocated_bytes += size;
			autoload_bytes += size;
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

			/* Init these datablock buffer indices to UNDEFINED_BLOCK_ID, means that data blocks are not loaded. */
			for (j = 0; j < hdat->dbfiles[i].num_datablocks; ++j)
				hdat->dbfiles[i].cache_bufferi[j] = UNDEFINED_BLOCK_ID;
		}
#endif
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
	int needed = needed_cache_buffers(hdat);
	if (needed > 0) {
		if ((allocated_bytes + MIN_CACHE_BUF_BYTES) / ONE_MB >= cache_mb) {

			/* We need more memory than he gave us, allocate 10mb of cache
			 * buffers if we can use that many.
			 */
			hdat->cacheblocks = std::min(MIN_CACHE_BUF_BYTES / CACHE_BLOCKSIZE, needed);
			sprintf(msg, "Allocating the minimum %d cache buffers\n", hdat->cacheblocks);
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
			hdat->cacheblocks = (int)(((int64_t)cache_mb * (int64_t)ONE_MB - (int64_t)allocated_bytes) / 
					(int64_t)(CACHE_BLOCKSIZE + sizeof(CCB)));
#endif
			hdat->cacheblocks = std::min(hdat->cacheblocks, needed);
		}

		/* Allocate the CCB array. */
		size = hdat->cacheblocks * sizeof(CCB);
		hdat->ccbs = (CCB *)malloc(size);
		if (!hdat->ccbs) {
			(*hdat->log_msg_fn)("Cannot allocate memory for ccbs\n");
			return(1);
		}

		/* Clear so we know which ones got used if we have to bail early. */
		memset(hdat->ccbs, 0, size);

		/* Init the lru list. */
		for (int i = 0; i < hdat->cacheblocks; ++i) {
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
		for (int i = 0; i < hdat->cacheblocks; i += CACHE_ALLOC_COUNT) {
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
		preload_cache_blocks(hdat);
	}
	else
		hdat->cacheblocks = 0;

	t3 = GetTickCount();
	sprintf(msg, "Egdb init took %d sec total\n", (t3 - t0) / 1000);
	(*hdat->log_msg_fn)(msg);
	std::sprintf(msg, "Available RAM: %smb\n", format_with_commas(get_mem_available_mb()).c_str());
	(*hdat->log_msg_fn)(msg);

	return(0);
}


/*
 * Parse an index file and write all information in subdb_table.
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
#ifdef BINARY_IDX_FILES

static bool get_nrecords(int64_t nbytes, size_t recordsize, size_t &nrecords)
{
	nrecords = (size_t)(nbytes / recordsize);
	return (nrecords * recordsize != nbytes);
}


static int parseindexfile(SLICE32_DATA *hdat, DBFILE *f, int64_t *allocated_bytes)
{
	char name[MAXFILENAME], basename[MAXFILENAME];
	char msg[MAXMSG];
	FILE *fp, *base_fp;
	uint32_t *indices;
	uint8_t *catalogidx;
	uint8_t *vmap;
	CPRSUBDB *dbpointer, *prev;
	int64_t filesize;
	HANDLE cprfp;
	subdb_list *dbp;

	/* Open the compressed data file. */
	sprintf(name, "%s%s.%s", hdat->db_filepath, f->name, hdat->wld_data_extension);
	cprfp = CreateFile((LPCSTR)name, GENERIC_READ, FILE_SHARE_READ,
					NULL, OPEN_EXISTING, 0, NULL);
	if (cprfp == INVALID_HANDLE_VALUE) {

		/* We can't find the compressed data file.  Its ok as long as 
		 * this is for more pieces than same_pieces_one_file pieces.
		 */
		if (f->pieces > hdat->same_pieces_one_file) {
			sprintf(msg, "%s not present\n", name);
			(*hdat->log_msg_fn)(msg);
			return(0);
		}
		else {
			sprintf(msg, "\nCannot open %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}
	}

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
		return(1);
	}
	Base *basep = (Base *)malloc((size_t)fsize);
	if (basep == nullptr) {
		sprintf(msg, "Cannot allocate memory for %s\n", basename);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	*allocated_bytes += fsize;

	size_t nsubdbs;
	if (get_nrecords(fsize, sizeof(Base), nsubdbs)) {
		sprintf(msg, "Fractional record read from %s\n", basename);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}

	size_t nread = fread(basep, sizeof(Base), nsubdbs, base_fp);
	if (nread != nsubdbs) {
		sprintf(msg, "Read %zu records from %s, expected %zu\n", nread, basename, nsubdbs);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	fclose(base_fp);
	sprintf(name, "%s%s.%s", hdat->db_filepath, f->name, hdat->idx_extension);
	fp = fopen(name, "rb");
	if (fp == 0) {
		sprintf(msg, "cannot open index file %s\n", name);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	status = file_size(name, fsize);
	if (status) {
		sprintf(msg, "Cannot get size of %s\n", name);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	size_t indexsize = sizeof(uint32_t);
	size_t catsize = sizeof(uint8_t);
	size_t vmapsize = sizeof(uint8_t);
	size_t num_index_records;
	if (get_nrecords(fsize, indexsize + catsize + vmapsize, num_index_records)) {
		sprintf(msg, "Fractional record in %s\n", name);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	indices = (uint32_t *)malloc(indexsize * num_index_records);
	catalogidx = (uint8_t *)malloc(catsize * num_index_records);
	vmap = (uint8_t *)malloc(vmapsize * num_index_records);
	if (indices == nullptr || catalogidx == nullptr || vmap == nullptr) {
		sprintf(msg, "Cannot allocate memory for index arrays for %s\n", name);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	*allocated_bytes += (indexsize + catsize + vmapsize) * num_index_records;

	nread = fread(indices, indexsize, num_index_records, fp);
	if (nread != num_index_records) {
		sprintf(msg, "Read %zu records from %s, expected %zu\n", nread, name, num_index_records);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	nread = fread(catalogidx, catsize, num_index_records, fp);
	if (nread != num_index_records) {
		sprintf(msg, "Read %zu records from %s, expected %zu\n", nread, name, num_index_records);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	nread = fread(vmap, vmapsize, num_index_records, fp);
	if (nread != num_index_records) {
		sprintf(msg, "Read %zu records from %s, expected %zu\n", nread, name, num_index_records);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}
	fclose(fp);

	prev = nullptr;
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
				return(1);
			}
			if (f->slices == nullptr)
				f->slices = new(std::vector<subdb_list *>);
			(*f->slices).push_back(dbp);
		}
		dbpointer = dbp->subdb + basep[i].subslicenum;
		dbpointer->num_idx_blocks = basep[i].num_idx_blocks;
		dbpointer->file = f;
		if (dbpointer->num_idx_blocks == 0)
			dbpointer->singlevalue = basep[i].singlevalue;
		else {
			dbpointer->startbyte = basep[i].startbyte;
			dbpointer->catalogidx = catalogidx + indices_offset;
			dbpointer->first_idx_block = basep[i].first_idx_block;
			dbpointer->indices = indices + indices_offset;
			dbpointer->singlevalue = NOT_SINGLEVALUE;
			dbpointer->vmap = vmap + indices_offset;
			if (indices_offset == 0)
				dbpointer->basenode = 1;
			indices_offset += basep[i].num_idx_blocks;

			/* Create the doubly linked list of subdbs.  We are excluding the subdbs that
			 * are all one value because we dont create subindices for those.
			 */
			dbpointer->prev = prev;
			dbpointer->next = nullptr;
			if (prev)
				prev->next = dbpointer;
			prev = dbpointer;

			/* Assign first_subidx_block and last_subidx_block. 
			 * This takes care of every subdb, except the very last subdb will not have
			 * its last_subidx_block field set.  Assign that one when we are done with
			 * this index file.
			 */
			dbpointer->first_subidx_block = dbpointer->startbyte / SUBINDEX_BLOCKSIZE;
			if (dbpointer->prev) {
				if ((dbpointer->startbyte % SUBINDEX_BLOCKSIZE) == 0) {
					if (dbpointer->first_subidx_block > 0)
						dbpointer->prev->last_subidx_block = dbpointer->first_subidx_block - 1;
					else
						dbpointer->prev->last_subidx_block = NUM_SUBINDICES - 1;
				}
				else
					dbpointer->prev->last_subidx_block = dbpointer->first_subidx_block;

				/* Check for special case of only 1 subidx block. */
				if (dbpointer->prev->num_idx_blocks == 1) {
					if (dbpointer->prev->first_subidx_block == dbpointer->prev->last_subidx_block)
						dbpointer->prev->single_subidx_block = 1;
				}
			}
		}
	}

	if (prev)
		prev->last_subidx_block = (unsigned char)(((filesize - 1) % IDX_BLOCKSIZE) / SUBINDEX_BLOCKSIZE);
	
	free(basep);
	sprintf(msg, "%10d index blocks: %s\n", f->num_datablocks, name);
	(*hdat->log_msg_fn)(msg);

	return(0);
}
#else
static int parseindexfile(SLICE32_DATA *hdat, DBFILE *f, __int64 *allocated_bytes)
{
	int stat0, stat;
	char name[MAXFILENAME];
	char msg[MAXMSG];
	FILE *fp;
	char c, colorchar;
	int bm, bk, wm, wk, subslicenum, color;
	int singlevalue, startbyte, ncatalog, nvmap;
	INDEX indices[IDX_READBUFSIZE];
	unsigned char vmap[IDX_READBUFSIZE];
	unsigned char catalogidx[IDX_READBUFSIZE];
	int i, count;
	INDEX block_index_start;
	int first_idx_block;
	CPRSUBDB *dbpointer, *prev;
	int size;
	int64_t filesize;
	HANDLE cprfp;
	subdb_list *dbp;

	/* Open the compressed data file. */
	sprintf(name, "%s%s.%s", hdat->db_filepath, f->name, hdat->wld_data_extension);
	cprfp = CreateFile(name, GENERIC_READ, FILE_SHARE_READ,
					NULL, OPEN_EXISTING, 0, NULL);
	if (cprfp == INVALID_HANDLE_VALUE) {

		/* We can't find the compressed data file.  Its ok as long as 
		 * this is for more pieces than SAME_PIECES_ONE_FILE pieces.
		 */
		if (f->pieces > hdat->same_pieces_one_file) {
			sprintf(msg, "%s not present\n", name);
			(*hdat->log_msg_fn)(msg);
			return(0);
		}
		else {
			sprintf(msg, "\nCannot open %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}
	}

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
	prev = 0;
	count = 0;
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

			if (f->slices == nullptr)
				f->slices = new(std::vector<subdb_list *>);
			(*f->slices).push_back(dbp);
		}
		dbpointer = dbp->subdb + subslicenum;

		/* Get the rest of the line.  It could be a n/n,n,n or it could just
		 * be a single character that is '+', '=', or '-'.
		 */
		stat = fgetc(fp);
		if (isdigit(stat)) {
			ungetc(stat, fp);
			stat0 = fscanf(fp, "%d/%d,%d,%d\n", &first_idx_block, &startbyte, &ncatalog, &nvmap);
			if (stat0 < 4) {
				stat = fscanf(fp, "%c\n", &c);
				if (stat < 1) {
					sprintf(msg, "Bad line in %s\n", name);
					(*hdat->log_msg_fn)(msg);
					return(1);
				}
			}

            dbpointer->first_idx_block = first_idx_block;       /* which block in db. */
			dbpointer->singlevalue = NOT_SINGLEVALUE;
			dbpointer->startbyte = startbyte;
			dbpointer->file = f;

			/* Create the doubly linked list of subdbs.  We are excluding the subdbs that
			 * are all one value because we dont create subindices for those.
			 */
			dbpointer->prev = prev;
			dbpointer->next = NULL;
			if (prev)
				prev->next = dbpointer;
			prev = dbpointer;

			/* Assign first_subidx_block and last_subidx_block. 
			 * This takes care of every subdb, except the very last subdb will not have
			 * its last_subidx_block field set.  Assign that one when we are done with
			 * this index file.
			 */
			dbpointer->first_subidx_block = dbpointer->startbyte / SUBINDEX_BLOCKSIZE;
			if (dbpointer->prev) {
				if ((dbpointer->startbyte % SUBINDEX_BLOCKSIZE) == 0) {
					if (dbpointer->first_subidx_block > 0)
						dbpointer->prev->last_subidx_block = dbpointer->first_subidx_block - 1;
					else
						dbpointer->prev->last_subidx_block = NUM_SUBINDICES - 1;
				}
				else
					dbpointer->prev->last_subidx_block = dbpointer->first_subidx_block;

				/* Check for special case of only 1 subidx block. */
				if (dbpointer->prev->num_idx_blocks == 1) {
					if (dbpointer->prev->first_subidx_block == dbpointer->prev->last_subidx_block)
						dbpointer->prev->single_subidx_block = 1;
				}
			}

			/* now, we got the first line, maybe there are more... 
			 * count the number of blocks in the db.
			 */
			count = 0;
			i = 1;
			indices[0] = 0;		/* first block is index 0. */
			catalogidx[0] = ncatalog;
			vmap[0] = nvmap;
			while (fscanf(fp, "%d,%d,%d\n", &block_index_start, &ncatalog, &nvmap) == 3) {
				indices[i] = block_index_start;
				catalogidx[i] = ncatalog;
				vmap[i] = nvmap;
				i++;

				/* If the local buffers are full then allocate more space
				 * and copy.
				 */
				if (i == IDX_READBUFSIZE) {
					dbpointer->indices = (INDEX *)realloc(dbpointer->indices, (count + IDX_READBUFSIZE) * sizeof(dbpointer->indices[0]));
					if (dbpointer->indices == NULL) {
						(*hdat->log_msg_fn)("malloc error for indices array!\n");
						return(1);
					}
					memcpy(dbpointer->indices + count, indices, IDX_READBUFSIZE * sizeof(dbpointer->indices[0]));

					dbpointer->catalogidx = (uint8_t *)realloc(dbpointer->catalogidx, (count + IDX_READBUFSIZE) * sizeof(dbpointer->catalogidx[0]));
					if (dbpointer->catalogidx == NULL) {
						(*hdat->log_msg_fn)("malloc error for catalogidx array!\n");
						return(1);
					}
					memcpy(dbpointer->catalogidx + count, catalogidx, IDX_READBUFSIZE * sizeof(dbpointer->catalogidx[0]));

					dbpointer->vmap = (uint8_t *)realloc(dbpointer->vmap, (count + IDX_READBUFSIZE) * sizeof(dbpointer->vmap[0]));
					if (dbpointer->vmap == NULL) {
						(*hdat->log_msg_fn)("malloc error for vmap array!\n");
						return(1);
					}
					memcpy(dbpointer->vmap + count, vmap, IDX_READBUFSIZE * sizeof(dbpointer->vmap[0]));

					i = 0;
					count += IDX_READBUFSIZE;
				}
			}

			/* We stopped reading numbers.  Resize the indices array and
			 * copy index numbers from the local buffer.
			 */
			if (i > 0) {
				dbpointer->indices = (INDEX *)realloc(dbpointer->indices, (count + i) * sizeof(dbpointer->indices[0]));
				if (dbpointer->indices == NULL) {
					(*hdat->log_msg_fn)("malloc error for indices array!\n");
					return(1);
				}
				memcpy(dbpointer->indices + count, indices, i * sizeof(dbpointer->indices[0]));

				dbpointer->catalogidx = (uint8_t *)realloc(dbpointer->catalogidx, (count + i) * sizeof(dbpointer->catalogidx[0]));
				if (dbpointer->catalogidx == NULL) {
					(*hdat->log_msg_fn)("malloc error for catalogidx array!\n");
					return(1);
				}
				memcpy(dbpointer->catalogidx + count, catalogidx, i * sizeof(dbpointer->catalogidx[0]));

				dbpointer->vmap = (uint8_t *)realloc(dbpointer->vmap, (count + i) * sizeof(dbpointer->vmap[0]));
				if (dbpointer->vmap == NULL) {
					(*hdat->log_msg_fn)("malloc error for vmap array!\n");
					return(1);
				}
				memcpy(dbpointer->vmap + count, vmap, i * sizeof(dbpointer->vmap[0]));
			}
			count += i;
			dbpointer->num_idx_blocks = count;
			size = count * (sizeof(dbpointer->indices[0]) + sizeof(dbpointer->catalogidx[0]) + sizeof(dbpointer->vmap[0]));
			*allocated_bytes += ROUND_UP(size, MALLOC_ALIGNSIZE);
		}
		else {
			switch (stat) {
			case '.':
				singlevalue = EGDB_UNKNOWN;
				break;
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
			dbpointer->indices = NULL;
			dbpointer->num_idx_blocks = 0;
			dbpointer->singlevalue = singlevalue;
			dbpointer->file = f;
		}
	}
	fclose(fp);

	if (prev) {
		prev->last_subidx_block = (unsigned char)(((filesize - 1) % IDX_BLOCKSIZE) / SUBINDEX_BLOCKSIZE);
	
		/* Check the total number of index blocks in this database. */
		if (f->num_datablocks != count + prev->first_idx_block) {
			sprintf(msg, "count of index blocks dont match: %d %d\n",
					f->num_datablocks, count + prev->first_idx_block);
			(*hdat->log_msg_fn)(msg);
		}
	}
	sprintf(msg, "%10d index blocks: %s\n", count + (prev == nullptr ? 0 : prev->first_idx_block), name);
	(*hdat->log_msg_fn)(msg);

	return(0);
}
#endif

/*
 * Build the table of db filenames.
 */
static void build_file_table(SLICE32_DATA *hdat)
{
	Slice slice(2);
	DBFILE file;

	memset(&file, 0, sizeof(file));
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


static int egdb_close_slice32(EGDB_DRIVER *handle)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
	int k;
	subdb_list *p;

	/* Free the cache buffers in groups of CACHE_ALLOC_COUNT at a time. */
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

		delete hdat->dbfiles[i].slices;
	}
#ifdef USE_HT
	hdat->hashtable.close();
#endif

	for (int i = 0; i < hdat->subdb_table.size(); ++i) {
		p = hdat->subdb_table.subslices_flat_view(i);
		if (p->subdb != NULL) {
			for (k = 0; k < p->num_subslices; ++k) {
#ifdef BINARY_IDX_FILES
				if (p->subdb[k].basenode && p->subdb[k].indices) {
					free(p->subdb[k].indices);
					p->subdb[k].indices = nullptr;
					free(p->subdb[k].vmap);
					p->subdb[k].vmap = nullptr;
					free(p->subdb[k].catalogidx);
					p->subdb[k].catalogidx = nullptr;
				}
#else
				if (p->subdb[k].indices)
					free(p->subdb[k].indices);
				if (p->subdb[k].vmap)
					free(p->subdb[k].vmap);
				if (p->subdb[k].catalogidx)
					free(p->subdb[k].catalogidx);
#endif
				if (p->subdb[k].autoload_subindices)
					free(p->subdb[k].autoload_subindices);
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
	int status;
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;

	status = 0;
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;

		status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->idx_extension, wld_12pc_crc, ARRAY_SIZE(wld_12pc_crc));
		status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->base_extension, wld_12pc_crc, ARRAY_SIZE(wld_12pc_crc));
		status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->wld_data_extension, wld_12pc_crc, ARRAY_SIZE(wld_12pc_crc));
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

} // namespace

EGDB_DRIVER *egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces,
				int cache_mb, const char *directory, void (*msg_fn)(const char *), EGDB_TYPE db_type)
{
	int status;
	EGDB_DRIVER *handle;

	if (db_type != EGDB_KINGSROW32_WLD_TUN_12PC) {
		(*msg_fn)("Invalid db type in egdb_open().\n");
		return(0);
	}
	handle = (EGDB_DRIVER *)calloc(1, sizeof(EGDB_DRIVER));
	if (!handle) {
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
#pragma warning(suppress : 4316)		// Can't do various aligns with "new".
	handle->internal_data = new tun_v3::SLICE32_DATA;
	if (!handle->internal_data) {
		free(handle);
		(*msg_fn)("Cannot allocate memory for driver handle.\n");
		return(0);
	}
	((tun_v3::SLICE32_DATA *)(handle->internal_data))->db_type = db_type;
	status = initdblookup((tun_v3::SLICE32_DATA *)handle->internal_data, pieces, cache_mb, directory, msg_fn);
	if (status) {
		tun_v3::egdb_close_slice32(handle);
		return(0);
	}
	if (bitboard_type == EGDB_NORMAL)
		/* This is a kingsrow bitboard. */
		handle->lookup = tun_v3::normal_lookup;
	else
		handle->lookup = (REV_ROW_LOOKUP_FNPTR)tun_v3::dblookup;
	
	handle->get_stats = tun_v3::get_db_stats;
	handle->reset_stats = tun_v3::reset_db_stats;
	handle->verify = tun_v3::verify_crc;
	handle->close = tun_v3::egdb_close_slice32;
	return(handle);
}



