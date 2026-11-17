#include <vector>
#include <utility>
#include "project.h"
#include "board.h"
#include "egdb.h"
#include "egdb_common.h"
#include "egdb_internal.h"
#include "rle_tables.h"
#include "bitboard_convert.h"
#include "format_with_commas.h"
#include "indexing.h"
#include "crc.h"
#include "SpinLock.h"
#include "slice.h"
#include "Huffman.h"
#include "Repair.h"
#include "timer.h"
#include "Packed_array.h"
#include "dtw_crcs.h"

namespace kr_dtw {

#define CACHE_MINIBLOCK_LENGTHS 0
const int miniblock_packed_bitlength = 17;

#define LOG_HITS 0

#define SAME_PIECES_ONE_FILE 5

#define IDX_BLOCKSIZE 4096
#define CACHE_BLOCKSIZE IDX_BLOCKSIZE

#define MAXLINE 256

#define MIN_CACHE_BUF_BYTES (10 * ONE_MB)

/* Allocate cache buffers CACHE_ALLOC_COUNT at a time. */
#define CACHE_ALLOC_COUNT 512


typedef unsigned int INDEX;

struct DBFILE {
	char is_present;
	char pieces;			/* number of pieces in this db file. */
	char max_pieces_1side;
	char name[20];			/* db filename prefix. */
	int num_datablocks;		/* number of cacheblock-sized data blocks in this db file. */
	HANDLE fp;
	int *cache_bufferi;		/* An array of indices into ccbs[], indexed by data block number. */
#if !CACHE_MINIBLOCK_LENGTHS
	FILE *fp_idx;			/* handle to index file. */
#endif
#if LOG_HITS
	int hits;
#endif
};

// definition of a structure for compressed databases
struct CPRSUBDB {
	CPRSUBDB() {
		num_idx_blocks = 0;
		first_idx_block = 0;
		first_miniblock = 0;
		file = nullptr;		/* stays nullptr if this subslice is not in the index file. */
#if !CACHE_MINIBLOCK_LENGTHS
		miniblock_lengths_offset = 0;
		miniblock_lengths_size = 0;
#endif
#if LOG_HITS
		hits = 0;
#endif
	};
	~CPRSUBDB() {};
	std::vector<Repair::Rule> repair_syms;
	std::vector<uint32_t> repair_lengths;
	std::vector<Huffcode> huffcodes;
	std::vector<Huffman::Lengthtable> lengthtable;
	std::vector<uint32_t> indices;
	Packed_array miniblock_lengths;
	int num_idx_blocks;				/* number of index blocks in this subdb. */
	int first_idx_block;			/* index block containing the first data value. */
	uint16_t first_miniblock;		/* number of first miniblock within first_idx_block. */
	DBFILE *file;					/* file info for this subdb */
#if !CACHE_MINIBLOCK_LENGTHS
	int miniblock_lengths_offset;	/* offset in the .idx file of the raw packed length data. */
	uint32_t miniblock_lengths_size;
#endif
#if LOG_HITS
	int hits;
#endif
};

struct subdb_list {
	CPRSUBDB *subdb;		/* an array of subdb pointers indexed by subslicenum. */
	int num_subslices;
	uint8_t permutation;	/* indexing permutation. */
};

/* cache control block type. */
struct CCB {
	int next;			/* index of next node */
	int prev;			/* index of previous node */
	int blocknum;			/* the block number within database file. */
	CPRSUBDB *subdb;		/* which subdb the block is for; there may be more than 1. */
	unsigned char *data;	/* data of this block. */
};

struct SLICE32_DATA {
	SLICE32_DATA() {
		memset(&lookup_stats, 0, sizeof(lookup_stats));
		db_type = EGDB_KINGSROW_DTW;	/* It might get over-written to Italian type. */
		db_filepath[0] = 0;
		maxpieces = 0;
		maxpiece = 0;
		same_pieces_one_file = 5;
		cacheblocks = 0;
		log_msg_fn = nullptr;
		ccbs = nullptr;
		ccbs_top = 0;
	}
	EGDB_TYPE db_type;				/* to allow for Italian type. */
	char db_filepath[MAXFILENAME];	/* Path to database files. */
	uint8_t maxpieces;
	uint8_t maxpiece;
	uint8_t same_pieces_one_file;
	int cacheblocks;				/* Total number of db cache blocks. */
	void (*log_msg_fn)(const char *);	/* for status and error messages. */
	CCB *ccbs;
	int ccbs_top;		/* index into ccbs[] of least recently used block. */
#if SMP
	SPINLOCK lock;
#endif
	std::vector<DBFILE> dbfiles;
	EGDB_STATS lookup_stats;
	dbaccess<subdb_list> subdb_table;
	const char *idx_extension;
	const char *dtw_data_extension;
	void log_msg(const char *fmt, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, fmt);

