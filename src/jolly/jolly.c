/* protocol handling
 *
 * (C) 2017 by Andreas Eversberg <jolly@eversberg.eu>
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

/*
 * Usage:
 *
 * 1. Dial '*' + <number> + '#' (start with * again to correct false digits).
 * 2. Listen to replied digits, dial again, if they are wrong or incomplete. 
 * 3. Acknowledge number with '#' (within a few seconds) to make the call
 * 4. Acknowledge incoming call with '#' and make the call
 * 5. At any time (also while dialing) dial '*' + '#' to release a call
 *
 * States:
 *
 * IDLE		No call, base station is idle
 * OUT-DIALING	Outgoing call, user is dialing
 * OUT-VERIFY	Outgoing call, digits are repeated
 * CALL		Active call
 * CALL-DIALING	User is dialing during call
 * IN-PAGING	Incoming call, user is paged
 * RELEASED	Fixed network released call
 *
 * Timers:
 *
 * T-DIAL	Maximum time between digits (several seconds)
 * T-DIAL2	Maximum time between digits during call (close to one second)
 * T-PAGING	Time to page user (30 or more seconds)
 *
 * Call events:
 *
 * call setup	Make or receive call
 * call release	Release call or call has been released
 * call alert	Indicate that call is alerting
 * call answer	Indicate that the call has been answered
 *
 * State machine:
 *
 * state	| event			| action
 * -------------+-----------------------+--------------------------------------
 * IDLE		| '*' received		| clear dial string
 * 		|			| start timer T-DIAL
 * 		|			| go to state OUT-DIALING
 * 		|			|
 * 		| call setup		| start timer T-PAGE
 * 		|			| start paging sequence
 * 		|			| call alert
 * 		|			| go to state IN-PAGING
 * 		|			|
 * -------------+-----------------------+--------------------------------------
 * OUT-DIALING	| '*' received		| clear dial string
 * 		|			| restart timer T-DIAL
 * 		|			|
 * 		| '0'..'9' received	| append digit to dial string
 * 		|			| restart timer T-DIAL
 * 		|			|
 * 		| '#' received		| stop timer
 * 		|			| if empty dial string:
 * 		|			|	go to state IDLE
 * 		|			| if dial string:
 * 		|			|	go to state OUT-VERIFY
 * 		|			|	play dialed digits
 * 		|			|
 * 		| timeout		| go to state IDLE
 * 		|			|
 * -------------+-----------------------+--------------------------------------
 * OUT-VERIFY	| end of playing digits	| start timer T-DIAL
 * 		|			|
 * 		| '*' received		| clear dial string
 * 		|			| restart timer T-DIAL
 * 		|			| go to state OUT-DIALING
 * 		|			|
 * 		| '#' received		| stop timer
 * 		|			| call setup
 * 		|			| if call setup fails:
 * 		|			| 	play release announcement
 * 		|			| 	go to state RELEASED
 * 		|			| go to state CALL
 * 		|			|
 * 		| timeout		| go to state IDLE
 * 		|			|
 * -------------+-----------------------+--------------------------------------
 * CALL		| '*' received		| start timer T-DIAL2
 * 		|			| go to state CALL-DIALING
 * 		|			|
 * 		| call release		| play release announcement
 * 		|			| go to state RELEASED
 * 		|			|
 * -------------+-----------------------+--------------------------------------
 * CALL-DIALING	| '#' received		| stop timer
 * 		|			| call release
 * 		|			| play release announcement
 * 		|			| go to state RELEASED
 * 		|			|
 * 		| timeout		| go state CALL
 * 		|			|
 * 		| call release		| play release announcement
 * 		|			| go to state RELEASED
 * 		|			|
 * -------------+-----------------------+--------------------------------------
 * IN-PAGING	| '#' received		| call answer
 * 		|			|
 * 		| timeout		| call release
 * 		|			| go to state IDLE
 * 		|			|
 * 		| call release		| go to state IDLE
 * 		|			|
 * -------------+-----------------------+--------------------------------------
 * RELEASED	| end of announcement	| go to state IDLE
 * 		|			|
 */

#define CHAN jolly->sender.kanal

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include "../libsample/sample.h"
#include "../liblogging/logging.h"
#include <osmocom/core/timer.h>
#include "../libmobile/call.h"
#include "../libmobile/cause.h"
#include <osmocom/cc/message.h>
#include "jolly.h"
#include "dsp.h"
#include "voice.h"

