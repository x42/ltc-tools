/* jack linear time code decoder
 * Copyright (C) 2006,2012 Robin Gareus
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

#define LTC_QUEUE_LEN (30) // should be >> ( max(jack period size) / (duration of LTC-frame) )
#define RBSIZE (128) // should be > ( max(duration of LTC-frame) / min(jack period size) )
                     // duration of LTC-frame= sample-rate / fps
                     // min(jack period size) = 16 or 32, usually >=64

#define _GNU_SOURCE

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <getopt.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <sys/mman.h>
#include <ltc.h>

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

static jack_port_t **input_port = NULL;
static jack_default_audio_sample_t **in = NULL;

static jack_client_t *j_client = NULL;
static jack_nframes_t j_latency = 0;
static uint32_t j_samplerate = 48000;
static const double signal_latency = 0.04; // in seconds (avg. w/o jitter)

static int nports = 0;

static LTCDecoder *decoder = NULL;
static volatile long long int monotonic_fcnt = 0;
jack_ringbuffer_t *rb = NULL;

static pthread_mutex_t ltc_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

static FILE *output;
static char *fileprefix=NULL;

static int use_signals = 0;
static int detect_framerate = 0;
static int fps_num = 25;
static int fps_den = 1;
static float rs_thresh = 0.02;
static int detected_fps;

/* TODO make a linear buffer of those.
 * To allow multiple start/stop events in a single cycle
 */
static volatile struct {
  enum {Idle, Starting, Started, Stopped} state;
  struct timespec ev_start;
  struct timespec ev_end;
  ltc_off_t audio_frame_start;
  ltc_off_t audio_frame_end;
} event_info;

/* a simple state machine for this client */
static volatile enum {
  Init,
  Run,
  Exit
} client_state = Init;

struct syncInfo {
  struct timespec tme;
  long long int fcnt;
  jack_nframes_t fpp;
};

void timespec_mult (
    struct timespec *res,
    const struct timespec *val,
    const double fact) {
  const double sec = (double) val->tv_sec * fact;
  const double rem_sec  = sec - floor(sec);

  const double nsec = val->tv_nsec * fact + 1000000000.0 * rem_sec;
  const double rem_ns  = nsec - 1000000000.0 * floor(nsec/1000000000.0);

  res->tv_sec = floor(sec + nsec / 1000000000.0);
  res->tv_nsec = rem_ns;
}

void timespec_add (
    struct timespec *res,
    const struct timespec *val1,
    const struct timespec *val2) {
  res->tv_sec = val1->tv_sec + val2->tv_sec;

  if (val1->tv_nsec + val2->tv_nsec < 1000000000 ) {
    res->tv_nsec = val1->tv_nsec + val2->tv_nsec;
  } else {
    res->tv_sec++;
    res->tv_nsec = val1->tv_nsec + val2->tv_nsec - 1000000000;
  }
}

void timespec_sub (
    struct timespec *res,
    const struct timespec *val1,
    const struct timespec *val2) {
  res->tv_sec = val1->tv_sec - val2->tv_sec;
  if (val1->tv_nsec < val2->tv_nsec) {
    res->tv_sec--;
    res->tv_nsec = val1->tv_nsec - val2->tv_nsec + 1000000000;
  } else {
    res->tv_nsec = val1->tv_nsec - val2->tv_nsec;
  }
}

void interpolate_tc(struct timespec *result, struct syncInfo *s0, struct syncInfo *s1, ltc_off_t off) {
  struct timespec calc;
  const double fact = (off - s0->fcnt) / (double) (s1->fcnt - s0->fcnt);
  timespec_sub(&calc, &s1->tme, &s0->tme);
  timespec_mult(&calc, &calc, fact);
  timespec_add(result, &s0->tme, &calc);
}

/**
 * cleanup and exit
 * call this function only _after_ everything has been initialized!
 */
static void cleanup(int sig) {
  if (j_client) {
    jack_client_close (j_client);
    j_client=NULL;
  }

  ltc_decoder_free(decoder);
  free(in);
  free(input_port);
  if (rb) jack_ringbuffer_free(rb);
  fprintf(stderr, "bye.\n");
}


void event_start (long long fcnt) {
  if (event_info.state != Idle) {
    fprintf(stderr, "sig-activate ignored -- not idle\n");
    return;
  }
  clock_gettime(CLOCK_REALTIME, (struct timespec*) &event_info.ev_start);
  event_info.audio_frame_start = fcnt;
  event_info.state = Starting;
}

