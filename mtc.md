# MTC database accuracy

How the moves-to-conversion (MTC) values are produced by the database builder, how exact they are, and why playing by them cannot cycle. Analysis of October 2026.

MTC values are counted in plies, not moves. Kingsrow uses them only after the WLD database has qualified the position as a win or a loss.

## Summary

- In almost all subdbs, the conversion distance Kingsrow shows is the true distance to conversion **±1 ply**, counted from the position before Kingsrow moves. In the exact check below it was never too large: it was exact or 1 ply short.
- Storing the values divided by 2 loses no information.
- Playing the best moves by the stored values cannot cycle: the underlying pass number drops by exactly 1 every ply.
- Positions not stored ("less than 10") are at most 8 plies from conversion.
- The exception is the equal kings-only subdbs (2K v 2K up to 6K v 6K). There the shown distance is 1 to 3 plies short, it sometimes stays the same for a ply, and unstored positions can be up to 11 plies from conversion. No cycles were found in them.

## Why it's built this way

The MTC data comes almost free with the WLD build. The non-conversion passes have to run anyway to resolve the positions, and each pass already writes the positions it resolved to a queue. Saving those queues to files, one per pass, and compressing them afterwards is the only extra work.

A conventional MTC build would hold each position's own conversion distance in the local copy of the subdb being built: at least a byte per position, plus room for the WLD value, where the WLD build packs 4 positions to a byte. It could also give each seed its true distance (0 or 1) and let both colors use the seeds from the start, so its distances would be exact.

The pass-number method keeps the build at 4 positions per byte. The cost is the ±1 ply described below. The way the passes alternate colors is also what makes the ÷2 storage lossless.

## Definitions

**d** is the true distance to conversion in plies, counting the conversion move. d = 1 when the side to move can convert now (a capture, or a man move), and d = 0 when it has no legal moves. The winner plays to minimize d, the loser to maximize it.

- For a win: d = 1 if there is a winning conversion, otherwise 1 + the smallest d of the king moves that lead to a loss for the opponent.
- For a loss: d = 1 + the largest d of the king moves (all of them lead to wins for the opponent). A losing conversion only gives 1, so the loser prefers a king move when there is one.

**T** is the pass of `buildsubdb_thai()` (builddb/build.c) on which a position was resolved.

- Positions resolved by `black_conversion_pass()` and `white_conversion_pass()` have T = 0. These are the seeds: wins with a winning conversion, losses with no king move, captures, and positions with no legal moves.
- After that, the non-conversion passes are numbered by the `pass` variable, starting at 1.

## How the build numbers the passes

In a normal subdb the non-conversion passes alternate colors: black on odd passes, white on even passes. This holds for the forward passes (`black_noio_pass()`, `white_noio_pass()`) and for the queue passes (`pos_queue_noio_pass()`), which only visit the parents of the positions resolved on the previous pass.

A black pass reads only the white table and writes only the black table, and the other way around. So a position resolved on pass T depends only on positions resolved on earlier passes. Between colors:

- A black position (odd T) is resolved on the first pass after the pass that decided it: T = 1 + (the smallest T of its losing successors, for a win) or 1 + (the largest T of its successors, for a loss).
- A white position is the same, except when the deciding successor is a black seed (T = 0). White's first pass is pass 2, so it gets T = 2, not 1.

## What is stored, and what Kingsrow shows

**Stored values**

| Step | Code | Value |
|---|---|---|
| Written to the `_mtc` file | `write_mtc_file(pass + 2, ...)` | T + 2, only when T + 2 >= 10 |
| Compressed (build format) | `compress_mtc()`, `MTC_ENCODE` | (T + 2) / 2 |
| Converted to engine format | `compress_subdb_mtc()` | the same |
| Decoded by the driver | `MTC_DECODE` | 2 × ((T + 2) / 2): T + 1 for black (odd T), T + 2 for white (even T) |

Each color only ever has one parity of T, so the decoded value identifies T exactly. **Dividing by 2 loses nothing.**

`mtc_probe()` (egdb/mtc_probe.cpp) shows the stored value of the successor it chooses, minus 1 when that equals the root's stored value (the "even/odd" fix). Working through both colors, wins and losses, the number shown is always **T of the root position**:

| Root | Chosen successor | Stored root | Stored successor | Shown |
|---|---|---|---|---|
| Black, T odd | White, T − 1 | T + 1 | T + 1 | T + 1 − 1 = T |
| White, T even | Black, T − 1 | T + 2 | T | T |

## The uncertainty: T − d is −1, 0 or +1

T and d differ for two reasons:

1. **The seeds don't all have the same distance.** All seeds have T = 0, but most have d = 1 (convert now), and the positions with no legal moves have d = 0.
2. **White's first pass is late.** A white position whose best line goes straight into a black seed gets T = 2 although its d is 1 or 2.

So the seeds have T − d of −1 or 0, and white's pass-2 positions have T − d of 0 or +1. For every later position, T and d are both 1 + the minimum (for a win) or maximum (for a loss) over the same successors. A min or max can't take T − d outside the range of its successors, so by induction **T − d is −1, 0 or +1 for every position**.

