/* C-Netz audio processing
 *
 * (C) 2016 by Andreas Eversberg <jolly@eversberg.eu>
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

#define CHAN cnetz->sender.kanal

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include "../libsample/sample.h"
#include "../liblogging/logging.h"
#include "../libmobile/call.h"
#include "../libmobile/get_time.h"
#include "cnetz.h"
#include "sysinfo.h"
#include "telegramm.h"
#include "dsp.h"

/* test function to mirror received audio from ratio back to radio */
//#define TEST_SCRAMBLE
/* test the audio quality after cascading two scramblers (TEST_SCRAMBLE must be defined) */
//#define TEST_UNSCRAMBLE

#define PI		M_PI

#define MAX_DEVIATION	4000.0
#define MAX_MODULATION	3000.0
#define FSK_DEVIATION	(2500.0 / speech_deviation) /* no emphasis */
#define MAX_DISPLAY	1.4	/* something above speech level, no emphasis */
#define BITRATE		5280.0	/* bits per second */
#define BLOCK_BITS	198	/* duration of one time slot including pause at beginning and end */

#ifdef TEST_SCRAMBLE
test_echo_t scrambler_test_echo = {};
scrambler_t scrambler_test_scrambler1;
scrambler_t scrambler_test_scrambler2;
#endif

static const char *cnetz_dsp_mode_name(enum dsp_mode mode)
{
        static char invalid[16];

	switch (mode) {
        case DSP_SCHED_NONE:
		return "SCHED_NONE";
	case DSP_MODE_OFF:
		return "OFF";
	case DSP_MODE_OGK:
		return "OGK";
	case DSP_MODE_SPK_K:
		return "SPK_K";
	case DSP_MODE_SPK_V:
		return "SPK_V";
	}

	sprintf(invalid, "invalid(%d)", mode);
	return invalid;
}

void dsp_init(void)
{
	compandor_init();
}

static void dsp_init_ramp(cnetz_t *cnetz)
{
	double c;
        int i;

	LOGP(DDSP, LOGL_DEBUG, "Generating smooth ramp table.\n");
	for (i = 0; i < 256; i++) {
		/* Use raised cosine ramp. */
		c = cos((double)i / 256.0 * PI);
		cnetz->fsk_ramp_down[i] = c * (double)cnetz->fsk_deviation;
		cnetz->fsk_ramp_up[i] = -cnetz->fsk_ramp_down[i];
	}
}

