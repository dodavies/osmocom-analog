/* DCF77 main
 *
 * (C) 2022 by Andreas Eversberg <jolly@eversberg.eu>
 * All Rights Reserved
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <termios.h>
#include <sched.h>
#include <time.h>
#include <math.h>
#include "../liblogging/logging.h"
#include "../liboptions/options.h"
#include "../libsample/sample.h"
#include "../libaaimage/aaimage.h"
#include <osmocom/cc/misc.h>
#include "dcf77.h"
#include "cities.h"

int num_kanal = 1;
dcf77_t *dcf77 = NULL;
static void *soundif = NULL;
static const char *dsp_device = "";
static int dsp_samplerate = 192000;
static int dsp_buffer = 50;
static int rx = 0, tx = 0;
static double timestamp = -1;
static int weather = 0;
static int weather_day;
static int weather_night;
static int extreme;
static int rain;
static int wind_dir;
static int wind_bft;
static int temperature_day;
static int temperature_night;
static int region = -1, region_advance;
static int double_amplitude = 0;
static int test_tone = 0;
static int dsp_interval = 1; /* ms */
static int rt_prio = 0;
static int fast_math = 0;

/* not static, in case we add libtimer some day, then compiler hits an error */
double get_time(void);
double get_time(void)
{
	static struct timespec tv;

	clock_gettime(CLOCK_REALTIME, &tv);

	return (double)tv.tv_sec + (double)tv.tv_nsec / 1000000000.0;
}

static time_t parse_time(char **argv)
{
	time_t t;
	struct tm *tm;
	int val;

	t = get_time();
	tm = localtime(&t);
	if (!tm)
		return -1;

	val = atoi(argv[0]);
	if (val < 1900)
		return -1;
	tm->tm_year = val - 1900;

	val = atoi(argv[1]);
	if (val < 1 || val > 12)
		return -1;
	tm->tm_mon = val - 1;

	val = atoi(argv[2]);
	if (val < 1 || val > 31)
		return -1;
	tm->tm_mday = val;

	val = atoi(argv[3]);
	if (val < 0 || val > 23)
		return -1;
	tm->tm_hour = val;

	val = atoi(argv[4]);
	if (val < 0 || val > 59)
		return -1;
	tm->tm_min = val;

	val = atoi(argv[5]);
	if (val < 0 || val > 59)
		return -1;
	tm->tm_sec = val;

	tm->tm_isdst = -1;

	return mktime(tm);
}

static time_t feierabend_time()
{
	time_t t;
	struct tm *tm;

	t = get_time();
	tm = localtime(&t);

	tm->tm_hour = 17;
	tm->tm_min = 0;
	tm->tm_sec = 0;

	tm->tm_isdst = -1;

	return mktime(tm);
}

static void print_usage(const char *app)
{
	printf("Usage: %s [-a hw:0,0] [<options>]\n", app);
}

