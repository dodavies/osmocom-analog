/* POCSAG (Radio-Paging Code #1) processing
 *
 * (C) 2021 by Andreas Eversberg <jolly@eversberg.eu>
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

#define CHAN pocsag->sender.kanal

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include "../libsample/sample.h"
#include "../libdebug/debug.h"
#include "../libmobile/call.h"
#include "../libmobile/cause.h"
#include "../libosmocc/message.h"
#include "pocsag.h"
#include "frame.h"
#include "dsp.h"

static struct channel_info {
	double		freq_mhz;	/* frequency in megahertz */
	double		deviation_khz;	/* deviation in kilohertz */
	int		baudrate;	/* default baudrate */
        char		*name;		/* name of channel */
} channel_info[] = {
	{ 466.230,	-4.5,	1200,	"Scall" },
	{ 448.475,	-4.5,	1200,	"Quix" },
	{ 448.425,	-4.5,	1200,	"TeLMI" },
	{ 465.970,	-4.5,	1200,	"Skyper" },
	{ 466.075,	-4.5,	1200,	"Cityruf" },
	{ 466.075,	-4.5,	1200,	"Euromessage" },
	{ 439.9875,	-4.5,	1200,	"DAPNET" },
        { 0.0, 0.0, 0, NULL}
};

static const char pocsag_lang[9][4] = {
	{ '@', 0xc2, 0xa7, '\0' },
	{ '[', 0xc3, 0x84, '\0' },
	{ '\\', 0xc3, 0x96, '\0' },
	{ ']', 0xc3, 0x9c, '\0' },
	{ '{', 0xc3, 0xa4, '\0' },
	{ '|', 0xc3, 0xb6, '\0' },
	{ '}', 0xc3, 0xbc, '\0' },
	{ '~', 0xc3, 0x9f, '\0' },
	{ '\0' },
};


void pocsag_list_channels(void)
{
        int i;
	char text[16];

        for (i = 0; channel_info[i].name; i++) {
                if (i == 0) {
                        printf("\nFrequency\tDeviation\tPolarity\tBaudrate\tChannel Name\n");
                        printf("--------------------------------------------------------------------------------\n");
                }
		if (channel_info[i].freq_mhz * 1e3 == floor(channel_info[i].freq_mhz * 1e3))
			sprintf(text, "%.3f MHz", channel_info[i].freq_mhz);
		else
			sprintf(text, "%.4f MHz", channel_info[i].freq_mhz);
                printf("%s\t%.3f KHz\t%s\t%d\t\t%s\n", text, fabs(channel_info[i].deviation_khz), (channel_info[i].deviation_khz < 0) ? "negative" : "positive", channel_info[i].baudrate, channel_info[i].name);
        }
        printf("-> Give channel name or any frequency in MHz.\n");
        printf("\n");
}

/* Convert channel name to frequency number of base station. */
double pocsag_channel2freq(const char *kanal, double *deviation, double *polarity, int *baudrate)
{
        int i;

        for (i = 0; channel_info[i].name; i++) {
                if (!strcasecmp(channel_info[i].name, kanal)) {
			if (deviation)
				*deviation = fabs(channel_info[i].deviation_khz) * 1e3;
			if (polarity)
				*polarity = (channel_info[i].deviation_khz > 0) ? 1.0 : -1.0;
			if (baudrate)
				*baudrate = channel_info[i].baudrate;
			return channel_info[i].freq_mhz * 1e6;
                }
        }

	return atof(kanal) * 1e6;
}

const char *pocsag_state_name[] = {
	"IDLE",
	"PREAMBLE",
	"MESSAGE",
};

const char *pocsag_function_name[4] = {
	"numeric",
	"beep1",
	"beep2",
	"alphanumeric",
};

int pocsag_function_name2value(const char *text)
{
	int i;

	for (i = 0; i < 4; i++) {
		if (!strcasecmp(pocsag_function_name[i], text))
			return i;
		if (text[0] == '0' + i && text[1] == '\0')
			return i;
		if (text[0] == 'A' + i && text[1] == '\0')
			return i;
		if (text[0] == 'a' + i && text[1] == '\0')
			return i;
	}

	return -EINVAL;
}

/* check if number is a valid station ID */
const char *pocsag_number_valid(const char *number)
{
	int i;
	int ric = 0;

	/* assume that the number has valid length(s) and digits */

	for (i = 0; i < 7; i++)
		ric = ric * 10 + number[i] - '0';
	if (ric > 2097151)
			return "Maximum allowed RIC is (2^21)-1. (2097151)";

	if ((ric & 0xfffffff8) == 2007664)
			return "Illegal RIC. (Used for idle codeword)";

	if (number[7] && (number[7] < '0' || number[7] > '3'))
			return "Illegal function digit #8 (Use 0..3 only)";
	return NULL;
}

