// egdb.cpp : Defines the entry point for the DLL application.
//

#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
#define NOMINMAX

#include <windows.h>
#include "egdb.h"
#include "bicoef.h"
#include "bitboard_convert.h"
#include "bitcount.h"
#include "reverse.h"

unsigned int egdb_version = 3;


BOOL APIENTRY DllMain( HANDLE hModule, 
                       DWORD  ul_reason_for_call, 
                       LPVOID lpReserved
					 )
{
    switch (ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
			initbicoef();
			init_reverse();
			init_bitcount();
			break;
		case DLL_THREAD_ATTACH:
		case DLL_THREAD_DETACH:
		case DLL_PROCESS_DETACH:
			break;
    }
    return TRUE;
}


