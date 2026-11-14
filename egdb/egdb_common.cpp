#include "egdb_common.h"


/*
 * Read size bytes from a file.
 * Since the file is read using ReadFile and the handle may have 
 * been opened with FILE_FLAG_NO_BUFFERING, the reads must be in
 * integer multiples of pagesize, and the read buffer must be sized accordingly.
 * Return 1 on success, 0 on error.
 */
int read_file(HANDLE fp, unsigned char *buf, SIZE_T size, int pagesize)
{
	int stat;
	DWORD bytes_read;
	DWORD request_size;
	const int CHUNKSIZE = 0x100000;

	while (size > 0) {
		if (size >= CHUNKSIZE)
			request_size = CHUNKSIZE;
		else
			request_size = ROUND_UP((int)size, pagesize);
		stat = ReadFile(fp, buf, request_size, &bytes_read, NULL);
		if (!stat)
			return(0);

		/* A short read means we reached the end of the file.
		 * The last request is rounded up to pagesize, so it can read more than size bytes.
		 * Stop before subtracting so that size cannot wrap.
		 */
		if (bytes_read < request_size || bytes_read >= size)
			return(1);
		buf += bytes_read;
		size -= bytes_read;
	}
	return(1);
}


/*
 * Read one cache block of cache_blocksize bytes, starting at byte offset filepos.
 * The last block of a file can be short. In that case the rest of buf is zero filled.
 * Errors are reported through log_msg_fn.
 * Return 1 on success, 0 on error.
 */
int read_cache_block(HANDLE fp, int64_t filepos, unsigned char *buf, int cache_blocksize, void (*log_msg_fn)(const char *))
{
	LARGE_INTEGER pos;
	DWORD bytes_read;
	char msg[160];

	/* Seek the start position. */
	pos.QuadPart = filepos;
	if (!SetFilePointerEx(fp, pos, NULL, FILE_BEGIN)) {
		sprintf(msg, "\nseek failed, file position %lld, error %lu\n", (long long)filepos, GetLastError());
		(*log_msg_fn)(msg);
		return(0);
	}
	if (!ReadFile(fp, buf, cache_blocksize, &bytes_read, NULL)) {
		sprintf(msg, "\nError reading file, file position %lld, error %lu\n", (long long)filepos, GetLastError());
		(*log_msg_fn)(msg);
		return(0);
	}

	/* Reading nothing means the block is past the end of the file. */
	if (bytes_read == 0) {
		sprintf(msg, "\nError reading file, file position %lld is past the end of the file\n", (long long)filepos);
		(*log_msg_fn)(msg);
		return(0);
	}
	if (bytes_read < (DWORD)cache_blocksize)
		memset(buf + bytes_read, 0, cache_blocksize - bytes_read);
	return(1);
}


bool is_wld(EGDB_TYPE type)
{
	switch (type) {
	case EGDB_CAKE_WLD:
	case EGDB_CHINOOK_WLD:
	case EGDB_KINGSROW32_WLD:
	case EGDB_CHINOOK_ITALIAN_WLD:
	case EGDB_KINGSROW32_ITALIAN_WLD:
	case EGDB_KINGSROW32_WLD_TUN:
	case EGDB_KINGSROW32_ITALIAN_WLD_TUN:
	case EGDB_KINGSROW32_WLD_TUN_BINIDX:
		return(true);

	}
	return(false);
}

bool is_dtw(EGDB_TYPE type)
{
	if (type == EGDB_KINGSROW_DTW || type == EGDB_KINGSROW_ITALIAN_DTW)
		return(true);
	else
		return(false);
}

bool is_mtc(EGDB_TYPE type)
{
	if (type == EGDB_KINGSROW32_MTC || type == EGDB_KINGSROW32_ITALIAN_MTC || type == EGDB_KINGSROW_MTC_BINIDX)
		return(true);
	else
		return(false);
}