static void print_help(void)
{
	/*      -                                                                             - */
	printf(" -h --help\n");
	printf("        This help\n");
	printf(" --config [~/]<path to config file>\n");
	printf("        Give a config file to use. If it starts with '~/', path is at home dir.\n");
	printf("        Each line in config file is one option, '-' or '--' must not be given!\n");
	logging_print_help();
	printf(" -a --audio-device hw:<card>,<device>\n");
	printf("        Sound card and device number (default = '%s')\n", dsp_device);
	printf(" -s --samplerate <rate>\n");
	printf("        Sample rate of sound device (default = '%d')\n", dsp_samplerate);
	printf(" -b --buffer <ms>\n");
	printf("        How many milliseconds are processed in advance (default = '%d')\n", dsp_buffer);
	printf("        A buffer below 10 ms requires low interval like 0.1 ms.\n");
	printf(" -T --tx\n");
	printf("        Transmit time signal (default)\n");
	printf(" -R --rx\n");
	printf("        Receive time signal\n");
	printf(" -F --fake\n");
	printf("        Use given time stamp: <year> <month> <day> <hour> <min> <sec>.\n");
	printf("        All values have to be numerical. The year must have 4 digits.\n");
	printf("    --feierabend\n");
	printf("    --end-of-working-day\n");
	printf("        Use fake time stamp that equals 5 O'Clock PM.\n");
	printf("    --geburtstag\n");
	printf("    --birthday\n");
	printf("        Use fake time stamp that equals birth of the author.\n");
	printf(" -W --weather <weather> <extreme> <rain> <wind dir> <wind bft> <temperature>\n");
	printf("        Send these weather info for all regions / all days.\n");
	printf("        See -L for infos on values.\n");
	printf("        weather = 1..15 for common day and night weather.\n");
	printf("        weather = 1..15,1..15 for specific day and night weather.\n");
	printf("        extreme = 0..15 for extreme weather conditions.\n");
	printf("        rain = 0..100 for rain/show probability. (closest is used)\n");
	printf("        wind dir = N | NE | E | SE | S | SW | W | NW | 0 for wind direction.\n");
	printf("        wind bft = <bft> for wind speed in bft. (closest is used)\n");
	printf("        temerature = <celsius> for common min and max temperature.\n");
	printf("        temerature = <celsius>,<celsius> for specific min and max temperature.\n");
	printf(" --beach-party\n");
	printf("        Beach weather, equivalent to -W 1 0 0 0 2 35,20\n");
	printf(" --santa-claus\n");
	printf(" --muenster-2005\n");
	printf("        Deep snow, equivalent to -W 7 1 100 E 3 1,-1\n");
	printf(" -A --at-region <region> <advance minutes>\n");
	printf("        Alter time, so that the weather of the given region is transmitted.\n");
	printf("        To allow the receiver to sync, give time to advance in minutes.\n");
	printf(" -L --list\n");
	printf("        List all regions / weather values.\n");
	printf(" -C --city <name fragment>\n");
	printf("        Search for city (case insensitive) and display its region code.\n");
	printf(" -D --double-amplitude\n");
	printf("        Transmit with double amplitude by using differential stereo output.\n");
	printf("     --test-tone\n");
	printf("        Transmit a test tone (10%% level, 1000 Hz) with the carrier.\n");
	printf(" -r --realtime <prio>\n");
	printf("        Set prio: 0 to disable, 99 for maximum (default = %d)\n", rt_prio);
	printf("    --fast-math\n");
	printf("        Use fast math approximation for slow CPU / ARM based systems.\n");
	printf("\n");
	printf("Press 'w' key to toggle display of RX wave form.\n");
	printf("Press 'm' key to toggle display of measurement values.\n");
}

#define OPT_F1		1001
#define OPT_F2		1002
#define OPT_G1		1003
#define OPT_G2		1004
#define OPT_BEACH	1005
#define OPT_SANTA	1006
#define OPT_MUENSTER	1007
#define OPT_TEST_TONE	1008
#define OPT_FAST_MATH	1009

static void add_options(void)
{
	option_add('h', "help", 0);
	option_add('v', "verbose", 1);
	option_add('a', "audio-device", 1);
	option_add('s', "samplerate", 1);
	option_add('b', "buffer", 1);
	option_add('T', "tx", 0);
	option_add('R', "rx", 0);
	option_add('F', "fake", 6);
	option_add(OPT_F1, "feierabend", 0);
	option_add(OPT_F2, "end-of-working-day", 0);
	option_add(OPT_G1, "geburtstag", 0);
	option_add(OPT_G2, "birthday", 0);
	option_add('W', "weather", 6);
	option_add(OPT_BEACH, "beach-party", 0);
	option_add(OPT_SANTA, "santa-claus", 0);
	option_add(OPT_MUENSTER, "muenster-2005", 0);
	option_add('A', "at-region", 2);
	option_add('L', "list", 0);
	option_add('C', "city", 1);
	option_add('D', "double-amplitude", 0);
	option_add(OPT_TEST_TONE, "test-tone", 0);
	option_add('r', "realtime", 1);
	option_add(OPT_FAST_MATH, "fast-math", 0);
}

