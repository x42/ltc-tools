/* gcc -o jltcntp jltcntp.c `pkg-config --cflags --libs jack ltc`
 *
 * Copyright (C) 2006, 2012, 2013 Robin Gareus
 * Copyright (C) 2015 Dimitry Ishenko
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

#define LTC_QUEUE_LEN 42 // should be >> ( max(jack period size) * max-speedup / (duration of LTC-frame) )

#define _GNU_SOURCE

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

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

static int keep_running = 1;

static jack_port_t *input_port = NULL;
static jack_client_t *j_client = NULL;
static uint32_t j_samplerate = 48000;

static LTCDecoder *decoder = NULL;

static pthread_mutex_t ltc_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER;

static int fps_num = 25;
static int fps_den = 1;

static int unit = -1;
static int no_date = 0;

static int verbose = 0;

struct shmTime
{
    int    mode; /* 0 - if valid is set,
                  *         use values and clear valid
                  * 1 - if valid is set && count before and after read of values is equal,
                  *         use values and clear valid
                  */
    int    count;
    time_t clockTimeStampSec;
    int    clockTimeStampUSec;
    time_t receiveTimeStampSec;
    int    receiveTimeStampUSec;
    int    leap;
    int    precision;
    int    nsamples;
    int    valid;
};

static volatile struct shmTime *shm = NULL;

/**
 * jack audio process callback
 */
int process(jack_nframes_t nframes, void *arg)
{
    jack_default_audio_sample_t *in = jack_port_get_buffer(input_port, nframes);

    ltc_decoder_write_float(decoder, in, nframes, 0);

    /* notify reader thread */
    if (pthread_mutex_trylock(&ltc_thread_lock) == 0)
    {
        pthread_cond_signal(&data_ready);
        pthread_mutex_unlock(&ltc_thread_lock);
    }
    return 0;
}

/**
 * jack_shutdown
 */
void jack_shutdown(void *arg)
{
    fprintf(stderr, "Received shutdown request from JACK\n");
    keep_running = 0;
    pthread_cond_signal(&data_ready);
}

/**
 * open a client connection to the JACK server
 */
static int init_jack(const char *client_name)
{
    jack_status_t status;

    j_client = jack_client_open(client_name, JackNullOption, &status);
    if (j_client == NULL)
    {
        fprintf(stderr, "jack_client_open() failed, status = 0x%2x\n", status);
        if (status & JackServerFailed)
        {
            fprintf(stderr, "Unable to connect to JACK server\n");
        }
        return -1;
    }
    if (status & JackServerStarted)
    {
        fprintf(stderr, "JACK server started\n");
    }
    if (status & JackNameNotUnique)
    {
        client_name = jack_get_client_name(j_client);
        fprintf(stderr, "jack-client name: '%s'\n", client_name);
    }

    jack_set_process_callback(j_client, process, 0);

#ifndef WIN32
    jack_on_shutdown(j_client, jack_shutdown, NULL);
#endif
    j_samplerate = jack_get_sample_rate(j_client);

    return 0;
}

/**
 * jack_portsetup
 */
