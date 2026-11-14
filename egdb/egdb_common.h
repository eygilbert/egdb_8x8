#pragma once
#define NOMINMAX
#include <stdint.h>
#include <windows.h>
#include <algorithm>
#include <new>
#include <exception>
#include "project.h"
#include "egdb.h"

#define NUMBEROFCOLORS 2
#define UNDEFINED_BLOCK_ID -1
#define MAXFILENAME MAX_PATH

/* The longest database directory accepted by egdb_open() and egdb_identify().
 * The drivers build filenames in MAXFILENAME buffers as directory + separator +
 * file name (at most 19 chars) + '.' + extension, so leave room for those.
 */
#define MAX_DB_DIRECTORY_LEN (MAXFILENAME - 64)
#define MALLOC_ALIGNSIZE 16
#define IDX_READBUFSIZE 20000

/* Round a value 'a' up to the next integer multiple of blocksize 's'. */
#define ROUND_UP(a, s) (((a) % (s)) ? ((1 + (a) / (s)) * (s)) : (a))
#define LOWORD32(x) (uint32_t)((x) & 0xffffffff)

/* If he didn't give us enough memory for at least MIN_CACHE_BUF_BYTES of
 * buffers, then use that much.
 */
#define ONE_MB 1048576

/* Upper and lower words of a 64-bit int, for large file access. */
typedef union {
	int64_t word64;
	struct {
		unsigned long low32;
		unsigned long high32;
	} words32;
} I64_HIGH_LOW;

int read_file(HANDLE fp, unsigned char *buf, SIZE_T size, int pagesize);
int read_cache_block(HANDLE fp, int64_t filepos, unsigned char *buf, int cache_blocksize, void (*log_msg_fn)(const char *));
bool is_wld(EGDB_TYPE type);
bool is_dtw(EGDB_TYPE type);
bool is_mtc(EGDB_TYPE type);


inline int needs_reversal(int nbm, int nbk, int nwm, int nwk, int color)
{
	if (nwm + nwk > nbm + nbk)
		return(true);

	if (nwm + nwk == nbm + nbk) {
		if (nwk > nbk)
			return(true);
		if (nbm == nwm && nbk == nwk)
			if (color == EGDB_WHITE)
				return(true);
	}
	return(false);
}


inline int needs_reversal(int nbm, int nbk, int nwm, int nwk, int bmrank, int wmrank, int color)
{
	if (nwm + nwk > nbm + nbk)
		return(true);

	if (nwm + nwk == nbm + nbk) {
		if (nwk > nbk)
			return(true);
		if (nbm == nwm && nbk == nwk) {
			if (wmrank > bmrank)
				return(true);
			if (wmrank == bmrank) {
				if (color == EGDB_WHITE)
					return(true);
			}
		}
	}
	return(false);
}


/*
 * Do a binary search to find the block number that contains the target index.
 * block_starts[] has the first index in each block[i].
 * begin is the index into block_starts[] of the first block to search.
 * end is index into block_starts[] of one past the last block to search.
 */
template <class T_index>
inline int find_block(int first, int last, T_index block_starts[], T_index target)
{
	int mid;

	while (last > first + 1) {
		mid = first + (last - first) / 2;
		if (block_starts[mid] <= target)
			first = mid;
		else
			last = mid;
	}
	return(first);
}


template <class DBCRC_T> 
const DBCRC_T *find_file_crc(char const *name, const DBCRC_T *table, int size)
{
	int i;

	for (i = 0; i < size; ++i)
		if (!_stricmp(name, table[i].filename))
			return(table + i);

	return(0);
}


/*
 * Read the block given by ccb->blocknum from ccb's file into ccb->data.
 * Return 1 on success, 0 on error.
 */
template <class CCB_T, class DBHANDLE_T>
int read_block_from_file(DBHANDLE_T *hdat, CCB_T *ccb, int cache_blocksize)
{
	return(read_cache_block(ccb->subdb->file->fp, (int64_t)ccb->blocknum * cache_blocksize,
					ccb->data, cache_blocksize, hdat->log_msg_fn));
}


/*
 * Return a pointer to a cache block.
 * Get the least recently used cache
 * block and load it into that.
 * Update the LRU list to make this block the most recently used.
 * Return NULL if the block cannot be read. In that case the block is not cached.
 */