static const char *wind_dirs[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };

static int handle_options(int short_option, int argi, char **argv)
{
	char *string, *string1;
	int rc, i;

	switch (short_option) {
	case 'h':
		print_usage(argv[0]);
		print_help();
		return 0;
	case 'v':
		rc = parse_logging_opt(argv[argi]);
		if (rc > 0)
			return 0;
		if (rc < 0) {
			fprintf(stderr, "Failed to parse logging option, please use -h for help.\n");
			return rc;
		}
		break;
	case 'a':
		dsp_device = options_strdup(argv[argi]);
		break;
	case 's':
		dsp_samplerate = atoi(argv[argi]);
		break;
	case 'b':
		dsp_buffer = atoi(argv[argi]);
		break;
	case 'T':
		tx = 1;
		break;
	case 'R':
		rx = 1;
		break;
	case 'F':
		timestamp = parse_time(argv + argi);
		if (timestamp < 0) {
			fprintf(stderr, "Given time stamp is invalid, please use -h for help.\n");
			return -EINVAL;
		}
		break;
	case OPT_F1:
	case OPT_F2:
		timestamp = feierabend_time() - 70;
		break;
	case OPT_G1:
	case OPT_G2:
		timestamp = 115099200 - 70;
		break;
	case 'W':
		if (weather) {
no_multiple_weathers:
			fprintf(stderr, "You cannot define more than one weather situation.\n");
			return -EINVAL;
		}
		weather = 1;
		string = options_strdup(argv[argi++]);
		string1 = strsep(&string, ",");
		weather_day = atoi(string1);
		if (string)
			weather_night = atoi(string);
		else
			weather_night = weather_day;
		extreme = atoi(argv[argi++]);
		rain = atoi(argv[argi++]);
		/* if wind is not found, wind 8 (changable) is selected */
		string = options_strdup(argv[argi++]);
		for (i = 0; i < 8; i++) {
			if (!strcasecmp(string, wind_dirs[i]))
				break;
		}
		wind_dir = i;
		wind_bft = atoi(argv[argi++]);
		string = options_strdup(argv[argi++]);
		string1 = strsep(&string, ",");
		temperature_day = atoi(string1);
		if (string)
			temperature_night = atoi(string);
		else
			temperature_night = temperature_day;
		break;
	case OPT_BEACH:
		if (weather)
			goto no_multiple_weathers;
		weather = 1;
		weather_day = 1;
		weather_night = 1;
		extreme = 0;
		rain = 0;
		wind_dir = 8;
		wind_bft = 2;
		temperature_day = 35;
		temperature_night = 20; /* tropical night >= 20 */
		break;
	case OPT_SANTA:
	case OPT_MUENSTER:
		if (weather)
			goto no_multiple_weathers;
		weather = 1;
		weather_day = 7;
		weather_night = 7;
		extreme = 0;
		rain = 100;
		wind_dir = 6;
		wind_bft = 3;
		temperature_day = 1;
		temperature_night = -1; /* freezing a little */
		break;
	case 'A':
		region = atoi(argv[argi++]);
		if (region < 0 || region > 89) {
			fprintf(stderr, "Given region number is is invalid, please use -L for list of valid regions.\n");
			return -EINVAL;
		}
		region_advance = atoi(argv[argi++]);
		break;
	case 'L':
		list_weather();
		return 0;
	case 'C':
		display_city(argv[argi++]);
		return 0;
	case OPT_TEST_TONE:
		test_tone = 1;
		break;
	case 'D':
		double_amplitude = 1;
		break;
	case 'r':
		rt_prio = atoi(argv[argi]);
		break;
	case OPT_FAST_MATH:
		fast_math = 1;
		break;
	default:
		return -EINVAL;
	}

	return 1;
}