static int jack_portsetup()
{
    /* Allocate data structures that depend on the number of ports. */
    decoder = ltc_decoder_create(j_samplerate * fps_den / fps_num, LTC_QUEUE_LEN);
    if (!decoder)
    {
        fprintf (stderr, "Cannot create LTC decoder (out of memory)\n");
        return -1;
    }

    /* create jack port */
    if ((input_port = jack_port_register(j_client, "input_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0)
    {
        fprintf(stderr, "Cannot register input port 'input_1'!\n");
        return -1;
    }

    return 0;
}

/**
 * jack_port_connect
 */
static void jack_port_connect(char **jack_port, int argc)
{
    int i;
    for (i = 0; i < argc; ++i)
    {
        if (!jack_port[i]) continue;
        if (jack_connect(j_client, jack_port[i], jack_port_name(input_port)))
        {
            fprintf(stderr, "Cannot connect port %s to %s\n", jack_port[i], jack_port_name(input_port));
        }
    }
}

/**
 * my_decoder_read
 */
static void my_decoder_read(LTCDecoder *d)
{
    static struct timeval prev = { 0, 0 };

    LTCFrameExt frame;
    while (ltc_decoder_read(d, &frame))
    {
        int use_date = !no_date && frame.ltc.binary_group_flag_bit0 == 0
                                && frame.ltc.binary_group_flag_bit2 == 1;

        SMPTETimecode stime;
        ltc_frame_to_time(&stime, &frame.ltc, use_date ? LTC_USE_DATE : 0);

        struct tm tm;
        time_t offset = 0;

        if (use_date)
        {
            int code = frame.ltc.user7 + (frame.ltc.user8 << 4);
            if (code != 0x38) // user-defined time offset
            {
                offset = atoi(stime.timezone);
                offset = (offset / 100) * 60 + (offset % 100);
                offset *= 60; // seconds West of UTC

                tzset();
                offset -= timezone; // offset between LTC and local timezone
            }

            tm.tm_mday = stime.days;        // 1..31
            tm.tm_mon = stime.months - 1;   // 0..11
            tm.tm_year = stime.years + 100; // years since 1900
            tm.tm_isdst = -1;               // look up DST
        }
        else
        {
            time_t tc = time(NULL);
            localtime_r(&tc, &tm);
        }

        tm.tm_sec = stime.secs;
        tm.tm_min = stime.mins;
        tm.tm_hour = stime.hours;

        struct timeval time;
        time.tv_sec = mktime(&tm);
        time.tv_usec = 0 /* stime.frame * 1000000 / fps */;

        int sent = 0;
        if (shm && time.tv_sec != -1 && (time.tv_sec != prev.tv_sec || time.tv_usec != prev.tv_usec))
        {
            shm->mode = 0;
            if (!shm->valid)
            {
                struct timeval tv;
                gettimeofday(&tv, NULL);

                shm->clockTimeStampSec = time.tv_sec - offset;
                shm->clockTimeStampUSec = time.tv_usec;
                shm->receiveTimeStampSec = tv.tv_sec;
                shm->receiveTimeStampUSec = tv.tv_usec;

                shm->valid = 1;
            }

            prev.tv_sec = time.tv_sec;
            prev.tv_usec = time.tv_usec;

            sent = 1;
        }

        if (verbose)
        {
            printf("%02d-%02d-%02d %s %02d:%02d:%02d%c%02d",
                stime.years,
                stime.months,
                stime.days,
                stime.timezone,
                stime.hours,
                stime.mins,
                stime.secs,
                frame.ltc.dfbit ? '.' : ':',
                stime.frame
            );

            if (sent)
                printf(" ==> %s", asctime(&tm));
            else printf("\n");
        }
    }
    fflush(stdout);
}

/**
 * main_loop
 */
static void main_loop()
{
    pthread_mutex_lock(&ltc_thread_lock);

    while (keep_running)
    {
        my_decoder_read(decoder);
        if (!keep_running) break;

        pthread_cond_wait(&data_ready, &ltc_thread_lock);
    }

    pthread_mutex_unlock (&ltc_thread_lock);
}

void catchsig(int sig)
{
#ifndef _WIN32
    signal(SIGHUP, catchsig);
#endif
    fprintf(stderr, "Caught signal - shutting down\n");
    keep_running = 0;
    pthread_cond_signal(&data_ready);
}

static struct option const long_options[] =
{
    { "help",    no_argument,       NULL, 'h' },
    { "fps",     required_argument, NULL, 'f' },
    { "unit",    required_argument, NULL, 'u' },
    { "no-date", no_argument,       NULL, 'n' },
    { "verbose", no_argument,       NULL, 'v' },
    { "version", no_argument,       NULL, 'V' },
    { NULL,      0,                 NULL,  0  },
};

/**
 * usage
 */
static void usage(int status)
{
    printf("jltcntp - JACK LTC parser with NTP SHM support\n\n");
    printf("Usage: jltcntp [ options ] [ JACK-ports ]\n\n");
    printf("Options:\n\
  -f, --fps  <num>[/den]     set expected framerate (default 25/1)\n\
  -u, --unit <u>             send LTC to NTP SHM driver unit <u> (default none)\n\
  -n, --no-date              ignore date received via LTC\n\
  -v, --verbose              output data to stdout\n\
  -h, --help                 display this help and exit\n\
  -V, --version              print version information and exit\n\n");

    printf("Website and manual: <https://github.com/x42/ltc-tools>\n");
    exit(status);
}

/**
 * decode_switches
 */
static int decode_switches(int argc, char **argv)
{
    int c;

    while ((c = getopt_long(argc, argv,
                          "h"  /* help */
                          "f:" /* fps */
                          "u:" /* unit */
                          "n"  /* no_date */
                          "v"  /* verbose */
                          "V", /* version */
                          long_options, NULL)) != EOF)
    {
        switch (c)
        {
        case 'f':
            {
                fps_num = atoi(optarg);
                char *tmp = strchr(optarg, '/');
                if (tmp) fps_den = atoi(++tmp);
            }
            break;

        case 'u':
            unit = atoi(optarg);
            break;

        case 'n':
            no_date = 1;
            break;

        case 'v':
            verbose = 1;
            break;

        case 'V':
            printf("jltcntp version %s\n\n", VERSION);
            printf("Copyright (C) GPL 2006,2012,2013 Robin Gareus <robin@gareus.org>\n");
            printf("Copyright (C) 2015 Dimitry Ishenko <dimitry.ishenko@gmail.com>\n");
            exit(0);

        case 'h': usage (0);

        default: usage(EXIT_FAILURE);
        }
    }

    return optind;
}

int main(int argc, char **argv)
{
    int i = decode_switches (argc, argv);

    if (init_jack("jltcntp")) goto out;

    if (jack_portsetup()) goto out;

    if (mlockall(MCL_CURRENT | MCL_FUTURE))
    {
        fprintf(stderr, "Warning: Can not lock memory\n");
    }

    if (jack_activate(j_client))
    {
        fprintf(stderr, "Cannot activate client\n");
        goto out;
    }

    jack_port_connect(&argv[i], argc - i);

#ifndef _WIN32
    signal(SIGHUP, catchsig);
    signal(SIGINT, catchsig);
#endif

    if (unit >= 0)
    {
        int shmid = -1;
        int errshm = EACCES;
        const int perms[] = {0777, 0666, 0770, 0660, 0700, 0600};
        int attempt = 0;

        while (shmid == -1 && errshm == EACCES && attempt < sizeof(perms)) {
            shmid = shmget(0x4e545030 + unit, sizeof (struct shmTime), IPC_CREAT | perms[attempt]);
            errshm = errno;
            attempt++;
        }

        if (shmid != -1)
        {
            shm = (struct shmTime *) shmat(shmid, NULL, 0);
            if (shm == (void *) -1)
            {
                perror("shmat");
                goto out;
            }
        }
        else
        {
            perror("shmget");
            goto out;
        }
    }

    main_loop();

out:
    if (j_client)
    {
        jack_client_close(j_client);
        j_client = NULL;
    }

    ltc_decoder_free(decoder);
    fprintf(stderr, "Bye\n");

    return 0;
}