int pocsag_init(void)
{
	return 0;
}

void pocsag_exit(void)
{
}

const char *print_ric(pocsag_msg_t *msg)
{
	static char text[16];

	sprintf(text, "%07d/%c", msg->ric, msg->function + '0');

	return text;
}

static void pocsag_display_status(void)
{
	sender_t *sender;
	pocsag_t *pocsag;
	pocsag_msg_t *msg;

	display_status_start();
	for (sender = sender_head; sender; sender = sender->next) {
		pocsag = (pocsag_t *) sender;
		display_status_channel(pocsag->sender.kanal, NULL, pocsag_state_name[pocsag->state]);
		for (msg = pocsag->msg_list; msg; msg = msg->next)
			display_status_subscriber(print_ric(msg), NULL);
	}
	display_status_end();
}

void pocsag_new_state(pocsag_t *pocsag, enum pocsag_state new_state)
{
	if (pocsag->state == new_state)
		return;
	PDEBUG(DPOCSAG, DEBUG_DEBUG, "State change: %s -> %s\n", pocsag_state_name[pocsag->state], pocsag_state_name[new_state]);
	pocsag->state = new_state;
	pocsag_display_status();
}

/* Create msg instance */
static pocsag_msg_t *pocsag_msg_create(pocsag_t *pocsag, uint32_t callref, uint32_t ric, enum pocsag_function function, const char *text)
{
	pocsag_msg_t *msg, **msgp;

	PDEBUG(DPOCSAG, DEBUG_INFO, "Creating msg instance to page RIC '%d' / function '%d' (%s).\n", ric, function, pocsag_function_name[function]);

	/* create */
	msg = calloc(1, sizeof(*msg));
	if (!msg) {
		PDEBUG(DPOCSAG, DEBUG_ERROR, "No mem!\n");
		abort();
	}
	if (strlen(text) > sizeof(msg->data)) {
		PDEBUG(DPOCSAG, DEBUG_ERROR, "Text too long!\n");
		return NULL;
	}

	/* init */
	msg->callref = callref;
	msg->ric = ric;
	msg->function = function;
	msg->repeat = 0;
	strncpy(msg->data, text, sizeof(msg->data));
	msg->data_length = (strlen(text) < sizeof(msg->data)) ? strlen(text) : sizeof(msg->data);

	/* link */
	msg->pocsag = pocsag;
	msgp = &pocsag->msg_list;
	while ((*msgp))
		msgp = &(*msgp)->next;
	(*msgp) = msg;

	/* kick transmitter */
	if (pocsag->state == POCSAG_IDLE) {
		pocsag_new_state(pocsag, POCSAG_PREAMBLE);
		pocsag->word_count = 0;
	} else
		pocsag_display_status();

	return msg;
}

/* Destroy msg instance */
void pocsag_msg_destroy(pocsag_msg_t *msg)
{
	pocsag_msg_t **msgp;

	/* unlink */
	msgp = &msg->pocsag->msg_list;
	while ((*msgp) != msg)
		msgp = &(*msgp)->next;
	(*msgp) = msg->next;

	/* remove from current transmitting message */
	if (msg == msg->pocsag->current_msg)
		msg->pocsag->current_msg = NULL;

	/* destroy */
	free(msg);

	/* update display */
	pocsag_display_status();
}

static int pocsag_scan_or_loopback(pocsag_t *pocsag)
{
	if (pocsag->scan_from < pocsag->scan_to) {
		char message[16];

		switch (pocsag->default_function) {
		case POCSAG_FUNCTION_NUMERIC:
			sprintf(message, "%05d", pocsag->scan_from / 100);
			break;
		case POCSAG_FUNCTION_ALPHA:
			sprintf(message, "%02x", pocsag->scan_from / 10000);
			break;
		default:
			message[0] = '\0';
		}
		PDEBUG_CHAN(DPOCSAG, DEBUG_NOTICE, "Transmitting %s message '%s' with RIC '%d'.\n", pocsag_function_name[pocsag->default_function], message, pocsag->scan_from);
		pocsag_msg_create(pocsag, 0, pocsag->scan_from, pocsag->default_function, message);
		pocsag->scan_from++;
		return 1;
	}

	if (pocsag->sender.loopback) {
		PDEBUG(DPOCSAG, DEBUG_INFO, "Sending message for loopback test.\n");
		pocsag_msg_create(pocsag, 0, 1234567, POCSAG_FUNCTION_NUMERIC, "1234");
		return 1;
	}

	return 0;
}