void event_end (long long int fcnt) {
  if (event_info.state == Starting) {
    event_info.state = Idle;
    fprintf(stderr, "sig-end -- flapping (Starting -> Idle)\n");
    return;
  }
  if (event_info.state != Started) {
    fprintf(stderr, "sig-end ignore -- not started\n");
    return;
  }
  clock_gettime(CLOCK_REALTIME, (struct timespec*) &event_info.ev_end);
  event_info.audio_frame_end = fcnt;
  event_info.state = Stopped;
}

static void detect_fps(SMPTETimecode *stime) {
#if 1 // detect fps: TODO use the maximum
      // frameno found in stream during the last N seconds
      // _not_ all time maximum
  if (detect_framerate) {
    static int ff_cnt = 0;
    static int ff_max = 0;
    if (stime->frame > ff_max) ff_max = stime->frame;
    ff_cnt++;
    if (ff_cnt > 60 && ff_cnt > ff_max) {
      detected_fps = ff_max + 1;
      ff_cnt= 61; //XXX prevent overflow..
    }
  }
#endif
}
/**
 *
 */
static void my_decoder_read(LTCDecoder *d) {
  static LTCFrameExt prev_time;
  static int frames_in_sequence = 0;
  LTCFrameExt frame;

  if (event_info.state == Idle) {
    int i=0;
    // read some frames to prevent queue overflow
    // but keep some in queue.
    int frames_in_queue = ltc_decoder_queue_length(d);
    for (i=LTC_QUEUE_LEN/2; i < frames_in_queue; i++) {
      SMPTETimecode stime;
      ltc_decoder_read(d,&frame);
      ltc_frame_to_time(&stime, &frame.ltc, 0);
      detect_fps(&stime);
      memcpy(&prev_time, &frame, sizeof(LTCFrameExt));
    }
    return;
  }
  if (event_info.state == Stopped) {
    // keep processing frames until (frame.off_end > event_info.audio_frame_end)
    if (prev_time.off_end > event_info.audio_frame_end) {
      event_info.state = Idle;

      // close TME file
      if (output)
	fprintf(output, "#End: sample: %lld tme: %ld.%09ld\n",
	    event_info.audio_frame_end,
	    event_info.ev_end.tv_sec, event_info.ev_end.tv_nsec
	    );

      if (fileprefix && output) {
	fclose(output);
	output=NULL;
      }
      return;
    }
  }
  if (event_info.state == Starting) {
    // open new TME file
    if (fileprefix && use_signals) {
      char tme[16];
      struct tm *now;
      time_t t = time(NULL);
      now = gmtime(&t);

      strftime(tme, 16, "%Y%m%d-%H%M%S", now);
      char *path = malloc(strlen(fileprefix) + 14 + 16);

      sprintf(path, "%s-%s.tme.XXXXXX", fileprefix, tme);
      int fd = mkstemp(path);
      if (fd<0) {
	fprintf(stderr, "error opening output file\n");
	output = NULL;
      }
      else {
	output = fdopen(fd, "a");
      }
      free(path);
    }
    else if (fileprefix) {
      output = fopen(fileprefix, "a");
    }

    if (output) {
      fprintf(output, "#Start: sample: %lld tme: %ld.%09ld\n",
	  event_info.audio_frame_start,
	  event_info.ev_start.tv_sec, event_info.ev_start.tv_nsec
	  );
      fflush(output);
    }
    event_info.state = Started;
    frames_in_sequence = 0;
  }

  int avail_tc = jack_ringbuffer_read_space (rb) / sizeof(struct syncInfo);
  struct syncInfo *tcs = calloc(avail_tc, sizeof (struct syncInfo));
  jack_ringbuffer_peek(rb, (void*) tcs, avail_tc * sizeof(struct syncInfo));
  int processed_tc = 0;

  while (ltc_decoder_read(d,&frame)) {
    SMPTETimecode stime;
    ltc_frame_to_time(&stime, &frame.ltc, 0);
    detect_fps(&stime);

    int discontinuity_detected = 0;

    /* detect discontinuities */
    ltc_frame_increment(&prev_time.ltc, detected_fps , 0);
    if (memcmp(&prev_time.ltc, &frame.ltc, sizeof(LTCFrame))) {
      discontinuity_detected = 1;
    }
    memcpy(&prev_time, &frame, sizeof(LTCFrameExt));

    if (use_signals) {
      // skip frames that have (frame.off_start < event_info.audio_frame_start)
      if (frame.off_start < event_info.audio_frame_start) continue;
      // skip frames that have (frame.off_end > event_info.audio_frame_end)
      if (event_info.state == Stopped &&
	  frame.off_end > event_info.audio_frame_end) continue;
    }

    /* notfify about discontinuities */
    if (frames_in_sequence > 0 && discontinuity_detected) {
      if (output)
	fprintf(output, "#DISCONTINUITY\n");
    }
    frames_in_sequence++;


    struct timespec tc_start = {0, 0};
    struct timespec tc_end = {0, 0};
    int tc_set=0;

    int tcl;
    for (tcl=0; tcl < avail_tc-1; tcl++) {
      if (tcs[tcl].fcnt < frame.off_start) {
	processed_tc = tcl;
      }
      if (tcs[tcl].fcnt < frame.off_start && tcs[tcl+1].fcnt > frame.off_start) {
	interpolate_tc(&tc_start, &tcs[tcl], &tcs[tcl+1], frame.off_start);
	tc_set|=1;
      }
      if (tcs[tcl].fcnt < frame.off_end && tcs[tcl+1].fcnt > frame.off_end) {
	interpolate_tc(&tc_end, &tcs[tcl], &tcs[tcl+1], frame.off_end);
	tc_set|=2;
      }
    }

    if (avail_tc > 1) {
      if ((tc_set&1) == 0) {
	interpolate_tc(&tc_start, &tcs[processed_tc], &tcs[processed_tc+1], frame.off_start);
      }
      if ((tc_set&2) == 0) {
	interpolate_tc(&tc_end, &tcs[avail_tc-2], &tcs[avail_tc-1], frame.off_end);
      }
    }

    if (output)
      fprintf(output, "%02d:%02d:%02d%c%02d | %8lld %8lld%s | %ld.%09ld  %ld.%09ld\n",
	  stime.hours,
	  stime.mins,
	  stime.secs,
	  (frame.ltc.dfbit) ? '.' : ':',
	  stime.frame,
	  frame.off_start,
	  frame.off_end,
	  frame.reverse ? " R" : "  ",
	  (long int) tc_start.tv_sec, tc_start.tv_nsec,
	  (long int) tc_end.tv_sec, tc_end.tv_nsec
	  );
  }

  if (avail_tc > RBSIZE - 4 ) {
    processed_tc+= avail_tc - (RBSIZE - 4);
  }

  if (processed_tc > 0) {
    jack_ringbuffer_read_advance(rb, processed_tc * sizeof(struct syncInfo));
  }
  free(tcs);

  if (output) {
    fflush(output);
  }
}

