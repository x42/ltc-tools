#ifndef TIMECODE_H
#define TIMECODE_H

#include <ltc.h>

long long int do_drop_frames (double fps, int f, int s, int m, int h);
long long int bcd_to_framecnt(double fps, int df, int f, int s, int m, int h);
long long int ltcframe_to_framecnt(LTCFrame *lf, double fps);
long long frame_to_ms(LTCFrame *f, int fps_num, int fps_den);

#endif