void pocsag_msg_receive(enum pocsag_language language, const char *channel, uint32_t ric, enum pocsag_function function, const char *message)
{
	char text[256 + strlen(message) * 4], *p;
	struct timeval tv;
	struct tm *tm;
	int i, j;

	gettimeofday(&tv, NULL);
	tm = localtime(&tv.tv_sec);

	sprintf(text, "%04d-%02d-%02d %02d:%02d:%02d.%03d @%s %d,%s", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, (int)(tv.tv_usec / 10000.0), channel, ric, pocsag_function_name[function]);
	p = strchr(text, '\0');

	if (message[0]) {
		*p++ = ',';

		if (language == LANGUAGE_DEFAULT) {
			strcpy(p, message);
			p += strlen(p);
		} else {
			for (i = 0; message[i]; i++) {
				/* decode special chracter */
				for (j = 0; pocsag_lang[j][0]; j++) {
					if (pocsag_lang[j][0] == message[i])
						break;
				}
				/* if character matches */
				if (pocsag_lang[j][0]) {
					strcpy(p, pocsag_lang[j] + 1);
					p += strlen(p);
				} else
					*p++ = message[i];
			}
		}
	}

	*p++ = '\0';

	msg_receive(text);
}

/* Create transceiver instance and link to a list. */
int pocsag_create(const char *kanal, double frequency, const char *device, int use_sdr, int samplerate, double rx_gain, double tx_gain, int tx, int rx, enum pocsag_language language, int baudrate, double deviation, double polarity, enum pocsag_function function, const char *message, uint32_t scan_from, uint32_t scan_to, const char *write_rx_wave, const char *write_tx_wave, const char *read_rx_wave, const char *read_tx_wave, int loopback)
{
	pocsag_t *pocsag;
	int rc;

	pocsag = calloc(1, sizeof(*pocsag));
	if (!pocsag) {
		PDEBUG(DPOCSAG, DEBUG_ERROR, "No memory!\n");
		return -ENOMEM;
	}

	PDEBUG(DPOCSAG, DEBUG_DEBUG, "Creating 'POCSAG' instance for 'Kanal' = %s (sample rate %d).\n", kanal, samplerate);

	/* init general part of transceiver */
	rc = sender_create(&pocsag->sender, kanal, frequency, frequency, device, use_sdr, samplerate, rx_gain, tx_gain, 0, 0, write_rx_wave, write_tx_wave, read_rx_wave, read_tx_wave, loopback, PAGING_SIGNAL_NONE);
	if (rc < 0) {
		PDEBUG(DPOCSAG, DEBUG_ERROR, "Failed to init transceiver process!\n");
		goto error;
	}

	/* init audio processing */
	rc = dsp_init_sender(pocsag, samplerate, (double)baudrate, deviation, polarity);
	if (rc < 0) {
		PDEBUG(DPOCSAG, DEBUG_ERROR, "Failed to init audio processing!\n");
		goto error;
	}

	pocsag->tx = tx;
	pocsag->rx = rx;
	pocsag->language = language;
	pocsag->default_function = function;
	pocsag->default_message = message;
	pocsag->scan_from = scan_from;
	pocsag->scan_to = scan_to;

	pocsag_display_status();

	PDEBUG(DPOCSAG, DEBUG_NOTICE, "Created 'Kanal' %s\n", kanal);

	pocsag_scan_or_loopback(pocsag);

	return 0;

error:
	pocsag_destroy(&pocsag->sender);

	return rc;
}

/* Destroy transceiver instance and unlink from list. */
void pocsag_destroy(sender_t *sender)
{
	pocsag_t *pocsag = (pocsag_t *) sender;

	PDEBUG(DPOCSAG, DEBUG_DEBUG, "Destroying 'POCSAG' instance for 'Kanal' = %s.\n", sender->kanal);

	while (pocsag->msg_list)
		pocsag_msg_destroy(pocsag->msg_list);
	dsp_cleanup_sender(pocsag);
	sender_destroy(&pocsag->sender);
	free(pocsag);
}

