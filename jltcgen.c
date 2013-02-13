/* JACK Linear Time Code encoder
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
#include "common_ltcgen.h"

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
double   sync_offset_ms = 0;
int      reinit=1;

/* options */
int fps_num = 25;
int fps_den = 1;
int fps_drop = 0;
enum LTC_TV_STANDARD ltc_tv = LTC_TV_625_50;

int sync_now =1; // set to 1 to start timecode at date('now')
float volume_dbfs = -18.0;

void set_encoder_time(double usec, long int date, int tz_minuteswest, int fps_num, int fps_den, int print);
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
    double sync_usec;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    sync_usec = (t.tv_sec%86400)*1000000.0 + (t.tv_nsec/1000.0);

    if (sync_now) {
      time_t now = t.tv_sec;
      struct tm gm;
      long int sync_date = 0;
      if (gmtime_r(&now, &gm))
	sync_date = gm.tm_mday*10000 + gm.tm_mon*100 + gm.tm_year;

      sync_usec += nframes*1000000.0/(double)j_samplerate; // start next callback
      sync_offset_ms= nframes*1000.0 / (double) j_samplerate;
      sync_usec += 1000000.0 * ltc_frame_alignment(j_samplerate * fps_den / (double) fps_num, ltc_tv) / j_samplerate;
      set_encoder_time(sync_usec, sync_date, 0, fps_num, fps_den, 0);
#if 1 // align fractional-frame msec with jack-period
      int frame = (int)floor(((long long int)floor(sync_usec)%1000000)*(double)fps_num/(double)fps_den/1000000.0);
      double foff= 1000000.0*(frame*(double)fps_den/(double)fps_num) - ((long long int)floor(sync_usec)%1000000);
      foff+=30; // slack - setup cost & clock_gettime latency . 0..2ms CPU and arch dep.
      cur_latency=rint((foff*(double)j_samplerate)/1000000.0);
#else
      cur_latency = 0;
#endif
    } else {
      LTCFrame lf;
      ltc_encoder_get_frame(encoder, &lf);
      double ms = frame_to_ms(&lf, fps_num, fps_den);
      sync_offset_ms = ms - sync_usec/1000.0;
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
      double us = frame_to_ms(&lf, fps_num, fps_den) * 1000.0;
      us-=(bo+cur_latency)*1000000.0 / (double)j_samplerate;
      us-=sync_offset_ms * 1000.0;

      struct timespec t;
      clock_gettime(CLOCK_REALTIME, &t);
      double usec = (t.tv_sec%86400)*1000000.0 + (t.tv_nsec/1000.0);
      printf("drift: %+.1f ltc-frames (off: %+.2f ms | lat:%d as)\n",(us-usec)*fps_num/1000000.0/fps_den, (us-usec)/1000.0, j_latency);
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
    if (active != 1) break;
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
" -f, --fps fps              set frame-rate NUM[/DEN][ndf|df] default: 25/1ndf \n"
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
	  parse_fps(optarg);
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

  fps_sanity_checks();

  init_jack();

  while (optind < argc) {
    jconnect(argv[optind++]);
  }

  encoder_setup(fps_num, fps_den, ltc_tv, j_samplerate,
      ((date != 0) ? LTC_USE_DATE : 0) | ((sync_now) ? (LTC_USE_DATE|LTC_TC_CLOCK) : 0)
      );

  if (sync_now==0) {
    set_encoder_time(msec*1000.0, date, tzoff, fps_num, fps_den, 1);
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