static int parse_ltc(jack_nframes_t nframes, jack_default_audio_sample_t *in, long int posinfo) {
  jack_nframes_t i;
  unsigned char sound[8192];
  if (nframes > 8192) return 1;

  for (i = 0; i < nframes; i++) {
    const int snd=(int)rint((127.0*in[i])+128.0);
    sound[i] = (unsigned char) (snd&0xff);
  }
  ltc_decoder_write(decoder, sound, nframes, posinfo);
  return 0;
}

struct RSParser {
  float y1; ///< previous sample;
  int snd_cnt;

  int state;
  int state_timeout;
};

static void parse_rs(jack_nframes_t nframes, jack_default_audio_sample_t *in, long int posinfo) {
  static struct RSParser rsparser = {0, 0}; //XXX
  static struct RSParser *rsp =  & rsparser;
  jack_nframes_t s;
  const float alpha = 0.7;
  float max = 0.0;
  for (s=0; s < nframes; ++s)  {
    const float y = rsp->y1 + alpha * ( in[s] - rsp->y1 );
    rsp->y1 = y;
    const float y_2 = y*y;
    if (y_2 > max) max= y_2;

    /* check if signal is above threshold -> new peak */
    if (y_2 > rs_thresh) {
      rsp->snd_cnt++;
    } else {
#if 0 // debug
      if (rsp->snd_cnt > 0) {
	fprintf(stderr, " SIG DURATION : %d\n", rsp->snd_cnt);
      }
#endif
      rsp->snd_cnt = 0;
    }

    /* once for each R/S peak */
    if (rsp->snd_cnt == 1 /* depends on SR */) {
#if 0
      fprintf(stderr, "Frame @ %ld\n", posinfo + s);
#endif
      if (rsp->state == 0) {
#if 0
      fprintf(stderr, "REC START @ %ld\n", posinfo + s);
#endif
	rsp->state = 1;
	event_start(posinfo + s);
      }
    }

    if (rsp->snd_cnt >= 1 /* depends on SR */) {
      rsp->state_timeout=0;
    }

    /* track R/S state */
    if (rsp->snd_cnt == 0) {
      rsp->state_timeout++;
      /* we expect two R/S signals per video-frame
       * but we should parse a bit further..
       */
      const int rs_timeout = 1.5 * j_samplerate * fps_den / fps_num;
      if (rsp->state_timeout > rs_timeout && rsp->state == 1) {
#if 0
      fprintf(stderr, "REC END   @ %ld\n", posinfo + s - rsp->state_timeout);
#endif
	rsp->state = 0;
	event_end(posinfo + s);
      }
    }

  }
  //fprintf(stderr, " MAX SIGNAL : %.5f\n", max);
}