/* application sends us a message, we need to deliver */
void pocsag_msg_send(enum pocsag_language language, const char *text)
{
	char buffer[strlen(text) + 1], *p = buffer, *ric_string, *function_string, *message;
	uint32_t ric;
	uint8_t function;
	pocsag_t *pocsag;
	int i, j, k;
	int rc;

	strcpy(buffer, text);
	ric_string = strsep(&p, ",");
	function_string = strsep(&p, ",");
	message = p;

	if (!ric_string || !function_string) {
inval:
		PDEBUG(DNMT, DEBUG_NOTICE, "Given message MUST be in the following format: RIC,function[,<message with comma and spaces>] (function must be A = 0 = numeric, B = 1 or C = 2 = beep, D = 3 = alphanumeric)\n");
		return;
	}
	ric = atoi(ric_string);
	if (ric > 2097151) {
		PDEBUG(DNMT, DEBUG_NOTICE, "Illegal RIC %d. Maximum allowed RIC is (2^21)-1. (2097151)\n", ric);
		goto inval;
	}

	if (ric == 1003832) {
		PDEBUG(DNMT, DEBUG_NOTICE, "Illegal RIC 1003832. (Used as idle codeword)\n");
		goto inval;
	}

	rc = pocsag_function_name2value(function_string);
	if (rc < 0) {
		PDEBUG(DNMT, DEBUG_NOTICE, "Illegal function '%s'.\n", function_string);
		goto inval;
	}
	function = rc;

	if (message && (function == 1 || function == 2)) {
		PDEBUG(DNMT, DEBUG_NOTICE, "Message text is not allowed with function %d.\n", function);
		goto inval;
	}

	if (message && language != LANGUAGE_DEFAULT) {
		i = 0;
		p = message;
		while (*p) {
			/* encode special chracter */
			for (j = 0; pocsag_lang[j][0]; j++) {
				for (k = 0; pocsag_lang[j][k + 1]; k++) {
					/* implies that (p[k] == '\0') causes to break */
					if (p[k] != pocsag_lang[j][k + 1])
						break;
				}
				if (!pocsag_lang[j][k + 1])
					break;
			}
			/* if character matches */
			if (pocsag_lang[j][0]) {
				message[i++] = pocsag_lang[j][0];
				p += k;
			} else
				message[i++] = *p++;
		}
		message[i] = '\0';
	}

	if (!message)
		message="";

	PDEBUG(DNMT, DEBUG_INFO, "Message for ID '%d/%d' with text '%s'\n", ric, function, message);

	pocsag = (pocsag_t *) sender_head;
	pocsag_msg_create(pocsag, 0, ric, function, message);
}

void call_down_clock(void)
{
}

/* Call control starts call towards paging network. */
int call_down_setup(int callref, const char __attribute__((unused)) *caller_id, enum number_type __attribute__((unused)) caller_type, const char *dialing)
{
	char channel = '\0';
	sender_t *sender;
	pocsag_t *pocsag;
	uint32_t ric;
	enum pocsag_function function;
	const char *message;
	int i;
	pocsag_msg_t *msg;

	/* find transmitter */
	for (sender = sender_head; sender; sender = sender->next) {
		/* skip channels that are different than requested */
		if (channel && sender->kanal[0] != channel)
			continue;
		pocsag = (pocsag_t *) sender;
		/* check if base station cannot transmit */
		if (!pocsag->tx)
			continue;
		break;
	}
	if (!sender) {
		if (channel)
			PDEBUG(DPOCSAG, DEBUG_NOTICE, "Cannot page, because given station not available, rejecting!\n");
		else
			PDEBUG(DPOCSAG, DEBUG_NOTICE, "Cannot page, no trasmitting station available, rejecting!\n");
		return -CAUSE_NOCHANNEL;
	}

	/* get RIC and function */
	for (ric = 0, i = 0; i < 7; i++)
		ric = ric * 10 + dialing[i] - '0';
	if (dialing[7] >= '0' && dialing[7] <= '3')
		function = dialing[7]- '0';
	else
		function = pocsag->default_function;

	/* get message */
	if (caller_id[0])
		message = caller_id;
	else
		message = pocsag->default_message;

	/* create call process to page station */
	msg = pocsag_msg_create(pocsag, callref, ric, function, message);
	if (!msg)
		return -CAUSE_INVALNUMBER;
	return -CAUSE_NORMAL;

	return 0;
}

/* message was transmitted */
void pocsag_msg_done(pocsag_t *pocsag)
{
	/* start scanning, if enabled, otherwise send loopback sequence, if enabled */
	pocsag_scan_or_loopback(pocsag);
}

void call_down_answer(int __attribute__((unused)) callref)
{
}


static void _release(int __attribute__((unused)) callref, int __attribute__((unused)) cause)
{
	PDEBUG(DPOCSAG, DEBUG_INFO, "Call has been disconnected by network.\n");
}

void call_down_disconnect(int callref, int cause)
{
	_release(callref, cause);

	call_up_release(callref, cause);
}

/* Call control releases call toward mobile station. */
void call_down_release(int callref, int cause)
{
	_release(callref, cause);
}

/* Receive audio from call instance. */
void call_down_audio(int __attribute__((unused)) callref, sample_t __attribute__((unused)) *samples, int __attribute__((unused)) count)
{
}

void dump_info(void) {}