template <class CCB_T, class DBHANDLE_T, class CPRSUBDB_T>
CCB_T *load_blocknum(DBHANDLE_T *hdat, CPRSUBDB_T *subdb, int blocknum, int cache_blocksize)
{
	int old_blocknum;
	CCB_T *ccbp;

	++hdat->lookup_stats.lru_cache_loads;

	/* Not cached, need to load this block from disk. */
	ccbp = hdat->ccbs + hdat->ccbs_top;
	if (ccbp->blocknum != UNDEFINED_BLOCK_ID) {

		/* The LRU block is in use.
		 * Unhook this block from the subdb that was using it.
		 */
		old_blocknum = ccbp->blocknum;
		ccbp->subdb->file->cache_bufferi[old_blocknum] = UNDEFINED_BLOCK_ID;
	}

	/* The ccbs_top block is now free for use.
	 * Read this block from disk into it.
	 */
	ccbp->subdb = subdb;
	ccbp->blocknum = blocknum;
	if (!read_block_from_file(hdat, ccbp, cache_blocksize)) {

		/* Leave this ccb unused. It stays at the top of the lru list, so the next load reuses it. */
		ccbp->blocknum = UNDEFINED_BLOCK_ID;
		return(NULL);
	}

	/* The read succeeded, so now it is safe to map the block to this ccb. */
	subdb->file->cache_bufferi[blocknum] = hdat->ccbs_top;

	assign_subindices(hdat, subdb, ccbp);

	/* Fix linked list to point to the next oldest entry */
	hdat->ccbs_top = hdat->ccbs[hdat->ccbs_top].next;
	return(ccbp);
}


/*
 * Read the block given by ccb->blocknum from ccb's file into ccb->data.
 * Return 1 on success, 0 on error.
 */
template <class CCB_T, class DBHANDLE_T>
int read_block_from_file_v2(DBHANDLE_T *hdat, CCB_T *ccb, int cache_blocksize)
{
	return(read_cache_block(ccb->dbfile->fp, (int64_t)ccb->blocknum * cache_blocksize,
					ccb->data, cache_blocksize, hdat->log_msg_fn));
}


/*
 * Return a pointer to a cache block.
 * Get the least recently used cache
 * block and load it into that.
 * Update the LRU list to make this block the most recently used.
 * Return NULL if the block cannot be read. In that case the block is not cached.
 */
template <class CCB_T, class DBHANDLE_T, class DBFILE_T>
CCB_T *load_blocknum_v2(DBHANDLE_T *hdat, DBFILE_T *db, int blocknum, int cache_blocksize)
{
	int old_blocknum;
	CCB_T *ccbp;

	++hdat->lookup_stats.lru_cache_loads;

	/* Not cached, need to load this block from disk. */
	ccbp = hdat->ccbs + hdat->ccbs_top;
	if (ccbp->blocknum != UNDEFINED_BLOCK_ID) {

		/* The LRU block is in use.
		 * Unhook this block from the subdb that was using it.
		 */
		old_blocknum = ccbp->blocknum;
		ccbp->dbfile->cache_bufferi[old_blocknum] = UNDEFINED_BLOCK_ID;
	}

	/* The ccbs_top block is now free for use.
	 * Read this block from disk into it.
	 */
	ccbp->dbfile = db;
	ccbp->blocknum = blocknum;
	if (!read_block_from_file_v2(hdat, ccbp, cache_blocksize)) {

		/* Leave this ccb unused. It stays at the top of the lru list, so the next load reuses it. */
		ccbp->blocknum = UNDEFINED_BLOCK_ID;
		return(NULL);
	}

	/* The read succeeded, so now it is safe to map the block to this ccb. */
	db->cache_bufferi[blocknum] = hdat->ccbs_top;

	/* Fix linked list to point to the next oldest entry */
	hdat->ccbs_top = hdat->ccbs[hdat->ccbs_top].next;
	return(ccbp);
}


/*
 * This cache block was just accessed.
 * Update the lru to make this the most recently used.
 */