static int quit = 0;
static void sighandler(int sigset)
{
	if (sigset == SIGHUP || sigset == SIGPIPE)
		return;

	fprintf(stderr, "\nSignal %d received.\n", sigset);

	quit = 1;
}

static int get_char()
{
        struct timeval tv = {0, 0};
        fd_set fds;
        char c = 0;
        int __attribute__((__unused__)) rc;

        FD_ZERO(&fds);
        FD_SET(0, &fds);
        select(0+1, &fds, NULL, NULL, &tv);
        if (FD_ISSET(0, &fds)) {
                rc = read(0, &c, 1);
                return c;
        } else
                return -1;
}

static int soundif_open(const char *audiodev, int samplerate, int buffer_size)
{
	enum sound_direction direction = SOUND_DIR_DUPLEX;

	if (!audiodev || !audiodev[0]) {
		LOGP(DDSP, LOGL_ERROR, "No audio device given!\n");
		return -EINVAL;
	}

	/* open audiodev */
	if (tx && !rx)
		direction = SOUND_DIR_PLAY;
	if (rx && !tx)
		direction = SOUND_DIR_REC;
	soundif = sound_open(direction, audiodev, NULL, NULL, NULL, (double_amplitude) ? 2 : 1, 0.0, samplerate, buffer_size, 1.0, 1.0, 0.0, 2.0);
	if (!soundif) {
		LOGP(DDSP, LOGL_ERROR, "Failed to open sound device!\n");
		return -EIO;
	}

	return 0;
}

static void soundif_start(void)
{
	sound_start(soundif);
	LOGP(DDSP, LOGL_DEBUG, "Starting audio stream!\n");
}

static void soundif_close(void)
{
	/* close audiodev */
	if (soundif) {
		sound_close(soundif);
		soundif = NULL;
	}
}

static void soundif_work(int buffer_size)
{
	int count;
	sample_t buff1[buffer_size], buff2[buffer_size], *samples[2] = { buff1, buff2 };
	double rf_level_db[2];
	int rc;
	int i;

	if (tx) {
		/* encode and write */
		count = sound_get_tosend(soundif, buffer_size);
		if (count < 0) {
			LOGP(DDSP, LOGL_ERROR, "Failed to get number of samples in buffer (rc = %d)!\n", count);
			return;
		}
		if (count) {
			dcf77_encode(dcf77, samples[0], count);
			if (double_amplitude) {
				for (i = 0; i < count; i++)
					samples[1][i] = -samples[0][i];
			}
			rc = sound_write(soundif, samples, NULL, count, NULL, NULL, (double_amplitude) ? 2 : 1);
			if (rc < 0) {
				LOGP(DDSP, LOGL_ERROR, "Failed to write TX data to audio device (rc = %d)\n", rc);
				return;
			}
		}
	}

	if (rx) {
		/* read */
		count = sound_read(soundif, samples, buffer_size, 1, rf_level_db);
		if (count < 0) {
			LOGP(DDSP, LOGL_ERROR, "Failed to read from audio device (rc = %d)!\n", count);
			return;
		}

		/* decode */
		dcf77_decode(dcf77, samples[0], count);
	}
}

