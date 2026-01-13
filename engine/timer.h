#pragma once
#define NOMINMAX
#include <Windows.h>
#include <chrono>

class QPCTimer {
public:
	QPCTimer() {QueryPerformanceFrequency(&frequency);} 
	void reset(void) {QueryPerformanceCounter(&t0);}
	double elapsed_usec(void) {
		LARGE_INTEGER t1, delta_time;
		QueryPerformanceCounter(&t1);
		delta_time.QuadPart = t1.QuadPart - t0.QuadPart;
		delta_time.QuadPart *= 1000000;
		delta_time.QuadPart /= frequency.QuadPart;
		return((double)delta_time.QuadPart);
	}

private:
	LARGE_INTEGER frequency;
	LARGE_INTEGER t0;
};


class Timer_hires {
public:
	void reset(void) {start = clock::now();}
	double elapsed_usec(void) {
		double elapsed = (double)(clock::now() - start).count();
		return(elapsed / 1000.0);
	}

private:
	typedef std::chrono::high_resolution_clock clock;

	clock::time_point start;
};

/* From https://www.learncpp.com/cpp-tutorial/timing-your-code/ */
class Timer
{
public:
	// Type aliases to make accessing nested type easier
	typedef std::chrono::steady_clock clock_type;
	typedef std::chrono::duration<double, std::ratio<1>> second_type;

	std::chrono::time_point<clock_type> t0;

	void reset(void)
	{
		t0 = clock_type::now();
	}

	double elapsed_secs(void) const
	{
		return std::chrono::duration_cast<second_type>(clock_type::now() - t0).count();
	}

	/* Safe means safe to be used as a divisor. No divide by zero is possible. */
	double safe_elapsed_secs(void) const
	{
		const double mintime = 0.001;
		double elapsed = elapsed_secs();
		if (elapsed < mintime)
			elapsed = mintime;
		return(elapsed);
	}

};