template <class CCB_T, class DBHANDLE_T> 
CCB_T *update_lru(DBHANDLE_T *hdat, int ccbi)
{
	int next, prev;
	CCB_T *ccbp;

	++hdat->lookup_stats.lru_cache_hits;

	/* This block is already cached.  Update the lru linked list. */
	ccbp = hdat->ccbs + ccbi;

	/* Unlink this ccb and insert as the most recently used.
	 * If this is the head of the list, then just move the lru
	 * list pointer forward one node, effectively moving newest
	 * to the end of the list.
	 */
	if (ccbi == hdat->ccbs[hdat->ccbs_top].prev)
		/* Nothing to do, this is already the most recently used block. */
		;
	else if (ccbi == hdat->ccbs_top)
		hdat->ccbs_top = hdat->ccbs[hdat->ccbs_top].next;
	else {

		/* remove ccbi from the lru list. */
		prev = hdat->ccbs[ccbi].prev;
		next = hdat->ccbs[ccbi].next;
		hdat->ccbs[prev].next = next;
		hdat->ccbs[next].prev = prev;
		
		/* Insert ccbi at the end of the lru list. */
		prev = hdat->ccbs[hdat->ccbs_top].prev;
		hdat->ccbs[prev].next = ccbi;
		hdat->ccbs[ccbi].prev = prev;
		hdat->ccbs[ccbi].next = hdat->ccbs_top;
		hdat->ccbs[hdat->ccbs_top].prev = ccbi;
	}
	return(ccbp);
}


/*
 * Return the maximum number of cacheblocks that could be used if 
 * we had unlimited ram.
 */
template <class DBHANDLE_T> 
int needed_cache_buffers(DBHANDLE_T *hdat)
{
	int i;
	int count;

	count = 0;
	for (i = 0; i < hdat->numdbfiles; ++i)
		if (hdat->dbfiles[i].pieces <= hdat->maxpieces && !hdat->dbfiles[i].file_cache)
			count += hdat->dbfiles[i].num_datablocks;

	return(count);
}


inline int64_t get_file_size(HANDLE handle)
{
	I64_HIGH_LOW filesize;
	filesize.words32.low32 = GetFileSize(handle, &filesize.words32.high32);
	return(filesize.word64);
}


inline int get_mem_available_mb(void)
{
	MEMORYSTATUS memstat;
	GlobalMemoryStatus(&memstat);
	return((int)(memstat.dwAvailPhys / (1024 * 1024)));
}


inline unsigned long get_page_size(void)
{
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	return sysinfo.dwPageSize;
}


inline bool get_nrecords(int64_t nbytes, size_t recordsize, size_t &nrecords)
{
	nrecords = (size_t)(nbytes / recordsize);
	return (nrecords * recordsize != nbytes);
}


template <class DBP_T>
class dbaccess {
private:
	int num_colors;
	int maxpiece;
	int tablesize;
	DBP_T *subslice_tables;

public:
	bool open(int maxpiece_)
	{
		maxpiece = maxpiece_;
		num_colors = 2;
		tablesize = (maxpiece + 1) * (maxpiece + 1) * (maxpiece + 1) * (maxpiece + 1) * num_colors;
		subslice_tables = (DBP_T *)calloc(tablesize, sizeof(DBP_T));
		return(subslice_tables == nullptr);
	}

	void close(void)
	{
		free(subslice_tables);
	}

	DBP_T *subslices(int nbm, int nbk, int nwm, int nwk, int color)
	{
		int table_index = 
				(color + 
				(nwk * num_colors) + 
				(nwm * num_colors * (maxpiece + 1)) + 
				(nbk * num_colors * (maxpiece + 1) * (maxpiece + 1)) + 
				(nbm * num_colors * (maxpiece + 1) * (maxpiece + 1) * (maxpiece + 1)));
		return(subslice_tables + table_index);
	}

	DBP_T *subslices_flat_view(int index)
	{
		return(subslice_tables + index);
	}

	int size(void)
	{
		return(tablesize);
	}
};

template <class DBHANDLE_T, class DBFILE_T> 
class autoload_order_list_builder {

	struct Pieces {
		bool all_npiece_slices_this_file;
		int nbm, nbk, nwm, nwk, npieces;
	};

