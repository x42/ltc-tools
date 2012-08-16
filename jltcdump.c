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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/** hardcoded Framerate */
#define FPS_NUM (25)
#define FPS_DEN (1)

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
#include <ltcsmpte/ltcsmpte.h>

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

#define FPS (FPS_NUM/(double)FPS_DEN)

static jack_port_t **input_port = NULL;
static jack_default_audio_sample_t **in = NULL;

static jack_client_t *j_client = NULL;
static jack_nframes_t j_latency = 0;
static uint32_t j_samplerate = 48000;
static const double signal_latency = 0.04; // in seconds (avg. w/o jitter)

static int nports = 0;

static SMPTEDecoder *decoder = NULL;
static volatile long int monotonic_fcnt = 0;

static pthread_mutex_t ltc_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

static FILE *output;
static char *fileprefix=NULL;

/* TODO make a linear buffer of those.
 * To allow multiple start/stop events in a single cycle
 */
static volatile struct {
  enum {Idle, Starting, Started, Stopped} state;
  struct timespec ev_start;
  struct timespec ev_end;
  long int audio_frame_start;
  long int audio_frame_end;
} event_info;

/* a simple state machine for this client */
static volatile enum {
  Init,
  Run,
  Exit
} client_state = Init;




/**
 * cleanup and exit
 * call this function only _after_ everything has been initialized!
 */
static void cleanup(int sig) {
  if (j_client) {
    jack_client_close (j_client);
    j_client=NULL;
  }

  SMPTEFreeDecoder(decoder);
  free(in);
  free(input_port);
  fprintf(stderr, "bye.\n");
}


void event_start (int fcnt) {
  if (event_info.state != Idle) {
    fprintf(stderr, "sig-activate ignored -- not idle\n");
    return;
  }
  clock_gettime(CLOCK_REALTIME, (struct timespec*) &event_info.ev_start);
  event_info.audio_frame_start = fcnt;
  event_info.state = Starting;
}

void event_end (long int fcnt) {
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

/**
 *
 */
static void myDecoderRead(SMPTEDecoder *d) {
  SMPTEFrameExt frame;
  int errors;
  int i=0;


  if (event_info.state == Idle) {
    //int frames_in_queue = (d->qWritePos - d->qReadPos + d->qLen) % d->qLen;
    // read some frames to prevent queue overflow
    // TODO keep some in queue
    while (SMPTEDecoderRead(d,&frame)) { ; }
    return;
  }
  if (event_info.state == Stopped) {
    // close XML file
    if (output) 
      fprintf(output, "#END: %ld.%09ld  @ %ld\n",
	  event_info.ev_end.tv_sec, event_info.ev_end.tv_nsec,
	  event_info.audio_frame_end);

    // TODO: keep processing frames until (frame.endpos > event_info.audio_frame_end)
    event_info.state = Idle;

    if (fileprefix && output) {
      fclose(output);
      output=NULL;
    }
    return;
  }
  if (event_info.state == Starting) {
    // open new XML file
    if (fileprefix) {
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

    if (output) {
      fprintf(output, "#Start: %ld.%09ld  @ %ld\n",
	  event_info.ev_start.tv_sec, event_info.ev_start.tv_nsec,
	  event_info.audio_frame_start);
      fflush(output);
    }
    event_info.state = Started;
    SMPTEDecoderErrorReset(d);
  }

  while (i || SMPTEDecoderRead(d,&frame)) {
    SMPTETime stime;
    i=0;

    // TODO skip frames that have (frame.startpos < event_info.audio_frame_start)
    // TODO skip frames that have (frame.endpos > event_info.audio_frame_end)

    SMPTEFrameToTime(&frame.base,&stime);
    SMPTEDecoderErrors(d,&errors);

    if (output) 
      fprintf(output, " %02d:%02d:%02d:%02d | %ld -> %ld (%d) (err:%d) \n",
	stime.hours,stime.mins,
	stime.secs,stime.frame,
	frame.startpos,
	frame.endpos,
	frame.delayed,
	errors);
  }

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
  SMPTEDecoderWrite(decoder, sound, nframes, posinfo);
  return 0;
}

/**
 * jack audio process callback
 */
int process (jack_nframes_t nframes, void *arg) {
  int i;

  for (i=0;i<nports;i++) {
    in[i] = jack_port_get_buffer (input_port[i], nframes);
  }

  parse_ltc(nframes, in[0], monotonic_fcnt - j_latency);

  for (i=1;i<nports;i++) {
    ;
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

  {
    FrameRate *fps;
    decoder = SMPTEDecoderCreate(j_samplerate, (fps=FR_create(FPS_NUM, FPS_DEN, FRF_NONE)), 30, 1);
    FR_free(fps);
  }

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
  pthread_mutex_lock (&ltc_thread_lock);
  while (client_state != Exit) {

    myDecoderRead(decoder);

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
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jltcdump - JACK app to parse linear time code.\n\n");
  printf ("Usage: jltcdump [ OPTIONS ] [ JACK-PORTS ]\n\n");
  printf ("Options:\n\
  -h, --help                 display this help and exit\n\
  -V, --version              output version information and exit\n\
\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n");
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;


  while ((c = getopt_long (argc, argv,
			   "h"	/* help */
			   "o:"	/* output-prefix */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF)
    {
      switch (c)
	{
	case 'o':
	  fileprefix = strdup(optarg);
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
  i = decode_switches (argc, argv);

  // -=-=-= INITIALIZE =-=-=-
  nports = 1; // or 2

  if (init_jack("jltcdump"))
    goto out;
  if (jack_portsetup())
    goto out;

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

  jack_port_connect(&(argv[i]), argc-i);

#ifndef _WIN32
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);

  signal (SIGUSR1, sig_ev_start);
  signal (SIGUSR2, sig_ev_end);
#endif

  output = stdout;
  main_loop();

out:
  cleanup(0);
  return(0);
}
/* vi:set ts=8 sts=2 sw=2: */