/* Init transceiver instance. */
int dsp_init_sender(cnetz_t *cnetz, int measure_speed, double clock_speed[2], enum demod_type demod, double speech_deviation)
{
	int rc = 0;
	double size;

	LOGP_CHAN(DDSP, LOGL_DEBUG, "Init FSK for 'Sender'.\n");

	/* set modulation parameters */
	sender_set_fm(&cnetz->sender, MAX_DEVIATION, MAX_MODULATION, speech_deviation, MAX_DISPLAY);

	if (measure_speed) {
		cnetz->measure_speed = measure_speed;
		cant_recover = 1;
	}

	if (clock_speed[0] > 1000 || clock_speed[0] < -1000 || clock_speed[1] > 1000 || clock_speed[1] < -1000) {
		LOGP_CHAN(DDSP, LOGL_ERROR, "Clock speed %.1f,%.1f ppm out of range! Please use range between +-1000 ppm!\n", clock_speed[0], clock_speed[1]);
		return -EINVAL;
	}
	LOGP_CHAN(DDSP, LOGL_INFO, "Using clock speed of %.1f ppm (RX) and %.1f ppm (TX) to correct sound card's clock.\n", clock_speed[0], clock_speed[1]);

	cnetz->fsk_bitduration = (double)cnetz->sender.samplerate / ((double)BITRATE / (1.0 + clock_speed[1] / 1000000.0));
	cnetz->fsk_tx_bitstep = 1.0 / cnetz->fsk_bitduration;
	LOGP_CHAN(DDSP, LOGL_DEBUG, "Use %.4f samples for one bit duration @ %d.\n", cnetz->fsk_bitduration, cnetz->sender.samplerate);

	size = cnetz->fsk_bitduration * (double)BLOCK_BITS * 16.0; /* 16 blocks for distributed frames */
	cnetz->fsk_tx_buffer_size = size * 1.1; /* more to compensate clock speed */
	cnetz->fsk_tx_buffer = calloc(sizeof(sample_t), cnetz->fsk_tx_buffer_size);
	if (!cnetz->fsk_tx_buffer) {
		LOGP_CHAN(DDSP, LOGL_ERROR, "No memory!\n");
		rc = -ENOMEM;
		goto error;
	}

	/* create deviation and ramp */
	cnetz->fsk_deviation = FSK_DEVIATION;
	dsp_init_ramp(cnetz);

	/* init low pass filter for received signal */
	iir_lowpass_init(&cnetz->lp, MAX_MODULATION, cnetz->sender.samplerate, 2);

	/* create speech buffer */
	cnetz->dsp_speech_buffer = calloc(sizeof(sample_t), (int)(cnetz->fsk_bitduration * 70.0)); /* more to compensate clock speed. we just need it to fill 62 bits (60 bits, including pause bits). */
	if (!cnetz->dsp_speech_buffer) {
		LOGP_CHAN(DDSP, LOGL_ERROR, "No memory!\n");
		rc = -ENOMEM;
		goto error;
	}

	/* reinit the sample rate to shrink/expand audio */
	init_samplerate(&cnetz->sender.srstate, 8000.0, (double)cnetz->sender.samplerate / (1.1 / (1.0 + clock_speed[0] / 1000000.0)), 3300.0); /* 66 <-> 60 */

	rc = fsk_fm_init(&cnetz->fsk_demod, cnetz, cnetz->sender.samplerate, (double)BITRATE / (1.0 + clock_speed[0] / 1000000.0), demod);
	if (rc < 0)
		goto error;

	/* init scrambler for shrinked audio */
	scrambler_setup(&cnetz->scrambler_tx, (double)cnetz->sender.samplerate / 1.1); 
	scrambler_setup(&cnetz->scrambler_rx, (double)cnetz->sender.samplerate / 1.1);

	/* init compandor, according to C-Netz specs, attack and recovery time
	 * shall not exceed according to ITU G.162 */
	setup_compandor(&cnetz->cstate, 8000, 5.0, 22.5);

	/* use duration of one bit to ramp level of last frame to current frame */
	cnetz->offset_range = ceil(cnetz->fsk_bitduration);

#ifdef TEST_SCRAMBLE
	rc = test_echo_alloc(&scrambler_test_echo, cnetz->sender.samplerate / 20);
	if (rc < 0) {
		LOGP_CHAN(DDSP, LOGL_ERROR, "Failed to init echo buffer for scrambler test!\n");
		exit(0);
	}
	scrambler_setup(&scrambler_test_scrambler1, cnetz->sender.samplerate);
	scrambler_setup(&scrambler_test_scrambler2, cnetz->sender.samplerate);
#endif

	cnetz->sched_dsp_mode_ts = -1;

	return 0;

error:
	dsp_cleanup_sender(cnetz);

	return rc;
}

void dsp_cleanup_sender(cnetz_t *cnetz)
{
        LOGP_CHAN(DDSP, LOGL_DEBUG, "Cleanup FSK for 'Sender'.\n");

	if (cnetz->fsk_tx_buffer) {
		free(cnetz->fsk_tx_buffer);
		cnetz->fsk_tx_buffer = NULL;
	}
	if (cnetz->dsp_speech_buffer) {
		free(cnetz->dsp_speech_buffer);
		cnetz->dsp_speech_buffer = NULL;
	}

	fsk_fm_exit(&cnetz->fsk_demod);
}

/* receive sample time and calculate speed against system clock
 * tx: indicates transmit stream
 * result: if set the actual signal speed is used (instead of sample rate) */
