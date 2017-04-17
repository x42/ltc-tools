/*
 * Copyright (C) 2013 Robin Gareus <robin@gareus.org>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "timecode.h"
#include "common_ltcgen.h"

extern ltcsnd_sample_t* enc_buf;
extern LTCEncoder*  encoder;

extern int fps_num;
extern int fps_den;
extern int fps_drop;
extern enum LTC_TV_STANDARD ltc_tv;

void encoder_setup(int fps_num, int fps_den, enum LTC_TV_STANDARD ltc_tv, int samplerate, int userbitmode) {
  encoder = ltc_encoder_create(samplerate,
      fps_num/(double)fps_den, ltc_tv,
      userbitmode);
  enc_buf = calloc(ltc_encoder_get_buffersize(encoder),sizeof(ltcsnd_sample_t));
}

static const char *ltc_tv_modes[4] = {
  "TV 525/60",  // LTC_TV_525_60, ///< 30fps
  "TV 625/50",  // LTC_TV_625_50, ///< 25fps
  "TV 1125/60", // LTC_TV_1125_60,///< 30fps
  "FILM"        // LTC_TV_FILM_24 ///< 24fps
};

void parse_fps(char *optarg) {
	fps_num=atoi(optarg);
	char *tmp = strchr(optarg, '/');
	if (tmp) {
		fps_den=atoi(++tmp);
	}
	fps_drop = (fps_num==30000 && fps_den==1001)?1:0;
	if (strstr(optarg, "ndf")) {
		fps_drop = 0;
	} else if (strstr(optarg, "df")) {
		fps_drop = 1;
	}
	switch ((int) ceil(fps_num/(double)fps_den)) {
		case 25:
			ltc_tv = LTC_TV_625_50;
			break;
		case 30:
			ltc_tv = fps_drop? LTC_TV_525_60 : LTC_TV_1125_60;
		default:
			// TODO allow to configure LTC_TV standard
			/* NB. LTC_TV_FILM_24 means
			 * - exactly align LTC-frame boundary with video-frame boundary
			 * - use SMPTE binary-group-flags mode (not EBU 25fps mode)
			 * this should be good for all non-standard cases.
			 */
		case 24:
			ltc_tv = LTC_TV_FILM_24;
			break;
	}
	printf("LTC framerate: %d/%d fps (%s) -- %s\n", fps_num, fps_den,
			fps_drop ? "drop-frame" : "non-drop-frame",
			ltc_tv_modes[ltc_tv]
			);
}

void fps_sanity_checks() {
  int warn = 0;
  if ( (fps_num/(double)fps_den) != 24
    && (fps_num/(double)fps_den) != 25
    && (fps_num/(double)fps_den) != 30
    && (floor(100.0 * fps_num / (double) fps_den) != 2997)
    ) {
      printf("Note: There is no official spec for the chosen fps.\n      Valid choises are 24 ,25, 30000/1001 and 30.\n");
      warn = 1;
  }

  if ( (floor(100.0 * fps_num / (double) fps_den) == 2997) && !fps_drop) {
    printf("Note: SMPTE-12M requires 29.97fps to be drop-frame.\n");
    warn = 1;
  }

  if ( (floor(100.0 * fps_num / (double) fps_den) != 2997) && fps_drop) {
    printf("Note: Only 30000/1001fps may use drop-frame counting.\n");
    warn = 1;
  }

  if (warn) {
    printf("Warning: The encoded LTC may or may not be what you want.\n");
  }
}

