#pragma once

#include "distance.h"
#include "egdb.h"
#include "wld_search.h"
#include "board.h"
#include "Move_api.h"
#include <vector>


int mtc_probe(wld_search &wld, EGDB_DRIVER *mtc, BOARD *board, int color, MOVELIST *movelist, int *wld_value, std::vector<move_distance> &dists,
			int short_msec = 0);
bool mtc_is_short_win(wld_search &wld, EGDB_DRIVER *mtc, BOARD *board, int color);