void calc_clock_speed(cnetz_t *cnetz, double samples, int tx, int result)
{
	struct clock_speed *cs = &cnetz->clock_speed;
	double ti;
	double speed_ppm_avg[4];
	int index = (result << 1) | tx;
	int i;

	if (!cnetz->measure_speed)
		return;

	ti = get_time();

	/* skip some time to avoid false measurement due to filling of buffers */
	if (cs->meas_ti == 0.0) {
		cs->meas_ti = ti + 1.0;
		return;
	}
	if (cs->meas_ti > ti)
		return;

	/* start sample counting */
	if (cs->start_ti[index] == 0.0) {
		cs->start_ti[index] = ti;
		cs->spl_count[index] = 0.0;
		return;
	}

	/* add elapsed time */
	cs->last_ti[index] = ti;
	cs->spl_count[index] += samples;

	/* only calculate speed, if one second has elapsed */
	if (ti - cs->meas_ti <= 2.0)
		return;
	cs->meas_ti += 2.0;

	/* add to ring buffer */
	if (!cs->spl_count[0] || !cs->spl_count[1] || !cs->spl_count[2] || !cs->spl_count[3])
		return;

	for (index = 0; index < 4; index++) {
		cs->speed_ppm[index][cs->idx[index] & 0xff] = (cs->spl_count[index] / (double)cnetz->sender.samplerate) / (cs->last_ti[index] - cs->start_ti[index]) * 1000000.0 - 1000000.0;
		cs->idx[index]++;
		if (cs->num[index]++ > 30)
			cs->num[index] = 30;
		speed_ppm_avg[index] = 0.0;
		for (i = 0; i < cs->num[index]; i++)
			speed_ppm_avg[index] += cs->speed_ppm[index][(cs->idx[index] - i - 1) & 0xff];
		speed_ppm_avg[index] /= (double)cs->num[index];
	}
	LOGP_CHAN(DDSP, LOGL_NOTICE, "Clock: RX=%.3f TX=%.3f; Signal: RX=%.3f TX=%.3f ppm\n", speed_ppm_avg[0], speed_ppm_avg[1], speed_ppm_avg[2], speed_ppm_avg[3]);
}

static int fsk_nothing_encode(cnetz_t *cnetz)
{
	sample_t *spl;
	double phase, bitstep;
	int i, count;

	spl = cnetz->fsk_tx_buffer;
	phase = cnetz->fsk_tx_phase;
	bitstep = cnetz->fsk_tx_bitstep * 256.0;

	/* add 198 bits of no power (silence) */
	for (i = 0; i < 198; i++) {
		do {
			*spl++ = -10.0; /* marker for power off */
			phase += bitstep;
		} while (phase < 256.0);
		phase -= 256.0;
	}

	/* depending on the number of samples, return the number */
	count = ((uintptr_t)spl - (uintptr_t)cnetz->fsk_tx_buffer) / sizeof(*spl);

	cnetz->fsk_tx_phase = phase;
	cnetz->fsk_tx_buffer_length = count;

	return count;
}

/* encode one data block into samples
 * input: 184 data bits (including barker code)
 * output: samples
 * return number of samples */
static int fsk_block_encode(cnetz_t *cnetz, const char *bits, int ogk)
{
	/* alloc samples, add 1 in case there is a rest */
	sample_t *spl;
	double phase, bitstep, deviation;
	int i, count;
	char last;

	deviation = cnetz->fsk_deviation;
	spl = cnetz->fsk_tx_buffer;
	phase = cnetz->fsk_tx_phase;
	bitstep = cnetz->fsk_tx_bitstep * 256.0;

	/* add 7 bits of pause */
	for (i = 0; i < 7; i++) {
		do {
			*spl++ = 0.0;
			phase += bitstep;
		} while (phase < 256.0);
		phase -= 256.0;
	}
	/* add 184 bits */
	last = ' ';
	for (i = 0; i < 184; i++) {
		switch (last) {
		case ' ':
			if (bits[i] == '1') {
				/* ramp up from 0 */
				do {
					*spl++ = cnetz->fsk_ramp_up[(uint8_t)phase] / 2 + deviation / 2;
					phase += bitstep;
				} while (phase < 256.0);
				phase -= 256.0;
			} else {
				/* ramp down from 0 */
				do {
					*spl++ = cnetz->fsk_ramp_down[(uint8_t)phase] / 2 - deviation / 2;
					phase += bitstep;
				} while (phase < 256.0);
				phase -= 256.0;
			}
			break;
		case '1':
			if (bits[i] == '1') {
				/* stay up */
				do {
					*spl++ = deviation;
					phase += bitstep;
				} while (phase < 256.0);
				phase -= 256.0;
			} else {
				/* ramp down */
				do {
					*spl++ = cnetz->fsk_ramp_down[(uint8_t)phase];
					phase += bitstep;
				} while (phase < 256.0);
				phase -= 256.0;
			}
			break;
		case '0':
			if (bits[i] == '1') {
				/* ramp up */
				do {
					*spl++ = cnetz->fsk_ramp_up[(uint8_t)phase];
					phase += bitstep;
				} while (phase < 256.0);
				phase -= 256.0;
			} else {
				/* stay down */
				do {
					*spl++ = -deviation;
					phase += bitstep;
				} while (phase < 256.0);
				phase -= 256.0;
			}
			break;
		}
		last = bits[i];
	}
	/* add 7 bits of pause */
	if (last == '0') {
		/* ramp up to 0 */
		do {
			*spl++ = cnetz->fsk_ramp_up[(uint8_t)phase] / 2 - deviation / 2;
			phase += bitstep;
		} while (phase < 256.0);
		phase -= 256.0;
	} else {
		/* ramp down to 0 */
		do {
			*spl++ = cnetz->fsk_ramp_down[(uint8_t)phase] / 2 + deviation / 2;
			phase += bitstep;
		} while (phase < 256.0);
		phase -= 256.0;
	}
	for (i = 1; i < 7; i++) {
		/* turn off power for OgK */
		if (ogk) {
			do {
				*spl++ = -10.0; /* marker for power off */
				phase += bitstep;
			} while (phase < 256.0);
		} else {
			do {
				*spl++ = 0.0;
				phase += bitstep;
			} while (phase < 256.0);
		}
		phase -= 256.0;
	}

	/* depending on the number of samples, return the number */
	count = ((uintptr_t)spl - (uintptr_t)cnetz->fsk_tx_buffer) / sizeof(*spl);

	cnetz->fsk_tx_phase = phase;
	cnetz->fsk_tx_buffer_length = count;

	return count;
}

