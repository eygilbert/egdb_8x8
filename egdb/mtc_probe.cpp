
#include "dtw_search.h"
#include "egdb.h"
#include "wld_search.h"
#include "bitcount.h"
#include "board.h"
#include "Move_api.h"
#ifdef CHECKERS_EXPORTS	// means we're compiling the engine dll
#include "logfile.h"
#include "movestr.h"
#include "rep_check.h"
#endif
#include <algorithm>
#include <time.h>
#include <unordered_map>
#include <vector>


/*
 * The mtc db does not store distances below MTC_THRESHOLD. This finds them with a small
 * search: can the winning side force a conversion within a number of plies? The winning side
 * only plays moves that keep the win. The losing side only plays non-conversion moves
 * unless it has no others, the same way the mtc db was built.
 */
class short_mtc_search {

public:
	short_mtc_search(wld_search &wld_, EGDB_DRIVER *mtc_, int winner_, int msec) :
		wld(wld_), mtc(mtc_), winner(winner_), timeout(msec), t0(clock()), nodes(0), aborted(false) {}

	/* Return true if the winning side can force a conversion within plies. */
	bool forced(BOARD *p, int color, int plies)
	{
		bool result;

		if (aborted)
			return(false);
		if ((++nodes & 1023) == 0 && (int)(clock() - t0) >= timeout) {
			aborted = true;
			return(false);
		}

		pos_key key = {p->black, p->white, p->king, color};
		std::unordered_map<pos_key, pos_bounds, pos_key_hash>::iterator it = table.find(key);
		if (it != table.end()) {
			if (plies >= it->second.proved)
				return(true);
			if (plies <= it->second.refuted)
				return(false);
		}

		if (color == winner)
			result = winner_forces(p, color, plies);
		else
			result = loser_is_forced(p, color, plies);
		if (aborted)
			return(false);

		pos_bounds &b = table.emplace(key, pos_bounds{127, -1}).first->second;
		if (result)
			b.proved = (signed char)std::min((int)b.proved, plies);
		else
			b.refuted = (signed char)std::max((int)b.refuted, plies);
		return(result);
	}
	bool was_aborted(void) { return(aborted); }
	int get_nodes(void) { return(nodes); }
	int get_msec(void) { return((int)(clock() - t0)); }

private:
	struct pos_key {
		BITBOARD black, white, king;
		int color;
		bool operator==(const pos_key &r) const {
			return(black == r.black && white == r.white && king == r.king && color == r.color);
		}
	};
	struct pos_key_hash {
		size_t operator()(const pos_key &k) const {
			const uint64_t mult = 0x9E3779B97F4A7C15ULL;
			uint64_t h = (uint64_t)k.black * mult ^ k.white;
			h = h * mult ^ k.king;
			h = h * mult ^ k.color;
			return((size_t)(h ^ (h >> 29)));
		}
	};
	struct pos_bounds {
		signed char proved;		/* A conversion is forced within this many plies. */
		signed char refuted;	/* A conversion is not forced within this many plies. */
	};

	/* A stored value means this position is too far from a conversion for the plies left. */
	bool is_too_far(BOARD *p, int color, int plies)
	{
		return(plies < MTC_THRESHOLD && (*mtc->lookup)(mtc, (EGDB_BITBOARD *)p, color, 0) >= MTC_THRESHOLD);
	}

	bool is_loss_for_loser(BOARD *p)
	{
		int value = wld.lookup_with_search(p, OTHER_COLOR(winner), false);

		if (value != EGDB_WIN && value != EGDB_LOSS && value != EGDB_DRAW)
			aborted = true;
		return(value == EGDB_LOSS);
	}

	bool winner_forces(BOARD *p, int color, int plies)
	{
		int i, movecount;
		MOVELIST movelist;

		if (plies < 1)
			return(false);

		if (canjump(p, color))
			movecount = build_jump_list(p, &movelist, color);
		else {
			if (is_too_far(p, color, plies))
				return(false);
			movecount = build_nonjump_list(p, &movelist, color);
		}

		for (i = 0; i < movecount; ++i)
			if (is_conversion_move(p, movelist.board + i) && is_loss_for_loser(movelist.board + i))
				return(true);

		if (plies < 3)
			return(false);

		for (i = 0; i < movecount && !aborted; ++i) {
			if (is_conversion_move(p, movelist.board + i))
				continue;
			if (!is_loss_for_loser(movelist.board + i))
				continue;
			if (forced(movelist.board + i, OTHER_COLOR(color), plies - 1))
				return(true);
		}
		return(false);
	}

