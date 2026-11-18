#pragma once
#include <stdint.h>
#include <string>


inline std::string format_with_commas(int64_t val)
{
	int i, count, first;
	char buf[30];
	std::string result;

	sprintf(buf, "%lld", val);

	/* Group only the digits; a minus sign is added back in front. */
	first = (buf[0] == '-') ? 1 : 0;
	for (count = 1, i = (int)strlen(buf) - 1; i >= first; --i, ++count) {
		result = buf[i] + result;
		if ((count % 3) == 0 && i > first)
			result = "," + result;
	}
	if (first)
		result = "-" + result;
	return(result);
}


