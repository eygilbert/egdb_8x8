#pragma once

#ifdef ITALIAN_RULES
#define ENGINENAME_STR "Kingsrow Italian"
#define LOGFILE "kr_italian.log"
#define LOGFILE_PREFIX "kr_italian"
#define OPENDB_FILENAME "kr_italian.odb"
#else
#define ENGINENAME_STR "Kingsrow"
#define LOGFILE "Kingsrow.log"
#define LOGFILE_PREFIX "Kingsrow"
#define OPENDB_FILENAME "Kingsrow.odb"
#endif

#ifdef _DEBUG
#  undef NDEBUG
#else
#ifndef NDEBUG
#  define NDEBUG
#endif
#endif

#include <cassert> // needs NDEBUG

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

#define L1_CACHE_SIZE 64
#define CACHE_ALIGN __declspec(align(L1_CACHE_SIZE))

