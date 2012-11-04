/*
 * JACK Linear Time Code encoder
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
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <ltc.h>

#include "timecode.h"

jack_port_t*       j_output_port = NULL;
jack_client_t*     j_client = NULL;
jack_ringbuffer_t* j_rb = NULL;
jack_nframes_t     j_latency = 0;
jack_nframes_t     j_samplerate = 48000;

LTCEncoder*  encoder = NULL;
ltcsnd_sample_t*      enc_buf = NULL;
int            underruns = 0;
int            cur_latency = 0;

pthread_mutex_t ltc_thread_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

int      active = 0; // 0: starting, 1:running, 2:shutdown
int      showdrift = 0; // set by signal handler
int      sync_initialized =0;
long int sync_offset_ms = 0;
int      reinit=1;

/* options */
int fps_num = 25;
int fps_den = 1;
int sync_now =1; // set to 1 to start timecode at date('now')
float volume_dbfs = -18.0;

void set_encoder_time(long int msec, long int date, int tz_minuteswest, int fps_num, int fps_den);
void cleanup(int sig);

int process (jack_nframes_t nframes, void *arg) {
  jack_default_audio_sample_t *out = jack_port_get_buffer (j_output_port, nframes);
  if (active != 1) {
    memset (out, 0, sizeof (jack_default_audio_sample_t) * nframes);
    return 0;
  }

  if (!sync_initialized) {
    /* compensate for initial jitter between program start and first audio-IRQ */
    sync_initialized=1;
    long int sync_msec;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    sync_msec = (t.tv_sec%86400)*1000 + (t.tv_nsec/1000000);

    if (sync_now) {
      time_t now = t.tv_sec;
      struct tm gm;
      long int sync_date = 0;
      if (gmtime_r(&now, &gm))
	sync_date = gm.tm_mday*10000 + gm.tm_mon*100 + gm.tm_year;

      sync_msec += nframes*1000/j_samplerate; // start next callback
      sync_offset_ms=nframes*1000LL / j_samplerate;
      set_encoder_time(sync_msec, sync_date, 0, fps_num, fps_den);
#if 1 // align fractional-frame msec with jack-period
      int frame = (int)floor((sync_msec%1000)*(double)fps_num/(double)fps_den/1000.0);
      int foff= ((1000*frame*fps_den/fps_num) - (sync_msec%1000));
      foff-=0; // slack - setup cost & clock_gettime latency . 0..5ms CPU and arch dep.
      cur_latency=(foff*(int)j_samplerate)/1000;
      cur_latency-= .0008 * j_samplerate; // fine-grained slack
#else
      cur_latency = 0;
#endif
    } else {
      LTCFrame lf;
      ltc_encoder_get_frame(encoder, &lf);
      int ms = frame_to_ms(&lf, fps_num, fps_den);
      sync_offset_ms=ms-sync_msec;
    }
    memset (out, 0, sizeof (jack_default_audio_sample_t) * nframes);
  } else {
#if 1 // compensate JACK port latency
    if (cur_latency != j_latency) {
      memset (out, 0, sizeof (jack_default_audio_sample_t) * nframes);
      int sa=jack_ringbuffer_read_space (j_rb)/ sizeof(jack_default_audio_sample_t);
      int ldiff = j_latency - cur_latency;
      if (ldiff>0) {
	if (sa > ldiff+nframes) sa=ldiff+nframes;
	jack_ringbuffer_read_advance (j_rb, sizeof(jack_default_audio_sample_t) * sa);
	cur_latency+=sa;
      }
      cur_latency-=nframes;
    }
    else
#endif
    if (jack_ringbuffer_read_space (j_rb) > sizeof(jack_default_audio_sample_t) * nframes) {
      jack_ringbuffer_read(j_rb, (void*) out, sizeof(jack_default_audio_sample_t) * nframes);
    } else {
      memset (out, 0, sizeof (jack_default_audio_sample_t) * nframes);
      underruns++;
    }
  }

  if (pthread_mutex_trylock (&ltc_thread_lock) == 0) {
    pthread_cond_signal (&data_ready);
    pthread_mutex_unlock (&ltc_thread_lock);
  }
  return 0;
}

void jack_shutdown (void *arg) {
  fprintf(stderr,"recv. shutdown request from jackd.\n");
  active=2;
  pthread_cond_signal (&data_ready);
  //cleanup(0);
}

int jack_graph_cb(void *arg) {
  jack_latency_range_t jlty;
  jack_port_get_latency_range(j_output_port, JackPlaybackLatency, &jlty);
  j_latency = jlty.max;
  return 0;
}