/* encode one distributed data block into samples
 * input: 184 data bits (including barker code)
 * output: samples
 * 	if a sample contains a marker, it indicates where to insert speech block
 * return number of samples
 *
 * the marker is placed in the middle of the 6th bit.
 * because we have a transition (ramp) in the middle of each bit.
 * the phone will see the position of the marker as start of the 6th bit.
 * the marker marks the point where the speech is ramped up, so the phone
 * will see the speech completely ramped up after the 6th bit
 */
static int fsk_distributed_encode(cnetz_t *cnetz, const char *bits)
{
	/* alloc samples, add 1 in case there is a rest */
	sample_t *spl, *marker;
	double phase, bitstep, deviation;
	int i, j, count;
	char last;

	deviation = cnetz->fsk_deviation;
	spl = cnetz->fsk_tx_buffer;
	phase = cnetz->fsk_tx_phase;
	bitstep = cnetz->fsk_tx_bitstep * 256.0;

	/* add 2 * (1+4+1 + 60) bits of pause / for speech */
	for (i = 0; i < 2; i++) {
		for (j = 0; j < 6; j++) {
			do {
				*spl++ = 0;
				phase += bitstep;
			} while (phase < 256.0);
			phase -= 256.0;
		}
		marker = spl - (int)(cnetz->fsk_bitduration / 2.0); /* in the middle of the 6th bit */
		for (j = 0; j < 60; j++) {
			do {
				*spl++ = 0;
				phase += bitstep;
			} while (phase < 256.0);
			phase -= 256.0;
		}
		*marker += 10.0; /* marker for inserting speech */
	}
	/* add 46 * (1+4+1 + 60) bits */
	for (i = 0; i < 46; i++) {
		/* unmodulated bit */
		do {
			*spl++ = 0;
			phase += bitstep;
		} while (phase < 256.0);
		phase -= 256.0;
		last = ' ';
		for (j = 0; j < 4; j++) {
			switch (last) {
			case ' ':
				if (bits[i * 4 + j] == '1') {
					/* ramp up from 0 */
					do {
						*spl++ = cnetz->fsk_ramp_up[(uint8_t)phase] / 2 + deviation / 2;
						phase += bitstep;
					} while (phase < 256.0);
					phase -= 256.0;
				} else {
					/* ramp down from 0 */
					do {
						*spl++ = cnetz->fsk_ramp_down[(uint8_t)phase] / 2 - deviation / 2;
						phase += bitstep;
					} while (phase < 256.0);
					phase -= 256.0;
				}
				break;
			case '1':
				if (bits[i * 4 + j] == '1') {
					/* stay up */
					do {
						*spl++ = deviation;
						phase += bitstep;
					} while (phase < 256.0);
					phase -= 256.0;
				} else {
					/* ramp down */
					do {
						*spl++ = cnetz->fsk_ramp_down[(uint8_t)phase];
						phase += bitstep;
					} while (phase < 256.0);
					phase -= 256.0;
				}
				break;
			case '0':
				if (bits[i * 4 + j] == '1') {
					/* ramp up */
					do {
						*spl++ = cnetz->fsk_ramp_up[(uint8_t)phase];
						phase += bitstep;
					} while (phase < 256.0);
					phase -= 256.0;
				} else {
					/* stay down */
					do {
						*spl++ = -deviation;
						phase += bitstep;
					} while (phase < 256.0);
					phase -= 256.0;
				}
				break;
			}
			last = bits[i * 4 + j];
		}
		/* ramp down */
		if (last == '0') {
			/* ramp up to 0 */
			do {
				*spl++ = cnetz->fsk_ramp_up[(uint8_t)phase] / 2 - deviation / 2;
				phase += bitstep;
			} while (phase < 256.0);
			phase -= 256.0;
		} else {
			/* ramp down to 0 */
			do {
				*spl++ = cnetz->fsk_ramp_down[(uint8_t)phase] / 2 + deviation / 2;
				phase += bitstep;
			} while (phase < 256.0);
			phase -= 256.0;
		}
		marker = spl - (int)(cnetz->fsk_bitduration / 2.0); /* in the middle of the 6th bit */
		for (j = 0; j < 60; j++) {
			do {
				*spl++ = 0;
				phase += bitstep;
			} while (phase < 256.0);
			phase -= 256.0;
		}
		*marker += 10.0; /* marker for inserting speech */
	}

	/* depending on the number of samples, return the number */
	count = ((uintptr_t)spl - (uintptr_t)cnetz->fsk_tx_buffer) / sizeof(*spl);

	cnetz->fsk_tx_phase = phase;
	cnetz->fsk_tx_buffer_length = count;

	return count;
}

