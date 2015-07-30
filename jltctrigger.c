/* jack linear time code decoder + trigger
 * Copyright (C) 2006, 2012-2014 Robin Gareus
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

#define LTC_QUEUE_LEN (96) // should be >> ( max(jack period size) * max-speedup / (duration of LTC-frame) )

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
#include <ltc.h>

#ifndef WIN32
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#endif

#include "ltcframeutil.h"
#include "timecode.h"

static jack_port_t *input_port = NULL;
static jack_client_t *j_client = NULL;
static uint32_t j_samplerate = 48000;
static char *jack_connect_port = NULL;

static LTCDecoder *decoder = NULL;

static pthread_mutex_t ltc_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

static FILE *output = NULL;

static int detect_framerate = 0;
static int fps_locked = 0;
static int fps_num = 25;
static int fps_den = 1;
static int detected_fps;
static int want_verbose = 0;

/* a simple state machine for this client */
static volatile enum {
  Run,
  Exit
} client_state = Run;

struct LtcAction {
  float trigger_time_sec;
  char *action_command;
  int called;
};

static struct LtcAction *ltc_actions = NULL;
static int action_count = 0;

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
  decoder = NULL;

  int i;
  for (i = 0; i < action_count; ++i) {
    free (ltc_actions[i].action_command);
  }
  free(ltc_actions);
  ltc_actions = NULL;
  action_count = 0;
  if (want_verbose)
    printf ("bye.\n");
}

static void action (float t0, float t1) {
  int i;
  for (i = 0; i < action_count; ++i) {
    if (t0 < ltc_actions[i].trigger_time_sec
	&& t1 >= ltc_actions[i].trigger_time_sec) {
      if (want_verbose) {
	if (output) fprintf(output, "\n");
	printf("# running %s\n", ltc_actions[i].action_command);
      }
      system(ltc_actions[i].action_command); // XXX use vfork() & background process
      ++ltc_actions[i].called;
    }
  }
}

/**
 * called in main (non-realtime) thread. parse and process LTC
 */
static void my_decoder_read (LTCDecoder *d) {
  static LTCFrameExt prev_frame;
  static int frames_in_sequence = 0;
  LTCFrameExt frame;

  /* process incoming LTC frames */
  while (ltc_decoder_read (d, &frame)) {
    SMPTETimecode stime;
    ltc_frame_to_time(&stime, &frame.ltc, 0);

    if (detect_framerate) {
      if (detect_fps (&detected_fps, &frame, &stime, output) > 0) {
	fps_locked = 1;
      }
    }

    if (frames_in_sequence > 0) {
      float t0 = ltcframe_to_framecnt(&prev_frame.ltc, detected_fps) / detected_fps;
      float t1 = ltcframe_to_framecnt(&frame.ltc, detected_fps) / detected_fps;
      action (t0, t1);
    }

    /* detect discontinuities in LTC */
    int discontinuity_detected = 0;
    if (fps_locked || !detect_framerate) {
      discontinuity_detected = detect_discontinuity (&frame, &prev_frame, detected_fps, 0, 0);
    } else {
      memcpy(&prev_frame, &frame, sizeof(LTCFrameExt));
    }
    if (discontinuity_detected) {
      fps_locked = 0;
    }

    /* notify about discontinuities */
    if (frames_in_sequence > 0 && discontinuity_detected) {
      if (output)
	fprintf(output, "\n#DISCONTINUITY\n");
    }
    frames_in_sequence++;

    if (output) {
      fprintf(output, "%02d:%02d:%02d%c%02d \r",
	  stime.hours,
	  stime.mins,
	  stime.secs,
	  (frame.ltc.dfbit) ? '.' : ':',
	  stime.frame
	  );
    }
  }
  if (output) {
    fflush (output);
  }
}

/**
 * jack audio process callback
 */
int process (jack_nframes_t nframes, void *arg) {
  jack_default_audio_sample_t *in;

  in = jack_port_get_buffer (input_port, nframes);

  ltc_decoder_write_float (decoder, in, nframes, 0);

  /* notify reader thread */
  if (pthread_mutex_trylock (&ltc_thread_lock) == 0) {
    pthread_cond_signal (&data_ready);
    pthread_mutex_unlock (&ltc_thread_lock);
  }
  return 0;
}

void jack_shutdown (void *arg) {
  fprintf (stderr, "recv. shutdown request from jackd.\n");
  client_state = Exit;
  pthread_cond_signal (&data_ready);
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

  jack_on_shutdown (j_client, jack_shutdown, NULL);
  j_samplerate = jack_get_sample_rate (j_client);
  return (0);
}

