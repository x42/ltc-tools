/*
 * Linear Time Code encoder
 * Copyright (C) 2012 Robin Gareus <robin@gareus.org>
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <libgen.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <ltc.h>
#include <sndfile.h>

#include "timecode.h"

static LTCEncoder * encoder = NULL;
static ltcsnd_sample_t * enc_buf = NULL;

/* options */
static int samplerate = 48000;
static int fps_num = 25;
static int fps_den = 1;
static int sync_now =1; // set to 1 to start timecode at date('now')

static long long int duration = 60000; // ms
static volatile int active = 0;

SNDFILE* sf = NULL;
int sf_format = SF_FORMAT_PCM_16;

void encoder_setup(int fps_num, int fps_den, int samplerate) {
  encoder = ltc_encoder_create(samplerate, fps_num/(double)fps_den, 1);
  enc_buf = calloc(ltc_encoder_get_buffersize(encoder),sizeof(ltcsnd_sample_t));
}

void set_encoder_time(long int msec, long int date, int tz_minuteswest, int fps_num, int fps_den) {
  double sec = msec/1000.0;
  SMPTETimecode st;
  sprintf(st.timezone, "%c%02d%02d", tz_minuteswest<0?'-':'+', abs(tz_minuteswest/60),abs(tz_minuteswest%60));
  st.years = date%100;
  st.months = (date/100)%100;
  st.days = (date/10000)%100;
  st.hours = (int)floor(sec/3600.0);
  st.mins  = (int)floor((sec-3600.0*floor(sec/3600.0))/60.0);
  st.secs  = (int)floor(sec)%60;
  st.frame = (int)floor((msec%1000)*(double)fps_num/(double)fps_den/1000.0);
  ltc_encoder_set_timecode(encoder, &st);
  printf("cfg LTC:   %02d/%02d/%02d (DD/MM/YY) %02d:%02d:%02d:%02d %s\n",
	  st.days,st.months,st.years,
	  st.hours,st.mins,st.secs,st.frame,
	  st.timezone);
}

void main_loop(void) {
  const long long int end = duration * samplerate / 1000;
  long long int written = 0;
  active=1;
  short *snd = NULL;

  while(active==1 && (duration <= 0 || end > written)) {
      int byteCnt;
      for (byteCnt = 0; byteCnt < 10; byteCnt++) {
	int i;
	ltc_encoder_encode_byte(encoder, byteCnt, 1.0);
	const int len = ltc_encoder_get_buffer(encoder, enc_buf);
	if (!snd) snd = malloc(len * sizeof(short));
	for (i=0;i<len;i++) {
	  const short val = (enc_buf[i] - 128) * 170;
	  snd[i] = val;
	}
	sf_writef_short(sf, snd, len);
	written += len;
      } /* end byteCnt - one video frames's worth of LTC */
      ltc_encoder_bump_timecode(encoder);
  }
  free(snd);
  printf("wrote %lld audio-samples\n", written);
}

/**************************
 * main application code
 */

char *program_name;