/* decode samples and hut for bit changes
 * use deviation to find greatest slope of the signal (bit change)
 */
void sender_receive(sender_t *sender, sample_t *samples, int length, double rf_level_db)
{
	cnetz_t *cnetz = (cnetz_t *) sender;

	cnetz->rf_level_db = rf_level_db;

	/* measure rx sample speed */
	calc_clock_speed(cnetz, length, 0, 0);

#ifdef TEST_SCRAMBLE
#ifdef TEST_UNSCRAMBLE
	scrambler(&scrambler_test_scrambler1, samples, length);
#endif
	test_echo_store(&scrambler_test_echo, samples, length);
	return;
#endif

	if (cnetz->dsp_mode != DSP_MODE_OFF) {
		iir_process(&cnetz->lp, samples, length);
		fsk_fm_demod(&cnetz->fsk_demod, samples, length); /* process */
	} else
		fsk_fm_demod(&cnetz->fsk_demod, NULL, length); /* just count bit time */
	return;
}

/* shrink audio segment from 12.5 ms to the duration of 60 bits */
static int shrink_speech(cnetz_t *cnetz, sample_t *speech_buffer)
{
	int speech_length;
	int16_t spl[100];

	jitter_load_samples(&cnetz->sender.dejitter, (uint8_t *)spl, 100, sizeof(*spl), jitter_conceal_s16, NULL);
	int16_to_samples_speech(speech_buffer, spl, 100);
	/* 1. compress dynamics */
	compress_audio(&cnetz->cstate, speech_buffer, 100);
	/* 2. upsample */
	speech_length = samplerate_upsample_output_num(&cnetz->sender.srstate, 100);
	samplerate_upsample(&cnetz->sender.srstate, speech_buffer, 100, speech_buffer, speech_length);
	/* 3. scramble */
	if (cnetz->scrambler)
		scrambler(&cnetz->scrambler_tx, speech_buffer, speech_length);
	/* 4. pre-emphasis is done by cnetz code, not by common code */
	/* pre-emphasis is only used when scrambler is off, see FTZ 171 TR 60 Clause 4 */
	if (cnetz->pre_emphasis && !cnetz->scrambler)
		pre_emphasis(&cnetz->estate, speech_buffer, speech_length);

	return speech_length;
}