#define db2level(db)	pow(10, (double)db / 20.0)

/* Timers */
#define T_DIAL		6,0		/* Time between digits */
#define T_DIAL2		1,500000	/* Time between digits during call*/
#define T_PAGING	30,0		/* How long do we page the mobile party */
#define SPEECH_DELAY_PAGING	1,0	/* time before speaking paging sequence */
#define SPEECH_DELAY_VERIFY	2,0	/* time before speaking verifying sequence */
#define SPEECH_DELAY_RELEASE	2,0	/* time before speaking release sequence */

static const char *jolly_state_name(enum jolly_state state)
{
	static char invalid[16];

	switch (state) {
	case STATE_NULL:
		return "(NULL)";
	case STATE_IDLE:
		return "IDLE";
	case STATE_OUT_DIALING:
		return "OUT-DIALING";
	case STATE_OUT_VERIFY:
		return "OUT-VERIFY";
	case STATE_CALL:
		return "CALL";
	case STATE_CALL_DIALING:
		return "CALL-DIALING";
	case STATE_IN_PAGING:
		return "IN-PAGING";
	case STATE_RELEASED:
		return "RELEASED";
	}

	sprintf(invalid, "invalid(%d)", state);
	return invalid;
}

static void jolly_display_status(void)
{
	sender_t *sender;
	jolly_t *jolly;

	display_status_start();
	for (sender = sender_head; sender; sender = sender->next) {
		jolly = (jolly_t *) sender;
		display_status_channel(jolly->sender.kanal, NULL, jolly_state_name(jolly->state));
		if (jolly->station_id[0])
			display_status_subscriber(jolly->station_id, NULL);
	}
	display_status_end();
}

static void jolly_new_state(jolly_t *jolly, enum jolly_state new_state)
{
	if (jolly->state == new_state)
		return;
	LOGP_CHAN(DJOLLY, LOGL_DEBUG, "State change: %s -> %s\n", jolly_state_name(jolly->state), jolly_state_name(new_state));
	jolly->state = new_state;
	jolly_display_status();
}

static void jolly_timeout(void *data);
static void jolly_speech_timeout(void *data);
static void jolly_go_idle(jolly_t *jolly);

/* Create transceiver instance and link to a list. */
int jolly_create(const char *kanal, double dl_freq, double ul_freq, double step, const char *device, int use_sdr, int samplerate, double rx_gain, double tx_gain, int pre_emphasis, int de_emphasis, const char *write_rx_wave, const char *write_tx_wave, const char *read_rx_wave, const char *read_tx_wave, int loopback, double squelch_db, int nbfm, int repeater)
{
	jolly_t *jolly;
	int rc;

	jolly = calloc(1, sizeof(jolly_t));
	if (!jolly) {
		LOGP(DJOLLY, LOGL_ERROR, "No memory!\n");
		return -EIO;
	}

	LOGP(DJOLLY, LOGL_DEBUG, "Creating 'JollyCom' instance for 'Kanal' = %s (sample rate %d).\n", kanal, samplerate);

	dl_freq = dl_freq * 1e6 + step * 1e3 * (double)atoi(kanal);
	ul_freq = ul_freq * 1e6 + step * 1e3 * (double)atoi(kanal);

	/* init general part of transceiver */
	rc = sender_create(&jolly->sender, kanal, dl_freq, ul_freq, device, use_sdr, samplerate, rx_gain, tx_gain, pre_emphasis, de_emphasis, write_rx_wave, write_tx_wave, read_rx_wave, read_tx_wave, loopback, PAGING_SIGNAL_NONE);
	if (rc < 0) {
		LOGP(DJOLLY, LOGL_ERROR, "Failed to init 'Sender' processing!\n");
		goto error;
	}

	/* init audio processing */
	rc = dsp_init_sender(jolly, nbfm, squelch_db, repeater);
	if (rc < 0) {
		LOGP(DANETZ, LOGL_ERROR, "Failed to init signal processing!\n");
		goto error;
	}

	/* timers */
	osmo_timer_setup(&jolly->timer, jolly_timeout, jolly);
	osmo_timer_setup(&jolly->speech_timer, jolly_speech_timeout, jolly);

	/* go into idle state */
	jolly_go_idle(jolly);

	LOGP(DJOLLY, LOGL_NOTICE, "Created 'Kanal' #%s\n", kanal);

	return 0;

error:
	jolly_destroy(&jolly->sender);

	return rc;
}

