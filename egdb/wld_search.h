#pragma once
#include "egdb.h"
#include "board.h"
#include "material.h"
#include <csetjmp>
#include <stdint.h>


struct egdb_info {
	EGDB_DRIVER *handle;
	int max_pieces;
	int max_pieces_1side;
	EGDB_TYPE dbtype;
	egdb_info() {clear();}
	void clear(void) {
		max_pieces = 0;
		max_pieces_1side = 0;
		dbtype = EGDB_KINGSROW_WLD;
		handle = nullptr;
	}
};

class wld_search {

public:
	wld_search(void) { clear(); }
	wld_search(EGDB_DRIVER *handle_, int dbpieces_, int dbpieces_1side_)
	{
		clear();
		handle = handle_;
		dbpieces = dbpieces_;
		dbpieces_1side = dbpieces_1side_;
	}
	void clear(void) {
		handle = 0;
		dbpieces = 0;
		dbpieces_1side = 0;
		maxdepth_reached = 0;
		timeout = 5000;			/* 0 means no limit. */
		maxnodes = 0;			/* 0 means no limit. */
	}
	bool requires_nonside_capture_test(BOARD *p) {return(true);}
	bool is_lookup_possible_pieces(MATERIAL *mat);
	bool is_lookup_possible_with_nonside_caps(MATERIAL *mat) {return(false);}
	bool is_lookup_possible(BOARD *board, int color, MATERIAL *mat);
	int lookup_with_search(BOARD *p, int color, bool force_root_search);
	int get_maxdepth() { return(maxdepth_reached); }
	void set_maxnodes(int maxnodes_) { maxnodes = maxnodes_; }
	void reset_maxdepth() { maxdepth_reached = 0; }
	void set_search_timeout(int msec) { timeout = msec; }

	EGDB_DRIVER *handle;
	EGDB_TYPE type;
	int dbpieces;
	int dbpieces_1side;

private:
	static const int maxrepdepth = 64;
	int timeout;
	int t0;
	int maxnodes;
	int nodes;
	int maxdepth_reached;
	int lookup_with_rep_check(BOARD *p, int color, int depth, int maxdepth, int alpha, int beta, bool force_root_search);
	int negate(int value);
	bool is_greater_or_equal(int left, int right);
	bool is_greater(int left, int right);
	int bestvalue_improve(int value, int bestvalue);
	void log_tree(int value, int depth, int color);
	jmp_buf env;
	BOARD rep_check_positions[maxrepdepth + 1];

};

bool is_repetition(BOARD *history, BOARD *p, int depth);