static int fsk_telegramm(cnetz_t *cnetz, sample_t *samples, uint8_t *power, int length)
{
	int count = 0, pos, copy, i, speech_length, speech_pos;
	sample_t *spl, *speech_buffer;
	const char *bits;

	speech_buffer = cnetz->dsp_speech_buffer;
	speech_length = cnetz->dsp_speech_length;
	speech_pos = cnetz->dsp_speech_pos;

again:
	/* there must be length, otherwise we would skip blocks */
	if (count == length)
		return count;

	pos = cnetz->fsk_tx_buffer_pos;
	spl = cnetz->fsk_tx_buffer + pos;

	/* start new telegramm, so we generate one */
	if (pos == 0) {
		/* a new super frame starts */
		if (cnetz->sched_ts == 0 && cnetz->sched_r_m == 0) {
			/* measure actual signal speed */
			calc_clock_speed(cnetz, (double)cnetz->sender.samplerate * 2.4, 1, 1);
			/* sync TX (might not be required, if there is no error in math calculation) */
			if (!cnetz->sender.master) { /* if no link to a master, we are master */
				/* we are master, so we store sample count and phase */
				cnetz->frame_last_scount = cnetz->fsk_tx_scount;
				cnetz->frame_last_phase = cnetz->fsk_tx_phase;
			} else {
				/* we are slave, so we sync to phase */
				cnetz_t *master = (cnetz_t *)cnetz->sender.master;
				/* it may happen that the sample count does not match with the master,
				 * because one has a phase wrap before and the other after a sample.
				 * then we do it next super frame cycle */
				if (master->frame_last_scount == cnetz->fsk_tx_scount) {
					LOGP(DDSP, LOGL_DEBUG, "Sync phase of slave to master: master=%.15f, slave=%.15f, diff=%.15f\n", master->frame_last_phase, cnetz->fsk_tx_phase, master->frame_last_phase - cnetz->fsk_tx_phase);
					cnetz->fsk_tx_phase = master->frame_last_phase;
				} else {
					LOGP(DDSP, LOGL_DEBUG, "Not sync phase of slave to master: Sample counts during frame change are different, ignoring this time!\n");
				}
			}
		}

		/* switch to speech channel */
		if (cnetz->sched_dsp_mode_ts >= 0 && cnetz->sched_r_m == 0) {
			if (cnetz->sched_dsp_mode_ts == cnetz->sched_ts) {
				/* OgK / SpK(K) / SpK(V) */
				LOGP_CHAN(DDSP, LOGL_INFO, "Now switching channel mode to %s at timeslot %d\n", cnetz_dsp_mode_name(cnetz->sched_dsp_mode), cnetz->sched_dsp_mode_ts);
				cnetz->sched_dsp_mode_ts = -1;
				cnetz_set_dsp_mode(cnetz, cnetz->sched_dsp_mode);
			}
		}

		switch (cnetz->dsp_mode) {
		case DSP_MODE_OGK:
			if (((si.timeslots >> cnetz->sched_ts) & 1)) {
				if (cnetz->sched_r_m == 0) {
					/* if automatic polarity selection is used, toggle between two polarities (every transmitted slot) until a response is received then continue to use the time slots of that polarity */
					if (cnetz->auto_polarity)
						cnetz->negative_polarity = (cnetz->sched_ts & 7) >> 2;
					/* set last time slot, so we know to which time slot the message from mobile station belongs to */
					cnetz->sched_last_ts = cnetz->sched_ts;
					bits = cnetz_encode_telegramm(cnetz);
					if (bits) {
						LOGP_CHAN(DDSP, LOGL_DEBUG, "Transmitting 'Rufblock' at timeslot %d\n", cnetz->sched_ts);
						fsk_block_encode(cnetz, bits, 1);
					} else
						fsk_nothing_encode(cnetz);
				} else {
					bits = cnetz_encode_telegramm(cnetz);
					if (bits) {
						LOGP_CHAN(DDSP, LOGL_DEBUG, "Transmitting 'Meldeblock' at timeslot %d\n", cnetz->sched_ts);
						fsk_block_encode(cnetz, bits, 1);
					} else
						fsk_nothing_encode(cnetz);
				}
			} else {
				fsk_nothing_encode(cnetz);
			}
			break;
		case DSP_MODE_SPK_K:
			bits = cnetz_encode_telegramm(cnetz);
			if (bits) {
				LOGP_CHAN(DDSP, LOGL_DEBUG, "Transmitting 'Konzentrierte Signalisierung' at timeslot %d.%d\n", cnetz->sched_ts, cnetz->sched_r_m * 5);
				fsk_block_encode(cnetz, bits, 0);
			} else
				fsk_nothing_encode(cnetz);
			break;
		case DSP_MODE_SPK_V:
			bits = cnetz_encode_telegramm(cnetz);
			if (bits) {
				LOGP_CHAN(DDSP, LOGL_DEBUG, "Transmitting 'Verteilte Signalisierung' starting at timeslot %d\n", cnetz->sched_ts);
				fsk_distributed_encode(cnetz, bits);
			} else
				fsk_nothing_encode(cnetz);
			break;
		case DSP_MODE_OFF:
		default:
			fsk_nothing_encode(cnetz);
		}

		if (cnetz->dsp_mode == DSP_MODE_SPK_V) {
			/* count sub frame */
			cnetz->sched_ts += 8;
		} else {
			/* count slot */
			if (cnetz->sched_r_m == 0)
				cnetz->sched_r_m = 1;
			else {
				cnetz->sched_r_m = 0;
				cnetz->sched_ts++;
			}
		}
		if (cnetz->sched_ts == 32)
			cnetz->sched_ts = 0;
	}

	copy = cnetz->fsk_tx_buffer_length - pos;
	if (length - count < copy)
		copy = length - count;
	for (i = 0; i < copy; i++) {
		if (*spl > 5.0) { /* speech marker found */
			int begin, end, j;
			/* correct marker (not the best way) */
			*spl -= 10.0;
			begin = (int)cnetz->fsk_bitduration;
			end = (int)(cnetz->fsk_bitduration * 61.0);
			/* get audio */
			speech_length = shrink_speech(cnetz, speech_buffer + begin);
			/* ramp before speech */
			for (j = 0; j < begin; j++) {
				/* ramp up from 0 to speech level */
				speech_buffer[j] = speech_buffer[begin] * (cnetz->fsk_ramp_up[j * 256 / begin] / cnetz->fsk_deviation / 2.0 + 0.5);
			}
			speech_length += begin; /* add one bit duration before speech*/
			/* ramp after speech */
			while (speech_length < end) {
				speech_buffer[speech_length] = speech_buffer[speech_length - 1];
				speech_length++;
			}
			speech_length = end; /* shorten 'speech_length', if greater than 'end' */
			for (j = 0; j < begin; j++) {
				/* ramp down from speech level to 0 */
				speech_buffer[end + j] = speech_buffer[end - 1] * (cnetz->fsk_ramp_down[j * 256 / begin] / cnetz->fsk_deviation / 2.0 + 0.5);
			}
			speech_length += begin; /* add one bit duration after speech */
			speech_pos = 0;
		}
		if (*spl < -5.0) { /* power off marker found */
			/* correct marker (not the best way) */
			*spl += 10.0;
			*power = 0;
		} else
			*power = 1;
		/* add speech as long as we have something left in buffer */
		if (speech_pos < speech_length)
			*samples++ = *spl + speech_buffer[speech_pos++];
		else
			*samples++ = *spl;
		spl++;
		power++;
	}
	cnetz->dsp_speech_length = speech_length;
	cnetz->dsp_speech_pos = speech_pos;
	cnetz->fsk_tx_scount += copy;
	pos += copy;
	count += copy;
	if (pos == cnetz->fsk_tx_buffer_length) {
		cnetz->fsk_tx_buffer_pos = 0;
		goto again;
	}

	cnetz->fsk_tx_buffer_pos = pos;

	return count;
}

