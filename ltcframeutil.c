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
  if (   a->frame_units != b->frame_units
	|| a->frame_tens != b->frame_tens
	|| a->dfbit != b->dfbit
	|| a->secs_units != b->secs_units
	|| a->secs_tens != b->secs_tens
	|| a->mins_units != b->mins_units
	|| a->mins_tens != b->mins_tens
	|| a->hours_units != b->hours_units
	|| a->hours_tens != b->hours_tens
     )
      return -1;

  return 0;
}

void detect_fps(int *fps, SMPTETimecode *stime, int df, FILE *output) {
  /* note: drop-frame-timecode fps rounded up, with the ltc.dfbit set */
  if (!fps) return;
  static int ff_cnt = 0;
  static int ff_max = 0;
  if (stime->frame > ff_max) ff_max = stime->frame;
  ff_cnt++;
  if (ff_cnt > 40 && ff_cnt > ff_max) {
    if (*fps != ff_max + 1) {
      if (output) {
	fprintf(output, "# detected fps: %d%s\n", ff_max + 1, df?"df":"");
      }
      *fps = ff_max + 1;
    }
    ff_cnt = ff_max = 0;
  }
}
/* vi:set ts=8 sts=2 sw=2: */