int main(int argc, char *argv[])
{
	int rc, argi;
	int buffer_size;
	struct termios term, term_orig;
	double begin_time, now, sleep;
	char c;

	logging_init();

	/* handle options / config file */
	add_options();
	rc = options_config_file(argc, argv, "~/.osmocom/dcf77/dcf77.conf", handle_options);
	if (rc < 0)
		return 0;
	argi = options_command_line(argc, argv, handle_options);
	if (argi <= 0)
		return argi;

	if (dsp_samplerate < 192000) {
		fprintf(stderr, "The sample rate must be at least 192000 to TX or RX 77.5 kHz. Quitting!\n");
		goto error;
	}

	/* default to TX, if --tx and --rx was not set */
	if (!tx && !rx)
		tx = 1;

	/* inits */
	dcf77_init(fast_math);

	/* size of dsp buffer in samples */
	buffer_size = dsp_samplerate * dsp_buffer / 1000;

	rc = soundif_open(dsp_device, dsp_samplerate, buffer_size);
	if (rc < 0) {
		printf("Failed to open sound for DCF77, use '-h' for help.\n");
		goto error;
	}

	dcf77 = dcf77_create(dsp_samplerate, tx, rx, test_tone);
	if (!dcf77) {
		fprintf(stderr, "Failed to create \"DCF77\" instance. Quitting!\n");
		goto error;
	}
	if (weather)
		dcf77_set_weather(dcf77, weather_day, weather_night, extreme, rain, wind_dir, wind_bft, temperature_day, temperature_night);

	/* no time stamp given, so we use our clock */
	if (tx) {
		if (timestamp < 0 && region < 0)
			printf("No alternative time given, so you might not notice the difference between our transmission and the real DCF77 transmission.\n");
		if (timestamp < 0)
			timestamp = get_time();
		if (region >= 0 && weather)
			timestamp = dcf77_start_weather((time_t)timestamp, region, region_advance);
	}

	/* set real time prio */
	if (rt_prio) {
		struct sched_param schedp;

		memset(&schedp, 0, sizeof(schedp));
		schedp.sched_priority = rt_prio;
		rc = sched_setscheduler(0, SCHED_RR, &schedp);
		if (rc) {
			fprintf(stderr, "Error setting SCHED_RR with prio %d\n", rt_prio);
			goto error;
		}
	}

	print_aaimage();
	printf("DCF77 ready.\n");

	/* prepare terminal */
	tcgetattr(0, &term_orig);
	term = term_orig;
	term.c_lflag &= ~(ISIG|ICANON|ECHO);
	term.c_cc[VMIN]=1;
	term.c_cc[VTIME]=2;
	tcsetattr(0, TCSANOW, &term);

	signal(SIGINT, sighandler);
	signal(SIGHUP, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, sighandler);

	soundif_start();
	if (tx)
		dcf77_tx_start(dcf77, (time_t)timestamp, fmod(timestamp, 1.0));

	while (!quit) {
		int w;

		begin_time = get_time();

		soundif_work(buffer_size);
		do {
			w = 0;
		} while (w);

		c = get_char();
		switch (c) {
		case 3:
			printf("CTRL+c received, quitting!\n");
			quit = 1;
			break;
		case 'w':
			/* toggle wave display */
			display_measurements_on(0);
                        display_wave_on(-1);
			break;
		case 'm':
			/* toggle measurements display */
			display_wave_on(0);
			display_measurements_on(-1);
			break;
		default:
			break;
		}

		display_measurements(dsp_interval / 1000.0);

		now = get_time();

		/* sleep interval */
		sleep = ((double)dsp_interval / 1000.0) - (now - begin_time);
		if (sleep > 0)
			usleep(sleep * 1000000.0);
	}

	signal(SIGINT, SIG_DFL);
	signal(SIGTSTP, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);

	/* reset real time prio */
	if (rt_prio > 0) {
		struct sched_param schedp;

		memset(&schedp, 0, sizeof(schedp));
		schedp.sched_priority = 0;
		sched_setscheduler(0, SCHED_OTHER, &schedp);
	}

	/* reset terminal */
	tcsetattr(0, TCSANOW, &term_orig);

error:
	/* destroy UK0 instances */
	if (dcf77)
		dcf77_destroy(dcf77);

	soundif_close();

	dcf77_exit();

	display_measurements_on(0);
	display_wave_on(0);

	options_free();

	return 0;
}

void osmo_cc_set_log_cat(int __attribute__((unused)) cc_log_cat) {}

sender_t *get_sender_by_empfangsfrequenz(double __attribute__((unused)) freq) { return NULL; }