		if (fmt == NULL)
			return;

		vsprintf(buf, fmt, args);
		log_msg_fn(buf);
	}
};

const bool black_only = false;
const bool smallest_only = false;
const int miniblock_size = 512;
const int minis_per_block = IDX_BLOCKSIZE / miniblock_size;

/* Function prototypes. */
int parseindexfile(SLICE32_DATA *, DBFILE *, int64_t *allocated_bytes);
void delete_subslices(subdb_list *dbp);
void build_file_table(SLICE32_DATA *hdat);
void remove_larger_color(SLICE32_DATA *hdat);


static void reset_db_stats(EGDB_DRIVER *handle)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
#if LOG_HITS
	int i, k;
	subdb_list *p;

	for (i = 0; i < sizeof(hdat->dbfiles) / sizeof(hdat->dbfiles[0]); ++i) {
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
	for (i = 0; i < sizeof(hdat->dbfiles) / sizeof(hdat->dbfiles[0]); ++i) {
		f = &hdat->dbfiles[i];
		if (f->pieces > hdat->maxpieces)
			continue;

		if (!f->is_present)
			continue;
		count += f->hits;
	}

	for (i = 0; i < sizeof(hdat->dbfiles) / sizeof(hdat->dbfiles[0]); ++i) {
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
	for (pieces = SAME_PIECES_ONE_FILE; pieces <= hdat->maxpieces; ++pieces) {
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


/* Stub so the load_blocknum() templated function can be used. */ 
static void assign_subindices(SLICE32_DATA *hdat, CPRSUBDB *subdb, CCB *ccbp)
{
}


static void read_blocknum_from_file(SLICE32_DATA *hdat, CCB *ccb)
{
	I64_HIGH_LOW filepos;
	DWORD bytes_read;
	BOOL stat;

	filepos.word64 = (int64_t)ccb->blocknum * CACHE_BLOCKSIZE;

	/* Seek the start position. */
	if (SetFilePointer(ccb->subdb->file->fp, filepos.words32.low32, (long *)&filepos.words32.high32, FILE_BEGIN) == -1) {
		(*hdat->log_msg_fn)("\nseek failed\n");
		return;
	}
	stat = ReadFile(ccb->subdb->file->fp, ccb->data, CACHE_BLOCKSIZE, &bytes_read, NULL);
	if (!stat)
		(*hdat->log_msg_fn)("\nError reading file\n");
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
		if (hdat->dbfiles[i].pieces <= hdat->maxpieces)
			count += hdat->dbfiles[i].num_datablocks;

	return(count);
}


#if !CACHE_MINIBLOCK_LENGTHS
uint32_t get_miniblock_length(CPRSUBDB *subdb, int index)
{
	const int nbits_ = miniblock_packed_bitlength;
	const uint64_t mask_ = ((uint64_t)1 << nbits_) - 1;
	int bits_copied, bitoffset;
	size_t start;
	union bytes64 {
		uint64_t word64;
		uint8_t word8[8];
	};
	bytes64 temp;

	bitoffset = (index * (uint64_t)nbits_) & 7;
	start = (index * (uint64_t)nbits_) / 8;
	fseek(subdb->file->fp_idx, (int32_t)start + subdb->miniblock_lengths_offset, SEEK_SET);
	temp.word64 = 0;
	temp.word8[0] = fgetc(subdb->file->fp_idx);
	bits_copied = 8 - bitoffset;
	for (int i = 1; bits_copied < nbits_; ++i, bits_copied += 8)
		temp.word8[i] = fgetc(subdb->file->fp_idx);

	temp.word64 >>= bitoffset;
	temp.word64 &= mask_;
	return((uint32_t)temp.word64);
}
#endif

int decode(uint32_t target_index, uint8_t *datap, CPRSUBDB *subdb)
{
	int i, symidx;
	uint16_t repair_sym;
	uint32_t index;
	uint32_t *block;
	Words32_64 codebuf;
	uint64_t temp;
	int bits_in_codebuf, block_index;

	block_index = 0;
	block = (uint32_t *)datap;
	codebuf.word32[1] = block[block_index++];
	codebuf.word32[0] = block[block_index++];
	bits_in_codebuf = 64;
	index = 0;
	while (1) {
		/* re-fill the codebuf. */
		if (bits_in_codebuf < 32 && block_index < miniblock_size / sizeof(block[0])) {
			temp = block[block_index++];
			temp <<= (32 - bits_in_codebuf);
			codebuf.word64 |= temp;
			bits_in_codebuf += 32;
		}

		/* Find the bit length of the next code. */
		for (i = 0; codebuf.word32[1] < subdb->lengthtable[i].huffcode; ++i)
			;

		/* Get the re-pair symbol. */
		symidx = (codebuf.word32[1] - subdb->lengthtable[i].huffcode) >> (32 - subdb->lengthtable[i].codelength);
		symidx += subdb->lengthtable[i].codetable_index;

		repair_sym = subdb->huffcodes[symidx].value;
		if (index + subdb->repair_lengths[repair_sym] > target_index)
			break;			/* Found the repair symbol containing the target value. */

		index += subdb->repair_lengths[repair_sym];
		codebuf.word64 <<= subdb->lengthtable[i].codelength;
		bits_in_codebuf -= subdb->lengthtable[i].codelength;
	}

	/* Find the symbol within repair_sym tree that corresponds to target_index. */
	while (subdb->repair_lengths[repair_sym] > 1) {
		uint16_t child_sym = subdb->repair_syms[repair_sym].left;
		if (index + subdb->repair_lengths[child_sym] > target_index)
			repair_sym = child_sym;
		else {
			index += subdb->repair_lengths[child_sym];
			repair_sym = subdb->repair_syms[repair_sym].right;
		}
	}

	return(repair_sym);
}


/*
 * Returns a dtw depth, EGDB_SUBDB_UNAVAILABLE, or EGDB_NOT_IN_CACHE
 * The depth returned is the real depth divided by 2. To convert to the actual depth,
 * multiply by 2, and add 1 if the position is a win.
 * If the value is not already in a cache buffer, the action depends on
 * the argument cl.  If cl is true, DB_NOT_IN_CACHE is returned, 
 * otherwise the disk block is read and cached and the value is obtained.
 */
static int dblookup(EGDB_DRIVER *handle, EGDB_ROW_REVERSED_BITBOARD *p, int color, int cl)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
	UINT32 index;
	int64_t index64;
	int bm, bk, wm, wk;
	int subslicenum;
	int idx_blocknum;	
	int cache_blocknum;
	INDEX *indices;
	EGDB_ROW_REVERSED_BITBOARD revpos;
	subdb_list *dbp;
	CPRSUBDB *dbpointer;
	Timer_hires timer;
	double tdiff;
	const bool benchmarks = false;

	if (benchmarks)
		timer.reset();

	/* Start tracking db stats here. */
	++hdat->lookup_stats.db_requests;

	/* set bm, bk, wm, wk. */
	bm = bitcount(p->black_man);
	wm = bitcount(p->white_man);
	bk = bitcount(p->black_king);
	wk = bitcount(p->white_king);

	/* if one side has nothing, return depth 0 */
	if ((bm + bk) == 0) {
		++hdat->lookup_stats.db_returns;
		return(0);
	}
	if ((wm + wk) == 0) {
		++hdat->lookup_stats.db_returns;
		return(0);
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

	if (black_only) {
		if (color == EGDB_WHITE)
			return EGDB_SUBDB_UNAVAILABLE;
	}

	/* get pointer to db. */
	dbp = hdat->subdb_table.subslices(bm, bk, wm, wk, color);
	dbpointer = dbp->subdb;

	/* check presence. */
	if (dbpointer == 0) {
		++hdat->lookup_stats.db_not_present_requests;
		return(EGDB_SUBDB_UNAVAILABLE);
	}

	index64 = position_to_index_slice(p, bm, bk, wm, wk, dbp->permutation);
	get_subindices(index64, subslicenum, index, DTW_SUBSLICE_INDICES);

	dbpointer += subslicenum;

	/* This subslice was not found in the index file. */
	if (dbpointer->file == nullptr) {
		++hdat->lookup_stats.db_not_present_requests;
		return(EGDB_SUBDB_UNAVAILABLE);
	}

	if (benchmarks) {
		tdiff = timer.elapsed_usec();
		hdat->log_msg("timer: indexing %.2f usec\n", tdiff);
		timer.reset();
	}

#if LOG_HITS
	++dbpointer->file->hits;
	++dbpointer->hits;
#endif

	int ccbi;
	CCB *ccbp;
	uint32_t first_miniblock;

	/* We know the index and the database, so look in 
	 * the indices array to find the right index block.
	 */
	indices = &dbpointer->indices[0];
	idx_blocknum = find_block(0, dbpointer->num_idx_blocks, indices, index);

	/* See if blocknumber is already in cache. */
	cache_blocknum = dbpointer->first_idx_block + idx_blocknum;
	if (benchmarks) {
		tdiff = timer.elapsed_usec();
		hdat->log_msg("timer: find_block %.2f usec\n", tdiff);
	}

	/* Is this block already cached? */
#if SMP
	AcquireSpinLock(hdat->lock);
#endif
	ccbi = dbpointer->file->cache_bufferi[cache_blocknum];
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
		ccbp = load_blocknum<CCB>(hdat, dbpointer, cache_blocknum, CACHE_BLOCKSIZE);
		if (ccbp == NULL) {
#if SMP
			ReleaseSpinLock(hdat->lock);
#endif
			return(EGDB_UNKNOWN);
		}
	}

	if (benchmarks)
		timer.reset();

	/* Find the first miniblock in idx_blocknum. */
	if (idx_blocknum == 0)
		first_miniblock = dbpointer->first_miniblock;
	else
		first_miniblock = minis_per_block * idx_blocknum;

	/* Do a linear search to find the miniblock that has the position. */
	uint32_t base_index = dbpointer->indices[idx_blocknum];
	uint32_t tablei = first_miniblock - dbpointer->first_miniblock;
#if !CACHE_MINIBLOCK_LENGTHS
		for ( ; tablei < dbpointer->miniblock_lengths_size; ++tablei) {
			int length = get_miniblock_length(dbpointer, tablei);
			if (base_index + length > index)
				break;

			base_index += length;
		}
#else
		for ( ; tablei < dbpointer->miniblock_lengths.size(); ++tablei) {
			if (base_index + dbpointer->miniblock_lengths[tablei] > index)
				break;

			base_index += dbpointer->miniblock_lengths[tablei];
		}
		assert(tablei < dbpointer->miniblock_lengths.size());
#endif
	uint8_t *datap;
	int retval;
	datap = ccbp->data + ((tablei + dbpointer->first_miniblock) % minis_per_block) * miniblock_size;
	retval = decode(index - base_index, datap, dbpointer);
	if (benchmarks) {
		tdiff = timer.elapsed_usec();
		hdat->log_msg("timer: decode %.2f usec\n", tdiff);
	}

#if SMP
	ReleaseSpinLock(hdat->lock);
#endif

	return(retval);
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
	int i, j, stat;
	int t0, t1, t3;
	char dbname[MAXFILENAME];
	char msg[MAXMSG];
	int64_t allocated_bytes;		/* keep track of heap allocations in bytes. */
	int cache_mb_avail;
	int64_t total_dbsize;
	SIZE_T size;
	int count;
	unsigned char *blockp;		/* Base address of an allocate group of cache buffers. */

	hdat->idx_extension = "idx_dtw";
	hdat->dtw_data_extension = "cpr_dtw";
	t0 = GetTickCount();

	/* Save off some global data. */
	strcpy(hdat->db_filepath, filepath);

	/* Add trailing backslash if needed. */
	size = (int)strlen(hdat->db_filepath);
	if (size && (hdat->db_filepath[size - 1] != '\\'))
		strcat(hdat->db_filepath, "\\");

	hdat->maxpieces = pieces;
	hdat->maxpiece = 5;
	hdat->same_pieces_one_file = 5;

	hdat->log_msg_fn = msg_fn;
	allocated_bytes = 0;

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

#if SMP
	ResetSpinLock(hdat->lock);
#endif

	/* Build table of db filenames. */
	build_file_table(hdat);

	/* Parse index files. */
	try {
		for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {

			/* Dont do more pieces than he asked for. */
			if (hdat->dbfiles[i].pieces > pieces)
				break;

			stat = parseindexfile(hdat, &hdat->dbfiles[i], &allocated_bytes);

			if (stat)
				return(1);
		}
	} catch (std::bad_alloc &e) {
		const char *p = e.what();	/* make the compiler happy. */
		hdat->log_msg("Out of memory reading index files.\n");
		return(1);
	} catch (...) {
		hdat->log_msg("Error reading index files.\n");
		return(1);
	}

	/* End of reading index files. */
	t1 = GetTickCount();
	sprintf(msg, "Reading index files took %d secs\n", (t1 - t0) / 1000);
	(*hdat->log_msg_fn)(msg);

	/* Find the total size of all the files that will be used. */
	total_dbsize = 0;
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {
		if (hdat->dbfiles[i].is_present)
			total_dbsize += (int64_t)hdat->dbfiles[i].num_datablocks * CACHE_BLOCKSIZE;
	}

	cache_mb_avail = (int)(cache_mb - allocated_bytes / ONE_MB);
	if (cache_mb_avail < 15)
		cache_mb_avail = 15;

	/* Open file pointers for each db: keep open all the time. */
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {

		/* Dont do more pieces than he asked for. */
		if (hdat->dbfiles[i].pieces > pieces)
			break;

		if (!hdat->dbfiles[i].is_present)
			continue;

		sprintf(dbname, "%s%s.cpr_dtw", hdat->db_filepath, hdat->dbfiles[i].name);
		hdat->dbfiles[i].fp = CreateFile((LPCSTR)dbname, GENERIC_READ, FILE_SHARE_READ,
						NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY,
						NULL);
		if (hdat->dbfiles[i].fp == INVALID_HANDLE_VALUE) {
			sprintf(msg, "Cannot open %s\n", dbname);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		/* Allocate the array of indices into ccbs[].
		 * Each array entry is either an index into ccbs or -1 if that block is not loaded.
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
	sprintf(msg, "Allocated %dkb for indexing\n", (int)((allocated_bytes) / 1024));
	(*hdat->log_msg_fn)(msg);

	/* Figure out how much ram is left for lru cache buffers. */
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
			hdat->cacheblocks = (int)(((int64_t)cache_mb * (int64_t)ONE_MB - (int64_t)allocated_bytes) / 
					(int64_t)(CACHE_BLOCKSIZE + sizeof(CCB)));
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

		t3 = GetTickCount();
		sprintf(msg, "Egdb init took %d sec total\n", (t3 - t0) / 1000);
		(*hdat->log_msg_fn)(msg);
	}
	else
		hdat->cacheblocks = 0;

	std::sprintf(msg, "Available RAM: %smb\n", format_with_commas(get_mem_available_mb()).c_str());
	(*hdat->log_msg_fn)(msg);

	if (smallest_only) {
		remove_larger_color(hdat);
	}

	return(0);
}


/*
 * Parse an index file and write all information in subdb_table.
 * A nonzero return value means some kind of error occurred.
 *
 * Index file layout (subdb header [subdb header, ...])		
 *
 * subdb header: (uint8 npieces, uint8 nbm, uint8 nbk, uint8 nwm, uint8 nwk, uint8 color, uint16 subslicenum)
 * number of repair symbols (uint16_t)
 * nsyms repair symbol pairs (uint16_t left, uint16_t right)
 * number of entries in Huffcode table (uint16_t)
 * n (uint16_t value, uint8_t length) pairs
 * <4k start blocknum> (uint32_t)
 * <miniblock num within start block>  0 .. 31 (uint16_t)
 * nblocks, the number of 4k cache blocks in the subdb (uint32_t)
 * nblocks start indexes (uint32_t)
 * nminiblocks, the number of 128b miniblocks in the subdb (uint32_t)
 * nminiblocks sizes, npositions in each miniblock (uint16_t)
 */
static int parseindexfile(SLICE32_DATA *hdat, DBFILE *f, int64_t *allocated_bytes)
{
	char name[MAXFILENAME];
	char msg[MAXMSG];
	FILE *fp;
	uint8_t nbm, nbk, nwm, nwk, color;
	uint16_t subslice_num;
	uint8_t subslice[6];
	int total_minis;
	CPRSUBDB *dbpointer, *prev;
	int64_t filesize;
	HANDLE cprfp;
	subdb_list *dbp;
	const bool verbose = false;

	/* Open the compressed data file. */
	sprintf(name, "%s%s.%s", hdat->db_filepath, f->name, hdat->dtw_data_extension);
	cprfp = CreateFile(name, GENERIC_READ, FILE_SHARE_READ,
					NULL, OPEN_EXISTING, 0, NULL);
	if (cprfp == INVALID_HANDLE_VALUE) {

		/* We can't find the compressed data file.  Its ok as long as 
		 * this is for more pieces than SAME_PIECES_ONE_FILE pieces.
		 */
		if (f->pieces > SAME_PIECES_ONE_FILE) {
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
	fp = fopen(name, "rb");
	if (fp == 0) {
		sprintf(msg, "cannot open index file %s\n", name);
		(*hdat->log_msg_fn)(msg);
		return(1);
	}

	f->is_present = 1;
#if !CACHE_MINIBLOCK_LENGTHS
	f->fp_idx = fp;
#endif
	prev = 0;
	total_minis = 0;
	while (1) {

		/* Read the subslice info: 
		 * (uint8 npieces, uint8 nbm, uint8 nbk, uint8 nwm, uint8 nwk, uint8 color, uint16 subslicenum)
		 */
		if (fread(&subslice, sizeof(subslice), 1, fp) != 1)
			break;			/* Done if end of file here. */
		if (fread(&subslice_num, sizeof(subslice_num), 1, fp) != 1) {
			sprintf(msg, "Error reading subslice data %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}
		uint8_t permutation;
		if (fread(&permutation, sizeof(permutation), 1, fp) != 1) {
			sprintf(msg, "Error reading permutation %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		nbm = subslice[1];
		nbk = subslice[2];
		nwm = subslice[3];
		nwk = subslice[4];
		color = subslice[5];
		if (subslice[0] != nbm + nbk + nwm + nwk) {
			sprintf(msg, "Index file %s header inconsistent\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		/* Get the subdb node. */
		dbp = hdat->subdb_table.subslices(nbm, nbk, nwm, nwk, color);

		/* If we have not allocated a subslice table for this subdb, then
		 * do it now.
		 */
		if (!dbp->subdb) {
			dbp->num_subslices = get_num_subslices(nbm, nbk, nwm, nwk, DTW_SUBSLICE_INDICES);
			dbp->permutation = permutation;

			/* Indexing by black men index uses a large table that would otherwise be built
			 * during the first lookup, without a lock. Build it now.
			 */
			if (permutation & men_index_by_bmindex)
				*allocated_bytes += man_index_monotonic::init();
			dbp->subdb = new CPRSUBDB[dbp->num_subslices];
			*allocated_bytes += dbp->num_subslices * sizeof(CPRSUBDB);
			if (!dbp->subdb) {
				(*hdat->log_msg_fn)("Cannot allocate subslice subdb\n");
				return(1);
			}
		}
		dbpointer = dbp->subdb + subslice_num;
		dbpointer->file = f;

		/* Read the repair data. */
		uint16_t nsyms;
		if (fread(&nsyms, sizeof(nsyms), 1, fp) != 1) {
			sprintf(msg, "Error in %s reading nsyms\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		dbpointer->repair_syms.reserve(nsyms);
		for (size_t i = 0; i < nsyms; ++i) {
			Repair::Rule rpsym;

			if (fread(&rpsym.left, sizeof(rpsym.left), 1, fp) != 1) {
				sprintf(msg, "Error reading rp syms from %s\n", name);
				(*hdat->log_msg_fn)(msg);
				return(1);
			}
			if (fread(&rpsym.right, sizeof(rpsym.right), 1, fp) != 1) {
				sprintf(msg, "Error reading rp syms from %s\n", name);
				(*hdat->log_msg_fn)(msg);
				return(1);
			}
			dbpointer->repair_syms.push_back(rpsym);
		}
		*allocated_bytes += dbpointer->repair_syms.size() * sizeof(dbpointer->repair_syms[0]);

		get_symbol_lengths(dbpointer->repair_syms, dbpointer->repair_lengths);
		*allocated_bytes += dbpointer->repair_lengths.size() * sizeof(dbpointer->repair_lengths[0]);

		/* Read the Huffman data. */
		if (fread(&nsyms, sizeof(nsyms), 1, fp) != 1) {
			sprintf(msg, "Error in %s reading nsyms\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}
		dbpointer->huffcodes.reserve(nsyms);
		for (size_t i = 0; i < nsyms; ++i) {
			Huffcode huffcode;

			huffcode.huffcode = 0;
			if (fread(&huffcode.value, sizeof(huffcode.value), 1, fp) != 1) {
				sprintf(msg, "Error reading huffcodes from %s\n", name);
				(*hdat->log_msg_fn)(msg);
				return(1);
			}
			if (fread(&huffcode.codelength, sizeof(huffcode.codelength), 1, fp) != 1) {
				sprintf(msg, "Error reading huffcodes from %s\n", name);
				(*hdat->log_msg_fn)(msg);
				return(1);
			}
			dbpointer->huffcodes.push_back(huffcode);
		}
		*allocated_bytes += dbpointer->huffcodes.size() * sizeof(dbpointer->huffcodes[0]);

		generate_codes(dbpointer->huffcodes);
		build_length_table(dbpointer->huffcodes, dbpointer->lengthtable);
		*allocated_bytes += dbpointer->lengthtable.size() * sizeof(dbpointer->lengthtable[0]);

		/* Read the start blocknum. */
		if (fread(&dbpointer->first_idx_block, sizeof(dbpointer->first_idx_block), 1, fp) != 1) {
			sprintf(msg, "Error reading start blocknum from %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		if (fread(&dbpointer->first_miniblock, sizeof(dbpointer->first_miniblock), 1, fp) != 1) {
			sprintf(msg, "Error reading first miniblock num from %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}

		/* Read the number of index blocks. */
		if (fread(&dbpointer->num_idx_blocks, sizeof(dbpointer->num_idx_blocks), 1, fp) != 1) {
			sprintf(msg, "Error reading num blocks from %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}
		dbpointer->indices.reserve(dbpointer->num_idx_blocks);
		*allocated_bytes += dbpointer->num_idx_blocks * sizeof(dbpointer->indices[0]);

		/* Read the number of miniblocks. */
		uint32_t nminis;
		if (fread(&nminis, sizeof(nminis), 1, fp) != 1) {
			sprintf(msg, "Error reading num mini blocks from %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}
		total_minis += nminis;
		dbpointer->miniblock_lengths = Packed_array(miniblock_packed_bitlength);
		dbpointer->miniblock_lengths.resize(nminis);
#if !CACHE_MINIBLOCK_LENGTHS
		dbpointer->miniblock_lengths_offset = ftell(dbpointer->file->fp_idx);
		dbpointer->miniblock_lengths_size = nminis;
#endif
		size_t bytes_read = fread(dbpointer->miniblock_lengths.raw_buf(), 1, dbpointer->miniblock_lengths.raw_size(), fp);
		if (bytes_read != dbpointer->miniblock_lengths.raw_size()) {
			sprintf(msg, "Error reading miniblock data from %s\n", name);
			(*hdat->log_msg_fn)(msg);
			return(1);
		}
#if CACHE_MINIBLOCK_LENGTHS
		*allocated_bytes += dbpointer->miniblock_lengths.raw_size();
#endif

		/* Write the block indexes using the miniblock lengths. */
		{
			uint32_t index, count;
			index = 0;
			dbpointer->indices.push_back(0);
			count = 1;
			for (size_t i = 1; i < dbpointer->miniblock_lengths.size(); ++i) {
				index += dbpointer->miniblock_lengths[i - 1];
				if (((i + dbpointer->first_miniblock) % minis_per_block) == 0) {
					dbpointer->indices.push_back(index);
					++count;
				}
			}
			assert(count == dbpointer->indices.size());
		}
		if (verbose)
			hdat->log_msg("db%d-%d%d%d%d-%d%c:\n"
					"\tfirst idx block %d\n"
					"\tfirst miniblock %d\n"
					"\tnum idx blocks %d\n"
					"\tnum miniblocks %zd\n",
			subslice[0], nbm, nbk, nwm, nwk, subslice_num, color == EGDB_BLACK ? 'b' : 'w',
				dbpointer->first_idx_block, dbpointer->first_miniblock, 
				dbpointer->num_idx_blocks, dbpointer->miniblock_lengths.size());
#if !CACHE_MINIBLOCK_LENGTHS
		dbpointer->miniblock_lengths.clear();
		dbpointer->miniblock_lengths.shrink_to_fit();
#endif
	}

#if CACHE_MINIBLOCK_LENGTHS
		fclose(fp);
#endif

	int blocks = (total_minis % minis_per_block) ? 1 + total_minis / minis_per_block : total_minis / minis_per_block;
	sprintf(msg, "%10d index blocks: %s\n", blocks, name);
	(*hdat->log_msg_fn)(msg);

	return(0);
}


/*
 * Build the table of db filenames.
 */
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


static int egdb_close_slice32(EGDB_DRIVER *handle)
{
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;
	int i;
	subdb_list *p;

	/* Free the cache buffers in groups of CACHE_ALLOC_COUNT at a time. */
	if (hdat->ccbs) {
		for (i = 0; i < hdat->cacheblocks; i += CACHE_ALLOC_COUNT) {
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

		if (hdat->dbfiles[i].cache_bufferi) {
			free(hdat->dbfiles[i].cache_bufferi);
			hdat->dbfiles[i].cache_bufferi = 0;
		}

		if (hdat->dbfiles[i].fp != INVALID_HANDLE_VALUE)
			CloseHandle(hdat->dbfiles[i].fp);
#if !CACHE_MINIBLOCK_LENGTHS

		/* The index file is opened before the data file, so after a failed open
		 * fp_idx can be open while fp is not.
		 */
		if (hdat->dbfiles[i].fp_idx)
			fclose(hdat->dbfiles[i].fp_idx);
#endif
	}
	hdat->dbfiles.clear();
	hdat->dbfiles.shrink_to_fit();

	for (i = 0; i < hdat->subdb_table.size(); ++i) {
		p = hdat->subdb_table.subslices_flat_view(i);
		if (p->subdb != NULL)
			delete_subslices(p);
	}
	hdat->subdb_table.close();
	delete hdat;
	free(handle);
	return(0);
}


static int verify_crc_stem(SLICE32_DATA *hdat, const char *stem, const char *ext, const DBCRC *crctbl, int crcsize) 
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


static int verify_crc(EGDB_DRIVER *handle)
{
	int status;
	SLICE32_DATA *hdat = (SLICE32_DATA *)handle->internal_data;

	status = 0;
	for (size_t i = 0; i < hdat->dbfiles.size(); ++i) {
		if (hdat->dbfiles[i].pieces > hdat->maxpieces)
			continue;

		if (hdat->db_type == EGDB_KINGSROW_DTW) {
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->idx_extension, english_dtw_crc, ARRAY_SIZE(english_dtw_crc));
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->dtw_data_extension, english_dtw_crc, ARRAY_SIZE(english_dtw_crc));
		}
		else {
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->idx_extension, italian_dtw_crc, ARRAY_SIZE(italian_dtw_crc));
			status |= verify_crc_stem(hdat, hdat->dbfiles[i].name, hdat->dtw_data_extension, italian_dtw_crc, ARRAY_SIZE(italian_dtw_crc));
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
		egdb_close_slice32(handle);
		return(0);
	}
	if (bitboard_type == EGDB_NORMAL) {

		if (db_type == EGDB_KINGSROW_DTW) {
			/* This is a kingsrow bitboard. */
			handle->lookup = normal_lookup;
		}
		else
			handle->lookup = normal_lookup_italian;
	}
	else {
		if (db_type == EGDB_KINGSROW_DTW)
			handle->lookup = (REV_ROW_LOOKUP_FNPTR)dblookup;
		else {
			handle->lookup = reversed_lookup_italian;
		}
	}
	
	handle->get_stats = get_db_stats;
	handle->reset_stats = reset_db_stats;
	handle->verify = verify_crc;
	handle->close = egdb_close_slice32;
	return(handle);
}


int get_minis(subdb_list *dbp)
{
	int count = 0;

	if (dbp == nullptr)
		return(0);

	for (int i = 0; i < dbp->num_subslices; ++i)
		count += (int)dbp->subdb[i].miniblock_lengths.size();

	return(count);
}


void delete_subslices(subdb_list *dbp)
{
	for (int i = 0; i < dbp->num_subslices; ++i) {
		dbp->subdb[i].huffcodes.clear();
		dbp->subdb[i].huffcodes.shrink_to_fit();
		dbp->subdb[i].indices.clear();
		dbp->subdb[i].indices.shrink_to_fit();
		dbp->subdb[i].lengthtable.clear();
		dbp->subdb[i].lengthtable.shrink_to_fit();
		dbp->subdb[i].miniblock_lengths.clear();
		dbp->subdb[i].miniblock_lengths.shrink_to_fit();
		dbp->subdb[i].repair_lengths.clear();
		dbp->subdb[i].repair_lengths.shrink_to_fit();
		dbp->subdb[i].repair_syms.clear();
		dbp->subdb[i].repair_syms.shrink_to_fit();
	}
	delete[] dbp->subdb;
	dbp->subdb = nullptr;
}


void remove_larger_color(SLICE32_DATA *hdat)
{
	int black_minis, white_minis;
	int black_only_total, smallest_total;
	Slice slice(2);
	subdb_list *dbp_black, *dbp_white;

	black_only_total = 0;
	smallest_total = 0;
	for (slice = Slice(2); slice.npieces() <= hdat->maxpieces; slice.increment()) {
		if (slice.nb() > hdat->maxpiece)
			continue;

		dbp_black = hdat->subdb_table.subslices(slice.nbm(), slice.nbk(), slice.nwm(), slice.nwk(), EGDB_BLACK);
		black_minis = get_minis(dbp_black);
		dbp_white = hdat->subdb_table.subslices(slice.nbm(), slice.nbk(), slice.nwm(), slice.nwk(), EGDB_WHITE);
		white_minis = get_minis(dbp_white);

		/* Get the subdb node. */
		black_only_total += black_minis;
		if (black_minis > 0 && white_minis > 0) {

			/* Delete the larger color. */
			if (black_minis > white_minis) {
				smallest_total += white_minis;
				delete_subslices(dbp_black);
			}
			else {
				smallest_total += black_minis;
				delete_subslices(dbp_white);
			}
		}
		else
			smallest_total += black_minis;
	}
	printf("black only %I64d mb\n", ((int64_t)black_only_total * miniblock_size) / ONE_MB);
	printf("smallest sizes %I64d\n", ((int64_t)smallest_total * miniblock_size) / ONE_MB);
}

}	// namespace