**The distance Kingsrow shows is the true distance to conversion ±1 ply.**

**Threshold.** A value is stored when T + 2 >= 10, that is T >= 8, so stored positions have d >= 7. A position that is not stored has T <= 7, so d <= 8: "less than 10" means at most 8 plies from conversion, which the engine search handles.

## Why playing by the values cannot cycle

- **Winner.** `mtc_probe()` chooses the losing successor with the smallest stored value. A successor with T(root) − 1 exists. Within a color the stored value identifies T, so the choice has T(root) − 1, never a successor resolved on the same pass as the root.
- **Loser.** Every successor of a loss was resolved before it, so whatever the loser chooses has a smaller T. `mtc_probe()` chooses T(root) − 1.

So T is a countdown that drops by exactly 1 each ply. The line reaches the threshold, where the search takes over, or a conversion, within T plies.

## Caveats

1. **Equal kings-only subdbs** (2K v 2K up to 6K v 6K, `is_thai_symmetric()`). Black and white share one table, and every pass is a black pass. `black_noio_pass()` and `pos_queue_noio_symmetric_pass()` look up the white successors by reversing them into the same table that the pass is writing, so a position can be resolved using a value set earlier in the same pass.
   - Some positions get a pass number lower than their true distance, and there is no parity, so the ÷2 does lose information. In the exact check the shown distance was 1 to 3 plies short.
   - The countdown argument doesn't hold strictly: the shown distance can stay the same for a ply, or drop by 2, while the true distance drops by 1 each ply.
   - The "≤ 8 plies" guarantee for unstored positions is weaker: up to 11 plies in 3K v 3K.
   - A fix would be to make the symmetric passes use only values from earlier passes, then rebuild those subdbs. The evidence below doesn't make it look worth doing.
2. **Forward passes never write MTC files.** Only the queue passes call `write_mtc_file()`. Normally the build switches to queue passes by pass 3 or 4, far below pass 8. But forward passes continue while each resolves more than 25% of the index range, so a small king range could in principle stay in forward passes past pass 7, and positions resolved there would wrongly read as "less than 10". This has not been observed. Build log lines `b fwd` / `w fwd` with a pass number of 8 or more would show it.
3. **Repetition avoidance.** In the engine, `mtc_probe()` may skip the best move to avoid a 3-fold repetition. That move can have a larger distance. This is not covered by the analysis.

## Tests with the English 10-piece databases

### Exact distances

A test program solved six kings-only slices completely by its own retrograde analysis, giving every win and loss its exact distance d. In kings-only slices the only conversions are captures, so a position where the side to move must capture has d = 1. WLD values were looked up only for the positions after a capture. It then compared d with the stored value, and with the distance `mtc_probe()` shows for each stored position.

**Shown distance minus exact distance, all stored positions**

| Slice | Longest d | −3 | −2 | −1 | 0 |
|---|---|---|---|---|---|
| 2K v 1K | 34 | | | | 18,070 |
| 3K v 1K | 13 | | | 12,092 | 7,310 |
| 3K v 2K | 49 | | | 1,607,046 | 548,158 |
| 4K v 2K | 13 | | | 211,156 | |
| 2K v 2K | 19 | 20 | 5,252 | 4,576 | |
| 3K v 3K | 36 | 96 | 19,454 | 56,310 | |

**Largest exact distance of a position not stored**

| Slice | Largest d |
|---|---|
| 2K v 1K, 3K v 1K, 3K v 2K, 4K v 2K | 8 or less |
| 2K v 2K | 9 |
| 3K v 3K | 11 |

The unequal slices are within the ±1 of the analysis, and in practice never too large. The equal slices (caveat 1) are 1 to 3 plies short, because a pass there can use values from the same pass, which gives some positions a pass number lower than their distance.

### Lines played

A test program played lines from random kings-only positions with the real `mtc_probe()` and `wld_search`, choosing moves as the engine does (without the repetition avoidance), until a capture or until `mtc_probe()` declined because a value was below the threshold.

**Lines played with mtc_probe()**

| Slice | Lines | Cycles | Longest line | Change in shown distance per ply |
|---|---|---|---|---|
| 2K v 1K | 1,267,713 | 0 | 27 plies | −1 on all 17,522,484 plies |
| 3K v 2K | 2,182,672 | 0 | 41 plies | −1 on all 30,320,059 plies |
| 4K v 3K | 1,243,794 | 0 | 68 plies | −1 on all 16,303,259 plies |
| 2K v 2K | 105,274 | 0 | 11 plies | −1: 219,639; 0: 41,509 |
| 3K v 3K | 10,354 | 0 | 27 plies | −1: 54,731; 0: 821 |
| 4K v 4K | 6,557 | 0 | 39 plies | −1: 57,619; 0: 516; −2: 34 |
| 5K v 5K | 2,964 | 0 | 57 plies | −1: 24,080; 0: 399; −2: 44 |

The unequal slices drop by exactly 1 on every ply, as the analysis predicts. The equal slices show the effects of caveat 1, without cycles.
