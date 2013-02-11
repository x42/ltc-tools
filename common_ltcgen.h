#ifndef COMMON_LTCGEN_H
#define COMMON_LTCGEN_H

#include <ltc.h>

// FORMAT [[[HH:]MM:]SS:]FF
enum { SMPTE_FRAME = 0, SMPTE_SEC, SMPTE_MIN, SMPTE_HOUR, SMPTE_OVERFLOW, SMPTE_LAST };

void encoder_setup(int fps_num, int fps_den, enum LTC_TV_STANDARD ltc_tv, int samplerate, int userbitmode);

void parse_fps(char *optarg);
void fps_sanity_checks();

void set_encoder_time(double usec, long int date, int tz_minuteswest, int fps_num, int fps_den, int print);

long long int bcdarray_to_framecnt(int bcd[SMPTE_LAST]);
void parse_string (int fps, int *bcd, char *val);
#endif
