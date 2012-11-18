/* jack linear time code to MIDI time code translator
 * Copyright (C) 2006, 20120, 2012 Robin Gareus <robin@gareus.org>
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
#define JACK_MIDI_QUEUE_SIZE (8*LTC_QUEUE_LEN)

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
#include <jack/midiport.h>
#include <sys/mman.h>
#include <ltc.h>

#include "ltcframeutil.h"

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

static jack_port_t *ltc_input_port = NULL;
static jack_port_t *mtc_output_port = NULL;

static jack_client_t *j_client = NULL;
static jack_nframes_t jltc_latency = 0;
static jack_nframes_t jmtc_latency = 0;
static uint32_t j_samplerate = 48000;

static LTCDecoder *decoder = NULL;
static volatile long long int monotonic_fcnt = 0;

static int debug = 0;
static int send_sysex = 0;
static int detect_framerate = 0;
static int fps_num = 25;
static int fps_den = 1;
static int use30df = 0;
static int detected_fps;
static char *ltcportname = NULL;
static char *mtcportname = NULL;

/* a simple state machine for this client */
static volatile enum {
  Init,
  Run,
  Exit
} client_state = Init;

typedef struct my_midi_event {
  long long int monotonic_align;
  jack_nframes_t time;
  size_t size;
  jack_midi_data_t buffer[16];
} my_midi_event_t;

static my_midi_event_t event_queue[JACK_MIDI_QUEUE_SIZE];
static int queued_events_start = 0;
static int queued_events_end = 0;
int jack_graph_cb(void *arg);


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
  fprintf(stderr, "bye.\n");
}

static int queue_mtc_quarterframe(const SMPTETimecode * const stime, const int mtc_tc, const long long int posinfo, const int qf) {
  unsigned char mtc_msg=0;
  switch(qf) {
    case 0: mtc_msg =  0x00 |  (stime->frame&0xf); break;
    case 1: mtc_msg =  0x10 | ((stime->frame&0xf0)>>4); break;
    case 2: mtc_msg =  0x20 |  (stime->secs&0xf); break;
    case 3: mtc_msg =  0x30 | ((stime->secs&0xf0)>>4); break;
    case 4: mtc_msg =  0x40 |  (stime->mins&0xf); break;
    case 5: mtc_msg =  0x50 | ((stime->mins&0xf0)>>4); break;
    case 6: mtc_msg =  0x60 |  ((mtc_tc|stime->hours)&0xf); break;
    case 7: mtc_msg =  0x70 | (((mtc_tc|stime->hours)&0xf0)>>4); break;
  }

  jack_midi_data_t *mmsg = event_queue[queued_events_start].buffer;
  mmsg[0] = (char) 0xf1;
  mmsg[1] = (char) mtc_msg;

  event_queue[queued_events_start].monotonic_align = posinfo;
  event_queue[queued_events_start].time = 0;
  event_queue[queued_events_start].size = 2;
  queued_events_start = (queued_events_start + 1)%JACK_MIDI_QUEUE_SIZE;

  return 0;
}

static void queue_mtc_quarterframes(const SMPTETimecode * const st, const int mtc_tc, const int reverse, const int speed, const long long int posinfo) {
  int i;
  const float qfl = speed / 4.0;
  static SMPTETimecode stime;
  static int next_quarter_frame_to_send = 0;

  if (next_quarter_frame_to_send != 0 && next_quarter_frame_to_send != 4) {
    /* this can actually never happen */
    fprintf(stderr, "quarter-frame mis-aligment: %d (should be 0 or 4)\n", next_quarter_frame_to_send);
    next_quarter_frame_to_send = 0;
  }
  if (mtc_tc != 0x20 && (st->frame%2) == 1 && next_quarter_frame_to_send == 0) {
    /* the MTC spec does note that for 24, 30 drop and 30 non-drop, the frame number computed from quarter frames is always even
     * but for 25 it might be odd or even "depending on whiuch frame number the 8 message sequence started"
     */
    fprintf(stderr, "re-align quarter-frame to even frame-number\n");
    return;
  }

  if (next_quarter_frame_to_send == 0) {
    /* MTC spans timecode over two frames.
     * remember the current timecode since the min/hour (2nd part)
     * may change.
     */
    memcpy(&stime, st, sizeof(SMPTETimecode));
  }

  for (i=0;i<4;++i) {
    if (reverse)
      next_quarter_frame_to_send--;
    if (next_quarter_frame_to_send < 0)
      next_quarter_frame_to_send = 7;

    queue_mtc_quarterframe(&stime, mtc_tc, posinfo + i*qfl, next_quarter_frame_to_send);

    if (!reverse)
      next_quarter_frame_to_send++;
    if (next_quarter_frame_to_send > 7)
      next_quarter_frame_to_send = 0;
  }
}