/* Destroy transceiver instance and unlink from list. */
void jolly_destroy(sender_t *sender)
{
	jolly_t *jolly = (jolly_t *) sender;

	LOGP(DJOLLY, LOGL_DEBUG, "Destroying 'JollyCom' instance for 'Kanal' = %s.\n", sender->kanal);

	dsp_cleanup_sender(jolly);
	osmo_timer_del(&jolly->timer);
	osmo_timer_del(&jolly->speech_timer);
	sender_destroy(&jolly->sender);
	free(sender);
}

/* Abort connection towards mobile station changing to IDLE state */
static void jolly_go_idle(jolly_t *jolly)
{
	osmo_timer_del(&jolly->timer);
	osmo_timer_del(&jolly->speech_timer);
	reset_speech_string(jolly);

	LOGP(DJOLLY, LOGL_INFO, "Entering IDLE state on channel %s.\n", jolly->sender.kanal);
	jolly->dialing[0] = '\0';
	jolly->station_id[0] = '\0'; /* remove station ID before state change, so status is shown correctly */
	jolly_new_state(jolly, STATE_IDLE);
}

/* Release connection towards mobile station by sending idle tone for a while. */
static void jolly_release(jolly_t *jolly)
{
	osmo_timer_del(&jolly->timer);
	osmo_timer_del(&jolly->speech_timer);
	reset_speech_string(jolly);

	LOGP(DJOLLY, LOGL_INFO, "Sending Release sequence on channel %s.\n", jolly->sender.kanal);
	osmo_timer_schedule(&jolly->speech_timer, SPEECH_DELAY_RELEASE);
	jolly_new_state(jolly, STATE_RELEASED);
}

/* Enter paging state and transmit 4 paging tones. */
static void jolly_page(jolly_t *jolly, const char *dial_string)
{
	LOGP_CHAN(DJOLLY, LOGL_INFO, "Entering paging state, sending paging sequence to '%s'.\n", dial_string);
	/* set station ID before state change, so status is shown correctly */
	strncpy(jolly->station_id, dial_string, sizeof(jolly->station_id) - 1);
	osmo_timer_schedule(&jolly->timer, T_PAGING);
	osmo_timer_schedule(&jolly->speech_timer, SPEECH_DELAY_PAGING);
	jolly_new_state(jolly, STATE_IN_PAGING);
}

void speech_finished(jolly_t *jolly)
{
	LOGP(DJOLLY, LOGL_DEBUG, "speaking finished.\n");
	switch (jolly->state) {
	case STATE_OUT_VERIFY:
		osmo_timer_schedule(&jolly->timer, T_DIAL);
		break;
	case STATE_IN_PAGING:
		osmo_timer_schedule(&jolly->speech_timer, SPEECH_DELAY_PAGING);
		break;
	case STATE_RELEASED:
	 	jolly_go_idle(jolly);
		break;
	default:
		break;
	}
}

#define level2db(level)	(20 * log10(level))

