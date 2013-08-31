//  gcc -o jltcdump-simple jltcdump-simple.c `pkg-config --cflags --libs jack ltc`/

/* jack linear time code decoder
 * Copyright (C) 2006, 2012, 2013 Robin Gareus
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

#define LTC_QUEUE_LEN (42) // should be >> ( max(jack period size) * max-speedup / (duration of LTC-frame) )

#define _GNU_SOURCE

#ifndef VERSION
#define VERSION "1"
#endif

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <getopt.h>
#include <sys/mman.h>

#include <jack/jack.h>
#include <ltc.h>

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

static int keep_running = 1;

static jack_port_t *input_port = NULL;
static jack_client_t *j_client = NULL;
static uint32_t j_samplerate = 48000;

static LTCDecoder *decoder = NULL;

static pthread_mutex_t ltc_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

static int fps_num = 25;
static int fps_den = 1;

/**
 * jack audio process callback
 */
int process (jack_nframes_t nframes, void *arg) {
  jack_default_audio_sample_t *in;

  in = jack_port_get_buffer (input_port, nframes);

  ltc_decoder_write_float(decoder, in, nframes, 0);

  /* notify reader thread */
  if (pthread_mutex_trylock (&ltc_thread_lock) == 0) {
    pthread_cond_signal (&data_ready);
    pthread_mutex_unlock (&ltc_thread_lock);
  }
  return 0;
}

void jack_shutdown (void *arg) {
  fprintf(stderr,"recv. shutdown request from jackd.\n");
  keep_running=0;
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

#ifndef WIN32
  jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif
  j_samplerate=jack_get_sample_rate (j_client);
  return (0);
}

static int jack_portsetup(void) {
  /* Allocate data structures that depend on the number of ports. */
  decoder = ltc_decoder_create(j_samplerate * fps_den / fps_num, LTC_QUEUE_LEN);
  if (!decoder) {
    fprintf (stderr, "cannot create LTC decoder (out of memory).\n");
    return (-1);
  }
  /* create jack port */
  if ((input_port = jack_port_register (j_client, "input_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
    fprintf (stderr, "cannot register input port \"input_1\"!\n");
    return (-1);
  }
  return (0);
}

static void jack_port_connect(char **jack_port, int argc) {
  int i;
  for (i=0; i < argc; i++) {
    if (!jack_port[i]) continue;
    if (jack_connect(j_client, jack_port[i], jack_port_name(input_port))) {
      fprintf(stderr, "cannot connect port %s to %s\n", jack_port[i], jack_port_name(input_port));
    }
  }
}


/**
 *
 */
static void my_decoder_read(LTCDecoder *d) {
  LTCFrameExt frame;

  while (ltc_decoder_read(d, &frame)) {

    SMPTETimecode stime;
    ltc_frame_to_time(&stime, &frame.ltc, /* use_date? LTC_USE_DATE : */ 0);

    if (/* use_date */ 0)
      printf("%02d-%02d-%02d ",
	  stime.years,
	  stime.months,
	  stime.days);
    printf("%02d:%02d:%02d%c%02d | %8lld %8lld%s | %.1fdB\n",
	stime.hours,
	stime.mins,
	stime.secs,
	(frame.ltc.dfbit) ? '.' : ':',
	stime.frame,
	frame.off_start,
	frame.off_end,
	frame.reverse ? " R" : "  ",
	frame.volume
	);
  }
  fflush(stdout);
}

/**
 *
 */
static void main_loop(void) {
  pthread_mutex_lock (&ltc_thread_lock);

  while (keep_running) {
    my_decoder_read(decoder);
    if (!keep_running) break;
    pthread_cond_wait (&data_ready, &ltc_thread_lock);
  }

  pthread_mutex_unlock (&ltc_thread_lock);
}


void catchsig (int sig) {
#ifndef _WIN32
  signal(SIGHUP, catchsig);
#endif
  fprintf(stderr,"caught signal - shutting down.\n");
  keep_running=0;
  pthread_cond_signal (&data_ready);
}

/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"help", no_argument, 0, 'h'},
  {"fps", required_argument, 0, 'f'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jltcdump - very simple JACK client to parse linear time code.\n\n");
  printf ("Usage: jltcdump [ OPTIONS ] [ JACK-PORTS ]\n\n");
  printf ("Options:\n\
  -f, --fps  <num>[/den]     set expected framerate (default 25/1)\n\
  -h, --help                 display this help and exit\n\
  -V, --version              print version information and exit\n\
\n\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
          "Website and manual: <https://github.com/x42/ltc-tools>\n"
	  );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;

  while ((c = getopt_long (argc, argv,
			   "h"	/* help */
			   "f:"	/* fps */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF)
    {
      switch (c)
	{
	case 'f':
	{
	  fps_num = atoi(optarg);
	  char *tmp = strchr(optarg, '/');
	  if (tmp) fps_den=atoi(++tmp);
	}
	break;

	case 'V':
	  printf ("jltcdump-simple version %s\n\n", VERSION);
	  printf ("Copyright (C) GPL 2006,2012,2013 Robin Gareus <robin@gareus.org>\n");
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

  i = decode_switches (argc, argv);

  // -=-=-= INITIALIZE =-=-=-

  if (init_jack("jack-ltc-dump"))
    goto out;

  if (jack_portsetup())
    goto out;

  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

  jack_port_connect(&(argv[i]), argc-i);

#ifndef _WIN32
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);
#endif

  printf("##  SMPTE   | audio-sample-num REV| \n");
  printf("##time-code |  start      end  ERS| \n");

  main_loop();

out:
  if (j_client) {
    jack_client_close (j_client);
    j_client=NULL;
  }
  ltc_decoder_free(decoder);
  fprintf(stderr, "bye.\n");

  return(0);
}
/* vi:set ts=8 sts=2 sw=2: */
