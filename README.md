# `egdb_8x8`: a driver for English and Italian checkers endgame databases.

`egdb64.dll` is a library to access the Kingsrow English and Italian endgame databases. This repos contains the source code for the library. It also contains an example C program that demonstates how to use it.

# License

    Original work Copyright (C) 2007-2026 Ed Gilbert
  
Distributed under the Boost Software License, Version 1.0.
(See accompanying file [LICENSE_1_0.txt](LICENSE_1_0.txt) or copy at
[http://www.boost.org/LICENSE_1_0.txt](http://www.boost.org/LICENSE_1_0.txt))


# Description

This library provides an API into several checkers endgame databases. These include:

* Kingsrow 10-piece WLD database version 1 (filenames ending in .cpr, .idx).
* Kingsrow 10-piece WLD database version 2 (filenames ending in .cpr1, .idx1).
* Kingsrow 12-piece WLD database version 3 (filenames ending in .cpr2, .base2, .bidx2).
* Kingsrow 10-piece MTC database (filenames ending in .cpr_mtc, .idx_mtc).
* Kingsrow 12-piece MTC database (filenames ending in .cpr_mtc, .base_mtc, .bidx_mtc).
* Kingsrow 8-piece DTW database.
* Cake 8-piece WLD database built by Martin Fierz.
* Chinook 8-piece WLD database built by Jonathan Schaeffer.
* Kingsrow Italian 9-piece and partial 10-piece WLD database version 1 (filenames ending in .cpr, .idx).
* Kingsrow Italian 10-piece WLD database version 2 (filenames ending in .cpr1, .idx1).
* Kingsrow Italian 10-piece MTC database.
* Kingsrow Italian 8-piece DTW database.
* Chinook Italian 6-piece WLD (note: as of 4/9/2005 this db has errors).

The API has functions to

* identify a database and return its attributes
* open a database for lookups
* look up values in an open database
* get statistics on database lookups, cache hits, misses, etc.
* perform a CRC check on each database file
* close a database

To use the library, include the header file egdb.h in the application source code and link with the lib file egdb64.lib (or egdb.lib for 32-bit builds). The dll must be somewhere in the search path for dynamic libraries when the application runs. The dll files are placed in the CheckerBoard directory when you install Kingsrow.

To look up values in a database, it must first be opened using egdb_open(). This returns a handle to an open database. The handle is a pointer to a structure of db access function pointers. These access functions are then used for all subsequent communication with the driver. The handle also holds all the state data used by each instance of an open driver, so multiple handles can be opened from a single process without conflicting with one another. The drivers are safe for multi-threading.

# Example program

This package contains an example program that demonstrates the use of the library. A project file for Visual Studio 2026 is included. The program opens a WLD database, reads some positions from a table, calls lookup() to get the database value of each position, and compares it to the known value that is also in the table. You can use this to verify that the basic functions of the driver are working on your system. Before compiling it, edit DB_PATH near the top of egdb_example.c to the directory where your database files are.

The example is a console application. You should run it from a Command Prompt window. Since it uses egdb64.dll (or egdb.dll), this file should either be in the directory that you run from, or in a directory that Windows searches for dll files. The dll file is placed in the CheckerBoard directory when you install Kingsrow.

# Excluded positions

The WLD databases do not have valid data for positions that are a capture, or for positions that would be a capture for the opposite side-to-move. There is no way to know this from the lookup() return value. It will be a win, loss, or draw value, but it will not be the correct value except by random luck. It will be whatever makes the compression work best. You should test that a position is not a capture or a non-side capture before calling lookup().

Version 2 of the Kingsrow WLD databases, both English and Italian types, only have data for one of the side-to-move colors in some 9 and 10 piece position subsets. If you call lookup() for one of these positions with the excluded color, it returns EGDB_UNKNOWN.

Version 3 of the Kingsrow WLD database (12 pieces) has the same exclusions as version 2, and the 9 and 10 piece exclusions also apply to positions with 11 and 12 pieces.

The Kingsrow DTW databases only have data for one of the side-to-move colors in all subsets. If you call lookup() for one of these positions with the excluded color, it returns EGDB_SUBDB_UNAVAILABLE. These databases also do not have data for positions that are captures, and there is no way to know this from the lookup() return value. You have to test that the position is not a capture before querying a DTW value.

For a position with more pieces than the database was opened for, or more pieces on one side than the database has, the Kingsrow WLD and DTW lookups return EGDB_SUBDB_UNAVAILABLE. The Kingsrow MTC lookup returns MTC_LESS_THAN_THRESHOLD for these positions, the same value it returns for positions close to a conversion move, so check the piece counts before calling it.

#### *EGDB_API int __cdecl egdb_identify(const char \*directory, EGDB_TYPE \*egdb_type, int \*max_pieces);*

egdb_identify() checks if an endgame database exists in directory. If so, the database type is written to egdb_type, and the maximum number of pieces for which the database has data is written to max_pieces. The function return value is 0 on success, or non-zero if no database is found in directory.

#### *EGDB_API EGDB_DRIVER \*__cdecl egdb_open(EGDB_BITBOARD_TYPE bitboard_type, int pieces, int cache_mb, const char \*directory, void (__cdecl \*msg_fn)(const char \*));*

This function opens a database, allocates memory for caching db values, and reads the index files.

The return value is a handle that is used for all subsequent access to the database. A NULL return value means that an error occurred. The likely reasons for errors are either that the wrong directory path was given for the database files, or that the driver was unable to allocate all the memory it needed from the heap. When egdb_open() returns NULL, an error message is passed back to the application through the callback message function.

The first argument to egdb_open() tells the driver which type of position bitboards will be used for lookups. The driver tailors the lookup function pointer for the requested bitboard type. The driver accepts positions in either Kingsrow format (EGDB_NORMAL, using the EGDB_NORMAL_BITBOARD structure) or Cake format (EGDB_ROW_REVERSED, using the EGDB_ROW_REVERSED_BITBOARD structure). These formats are described in egdb.h.

The second argument to egdb_open() is the maximum number of pieces for which the database is being opened for lookups. Normally this will be 8, 10 or 12, but a smaller number of pieces can be given. The driver uses less RAM and initializes more quickly when a smaller than maximum number of pieces is given. egdb_open() fails if the pieces argument is greater than the capability of the database. The attributes of a database can be queried using egdb_identify() before calling egdb_open().

The third argument to egdb_open() is the number of megabytes (2^20 bytes) of heap memory that the driver will use. This is the driver's total memory budget, not just the size of its cache. The driver first allocates the memory it needs for indexing the database files, and for static buffers that hold some of the smaller subsets of the database in full (in the statistics counters these are called 'autoload' buffers). Whatever is left of the budget is used for cache buffers, which are allocated in 4 KB blocks.

The driver always uses some minimum amount of memory, even if you give a lower value. If the budget does not leave room for a minimum amount of cache after indexing, the driver allocates the minimum cache anyway, and reports the actual amounts with a warning through the message function. The minimum cache is a quarter of the indexing memory, but at least 10 MB and at most 1000 MB. For example, these are the minimums for the 12-piece WLD database:

| Opened for | Indexing and autoload buffers | Minimum cache | Minimum total |
|---|---|---|---|
| 8 pieces | 37 MB | 10 MB | 47 MB |
| 10 pieces | 188 MB | 39 MB | 227 MB |
| 12 pieces | 4,468 MB | 1,000 MB | 5,468 MB |

So if you open the 12-piece database for all 12 pieces with a memory setting below about 5500 MB, it will still use about 5500 MB, of which 1000 MB is cache. Any memory that you give above the minimum is used for more cache buffers. The driver's startup messages report how much memory went to indexing, to autoload buffers, and to the cache.

The last argument to egdb_open() is a pointer to a function that receives status and error messages from the driver. The driver normally emits status messages when it starts up, with information about files opened and memory used. If any errors occur, either during the open call or in later calls through the driver handle, the details are reported through the message function. A message function that writes messages to stdout can be provided like this:

```c
void print_msg(const char *msg)
{
    printf("%s", msg);
}
```

which can be passed to the driver like this:

```c
handle = egdb_open(EGDB_NORMAL, 10, 5000, "c:/db_english/wld_v2", print_msg);
```

In Kingsrow I write these messages to a log file.

#### *int (__cdecl \*lookup)(struct egdb_driver \*handle, EGDB_BITBOARD \*position, int color, int cl);*

The lookup() function returns the value of a position in an open database. It takes 4 arguments: a driver handle, a position, a color, and a conditional lookup flag.

* handle is the value returned by egdb_open().
* position is the position to look up.
* color is the side-to-move, either EGDB_BLACK or EGDB_WHITE.
* cl is the conditional lookup argument. If it is non-zero, the driver only gets the value of the position if it is already cached in RAM; otherwise EGDB_NOT_IN_CACHE is returned. If the conditional lookup argument is zero, the driver always attempts to get a value for the position, even if it has to read a disk file to get it. In Kingsrow I calculate a priority for each position that is passed to handle->lookup(). If the priority is low, I set the conditional lookup argument non-zero, else I set it to zero to look up the position unconditionally.

For the WLD databases, the values returned by lookup are defined by the enums EGDB_UNKNOWN, EGDB_WIN, EGDB_LOSS, EGDB_DRAW, EGDB_NOT_IN_CACHE, and EGDB_SUBDB_UNAVAILABLE.

For the MTC databases, the values returned by lookup are either the number of plies to a conversion move, or the value MTC_LESS_THAN_THRESHOLD for positions which are close to a conversion move. The Kingsrow MTC databases do not store positions which are closer than MTC_THRESHOLD plies to a conversion move. The lookup function only returns even values, because the database represents the value internally as (distance / 2). The true value is either the value returned, or (value - 1). An application can infer the true even or odd value of a position by looking up the values of the position's successors. If the best successor value is the same as the position's, then the position's true value is 1 less than the returned value. The MTC database only has values for positions that are a win or a loss. You cannot tell this from the return value from lookup(). Before calling lookup in the MTC database, check that the position is a win or a loss using the WLD database.

For DTW databases, the values returned by lookup are (number_of_plies_to_win_or_loss / 2). To convert the return value to the actual depth to win or loss, multiply by 2, and add 1 if the position is a win. Depths to win are always an odd number of plies, losses an even number of plies. Queries on positions that are draws return a meaningless value, and you cannot know this from the return value alone. You must query a WLD db first. Similarly, positions that are captures do not have valid db data and return a meaningless value. Also keep in mind that the DTW db has data for only one side to move in each db subset. Lookup returns EGDB_SUBDB_UNAVAILABLE if you query a position for which no data is available for that color. See the section "Excluded positions" above. In general, to obtain the DTW value of a position that is excluded from the db because of its side-to-move or capture status, do a recursive query of successors until a valid DTW value can be propagated back to the root position.

All the WLD databases supported by this driver (Kingsrow, Cake, and Chinook) do not return valid values for positions where either the side to move has a capture, or the opposite side would have a capture if it were that side's turn to move. There is no way to tell this from the return value, which will normally be one of the win, loss, or draw values. The application must test for these captures and avoid calling lookup for any positions where they are present. The MTC databases do not have this restriction: they have valid values for positions with a non-side capture, and for a position where the side to move has a capture, lookup returns MTC_LESS_THAN_THRESHOLD, since a capture is a conversion move.

The Cake database does not have data for positions where one side has more than 4 pieces. If you give the lookup function a position where one side has more pieces than it knows about, it returns EGDB_UNKNOWN. The Chinook database has all positions for up to 6 pieces, but the 7 piece and 8 piece slices do not have data for more than 4 pieces on a side. The Kingsrow 10-piece databases have data for positions with up to 5 pieces on a side, and the 12-piece databases up to 6 pieces on a side.

#### *int (__cdecl \*verify)(struct egdb_driver \*handle);*

The driver has tables of good CRC values for all db files. The verify() function performs a CRC check on each index and data file in the open database. It returns 0 if all CRCs compared OK. If any file fails the check, verify returns a non-zero value, and error messages are sent through the callback msg_fn that was provided when the driver was opened.

#### *int (__cdecl \*close)(struct egdb_driver \*handle);*

The close() function frees all resources used by the driver. If the application needs to change anything about an egdb driver after it has been opened, such as the number of pieces or megabytes of RAM to use, it must be closed and then opened again with the new parameters. close() returns 0 on success, non-zero if there are any errors.

#### *void (__cdecl \*reset_stats)(struct egdb_driver \*handle);*

#### *EGDB_STATS \*(__cdecl \*get_stats)(struct egdb_driver \*handle);*

The API provides two functions for gathering statistics on db lookups. get_stats() returns a pointer to a structure of counts (cache hits, cache loads, autoload hits, requests, returns, and requests for positions not in the db) that have accumulated since the counts were last reset. reset_stats() resets all the counts to 0.

#### *EGDB_API extern unsigned int egdb_version;*

The library's version number (currently 3).
