#pragma once
#include <stdint.h>
#include <string>


inline std::string format_with_commas(int64_t val)
{
	int i, count;
	char buf[30];
	std::string result;

	sprintf(buf, "%lld", val);
	for (count = 1, i = (int)strlen(buf) - 1; i >= 0; --i, ++count) {
		result = buf[i] + result;
		if ((count % 3) == 0 && i > 0)
			result = "," + result;
	}
	return(result);
}