void set_encoder_time(double usec, long int date, int tz_minuteswest, int fps_num, int fps_den, int print) {
  double sec = usec/1000000.0;
  SMPTETimecode st;
  sprintf(st.timezone, "%c%02d%02d", tz_minuteswest<0?'-':'+', abs(tz_minuteswest/60),abs(tz_minuteswest%60));
  st.years = date%100;
  st.months = (date/100)%100;
  st.days = (date/10000)%100;
  st.hours = (int)floor(sec/3600.0);
  st.mins  = (int)floor((sec-3600.0*floor(sec/3600.0))/60.0);
  st.secs  = (int)floor(sec)%60;
  st.frame = (int)floor(((long long int)floor(usec)%1000000)*(double)fps_num/(double)fps_den/1000000.0);
  ltc_encoder_set_timecode(encoder, &st);
#if 1
  if (ceil(fps_num/(double)fps_den) == 30) {
    /* libltc recognizes 29.97 and 30000/1001 as drop-frame TC.
     * while there is no official spec for 29.97ndf, we
     * educate the user, but don't stop him.
     * If 29.97ndf is what s/he want. 29.97ndf is what s/he gets.
     */
    LTCFrame ltcframe;
    ltc_encoder_get_frame(encoder, &ltcframe);
    ltcframe.dfbit = fps_drop?1:0;
    ltc_encoder_set_frame(encoder, &ltcframe);
  }
#endif
  if (print) {
    printf("cfg LTC:   %02d/%02d/%02d (DD/MM/YY) %02d:%02d:%02d:%02d %s\n",
	  st.days,st.months,st.years,
	  st.hours,st.mins,st.secs,st.frame,
	  st.timezone);
  }
}

void set_user_bits(unsigned char user_bit_array[MAX_USER_BITS])
{
  LTCFrame f;
  ltc_encoder_get_frame(encoder, &f);
  f.user1 = user_bit_array[0];
  f.user2 = user_bit_array[1];
  f.user3 = user_bit_array[2];
  f.user4 = user_bit_array[3];
  f.user5 = user_bit_array[4];
  f.user6 = user_bit_array[5];
  f.user7 = user_bit_array[6];
  f.user8 = user_bit_array[7];
  ltc_encoder_set_frame(encoder, &f);
}

long long int bcdarray_to_framecnt(int bcd[SMPTE_LAST]) {
  return bcd_to_framecnt(
      ((double)fps_num)/((double)fps_den), fps_drop,
      bcd[SMPTE_FRAME],
      bcd[SMPTE_SEC],
      bcd[SMPTE_MIN],
      bcd[SMPTE_HOUR]
      );
}


#define FIX_SMPTE_OVERFLOW(THIS,NEXT,INC) \
        if (bcd[(THIS)] >= (INC)) { int ov= (int) floor((double) bcd[(THIS)] / (INC));  bcd[(THIS)] -= ov*(INC); bcd[(NEXT)]+=ov;} \
        if (bcd[(THIS)] < 0 )     { int ov= (int) floor((double) bcd[(THIS)] / (INC));  bcd[(THIS)] -= ov*(INC); bcd[(NEXT)]+=ov;}

void parse_string (int fps, int *bcd, char *val) {
        int i;
        char *buf = strdup(val);
        char *t;

        for (i=0;i<SMPTE_LAST;i++) bcd[i]=0;

        i=0;
        while (i < SMPTE_OVERFLOW && buf && (t=strrchr(buf,':'))) {
                char *tmp=t+1;
                bcd[i] = (int) atoi(tmp);
                *t=0;
                i++;
        }
        if (i < SMPTE_OVERFLOW) bcd[i]= (int) atoi(buf);

        free(buf);
        int smpte_table[SMPTE_LAST] =  { 1, 60, 60, 24, 0 };
        smpte_table[0] = fps;
        for (i = 0;(i+1)<SMPTE_LAST;i++)
                FIX_SMPTE_OVERFLOW(i, i+1, smpte_table[i]);
}

void parse_user_bits (unsigned char user_bit_array[MAX_USER_BITS], const char *opt) {
        int user_number = atoi(opt);
        if (user_number > MAX_BCD_NUMBER)
                user_number = MAX_BCD_NUMBER;
        if (user_number < 0)
                user_number = 0;
        for (int i = 0; i < MAX_USER_BITS; ++i) {
                user_bit_array[i] = user_number % 10;
                user_number /= 10;
        }
}