void init_jack() {
  jack_status_t status;
  jack_options_t options = JackNullOption;
  const char *client_name = "genltc";
  j_client = jack_client_open(client_name, options, &status);
  if (j_client == NULL) {
    fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      fprintf (stderr, "Unable to connect to JACK server\n");
    }
    exit(1);
  }
  jack_set_process_callback (j_client, process, 0);
  jack_on_shutdown (j_client, jack_shutdown, 0);
  jack_set_graph_order_callback (j_client, jack_graph_cb, NULL);
  j_samplerate=jack_get_sample_rate (j_client);

  if ((j_output_port = jack_port_register (j_client, "ltc", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0)) == 0) {
    fprintf (stderr, "cannot register jack output port \"ltc\".\n");
    cleanup(0);
  }

  const size_t rbsize = j_samplerate * sizeof(jack_default_audio_sample_t);
  j_rb = jack_ringbuffer_create (rbsize);
  jack_ringbuffer_mlock(j_rb);
  memset(j_rb->buf, 0, rbsize);

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client");
    cleanup(0);
  }
}

void jconnect(char * jack_autoconnect) {
  if (!jack_autoconnect) return;

  const char **ports = jack_get_ports(j_client, jack_autoconnect, NULL, JackPortIsInput);
  if (ports == NULL) {
    fprintf(stderr, "port '%s' not found\n", jack_autoconnect);
    return;
  }

  if (jack_connect (j_client, jack_port_name(j_output_port), ports[0])) {
    fprintf (stderr, "cannot connect output port %s to %s\n", jack_port_name (j_output_port), ports[0]);
    return;
  }
  jack_free (ports);
}