static void queue_mtc_sysex(const SMPTETimecode * const stime, const int mtc_tc, const long long int posinfo) {
  jack_midi_data_t *sysex = event_queue[queued_events_start].buffer;
#if 1
  sysex[0]  = (unsigned char) 0xf0; // fixed
  sysex[1]  = (unsigned char) 0x7f; // fixed
  sysex[2]  = (unsigned char) 0x7f; // sysex channel
  sysex[3]  = (unsigned char) 0x01; // fixed
  sysex[4]  = (unsigned char) 0x01; // fixed
  sysex[5]  = (unsigned char) 0x00; // hour
  sysex[6]  = (unsigned char) 0x00; // minute
  sysex[7]  = (unsigned char) 0x00; // seconds
  sysex[8]  = (unsigned char) 0x00; // frame
  sysex[9]  = (unsigned char) 0xf7; // fixed

  sysex[5] |= (unsigned char) (mtc_tc&0x60);
  sysex[5] |= (unsigned char) (stime->hours&0x1f);
  sysex[6] |= (unsigned char) (stime->mins&0x7f);
  sysex[7] |= (unsigned char) (stime->secs&0x7f);
  sysex[8] |= (unsigned char) (stime->frame&0x7f);

  event_queue[queued_events_start].size = 10;

#else

  sysex[0]   = (char) 0xf0;
  sysex[1]   = (char) 0x7f;
  sysex[2]   = (char) 0x7f;
  sysex[3]   = (char) 0x06;
  sysex[4]   = (char) 0x44;
  sysex[5]   = (char) 0x06;
  sysex[6]   = (char) 0x01;
  sysex[7]   = (char) 0x00;
  sysex[8]   = (char) 0x00;
  sysex[9]   = (char) 0x00;
  sysex[10]  = (char) 0x00;
  sysex[11]  = (char) 0x00;
  sysex[12]  = (char) 0xf7;

  sysex[7]  |= (char) 0x20; // 25fps
  sysex[7]  |= (char) (stime->hours&0x1f);
  sysex[8]  |= (char) (stime->mins&0x7f);
  sysex[9]  |= (char) (stime->secs&0x7f);
  sysex[10] |= (char) (stime->frame&0x7f);

  int checksum = (sysex[7] + sysex[8] + sysex[9] + sysex[10] + 0x3f)&0x7f ;
  sysex[11]  = (char) (127-checksum); //checksum
  event_queue[queued_events_start].size = 13;
#endif

  event_queue[queued_events_start].monotonic_align = posinfo;
  event_queue[queued_events_start].time = 0;
  queued_events_start = (queued_events_start + 1)%JACK_MIDI_QUEUE_SIZE;
}

/**
 *
 */