static struct option const long_options[] =
{
  {"help", no_argument, 0, 'h'},
  {"version", no_argument, 0, 'V'},
  {"fps", required_argument, 0, 'f'},
  {"date", required_argument, 0, 'd'},
  {"timezone", required_argument, 0, 'z'},
  {"minuteswest", required_argument, 0, 'm'},
  {"timecode", required_argument, 0, 't'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("ltcgen - generate linear time code audio-file.\n");
  printf ("Usage: %s [OPTION] <output-file>\n", basename(program_name));
  printf ("\n"
"Options:\n"
" -d, --date datestring      set date, format is either DDMMYY or MM/DD/YY\n"
" -f, --fps fps              set frame-rate NUM[/DEN] default: 25/1 \n"
" -h, --help                 display this help and exit\n"
" -l, --duration time        set duration of file to encode [[[HH:]MM:]SS:]FF.\n"
" -m, --timezone tz          set timezone in minutes-west of UTC\n"
" -s, --samplerate sr        specify samplerate (default 48000)\n"
" -t, --timecode time        specify start-time/timecode [[[HH:]MM:]SS:]FF\n"
" -V, --version              print version information and exit\n"
" -z, --timezone tz          set timezone +HHMM\n"
"\n"
"Unless a timecode (-t) is given, the current time/date are used.\n"
"Date (-d) and timezone (-z, -m) are only used if a timecode is given.\n"
"The timezome may be specified either as HHMM zone, or in minutes-west of UTC.\n"
"\n"
"If the duration is <=0, ltcgen write until it receives SIGINT.\n"
"\n"
"The output file-format is WAV, signed 16 bit, mono.\n"
"\n"
"Report bugs to <robin@gareus.org>.\n"
"Website and manual: <https://github.com/x42/ltc-tools>\n"
"\n");
  exit (status);
}

// FORMAT [[[HH:]MM:]SS:]FF
enum { SMPTE_FRAME = 0, SMPTE_SEC, SMPTE_MIN, SMPTE_HOUR, SMPTE_OVERFLOW, SMPTE_LAST };

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

long long int bcdarray_to_framecnt(int bcd[SMPTE_LAST]) {
  return bcd_to_framecnt(
      ((double)fps_num)/((double)fps_den),
      (fps_num==30000 && fps_den==1001)?1:0,
      bcd[SMPTE_FRAME],
      bcd[SMPTE_SEC],
      bcd[SMPTE_MIN],
      bcd[SMPTE_HOUR]
      );
}

void endnow(int sig) {
  active=2;
}

int main (int argc, char **argv) {
  int c;

  program_name = argv[0];
  long long int msec = 0;// start timecode in ms from 00:00:00.00
  long int date = 0;// bcd: 201012 = 20 Oct 2012
  long int tzoff = 0;// time-zone in minuteswest

  while ((c = getopt_long (argc, argv,
	   "h"	/* help */
	   "f:"	/* fps */
	   "d:"	/* date */
	   "l:"	/* duration */
	   "s:"	/* samplerate */
	   "t:"	/* timecode */
	   "z:"	/* timezone */
	   "m:"	/* timezone */
	   "V",	/* version */
	   long_options, (int *) 0)) != EOF)
  {
      switch (c) {
	case 'V':
	  printf ("%s %s\n\n",basename(argv[0]), VERSION);
	  printf (
		  "Copyright (C) 2012 Robin Gareus <robin@gareus.org>\n"
		  "This is free software; see the source for copying conditions.  There is NO\n"
		  "warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n\n"
		  );
	  exit (0);

	case 'h':
	  usage (0);

	case 'f':
	  {
	    fps_num=atoi(optarg);
	    char *tmp = strchr(optarg, '/');
	    if (tmp) {
	      fps_den=atoi(++tmp);
	    }
	    printf("LTC framerate: %d/%d fps\n", fps_num, fps_den);
	  }
	  break;

	case 'd':
	  {
	    date=atoi(optarg);
	    char *tmp = optarg;
	    if (tmp && (tmp = strchr(tmp, '/'))) date=date*100+(atoi(++tmp)*10000);
	    if (tmp) {
	      if ((tmp = strchr(tmp, '/'))) date+=atoi(++tmp);
	      else date+=12;// 2012
	    }
	  }
	  break;

	case 'm':
	  tzoff=atoi(optarg); //minuteswest
	  break;

	case 's':
	  samplerate=atoi(optarg);
	  break;

	case 'z':
	  {
	    int hh = atoi(optarg)/100;
	    tzoff= 60*hh + ((atoi(optarg)-(100*hh))%60); // HHMM
	  }
	  break;

	case 'l':
	  {
	    int bcd[SMPTE_LAST];
	    parse_string(rint(fps_num/(double)fps_den), bcd, optarg);
	    duration = bcdarray_to_framecnt(bcd) * 1000.0 / (((double)fps_num)/(double)fps_den);
	  }
	  break;

	case 't':
	  {
	    sync_now=0;
	    int bcd[SMPTE_LAST];
	    parse_string(rint(fps_num/(double)fps_den), bcd, optarg);
	    msec = bcdarray_to_framecnt(bcd) * 1000.0 / (((double)fps_num)/(double)fps_den);
	  }
	  break;

	default:
	  usage (EXIT_FAILURE);
      }
  }

  if (optind >= argc) {
    usage (EXIT_FAILURE);
  }

  {
    SF_INFO sfnfo;
    memset(&sfnfo, 0, sizeof(SF_INFO));
    sfnfo.samplerate = samplerate;
    sfnfo.channels = 1;
    sfnfo.format = SF_FORMAT_WAV | sf_format;
    sf = sf_open(argv[optind], SFM_WRITE, &sfnfo);
    if (!sf) {
      fprintf(stderr, "cannot open output file '%s'\n", argv[optind]);
      return 1;
    }
  }
  printf("writing to '%s'\n", argv[optind]);
  printf("samplerate: %d, duration %lld ms\n", samplerate, duration);

  encoder_setup(fps_num, fps_den, samplerate);

  if (sync_now==0) {
#if 0 // DEBUG
    printf("date: %06ld (DDMMYY)\n", date);
    printf("time: %lldms\n", msec);
    printf("zone: %c%02d%02d = %ld minutes west\n", tzoff<0?'-':'+', abs(tzoff/60),abs(tzoff%60), tzoff);
#endif
    set_encoder_time(msec, date, tzoff, fps_num, fps_den);
  } else {
    struct timespec t;
    long int sync_msec;
    clock_gettime(CLOCK_REALTIME, &t);
    sync_msec = (t.tv_sec%86400)*1000 + (t.tv_nsec/1000000);

    time_t now = t.tv_sec;
    struct tm gm;
    long int sync_date = 0;
    if (gmtime_r(&now, &gm))
      sync_date = gm.tm_mday*10000 + gm.tm_mon*100 + gm.tm_year;
    set_encoder_time(sync_msec, sync_date, 0, fps_num, fps_den);
  }

  signal(SIGINT, endnow);

  main_loop();

  if (sf) sf_close(sf); 
  if (enc_buf) free(enc_buf);
  if (encoder) ltc_encoder_free(encoder);
  return(0);
}

/* vi:set ts=8 sts=2 sw=2: */
