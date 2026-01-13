#pragma once
#define NOMINMAX
#include <Windows.h>
#include <stdint.h>


/*
 * Return true if the directory name exists.
 */
inline bool dir_exists(const char *name)
{
	HANDLE h;
	WIN32_FIND_DATA FindFileData;
	bool retval = false;

	h = FindFirstFile(name, &FindFileData);
	if (h != INVALID_HANDLE_VALUE)
		if (FindFileData.dwFileAttributes && FILE_ATTRIBUTE_DIRECTORY)
			retval = true;
	FindClose(h);
	return(retval);
}


/*
 * Return true if there was an error.
 */
inline bool file_size(const char *name, int64_t &size)
{
	LARGE_INTEGER filesize;
	HANDLE fp;
	BOOL result;

	fp = CreateFile(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (fp == INVALID_HANDLE_VALUE)
		return(true);

	/* Get the size. */
	result = GetFileSizeEx(fp, &filesize);
	CloseHandle(fp);
	if (result == 0)
		return(true);

	size = filesize.QuadPart;
	return(false);
}