	static bool get_pieces(const char *name, Pieces &pieces)
	{
		int status;
		char nbm, nbk, nwm, nwk;
		status = sscanf(name, "db%d-%c%c%c%c", &pieces.npieces, &nbm, &nbk, &nwm, &nwk);
		if (status == 1) {
			pieces.all_npiece_slices_this_file = true;
			return(false);
		}
		pieces.all_npiece_slices_this_file = false;
		if (status != 5)
			return(true);
		pieces.nbm = nbm - '0';
		pieces.nbk = nbk - '0';
		pieces.nwm = nwm - '0';
		pieces.nwk = nwk - '0';
		return(false);
	}

	static double priority(Pieces &pieces)
	{
		double pri = 2 * pieces.npieces;				/* favor fewer pieces. */
		pri += 1.5 * (pieces.nbk + pieces.nwk);			/* favor fewer kings. */
		double imbalance = std::abs((pieces.nbm + 1.3 * pieces.nbk) - (pieces.nwm + 1.3 * pieces.nwk));
		if (imbalance > 1.15)
			imbalance *= 1.75;
		pri += 1.75 * imbalance;		/* Discourage material imbalance. */
		return(pri);
	}

	/* Sort key for a file. Files that hold all the slices of one piece count
	 * ("dbN") come first, ordered by pieces. Single slice files ("dbN-bmbkwmwk")
	 * come next, ordered by priority. Files whose names don't parse go last.
	 * Using one key for every pair keeps this a valid ordering for std::sort.
	 */
	static void sort_key(const DBFILE_T *f, int &group, double &value)
	{
		Pieces pieces = {false, 0, 0, 0, 0, 0};

		if (get_pieces(f->name, pieces)) {
			group = 2;
			value = f->pieces;
		}
		else if (pieces.all_npiece_slices_this_file) {
			group = 0;
			value = f->pieces;
		}
		else {
			group = 1;
			value = priority(pieces);
		}
	}

	static bool compare(const DBFILE_T *f1, const DBFILE_T *f2)
	{
		int group1, group2;
		double value1, value2;

		sort_key(f1, group1, value1);
		sort_key(f2, group2, value2);
		if (group1 != group2)
			return(group1 < group2);
		return(value1 < value2);
	}

public:
	static void build(DBHANDLE_T *hdat)
	{
		const bool print = false;
		hdat->files_autoload_order.reserve(hdat->dbfiles.size());
		for (size_t i = 0; i < hdat->dbfiles.size(); ++i)
			hdat->files_autoload_order.push_back(&hdat->dbfiles[i]);

		std::sort(hdat->files_autoload_order.begin(), hdat->files_autoload_order.end(), compare);
		if (print) {
			hdat->log_msg_fn("Autoload order\n");
			for (size_t i = 0; i < hdat->files_autoload_order.size(); ++i) {
				hdat->log_msg_fn(hdat->files_autoload_order[i]->name);
				hdat->log_msg_fn("\n");
			}
		}
	}
};


template <class T>
struct adjustable_array {
	T *data {nullptr};
	int size {0};
	int maxsize {0};

	T *reserve(int size)
	{
		if (size > maxsize) {
			T *newdata;

			/* On failure keep the old block and maxsize, so data stays valid. */
			if (data == nullptr)
				newdata = (T *)malloc(size * sizeof(T));
			else
				newdata = (T *)realloc(data, size * sizeof(T));
			if (newdata == nullptr)
				return(nullptr);
			data = newdata;
			maxsize = size;
		}
		return(data);
	}

	T *push_back(T item)
	{
		if (size >= maxsize) {
			if (reserve(100 + (maxsize * 5) / 3) == nullptr)
				return(nullptr);
		}
		data[size] = item;
		++size;
		return(data);
	}

	T *shrink_to_fit(void)
	{
		if (maxsize > size && size > 0) {
			T *newdata;

			/* If the shrink fails the old, larger block is still valid. */
			newdata = (T *)realloc(data, size * sizeof(T));
			if (newdata != nullptr) {
				data = newdata;
				maxsize = size;
			}
		}
		return(data);
	}

	/* Note: this doesn't free the allocated data buffer (the way you might expect).
	 * It is freed when the driver is closed.
	 */
	void reset(void)
	{
		data = nullptr;
		size = 0;
		maxsize = 0;
	}
};