/**
 * jack audio process callback
 */
int process (jack_nframes_t nframes, void *arg) {
  int i;

  // save monotonic_fcnt, clock, nframes.
  if (jack_ringbuffer_write_space(rb) > sizeof(struct syncInfo) ) {
    struct syncInfo si;
    si.fcnt = monotonic_fcnt - j_latency;
    si.fpp = nframes;
    clock_gettime(CLOCK_REALTIME, &si.tme);
    jack_ringbuffer_write(rb, (void *) &si, sizeof(struct syncInfo));
  }

  for (i=0;i<nports;i++) {
    in[i] = jack_port_get_buffer (input_port[i], nframes);
  }

  parse_ltc(nframes, in[0], monotonic_fcnt - j_latency);

  for (i=1;i<nports;i++) {
    parse_rs(nframes, in[i], monotonic_fcnt - j_latency);
    // TODO check 2nd audio port for event
    // call  event_start(monotonic_fcnt + offset);  // falling edge
    // or    event_end(monotonic_fcnt + offset);    // rising edge
  }

  monotonic_fcnt += nframes;

  if (pthread_mutex_trylock (&ltc_thread_lock) == 0) {
    pthread_cond_signal (&data_ready);
    pthread_mutex_unlock (&ltc_thread_lock);
  }

  return 0;
}

int jack_latency_cb(void *arg) {
  jack_latency_range_t jlty;
  if (!input_port) return 0;
  jack_port_get_latency_range(input_port[0], JackCaptureLatency, &jlty);
  j_latency = jlty.max;
  return 0;
}


void jack_shutdown (void *arg) {
  fprintf(stderr,"recv. shutdown request from jackd.\n");
  client_state=Exit;
}

/**
 * open a client connection to the JACK server
 */
static int init_jack(const char *client_name) {
  jack_status_t status;
  j_client = jack_client_open (client_name, JackNullOption, &status);
  if (j_client == NULL) {
    fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      fprintf (stderr, "Unable to connect to JACK server\n");
    }
    return (-1);
  }
  if (status & JackServerStarted) {
    fprintf (stderr, "JACK server started\n");
  }
  if (status & JackNameNotUnique) {
    client_name = jack_get_client_name(j_client);
    fprintf (stderr, "jack-client name: `%s'\n", client_name);
  }
  jack_set_process_callback (j_client, process, 0);

  jack_set_graph_order_callback (j_client, jack_latency_cb, NULL);

#ifndef WIN32
  jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif
  j_samplerate=jack_get_sample_rate (j_client);
  return (0);
}