/* Provide stream of audio toward radio unit */
void sender_send(sender_t *sender, sample_t *samples, uint8_t *power, int length)
{
	cnetz_t *cnetz = (cnetz_t *) sender;
	int count;

	/* measure tx sample speed */
	calc_clock_speed(cnetz, length, 1, 0);

#ifdef TEST_SCRAMBLE
	test_echo_load(&scrambler_test_echo, samples, length);
	scrambler(&scrambler_test_scrambler2, samples, length);
	return;
#endif

	count = fsk_telegramm(cnetz, samples, power, length);
	if (count < length) {
		printf("length=%d < count=%d\n", length, count);
		printf("this shall not happen, so please fix!\n");
		exit(0);
	}

}

/* unshrink audio segment from the duration of 60 bits to 12.5 ms */
void unshrink_speech(cnetz_t *cnetz, sample_t *speech_buffer, int count)
{
	sample_t *spl;
	int pos, i;
	int range;
	double offset;

	/* check if we still have a transaction
	 * this might not be true, if we just released transaction, but still
	 * get a complete frame before we already switched back to OgK.
	 */
	if (!cnetz->trans_list)
		return;

	/* ramp from level of last frame to level of current frame */
	range = cnetz->offset_range;
	offset = speech_buffer[0] - cnetz->offset_last;
	for (i = 0; i < range; i++) {
		speech_buffer[i] -= offset * (1.0 - (double)i / (double)range);
	}
	cnetz->offset_last = speech_buffer[count - 1];

	/* 4. de-emphasis is done by cnetz code, not by common code */
	/* de-emphasis is only used when scrambler is off, see FTZ 171 TR 60 Clause 4 */
	if (cnetz->de_emphasis)
		dc_filter(&cnetz->estate, speech_buffer, count);
	if (cnetz->de_emphasis && !cnetz->scrambler)
		de_emphasis(&cnetz->estate, speech_buffer, count);
	/* 3. descramble */
	if (cnetz->scrambler)
		scrambler(&cnetz->scrambler_rx, speech_buffer, count);
	/* 2. decompress time */
	count = samplerate_downsample(&cnetz->sender.srstate, speech_buffer, count);
	/* 1. expand dynamics */
	expand_audio(&cnetz->cstate, speech_buffer, count);
	/* to call control */
	spl = cnetz->sender.rxbuf;
	pos = cnetz->sender.rxbuf_pos;
	for (i = 0; i < count; i++) {
		spl[pos++] = speech_buffer[i];
		if (pos == 160) {
			call_up_audio(cnetz->trans_list->callref, spl, 160);
			pos = 0;
		}
	}
	cnetz->sender.rxbuf_pos = pos;
}

