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
		if (bytes_read < request_size)
			return(1);
		buf += bytes_read;
		size -= bytes_read;
	}
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
	if (type == EGDB_KINGSROW32_MTC || type == EGDB_KINGSROW32_ITALIAN_MTC)
		return(true);
	else
		return(false);
}