	bool loser_is_forced(BOARD *p, int color, int plies)
	{
		int i, movecount, nonconversions;
		MOVELIST movelist;

		if (canjump(p, color))
			return(plies >= 1);

		movecount = build_nonjump_list(p, &movelist, color);
		if (movecount == 0)
			return(true);		/* No moves, the game is over. */
		if (plies < 1)
			return(false);

		nonconversions = 0;
		for (i = 0; i < movecount; ++i)
			if (!is_conversion_move(p, movelist.board + i))
				++nonconversions;
		if (nonconversions == 0)
			return(true);
		if (plies < 2 || is_too_far(p, color, plies))
			return(false);

		for (i = 0; i < movecount; ++i) {
			if (is_conversion_move(p, movelist.board + i))
				continue;
			if (!forced(movelist.board + i, OTHER_COLOR(color), plies - 1))
				return(false);
		}
		return(true);
	}

	wld_search &wld;
	EGDB_DRIVER *mtc;
	int winner;
	int timeout;
	clock_t t0;
	int nodes;
	bool aborted;
	std::unordered_map<pos_key, pos_bounds, pos_key_hash> table;
};


/* Replace MTC_LESS_THAN_THRESHOLD in dists[] with the plies to a conversion from board.
 * Return false if the shortest distance could not be found in the time allowed.
 */
static bool find_short_distances(wld_search &wld, EGDB_DRIVER *mtc, BOARD *board, int color, MOVELIST *movelist,
								std::vector<move_distance> &dists, int msec)
{
	const int maxplies = MTC_THRESHOLD - 1;
	bool found;
	std::vector<size_t> pending;
	short_mtc_search search(wld, mtc, color, msec);

	/* The move itself can be the conversion, whatever the distance to the next conversion after it.
	 * Sort first so that the conversion nearest to the next one stays ahead of the other conversions.
	 */
	std::sort(dists.begin(), dists.end(), [](const move_distance &l, const move_distance &r) {return(l.distance < r.distance);});
	found = false;
	for (size_t i = 0; i < dists.size(); ++i) {
		if (is_conversion_move(board, movelist->board + dists[i].move)) {
			dists[i].distance = 1;
			found = true;
		}
		else if (dists[i].distance == MTC_LESS_THAN_THRESHOLD)
			pending.push_back(i);
	}

	/* Deepen all of the moves together, the first to succeed is a shortest one.
	 * Any moves not resolved are longer than the ones that were.
	 */
	for (size_t k = 0; k < pending.size(); ++k)
		dists[pending[k]].distance = maxplies + 2;
	for (int plies = 0; plies <= maxplies && pending.size() && !search.was_aborted(); ++plies) {
		for (size_t k = 0; k < pending.size() && !search.was_aborted(); ) {
			if (search.forced(movelist->board + dists[pending[k]].move, OTHER_COLOR(color), plies)) {
				dists[pending[k]].distance = plies + 1;
				found = true;
				pending.erase(pending.begin() + k);
			}
			else
				++k;
		}
	}

#ifdef CHECKERS_EXPORTS
	log_msg("short mtc search: %d nodes, %d msec%s\n", search.get_nodes(), search.get_msec(), search.was_aborted() ? ", aborted" : "");
#endif

	return(found);
}


/* Return true if color has a win that mtc_probe with no short_msec leaves to the heuristic search:
 * the position or one of its winning moves has a distance below MTC_THRESHOLD.
 */
bool mtc_is_short_win(wld_search &wld, EGDB_DRIVER *mtc, BOARD *board, int color)
{
	int i, movecount;
	MOVELIST movelist;

	if (mtc == nullptr || wld.handle == nullptr)
		return(false);
	if (wld.lookup_with_search(board, color, false) != EGDB_WIN)
		return(false);
	if ((*mtc->lookup)(mtc, (EGDB_BITBOARD *)board, color, 0) == MTC_LESS_THAN_THRESHOLD)
		return(true);

	movecount = build_jump_list(board, &movelist, color);
	if (!movecount)
		movecount = build_nonjump_list(board, &movelist, color);
	for (i = 0; i < movecount; ++i) {
		if ((*mtc->lookup)(mtc, (EGDB_BITBOARD *)(movelist.board + i), OTHER_COLOR(color), 0) == MTC_LESS_THAN_THRESHOLD &&
					wld.lookup_with_search(movelist.board + i, OTHER_COLOR(color), false) == EGDB_LOSS)
			return(true);
	}
	return(false);
}


/* Return non-zero on success, with egdb score in *score, and distances in dists[].
 * Return zero if there are no conversion distances to return.
 * If short_msec is not zero, then wins with distances below MTC_THRESHOLD are found by a search
 * limited to that time, instead of being left to the heuristic search.
 */
