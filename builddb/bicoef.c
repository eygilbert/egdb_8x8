#include <algorithm>
#include "bicoef.h"


uint32_t bicoef[MAXSQUARE_BICOEF + 1][MAXPIECE_BICOEF + 1];


/*
 * Return the number of ways to choose k objects from a set of n objects.
 */
static uint32_t binomial_coefficient(int n, int k)
{
	int64_t result = 1;
	int i;

	i = k;
	while (i) {
		result *= (n - i + 1);
		i--;
	}

	i = k;
	while (i) {
		result /= i;
		i--;
	}

	assert(result < ((int64_t)1 << 32));
	return((uint32_t)(result));
}


/*
 * Initialize binomial coefficients table.
 */

void initbicoef(void)
{
	int i, j;

	for (i = 0; i <= MAXSQUARE_BICOEF; i++) {
		for (j = 1; j <= std::min(MAXPIECE_BICOEF, i); j++) {

			// choose j from i:
			bicoef[i][j] = binomial_coefficient(i, j);
		}
		// define choosing 0 for simplicity 
		bicoef[i][0] = 1;
	}

	// choosing n from 0: bicoef = 0
	for (i = 1; i <= MAXPIECE_BICOEF; i++)
		bicoef[0][i] = 0;
}