static void generate_mtc(LTCDecoder *d, int latency) {
  LTCFrameExt frame;
  static SMPTETimecode ptime; // previous time

  while (ltc_decoder_read(d,&frame)) {
    SMPTETimecode stime;
    int moving;
    int frame_duration;
    enum LTC_TV_STANDARD tv_standard = LTC_TV_625_50;

    static int fps_warn = 0;
    int mtc_tc = 0x20;

    memset(&stime, 0, sizeof(SMPTETimecode)); // libltc <= 1.0.1, may not zero date
    ltc_frame_to_time(&stime, &frame.ltc, 0);
    if (detect_framerate) {
      detect_fps(&detected_fps, &frame, &stime, stdout);
    }

    moving = memcmp(&stime, &ptime, sizeof(SMPTETimecode));
    memcpy(&ptime, &stime, sizeof(SMPTETimecode));
    frame_duration = 1 + frame.off_end - frame.off_start;

    /*set MTC fps */
    switch (detected_fps) {
      case 24:
	mtc_tc = 0x00;
	tv_standard = LTC_TV_FILM_24;
	fps_warn = 0;
	break;
      case 25:
	mtc_tc = 0x20;
	tv_standard = LTC_TV_625_50;
	fps_warn = 0;
	break;
      case 29:
	mtc_tc = 0x40;
	tv_standard = LTC_TV_525_60;
	fps_warn = 0;
	break;
      case 30:
	if (frame.ltc.dfbit || use30df) {
	  mtc_tc = 0x40;
	  tv_standard = LTC_TV_525_60;
	} else {
	  mtc_tc = 0x60;
	  tv_standard = LTC_TV_1125_60;
	}
	fps_warn = 0;
	break;
      default:
	if (!fps_warn) {
	  fps_warn = 1;
	  fprintf(stderr, "WARNING: invalid video framerate %d (using 25fps instead)\n", detected_fps);
	}
	break;
    }

    if (debug)
      fprintf(stdout, "%02d:%02d:%02d%c%02d | %8lld %8lld%s\n",
	  stime.hours,
	  stime.mins,
	  stime.secs,
	  (frame.ltc.dfbit) ? '.' : ':',
	  stime.frame,
	  frame.off_start,
	  frame.off_end,
	  frame.reverse ? " R" : "  "
	  );


    /* when a full LTC frame is decoded, the timecode the LTC frame
     * is referring has just passed.
     * So we send the _next_ timecode which
     * is expected to start at the end of the current frame
     */
    if (!moving) {
      /* we're not moving */
      if (debug) printf(" Not moving..\n");
    } else if (!frame.reverse) {
      /* moving forward */
      ltc_frame_increment(&frame.ltc, detected_fps, tv_standard, 0);
      ltc_frame_to_time(&stime, &frame.ltc, 0);
      frame.off_start += ltc_frame_alignment(j_samplerate, tv_standard);
      frame.off_end += ltc_frame_alignment(j_samplerate, tv_standard);
    } else {
      /* moving backward */
      ltc_frame_decrement(&frame.ltc, detected_fps, tv_standard, 0);
      ltc_frame_to_time(&stime, &frame.ltc, 0);
      frame.off_start += frame_duration + ltc_frame_alignment(j_samplerate, tv_standard);
      frame.off_end += frame_duration + ltc_frame_alignment(j_samplerate, tv_standard);
    }

    /* compensate for latency */
    if (latency > 0 && frame_duration > 0) {
      int i;
      int foff = (int) ceil((double)latency / (double)frame_duration);
      if (debug) printf("tot latency: %d audio-frames, extrapolating %d timecode-frame(s)\n", latency, foff);
      for (i = 0 ; i < foff ; ++i) {
	if (!frame.reverse) {
	  ltc_frame_increment(&frame.ltc, detected_fps, tv_standard, 0);
	} else {
	  ltc_frame_decrement(&frame.ltc, detected_fps, tv_standard, 0);
	}
	frame.off_start += frame_duration;
	frame.off_end += frame_duration;
      }
      ltc_frame_to_time(&stime, &frame.ltc, 0);
    }

    if (send_sysex) {
      queue_mtc_sysex(&stime, mtc_tc, frame.off_end + 1);
    } else {
      queue_mtc_quarterframes(&stime, mtc_tc, frame.reverse, frame_duration, frame.off_end + 1);
    }
  }
}

