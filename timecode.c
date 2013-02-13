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

#include <math.h>
#include "timecode.h"

long long int do_drop_frames (double fps, int f, int s, int m, int h) {
  int fpsi = (int) rint(fps); // 30.0 !
  long long int base_time = ((h*3600) + ((m/10) * 10 * 60)) * fps;
  long off_m = m % 10;
  long off_s = (off_m * 60) + s;
  long long off_f = (fpsi * off_s) + f - (2 * off_m);
  return (base_time + off_f);
}

long long int bcd_to_framecnt(double fps, int df, int f, int s, int m, int h) {
  if (df) {
    return do_drop_frames(fps, f, s, m, h);
  } else {
    return f + fps * ( s + 60*m + 3600*h);
  }
}

long long int ltcframe_to_framecnt(LTCFrame *lf, double fps) {
  int h= (lf->hours_units + lf->hours_tens*10);
  int m= (lf->mins_units  + lf->mins_tens*10);
  int s= (lf->secs_units  + lf->secs_tens*10);
  int f= (lf->frame_units + lf->frame_tens*10);
  return bcd_to_framecnt(fps, lf->dfbit, f, s, m, h);
}

double frame_to_ms(LTCFrame *f, int fps_num, int fps_den) {
  const double fps = ((double)fps_num) / ((double)fps_den);

  long long int frame_count = ltcframe_to_framecnt(f, fps);
  return ((double)(1000.0 * frame_count) / fps);
}
