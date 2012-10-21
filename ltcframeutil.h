#ifndef LTCFRAMEUTIL_H
#define LTCFRAMEUTIL_H

#include <stdio.h>
#include <ltc.h>

int cmp_ltc_frametime(LTCFrame *a, LTCFrame *b, int what);
void detect_fps(int *fps, SMPTETimecode *stime, int df, FILE *output);

#endif