static int jack_portsetup(void) {
  /* Allocate data structures that depend on the number of ports. */
  int i;
  input_port = (jack_port_t **) malloc (sizeof (jack_port_t *) * nports);
  in = (jack_default_audio_sample_t **) calloc (nports,sizeof (jack_default_audio_sample_t *));

  decoder = ltc_decoder_create(j_samplerate * fps_den / fps_num, LTC_QUEUE_LEN);

  for (i = 0; i < nports; i++) {
    char name[64];
    sprintf (name, "input%d", i+1);
    if ((input_port[i] = jack_port_register (j_client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
      fprintf (stderr, "cannot register input port \"%s\"!\n", name);
      return (-1);
    }
  }
  return (0);
}

static void jack_port_connect(char **jack_port, int argc) {
  int i;
  for (i=0; i < nports && i < argc; i++) {
    if (!jack_port[i]) continue;
    if (jack_connect(j_client, jack_port[i], jack_port_name(input_port[i]))) {
      fprintf(stderr, "cannot connect port %s to %s\n", jack_port[i], jack_port_name(input_port[i]));
    }
  }
}

/**
 *
 */
static void main_loop(void) {
  detected_fps = ceil((double)fps_num/fps_den);

  pthread_mutex_lock (&ltc_thread_lock);
  while (client_state != Exit) {

    my_decoder_read(decoder);

    pthread_cond_wait (&data_ready, &ltc_thread_lock);
  } /* while running */
  pthread_mutex_unlock (&ltc_thread_lock);
}

void catchsig (int sig) {
#ifndef _WIN32
  signal(SIGHUP, catchsig);
#endif
  fprintf(stderr,"caught signal - shutting down.\n");
  client_state=Exit;
}

void sig_ev_start (int sig) {
  event_start(monotonic_fcnt - (signal_latency * j_samplerate));
}

void sig_ev_end (int sig) {
  event_end(monotonic_fcnt - (signal_latency * j_samplerate));
}

/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"help", no_argument, 0, 'h'},
  {"output", required_argument, 0, 'o'},
  {"signals", no_argument, 0, 's'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jltcdump - JACK app to parse linear time code.\n\n");
  printf ("Usage: jltcdump [ OPTIONS ] [ JACK-PORTS ]\n\n");
  printf ("Options:\n\
  -f, --fps  <num>[/den]     set expected [initial] framerate (default 25/1)\n\
  -F                         autodetect framerate from LTC\n\
  -h, --help                 display this help and exit\n\
  -o, --output <path>        write to file(s)\n\
  -s, --signals              start/stop parser using SIGUSR1/SIGUSR2\n\
  -r                         parse R/S signal on 2nd channel\n\
  -R  <float>                R/S signal threshold (default 0.02)\n\
  -V, --version              print version information and exit\n\
\n");
  printf ("\n\
If both -s and -o are given, <path> is used a prefix:\n\
The filename will be <path>YYMMDD-HHMMSS.tme.XXXXX .\n\
If only -o is set, <path> is as filename.\n\
\n\
In 'signal' mode, the application starts in 'idle' state\n\
and won't record LTC until it receives SIGUSR1.\n\
\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
          "Website and manual: <https://github.com/x42/ltc-tools>\n"
	  );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;

  while ((c = getopt_long (argc, argv,
			   "h"	/* help */
			   "F"	/* detect framerate */
			   "f:"	/* fps */
			   "o:"	/* output-prefix */
			   "r "	/* parse R/S */
			   "R:"	/* R/S signal threshold */
			   "s"	/* signals */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF)
    {
      switch (c)
	{

	case 'f':
	{
	  fps_num = atoi(argv[3]);
	  char *tmp = strchr(argv[3], '/');
	  if (tmp) fps_den=atoi(++tmp);
	}
	break;

	case 'F':
	  detect_framerate = 1;
	  break;

	case 'o':
	  fileprefix = strdup(optarg);
	  break;

	case 'r':
	  nports = 2;
	  break;

	case 'R':
	  rs_thresh = atof(optarg);
	  break;

	case 's':
	  use_signals = 1;
	  break;

	case 'V':
	  printf ("jltcdump version %s\n\n", VERSION);
	  printf ("Copyright (C) GPL 2006,2012 Robin Gareus <robin@gareus.org>\n");
	  exit (0);

	case 'h':
	  usage (0);

	default:
	  usage (EXIT_FAILURE);
	}
    }

  return optind;
}

int main (int argc, char **argv) {
  int i;
  nports = 1; // or 2 -> detect signals

  i = decode_switches (argc, argv);

  // -=-=-= INITIALIZE =-=-=-

  if (init_jack("jltcdump"))
    goto out;
  if (jack_portsetup())
    goto out;

  rb = jack_ringbuffer_create(RBSIZE * sizeof(struct syncInfo));

  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

  jack_port_connect(&(argv[i]), argc-i);

  event_info.state = Idle;

#ifndef _WIN32
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);

  if (use_signals) {
    signal (SIGUSR1, sig_ev_start);
    signal (SIGUSR2, sig_ev_end);
  } else
#endif
  {
    event_info.state = Starting;
  }

  output = stdout;

  if (!fileprefix) {
    fprintf(output,"##  SMPTE   | audio-sample-num REV|             unix-system-time\n");
    fprintf(output,"##time-code |  start      end  ERS|       start                   end   \n");
  }

  main_loop();

  if (!use_signals) {
    event_info.state = Stopped;
    my_decoder_read(decoder);
  }

out:
  cleanup(0);
  return(0);
}
/* vi:set ts=8 sts=2 sw=2: */