static int jack_portsetup(void) {
  if (!(decoder = ltc_decoder_create (j_samplerate * fps_den / fps_num, LTC_QUEUE_LEN))) {
    fprintf (stderr, "Cannot create LTC decoder!\n");
    return -1;
  }

  if (0 == (input_port = jack_port_register (j_client, "ltc_input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
    fprintf (stderr, "Cannot register ltc audio input port!\n");
    return -1;
  }

  return (0);
}

static void jack_port_connect(char *jack_port) {
  if (!jack_port) return;
  if (jack_connect (j_client, jack_port, jack_port_name(input_port))) {
    fprintf(stderr, "Cannot connect port %s to %s\n", jack_port, jack_port_name(input_port));
  }
}

static void main_loop(void) {
  detected_fps = ceil((double)fps_num/fps_den);

  pthread_mutex_lock (&ltc_thread_lock);
  while (client_state != Exit) {
    my_decoder_read(decoder);
    if (client_state == Exit) break;
    pthread_cond_wait (&data_ready, &ltc_thread_lock);
  } /* while running */
  pthread_mutex_unlock (&ltc_thread_lock);
}

void catchsig (int sig) {
#ifndef _WIN32
  signal(SIGHUP, catchsig);
#endif
  fprintf(stderr,"caught signal - shutting down.\n");
  client_state = Exit;
  pthread_cond_signal (&data_ready);
}

/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"connect",   required_argument, 0, 'c'},
  {"fps",       required_argument, 0, 'f'},
  {"detectfps", no_argument, 0,       'F'},
  {"help",      no_argument, 0,       'h'},
  {"print",     no_argument, 0,       'p'},
  {"verbose",   no_argument, 0,       'v'},
  {"version",   no_argument, 0,       'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jltctrigger - JACK app to trigger actions on given LTC.\n\n");
  printf ("Usage: jltctrigger [ OPTIONS ] <cfg-file> ...\n\n");
  printf ("Options:\n\
  -c, --connect <port>       auto-connect to given jack-port\n\
  -f, --fps <num>[/den]      set expected [initial] framerate (default 25/1)\n\
  -F, --detectfps            autodetect framerate from LTC\n\
  -h, --help                 display this help and exit\n\
  -p, --print                output decoded LTC (live)\n\
  -v, --verbose              be verbose\n\
  -V, --version              print version information and exit\n\
\n");
  printf ("\n\
\n\
Actions are defined in a config file, one per line.\n\
  Timecode <Space> Command\n\
Multiple config files can be given.\n\
The fps parameter is used when parsing the config file,\n\
...\n\
The fps option is also used properly track the first LTC frame,\n\
and timecode discontinuity notification.\n\
The LTC-decoder detects and tracks the speed but it takes a few samples\n\
to establish initial synchronization. Setting fps to the expected fps\n\
speeds up the initial sync process. The default is 25/1.\n\
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
	  "c:"	/* connect */
	  "p"	/* print */
	  "v"	/* verbose */
	  "V",	/* version */
	  long_options, (int *) 0)) != EOF)
  {
    switch (c)
    {
      case 'f':
	{
	  fps_num = atoi (optarg);
	  char *tmp = strchr (optarg, '/');
	  if (tmp) fps_den = atoi (++tmp);
	}
	break;

      case 'F':
	detect_framerate = 1;
	break;

      case 'c':
	jack_connect_port = optarg;
	break;

      case 'p':
	output = stdout;
	break;

      case 'v':
	want_verbose = 1;
	break;

      case 'V':
	printf ("jltctrigger version %s\n\n", VERSION);
	printf ("Copyright (C) GPL 2006, 2012-2014 Robin Gareus <robin@gareus.org>\n");
	exit (0);

      case 'h':
	usage (0);

      default:
	usage (EXIT_FAILURE);
    }
  }
  return optind;
}

static int parse_config (const char *fn) {
  FILE *f = fopen (fn, "r");
  char line[1024];
  int parsed = 0;
  const float cfps = ceil((double)fps_num/fps_den);
  if (!f) return -1;

  while (fgets(line, sizeof(line), f)) {
    char *t = strchr(line, ' ');
    if (!t) { continue; }
    if (line[0] == '#') { continue; }

    // parse TC
    int i = 0;
    int32_t bcd[4];
    char *pe = line;
    bcd[i++] = (int) atoi (++pe);
    while (i < 4 && *pe && (pe = (char*) strpbrk (pe,":;"))) {
      bcd[i++] = (int) atoi (++pe);
    }
    if (i != 4) continue;
    while (*t && *t == ' ') ++t;
    while (strlen (t) > 0 && (t[strlen (t) - 1] == '\r' || t[strlen (t) - 1] == '\n')) {
      t[strlen (t) - 1] = '\0';
    }
    if (strlen (t) == 0) continue;

    float ms = bcd_to_framecnt(cfps, 0, bcd[3], bcd[2], bcd[1], bcd[0]) / cfps;
    ltc_actions = realloc (ltc_actions, (action_count + 1) * sizeof(struct LtcAction));
    ltc_actions[action_count].trigger_time_sec = ms;
    ltc_actions[action_count].action_command = strdup(t);
    ++action_count;
    ++parsed;
  }

  fclose(f);
  return parsed;
}

int main (int argc, char **argv) {
  int i;

  i = decode_switches (argc, argv);

  while (i < argc) {
    if (want_verbose)
      printf ("Parsing '%s'..", argv[i]);
    const int e = parse_config (argv[i++]);
    if (!want_verbose)
      continue;
    if (e < 0) {
      printf (" Error.\n");
    } else {
      printf (" %d entries.\n", e);
    }
  }

  if (action_count == 0) {
    fprintf(stderr, "Error: No actions defined.\n");
    goto out;
  }


  if (want_verbose) {
    for (i = 0; i < action_count; ++i) {
      printf("#%d @%.2f '%s'\n", i + 1, ltc_actions[i].trigger_time_sec, ltc_actions[i].action_command);
    }
  }

  if (init_jack("jltctrigger"))
    goto out;
  if (jack_portsetup())
    goto out;

#ifndef WIN32
  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }
#endif

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

  jack_port_connect (jack_connect_port);

#ifndef _WIN32
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);
#endif

  main_loop();

  if (want_verbose) {
    for (i = 0; i < action_count; ++i) {
      printf("# action #%d called %d time(s)\n", i, ltc_actions[i].called);
    }
  }

out:
  cleanup(0);
  return(0);
}

/* vi:set ts=8 sts=2 sw=2: */
