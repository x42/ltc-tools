#ifndef _MY_CLOCK_H
#define _MY_CLOCK_H

#include <time.h>
#include <sys/time.h>

#ifdef __APPLE__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

static inline int my_clock_gettime(struct timespec *ts) {
#ifdef __APPLE__
	clock_serv_t cclock;
	mach_timespec_t mts;
	host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
	clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);
	ts->tv_sec = mts.tv_sec;
	ts->tv_nsec = mts.tv_nsec;
	return 0;
#else
	return clock_gettime(CLOCK_REALTIME, ts);
#endif
}
#endif
