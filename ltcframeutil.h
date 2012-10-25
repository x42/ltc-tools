#ifndef LTCFRAMEUTIL_H
#define LTCFRAMEUTIL_H

#include <stdio.h>
#include <ltc.h>

int cmp_ltc_frametime(LTCFrame *a, LTCFrame *b, int what);
int detect_fps(int *fps, LTCFrameExt *frame, SMPTETimecode *stime, FILE *output);
int detect_discontinuity(LTCFrameExt *frame, LTCFrameExt *prev, int fps, int fuzzyfps);

#endif
