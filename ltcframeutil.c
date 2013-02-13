/*
 * Copyright (C) 2012, 2013 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include "ltcframeutil.h"

/* what:
 *  bit 1: with user-fields/date
 *  bit 2: with parity
 */
int cmp_ltc_frametime(LTCFrame *a, LTCFrame *b, int what) {
  if ((what & 7) == 7) return memcmp(a, b, sizeof(LTCFrame));
  if (what & 4) {
    if (   a->col_frame != b->col_frame
	|| a->binary_group_flag_bit1 != b->binary_group_flag_bit1
	|| a->binary_group_flag_bit2 != b->binary_group_flag_bit2
	)
      return -1;
  }
  if (what & 2) {
    if (a->biphase_mark_phase_correction != b->biphase_mark_phase_correction)
      return -1;
  }
  if (what & 1) {
    if (   a->user1 != b->user1
	|| a->user2 != b->user2
	|| a->user3 != b->user3
	|| a->user4 != b->user4
	|| a->user5 != b->user5
	|| a->user6 != b->user6
	|| a->user7 != b->user7
	|| a->user8 != b->user8
       )
      return -1;
  }
  if (     a->frame_units != b->frame_units
	|| a->frame_tens  != b->frame_tens
	|| a->dfbit       != b->dfbit
	|| a->secs_units  != b->secs_units
	|| a->secs_tens   != b->secs_tens
	|| a->mins_units  != b->mins_units
	|| a->mins_tens   != b->mins_tens
	|| a->hours_units != b->hours_units
	|| a->hours_tens  != b->hours_tens
     )
      return -1;

  return 0;
}

int detect_discontinuity(LTCFrameExt *frame, LTCFrameExt *prev, int fps, int use_date, int fuzzyfps) {
    int discontinuity_detected = 0;

    if (fuzzyfps && (
	  (frame->reverse  && prev->ltc.frame_units == 0)
	||(!frame->reverse && frame->ltc.frame_units == 0)
	)){
      memcpy(prev, frame, sizeof(LTCFrameExt));
      return 0;
    }

    if (frame->reverse)
      ltc_frame_decrement(&prev->ltc, fps,
	  fps == 25? LTC_TV_625_50 : LTC_TV_525_60, use_date?LTC_USE_DATE:0);
    else
      ltc_frame_increment(&prev->ltc, fps,
	  fps == 25? LTC_TV_625_50 : LTC_TV_525_60, use_date?LTC_USE_DATE:0);
    if (cmp_ltc_frametime(&prev->ltc, &frame->ltc, use_date?1:0))
      discontinuity_detected = 1;
    memcpy(prev, frame, sizeof(LTCFrameExt));
    return discontinuity_detected;
}

int detect_fps(int *fps, LTCFrameExt *frame, SMPTETimecode *stime, FILE *output) {
  int rv =0;
  /* note: drop-frame-timecode fps rounded up, with the ltc.dfbit set */
  if (!fps) return -1;
  static int ff_cnt = 0;
  static int ff_max = 0;
  static LTCFrameExt prev;
  int df = (frame->ltc.dfbit)?1:0;

  if (!cmp_ltc_frametime(&prev.ltc, &frame->ltc, 0)) {
    ff_cnt = ff_max = 0;
  }
  if (detect_discontinuity(frame, &prev, *fps, 0, 1)) {
    ff_cnt = ff_max = 0;
  }
  if (stime->frame > ff_max) ff_max = stime->frame;
  ff_cnt++;
  if (ff_cnt > 40 && ff_cnt > ff_max) {
    if (*fps != ff_max + 1) {
      if (output) {
	fprintf(output, "# detected fps: %d%s\n", ff_max + 1, df?"df":"");
      }
      *fps = ff_max + 1;
      rv|=1;
    }
    rv|=2;
    ff_cnt = ff_max = 0;
  }
  return rv;
}
/* vi:set ts=8 sts=2 sw=2: */
