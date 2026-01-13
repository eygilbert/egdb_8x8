#pragma once
#include <stdint.h>
#include "project.h"

#define MAXSQUARE_BICOEF 32
#define MAXPIECE_BICOEF 7

extern unsigned int bicoef[MAXSQUARE_BICOEF + 1][MAXPIECE_BICOEF + 1];

void initbicoef(void);

/*
 * Return the number of ways to choose k objects from a set of n objects.
 */
inline unsigned int choose(int n, int k)
{
	return(bicoef[n][k]);
}