void encoder_setup(int fps_num, int fps_den, jack_nframes_t samplerate) {
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
  /* default range from libltc (38..218) || - 128.0  -> (-90..90) */
  const float smult = pow(10, volume_dbfs/20.0)/(90.0);
  pthread_mutex_lock (&ltc_thread_lock);
  int last_underruns=0;
  active=1;

  while(active==1) {
    if (!sync_initialized) {
      pthread_cond_wait (&data_ready, &ltc_thread_lock);
      continue;
    }
    if (reinit) {
      reinit=0;
      SMPTETimecode stime;
      memset(&stime, 0, sizeof(SMPTETimecode));
      strcpy(stime.timezone,"+0000");
      ltc_encoder_get_timecode(encoder, &stime);
      printf("start LTC: %02d/%02d/%02d (DD/MM/YY) %02d:%02d:%02d:%02d %s\n",
	  stime.days,stime.months,stime.years,
	  stime.hours,stime.mins,stime.secs,stime.frame,
	  stime.timezone
	  );
      jack_ringbuffer_reset(j_rb);
    }

    if (last_underruns != underruns) {
      last_underruns = underruns;
      printf("audio ringbuffer underrun (%d)\n", underruns);
    }

    if (showdrift) {
      showdrift=0;
      SMPTETimecode stime;
      int bo = jack_ringbuffer_read_space (j_rb)/sizeof(jack_default_audio_sample_t);
      LTCFrame lf;
      ltc_encoder_get_frame(encoder, &lf);
      int ms = frame_to_ms(&lf, fps_num, fps_den);
      ms-=(bo+cur_latency)*1000/(int)j_samplerate;
      ms-=sync_offset_ms;

      struct timespec t;
      clock_gettime(CLOCK_REALTIME, &t);
      int msec = (t.tv_sec%86400)*1000 + (t.tv_nsec/1000000);
      printf("drift: %+d ltc-frames (off: %+d ms | lat:%d)\n",(ms-msec)*fps_num/1000/fps_den, ms-msec, j_latency);
      ltc_encoder_get_timecode(encoder, &stime);
      printf("TC: %02d/%02d/%02d (DD/MM/YY) %02d:%02d:%02d:%02d %s\n",
	  stime.days,stime.months,stime.years,
	  stime.hours,stime.mins,stime.secs,stime.frame,
	  stime.timezone
	  );
    }

    const int precache = 8192;
    while (jack_ringbuffer_read_space (j_rb) < (precache * sizeof(jack_default_audio_sample_t))) {
      int byteCnt;
      for (byteCnt = 0; byteCnt < 10; byteCnt++) {
	int i;
	ltc_encoder_encode_byte(encoder, byteCnt, 1.0);
	const int len = ltc_encoder_get_buffer(encoder, enc_buf);
	for (i=0;i<len;i++) {
	  const float v1 = enc_buf[i] - 128;
	  jack_default_audio_sample_t val = (jack_default_audio_sample_t) (v1*smult);
	  if (jack_ringbuffer_write(j_rb, (void *)&val, sizeof(jack_default_audio_sample_t)) != sizeof(jack_default_audio_sample_t)) {
	    fprintf(stderr,"ERR: ringbuffer overflow\n");
	  }
	}
      } /* end byteCnt - one video frames's worth of LTC */
      ltc_encoder_inc_timecode(encoder);
    } /* while ringbuffer below limit */
    pthread_cond_wait (&data_ready, &ltc_thread_lock);
  }
  pthread_mutex_unlock (&ltc_thread_lock);
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
  {"volume", required_argument, 0, 'g'},
  {"date", required_argument, 0, 'd'},
  {"timezone", required_argument, 0, 'z'},
  {"minuteswest", required_argument, 0, 'm'},
  {"wait", required_argument, 0, 'w'},
  {"timecode", required_argument, 0, 't'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("ltcgen - JACK audio client to generate linear time code in realtime.\n");
  printf ("Usage: %s [OPTION] [JACK-PORT-TO-CONNECT]*\n", basename(program_name));
  printf ("\n"
"Options:\n"
" -d, --date datestring      set date, format is either DDMMYY or MM/DD/YY\n"
" -f, --fps fps              set frame-rate NUM[/DEN] default: 25/1 \n"
" -h, --help                 display this help and exit\n"
" -g, --volume float         set output level in dBFS default -18db\n"
" -m, --timezone tz          set timezone in minutes-west of UTC\n"
" -t, --timecode time        specify start-time/timecode [[[HH:]MM:]SS:]FF\n"
" -w, --wait                 wait for a key-stroke before starting.\n"
" -V, --version              print version information and exit\n"
" -z, --timezone tz          set timezone +HHMM\n"
"\n"
"Unless a timecode (-t) is given, the current time/date are used.\n"
"Date (-d) and timezone (-z, -m) are only used if a timecode is given.\n"
"The timezome may be specified either as HHMM zone, or in minutes-west of UTC.\n"
"\n"
"SIGINT (CTRL+C) prints current clock-drift (audio-clock - system-clock).\n"
"SIGQUIT (CTRL+\\) terminates the program.\n"
"SIGHUP initialize a re-sync to system clock (unless -t is given).\n"
"\n"
"Report bugs to <robin@gareus.org>.\n"
"Website and manual: <https://github.com/x42/ltc-tools>\n"
"\n");
  exit (status);
}

void cleanup(int sig) {
  active=2;
  if (j_client) {
    jack_deactivate(j_client);
    jack_client_close (j_client);
  }
  if (j_rb) jack_ringbuffer_free (j_rb);
  if (enc_buf) free(enc_buf);
  if (encoder) ltc_encoder_free(encoder);
  printf("bye.\n");
  exit(0);
}


void resync(int sig) {
  if (!sync_now) {
    //TODO: re-start at configured timecode?
    return;
  } else {
    // sync LTC to 'now'.
    sync_initialized=0;
    reinit=1;
  }
}

void printdebug(int sig) {
  showdrift=1;
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

int main (int argc, char **argv) {
  int c;

  program_name = argv[0];
  long long int msec = 0;// start timecode in ms from 00:00:00.00
  long int date = 0;// bcd: 201012 = 20 Oct 2012
  long int tzoff = 0;// time-zone in minuteswest
  int wait_for_key = 0;

  while ((c = getopt_long (argc, argv,
	   "h"	/* help */
	   "f:"	/* fps */
	   "d:"	/* date */
	   "g:"	/* gain^wvolume */
	   "t:"	/* timecode */
	   "z:"	/* timezone */
	   "m:"	/* timezone */
	   "w"	/* wait */
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

	case 'g':
	  volume_dbfs = atof(optarg);
	  if (volume_dbfs > 0) volume_dbfs=0;
	  if (volume_dbfs < -192.0) volume_dbfs=-192.0;
	  printf("Output volume %.2f dBfs\n", volume_dbfs);
	  break;

	case 'w':
	  wait_for_key=1;
	  break;

	case 'm':
	  tzoff=atoi(optarg); //minuteswest
	  break;

	case 'z':
	  {
	    int hh = atoi(optarg)/100;
	    tzoff= 60*hh + ((atoi(optarg)-(100*hh))%60); // HHMM
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

  init_jack();

  while (optind < argc) {
    jconnect(argv[optind++]);
  }


  encoder_setup(fps_num, fps_den, j_samplerate);

  if (sync_now==0) {
#if 0 // DEBUG
    printf("date: %06ld (DDMMYY)\n", date);
    printf("time: %lldms\n", msec);
    printf("zone: %c%02d%02d = %ld minutes west\n", tzoff<0?'-':'+', abs(tzoff/60),abs(tzoff%60), tzoff);
#endif
    set_encoder_time(msec, date, tzoff, fps_num, fps_den);
  }

  if (sync_now==0 && wait_for_key) {
    signal(SIGINT, cleanup);
    printf("Press 'Enter' to start.\n");
    fgetc(stdin);
  }

  signal(SIGQUIT, cleanup);
  signal(SIGINT, printdebug);
  signal(SIGHUP, resync);
  main_loop();
  cleanup(0);
  return(0);
}

/* vi:set ts=8 sts=2 sw=2: */