int mtc_probe(wld_search &wld, EGDB_DRIVER *mtc, BOARD *board, int color, MOVELIST *movelist, int *wld_value, std::vector<move_distance> &dists,
			int short_msec)
{
	int i;
	int npieces;
	int egdb_value, parent_distance, offset;
	bool have_short = false;

	dists.clear();

	/* We need both an open mtc driver and egdb driver. */
	if (mtc == nullptr || wld.handle == nullptr)
		return(0);

	/* Must be few enough pieces for db lookups. */
	npieces = bitcount(board->black | board->white);
	if (npieces > wld.dbpieces)
		return(0);

	/* If this position is not in the mtc db, then we have to do a heuristic search. */
	parent_distance = (*mtc->lookup)(mtc, (EGDB_BITBOARD *)board, color, 0);
	if (parent_distance == MTC_LESS_THAN_THRESHOLD && !short_msec)
		return(0);

	egdb_value = wld.lookup_with_search(board, color, false);
	if (!(egdb_value == EGDB_WIN || egdb_value == EGDB_LOSS))
		return(0);
	if (parent_distance == MTC_LESS_THAN_THRESHOLD) {
		if (egdb_value != EGDB_WIN)
			return(0);
		have_short = true;
	}

	*wld_value = egdb_value;
	if (egdb_value == EGDB_WIN) {
		for (i = 0; i < movelist->count; ++i) {
			int succ_value = wld.lookup_with_search(movelist->board + i, OTHER_COLOR(color), false);
			if (succ_value == EGDB_LOSS) {
				move_distance dist;
				dist.distance = (*mtc->lookup)(mtc, (EGDB_BITBOARD *)(movelist->board + i), OTHER_COLOR(color), 0);

				/* If any wins are short then let the search find the move,
				 * unless we were asked to find the short distances.
				 */
				if (dist.distance == MTC_LESS_THAN_THRESHOLD) {
					if (!short_msec)
						return(0);
					have_short = true;
				}

				dist.move = i;
				dists.push_back(dist);
			}
			else if (succ_value != EGDB_DRAW && succ_value != EGDB_WIN)
				return(0);
		}

		if (dists.size() == 0)
			return(0);

		if (have_short && !find_short_distances(wld, mtc, board, color, movelist, dists, short_msec))
			return(0);

		/* Sort moves to pick the best one. */
		std::stable_sort(dists.begin(), dists.end(), [](const move_distance &l, const move_distance &r) {return(l.distance < r.distance);});
	}
	else if (egdb_value == EGDB_LOSS) {
		for (i = 0; i < movelist->count; ++i) {
			move_distance dist;
			if (is_conversion_move(board, movelist->board + i))
				continue;

			dist.distance = (*mtc->lookup)(mtc, (EGDB_BITBOARD *)(movelist->board + i), OTHER_COLOR(color), 0);
			if (dist.distance == MTC_LESS_THAN_THRESHOLD)
				continue;

			dist.move = i;
			dists.push_back(dist);
		}

		if (dists.size() == 0)
			return(0);

		std::sort(dists.begin(), dists.end(), [](const move_distance &l, const move_distance &r) {return(l.distance > r.distance);});
	}

	/* Fix the even/odd problem. */
	if (!have_short && parent_distance == dists[0].distance) {
		offset = -1;
		for (size_t i = 0; i < dists.size(); ++i)
			dists[i].distance += offset;
	}

	/* Avoid draw by 3-fold repetition. */
#ifdef CHECKERS_EXPORTS
	if (egdb_value == EGDB_WIN) {
		BOARD *new_board;
		for (i = 0; i < (int)dists.size(); ++i) {
			new_board = movelist->board + dists[i].move;
			Search_pos newpos = (rep_draw::game_positions.end() - 1)->next(*new_board, 0);
			int count = rep_draw::get_repetition_count(newpos);
			if (count == 2) {
				char buf[50];
				log_msg("Avoiding move %s, it would be a 3-fold repetition.\n", movestr(buf, board, new_board));
				continue;
			}

			/* See if the opponent can move into a 3-fold repetition. */
			MOVELIST opp_movelist;
			int movecount;
			
			bool skip = false;
			movecount = build_nonjump_list(new_board, &opp_movelist, OTHER_COLOR(color));
			for (int k = 0; k < movecount; ++k) {
				BOARD *opp_new_board = opp_movelist.board + k;
				Search_pos opp_newpos = newpos.next(*opp_new_board, 0);
				count = rep_draw::get_repetition_count(opp_newpos);
				if (count == 2) {
					char buf[50];
					log_msg("Avoiding move %s, it would allow opponent to create a 3-fold repetition.\n", movestr(buf, board, new_board));
					skip = true;
					break;
				}
			}

			if (!skip)
				break;
		}
		if (i > 0 && i < (int)dists.size()) {
			/* dists[i] has the best move to make. */
			std::swap(dists[i], dists[0]);
		}
	}
#endif
	return(1);
}