/* A DTMF digit was received */
void jolly_receive_dtmf(void *priv, char digit, dtmf_meas_t *meas)
{
	jolly_t *jolly = (jolly_t *) priv;

	LOGP_CHAN(DJOLLY, LOGL_INFO, "Received dtmf digit '%c'  frequency %.1f %.1f  amplitude %.1f %.1f dB.\n",
		digit,
		meas->frequency_low, meas->frequency_high,
		level2db(meas->amplitude_low), level2db(meas->amplitude_high));

	/* update measurements */
	display_measurements_update(jolly->dmp_dtmf_low, level2db(meas->amplitude_low), 0.0);
	display_measurements_update(jolly->dmp_dtmf_high, level2db(meas->amplitude_high), 0.0);

	switch (jolly->state) {
	case STATE_IDLE:
		if (digit == '*') {
			LOGP_CHAN(DJOLLY, LOGL_INFO, "Received start digit, entering dialing state.\n");
			jolly->dialing[0] = '\0';
			osmo_timer_schedule(&jolly->timer, T_DIAL);
			jolly_new_state(jolly, STATE_OUT_DIALING);
			break;
		}
		break;
	case STATE_OUT_DIALING:
		if (digit == '*') {
			LOGP_CHAN(DJOLLY, LOGL_INFO, "Received start digit again, resetting dialing state.\n");
			jolly->dialing[0] = '\0';
			osmo_timer_schedule(&jolly->timer, T_DIAL);
			break;
		}
		if (digit >= '0' && digit <= '9' && strlen(jolly->dialing) < sizeof(jolly->dialing) - 1) {
			LOGP_CHAN(DJOLLY, LOGL_INFO, "Received dialed digit '%c'\n", digit);
			jolly->dialing[strlen(jolly->dialing) + 1] = '\0';
			jolly->dialing[strlen(jolly->dialing)] = digit;
			osmo_timer_schedule(&jolly->timer, T_DIAL);
			break;
		}
		if (digit == '#') {
			osmo_timer_del(&jolly->timer);
			if (!jolly->dialing[0]) {
				LOGP_CHAN(DJOLLY, LOGL_INFO, "Received stop digit but no dial string, entering idle state.\n");
				jolly_go_idle(jolly);
				break;
			}
			LOGP_CHAN(DJOLLY, LOGL_INFO, "Received stop digit, entering verify state.\n");
			osmo_timer_schedule(&jolly->speech_timer, SPEECH_DELAY_VERIFY);
			jolly_new_state(jolly, STATE_OUT_VERIFY);
			break;
		}
		break;
	case STATE_OUT_VERIFY:
		if (digit == '*') {
			LOGP_CHAN(DJOLLY, LOGL_INFO, "Received start digit, entering dialing state.\n");
			reset_speech_string(jolly);
			jolly->dialing[0] = '\0';
			osmo_timer_schedule(&jolly->timer, T_DIAL);
			jolly_new_state(jolly, STATE_OUT_DIALING);
			break;
		}
		if (digit == '#') {
			LOGP_CHAN(DJOLLY, LOGL_INFO, "Received ack digit, entering call state.\n");
			osmo_timer_del(&jolly->timer);
			jolly->callref = call_up_setup(NULL, jolly->dialing, OSMO_CC_NETWORK_JOLLYCOM_NONE, "");
			jolly_new_state(jolly, STATE_CALL);
		}
		break;
	case STATE_CALL:
		if (digit == '*') {
			LOGP_CHAN(DJOLLY, LOGL_INFO, "Received start digit, entering call dialing state.\n");
			jolly->dialing[0] = '\0';
			osmo_timer_schedule(&jolly->timer, T_DIAL2);
			jolly_new_state(jolly, STATE_CALL_DIALING);
			break;
		}
		break;
	case STATE_CALL_DIALING:
		if (digit == '#') {
			LOGP_CHAN(DJOLLY, LOGL_INFO, "Received stop digit, going idle.\n");
			call_up_release(jolly->callref, CAUSE_NORMAL);
			jolly->callref = 0;
			jolly_release(jolly);
			break;
		}
		break;
	case STATE_IN_PAGING:
		if (digit == '#') {
			LOGP_CHAN(DJOLLY, LOGL_INFO, "Received answer digit, entering call state.\n");
			call_up_answer(jolly->callref, jolly->station_id);
			jolly_new_state(jolly, STATE_CALL);
			break;
		}
		break;
	default:
		break;
	}
}

/* Timeout handling */
static void jolly_timeout(void *data)
{
	jolly_t *jolly = data;

	switch (jolly->state) {
	case STATE_OUT_DIALING:
		LOGP_CHAN(DJOLLY, LOGL_NOTICE, "Timeout while dialing, going idle.\n");
	 	jolly_go_idle(jolly);
		break;
	case STATE_OUT_VERIFY:
		LOGP_CHAN(DJOLLY, LOGL_NOTICE, "Timeout while verifying, going idle.\n");
	 	jolly_go_idle(jolly);
		break;
	case STATE_CALL_DIALING:
		LOGP_CHAN(DJOLLY, LOGL_NOTICE, "Timeout while dialing during call.\n");
		jolly_new_state(jolly, STATE_CALL);
		break;
	case STATE_IN_PAGING:
		LOGP_CHAN(DJOLLY, LOGL_NOTICE, "Timeout while paging, going idle.\n");
		call_up_release(jolly->callref, CAUSE_NOANSWER);
		jolly->callref = 0;
	 	jolly_go_idle(jolly);
		break;
	default:
		break;
	}
}