void cnetz_set_dsp_mode(cnetz_t *cnetz, enum dsp_mode mode)
{
	if (mode != cnetz->dsp_mode) {
		LOGP_CHAN(DDSP, LOGL_INFO, "DSP mode %s -> %s\n", cnetz_dsp_mode_name(cnetz->dsp_mode), cnetz_dsp_mode_name(mode));
		cnetz->dsp_mode = mode;
	}
	if (mode == DSP_MODE_SPK_V && cnetz->dsp_mode != mode)
		jitter_reset(&cnetz->sender.dejitter);

	/* we must get rid of partly received frame */
	fsk_demod_reset(&cnetz->fsk_demod);
}

void cnetz_set_sched_dsp_mode(cnetz_t *cnetz, enum dsp_mode mode, int timeslot)
{
	if (cnetz->sched_dsp_mode_ts < 0 && mode == cnetz->dsp_mode) {
		LOGP_CHAN(DDSP, LOGL_INFO, "Schedule DSP mode %s not required, we are already in that mode\n", cnetz_dsp_mode_name(mode));
		return;
	}
	LOGP_CHAN(DDSP, LOGL_INFO, "Schedule DSP mode %s -> %s at timeslot %d\n", cnetz_dsp_mode_name(cnetz->dsp_mode), cnetz_dsp_mode_name(mode), timeslot);
	cnetz->sched_dsp_mode = mode;
	cnetz->sched_dsp_mode_ts = timeslot;
}

/* Receive audio from call instance. */
void call_down_audio(void *decoder, void *decoder_priv, int callref, uint16_t sequence, uint8_t marker, uint32_t timestamp, uint32_t ssrc, uint8_t *payload, int payload_len)
{
	sender_t *sender;
	cnetz_t *cnetz;

	for (sender = sender_head; sender; sender = sender->next) {
		cnetz = (cnetz_t *) sender;
		if (cnetz->trans_list && cnetz->trans_list->callref == callref)
			break;
	}
	if (!sender)
		return;

	if (cnetz->dsp_mode == DSP_MODE_SPK_V) {
		jitter_frame_t *jf;
		jf = jitter_frame_alloc(decoder, decoder_priv, payload, payload_len, marker, sequence, timestamp, ssrc);
		if (jf)
			jitter_save(&cnetz->sender.dejitter, jf);
	}
}

void call_down_clock(void) {}