static int parse_ltc(const jack_nframes_t nframes, const jack_default_audio_sample_t * const in, const long long int posinfo) {
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

/**
 * jack audio process callback
 */
int process (jack_nframes_t nframes, void *arg) {
  void *out;
  jack_default_audio_sample_t *in;

  in = jack_port_get_buffer (ltc_input_port, nframes);
  out = jack_port_get_buffer(mtc_output_port, nframes);

#if 0 // workaround jack2 latency cb order -- fixed in jack1.9.9.4 -- e577581de
  jack_graph_cb(in);
#endif

  parse_ltc(nframes, in, monotonic_fcnt - jltc_latency);

  generate_mtc(decoder, jltc_latency + jmtc_latency);

  jack_midi_clear_buffer(out);
  while (queued_events_end != queued_events_start) {
    const long long int mt = event_queue[queued_events_end].monotonic_align - jmtc_latency;
    if (mt >= monotonic_fcnt + nframes) {
      if (debug){
	fprintf(stderr, "DEBUG: MTC timestamp is for next jack cycle.\n");
	fprintf(stderr, " TME: %lld >= %lld)\n", mt, monotonic_fcnt + nframes);
      }
      break;
    }
    if (mt < monotonic_fcnt) {
      fprintf(stderr, "WARNING: MTC was for previous jack cycle (port latency too large?)\n"); // XXX
      if (debug) fprintf(stderr, "TME: %lld < %lld)\n", mt, monotonic_fcnt);
    } else {

#if 0 // DEBUG quarter frame timing
    static long long int prev = 0;
    if (mt-prev != (j_samplerate / detected_fps / 4)) {
      fprintf(stderr, " QT time %lld != %u\n", mt-prev,  j_samplerate / detected_fps / 4);
    }
    prev = mt;
#endif

      event_queue[queued_events_end].time = mt - monotonic_fcnt;
      jack_midi_event_write(out,
	  event_queue[queued_events_end].time,
	  event_queue[queued_events_end].buffer,
	  event_queue[queued_events_end].size
	  );
    }
    queued_events_end = (queued_events_end + 1)%JACK_MIDI_QUEUE_SIZE;
  }

  monotonic_fcnt += nframes;

  return 0;
}

int jack_graph_cb(void *arg) {
  jack_latency_range_t jlty;
  if (ltc_input_port) {
    jack_port_get_latency_range(ltc_input_port, JackCaptureLatency, &jlty);
    jltc_latency = jlty.max;
    if (debug && !arg)
      fprintf(stderr, "JACK port latency: %d\n", jltc_latency);
  }
  if (mtc_output_port) {
    jack_port_get_latency_range(mtc_output_port, JackPlaybackLatency, &jlty);
    jmtc_latency = jlty.max;
    if (debug && !arg)
      fprintf(stderr, "MTC port latency: %d\n", jmtc_latency);
  }
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

  jack_set_graph_order_callback (j_client, jack_graph_cb, NULL);

#ifndef WIN32
  jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif
  j_samplerate=jack_get_sample_rate (j_client);

  return (0);
}

static int jack_portsetup(void) {
  decoder = ltc_decoder_create(j_samplerate * fps_den / fps_num, LTC_QUEUE_LEN);

  if ((ltc_input_port = jack_port_register (j_client, "ltc_in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
    fprintf (stderr, "cannot register ltc input port !\n");
    return (-1);
  }
  if ((mtc_output_port = jack_port_register(j_client, "mtc_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0)) == 0) {
    fprintf (stderr, "cannot register mtc ouput port !\n");
    return (-1);
  }
  return (0);
}

static void port_connect(char *ltc_port, char *mtc_port) {
  if (ltc_port && jack_connect(j_client, ltc_port, jack_port_name(ltc_input_port))) {
    fprintf(stderr, "cannot connect port %s to %s\n", ltc_port, jack_port_name(ltc_input_port));
  }
  if (mtc_port && jack_connect(j_client, jack_port_name(mtc_output_port), mtc_port)) {
    fprintf(stderr, "cannot connect port %s to %s\n", jack_port_name(mtc_output_port), mtc_port);
  }
}

void catchsig (int sig) {
#ifndef _WIN32
  signal(SIGHUP, catchsig);
#endif
  fprintf(stderr,"caught signal - shutting down.\n");
  client_state=Exit;
}

/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"help", no_argument, 0, 'h'},
  {"fps", required_argument, 0, 'f'},
  {"detectfps", no_argument, 0, 'F'},
  {"ltcport", required_argument, 0, 'l'},
  {"mtcport", required_argument, 0, 'm'},
  {"sysex", no_argument, 0, 's'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jltc2mtc - JACK app to translate linear time code to midi time code.\n\n");
  printf ("Usage: jltc2mtc [ OPTIONS ]\n\n");
  printf ("Options:\n\
  -f, --fps <num>[/den]      set expected [initial] framerate (default 25/1)\n\
  -F, --detectfps            autodetect framerate from LTC (recommended)\n\
  -l, --ltcport <portname>   autoconnect LTC input port\n\
  -m, --mtcport <portname>   autoconnect MTC output port\n\
  -s, --sysex                send system-excluve seek message\n\
                             instead of MTC quarter frames\n\
  -h, --help                 display this help and exit\n\
  -V, --version              print version information and exit\n\
\n");
  printf ("\n\
This tool reads LTC from a JACK-audio port and generates corresponding\n\
MTC on a JACK-midi port.\n\
\n\
jltc2mtc supports both forward and backwards played timecode, and compensates\n\
for decoder and port latencies.\n\
Note that MTC only supports 4 framerates: 24, 25, 30df and 30 fps.\n\
Framerates other than that are announced as 25fps MTC.\n\
Drop-frame-timecode is detected by the corresponding bit in the LTC frame,\n\
regardless of the -F option. You can /force/ it with -f 30000/1001.\n\
\n\
Note that MTC distinguishes between film speed and video speed only by the\n\
rate at which timecode advances, not by the information contained in the\n\
timecode messages; thus, 29.97 fps dropframe is represented as 30 fps\n\
dropframe with 0.1%% pulldown\n\
\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
          "Website and manual: <https://github.com/x42/ltc-tools>\n"
	  );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;

  while ((c = getopt_long (argc, argv,
			   "d"	/* debug */
			   "f:"	/* fps */
			   "F"	/* detect framerate */
			   "h"	/* help */
			   "l:"	/* ltcport */
			   "m:"	/* mtcport */
			   "s"	/* sysex */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF)
    {
      switch (c)
	{

	case 'd':
	  debug = 1; /* undocumented, not RT-safe, don't use in production */
	  break;

	case 'f':
	{
	  fps_num = atoi(optarg);
	  char *tmp = strchr(optarg, '/');
	  if (tmp) fps_den=atoi(++tmp);
	}
	break;

	case 'F':
	  detect_framerate = 1;
	  break;

	case 'l':
	  free(ltcportname);
	  ltcportname = strdup(optarg);
	  break;

	case 'm':
	  free(mtcportname);
	  mtcportname = strdup(optarg);
	  break;

	case 's':
	  send_sysex = 1;
	  break;

	case 'V':
	  printf ("jltc2mtc version %s\n\n", VERSION);
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

  if (decode_switches (argc, argv) != argc) {
    usage(EXIT_FAILURE);
  }

  // -=-=-= INITIALIZE =-=-=-

  if (init_jack("jltc2mtc"))
    goto out;
  if (jack_portsetup())
    goto out;

  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

  detected_fps = ceil((double)fps_num/fps_den);

  if (!detect_framerate && (rint(100.0*(double)fps_num/fps_den) == 2997.0) )
    use30df=1;

  // -=-=-= RUN =-=-=-

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

  port_connect(ltcportname, mtcportname);

#ifndef _WIN32
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);
#endif

  // -=-=-= JACK DOES ALL THE WORK =-=-=-

  while (client_state != Exit) {
    sleep(1);
  }

  // -=-=-= CLEANUP =-=-=-

out:
  cleanup(0);
  return(0);
}
/* vi:set ts=8 sts=2 sw=2: */