static void jolly_speech_timeout(void *data)
{
	jolly_t *jolly = data;

	switch (jolly->state) {
	case STATE_OUT_VERIFY:
		LOGP_CHAN(DJOLLY, LOGL_DEBUG, "Start verifying speech.\n");
		set_speech_string(jolly, 'o', jolly->dialing);
		break;
	case STATE_IN_PAGING:
		LOGP_CHAN(DJOLLY, LOGL_DEBUG, "Start paging speech.\n");
		set_speech_string(jolly, 'i', jolly->station_id);
		break;
	case STATE_RELEASED:
		LOGP_CHAN(DJOLLY, LOGL_DEBUG, "Start release speech.\n");
		set_speech_string(jolly, 'r', "");
	default:
		break;
	}
}

/* Call control starts call towards mobile station. */
int call_down_setup(int callref, const char __attribute__((unused)) *caller_id, enum number_type __attribute__((unused)) caller_type, const char *dialing)
{
	sender_t *sender;
	jolly_t *jolly;

	/* 1. check if given number is already in a call, return BUSY */
	for (sender = sender_head; sender; sender = sender->next) {
		jolly = (jolly_t *) sender;
		if (!strcmp(jolly->station_id, dialing))
			break;
	}
	if (sender) {
		LOGP(DJOLLY, LOGL_NOTICE, "Outgoing call to busy number, rejecting!\n");
		return -CAUSE_BUSY;
	}

	/* 2. check if all senders are busy, return NOCHANNEL */
	for (sender = sender_head; sender; sender = sender->next) {
		jolly = (jolly_t *) sender;
		if (jolly->state == STATE_IDLE)
			break;
	}
	if (!sender) {
		LOGP(DJOLLY, LOGL_NOTICE, "Outgoing call, but no free channel, rejecting!\n");
		return -CAUSE_NOCHANNEL;
	}

	LOGP_CHAN(DJOLLY, LOGL_INFO, "Call to mobile station.\n");

	/* 3. trying to page mobile station */
	jolly->callref = callref;
	jolly_page(jolly, dialing);

	call_up_alerting(callref);

	return 0;
}

void call_down_answer(int __attribute__((unused)) callref, struct timeval __attribute__((unused)) *tv_meter)
{
}

/* Call control sends disconnect (with tones).
 * An active call stays active, so tones and annoucements can be received
 * by mobile station.
 */
void call_down_disconnect(int callref, int cause)
{
	sender_t *sender;
	jolly_t *jolly;

	LOGP(DJOLLY, LOGL_INFO, "Call has been disconnected by network.\n");

	for (sender = sender_head; sender; sender = sender->next) {
		jolly = (jolly_t *) sender;
		if (jolly->callref == callref)
			break;
	}
	if (!sender) {
		LOGP(DJOLLY, LOGL_NOTICE, "Outgoing disconnect, but no callref!\n");
		call_up_release(callref, CAUSE_INVALCALLREF);
		return;
	}

	/* Release when not active */
	if (jolly->state == STATE_CALL || jolly->state == STATE_CALL_DIALING)
		return;
	switch (jolly->state) {
	case STATE_IN_PAGING:
		LOGP_CHAN(DJOLLY, LOGL_NOTICE, "Outgoing disconnect, during paging, releaseing.\n");
	 	jolly_go_idle(jolly);
		break;
	default:
		break;
	}

	call_up_release(callref, cause);

	jolly->callref = 0;

}

/* Call control releases call toward mobile station. */
void call_down_release(int callref, __attribute__((unused)) int cause)
{
	sender_t *sender;
	jolly_t *jolly;

	LOGP(DJOLLY, LOGL_INFO, "Call has been released by network, releasing call.\n");

	for (sender = sender_head; sender; sender = sender->next) {
		jolly = (jolly_t *) sender;
		if (jolly->callref == callref)
			break;
	}
	if (!sender) {
		LOGP(DJOLLY, LOGL_NOTICE, "Outgoing release, but no callref!\n");
		/* don't send release, because caller already released */
		return;
	}

	jolly->callref = 0;

	switch (jolly->state) {
	case STATE_CALL:
	case STATE_CALL_DIALING:
		LOGP_CHAN(DJOLLY, LOGL_NOTICE, "Outgoing release, during call, releasing\n");
	 	jolly_release(jolly);
		break;
	case STATE_IN_PAGING:
		LOGP_CHAN(DJOLLY, LOGL_NOTICE, "Outgoing release, during paging, releaseing.\n");
	 	jolly_go_idle(jolly);
		break;
	default:
		break;
	}
}

void dump_info(void) {}

