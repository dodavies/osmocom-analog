/* Osmo-CC: helpers to simplify Osmo-CC usage
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

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include <inttypes.h>
#include "../libtimer/timer.h"
#include "../libselect/select.h"
#include "../libdebug/debug.h"
#include "session.h"
#include "message.h"
#include "rtp.h"
#include "helper.h"

osmo_cc_session_t *osmo_cc_helper_audio_offer(osmo_cc_session_config_t *conf, void *priv, struct osmo_cc_helper_audio_codecs *codecs, void (*receiver)(struct osmo_cc_session_codec *codec, uint8_t marker, uint16_t sequence_number, uint32_t timestamp, uint32_t ssrc, uint8_t *data, int len), osmo_cc_msg_t *msg, int debug)
{
	osmo_cc_session_t *session;
	osmo_cc_session_media_t *media;
	const char *sdp;
	int i;

	session = osmo_cc_new_session(conf, priv, NULL, NULL, NULL, 0, 0, NULL, NULL, debug);
	if (!session)
		return NULL;

	media = osmo_cc_add_media(session, 0, 0, NULL, osmo_cc_session_media_type_audio, 0, osmo_cc_session_media_proto_rtp, 1, 1, receiver, debug);
	osmo_cc_rtp_open(media);

	for (i = 0; codecs[i].payload_name; i++) {
		osmo_cc_add_codec(media, codecs[i].payload_name, codecs[i].payload_rate, codecs[i].payload_channels, codecs[i].encoder, codecs[i].decoder, debug);
	}

	sdp = osmo_cc_session_send_offer(session);
	osmo_cc_add_ie_sdp(msg, sdp);

	return session;
}

const char *osmo_cc_helper_audio_accept(osmo_cc_session_config_t *conf, void *priv, struct osmo_cc_helper_audio_codecs *codecs, void (*receiver)(struct osmo_cc_session_codec *codec, uint8_t marker, uint16_t sequence_number, uint32_t timestamp, uint32_t ssrc, uint8_t *data, int len), osmo_cc_msg_t *msg, osmo_cc_session_t **session_p, osmo_cc_session_codec_t **codec_p, int force_our_codec)
{
	return osmo_cc_helper_audio_accept_te(conf, priv, codecs, receiver, msg, session_p, codec_p, NULL, force_our_codec);
}

const char *osmo_cc_helper_audio_accept_te(osmo_cc_session_config_t *conf, void *priv, struct osmo_cc_helper_audio_codecs *codecs, void (*receiver)(struct osmo_cc_session_codec *codec, uint8_t marker, uint16_t sequence_number, uint32_t timestamp, uint32_t ssrc, uint8_t *data, int len), osmo_cc_msg_t *msg, osmo_cc_session_t **session_p, osmo_cc_session_codec_t **codec_p, osmo_cc_session_codec_t **telephone_event_p, int force_our_codec)
{
	char offer_sdp[65536];
	const char *accept_sdp;
	osmo_cc_session_media_t *media, *selected_media;
	osmo_cc_session_codec_t *codec, *selected_codec, *telephone_event;
	int rc;
	int i, selected_codec_i, telephone_event_i;

	if (*session_p) {
		LOGP(DCC, LOGL_ERROR, "Session already set, please fix!\n");
		abort();
	}
	if (*codec_p) {
		LOGP(DCC, LOGL_ERROR, "Codec already set, please fix!\n");
		abort();
	}

	/* SDP IE */
	rc = osmo_cc_get_ie_sdp(msg, 0, offer_sdp, sizeof(offer_sdp));
	if (rc < 0) {
		LOGP(DCC, LOGL_ERROR, "There is no SDP included in setup request.\n");
		return NULL;
	}

	*session_p = osmo_cc_session_receive_offer(conf, priv, offer_sdp);
	if (!*session_p) {
		LOGP(DCC, LOGL_ERROR, "Failed to parse SDP.\n");
		return NULL;
	}

	selected_media = NULL;
	osmo_cc_session_for_each_media((*session_p)->media_list, media) {
		/* only audio */
		if (media->description.type != osmo_cc_session_media_type_audio)
			continue;
		selected_codec_i = -1;
		selected_codec = NULL;
		telephone_event_i = -1;
		telephone_event = NULL;
		osmo_cc_session_for_each_codec(media->codec_list, codec) {
			if (!!strcasecmp(codec->payload_name, "telephone-event")) {
				for (i = 0; codecs[i].payload_name; i++) {
					if (osmo_cc_session_if_codec(codec, codecs[i].payload_name, codecs[i].payload_rate, codecs[i].payload_channels)) {
						/* select the first matchting codec or the one we prefer */
						if (selected_codec_i < 0 || i < selected_codec_i) {
							selected_codec = codec;
							selected_codec_i = i;
							selected_media = media;
						}
						/* if we don't force our preferred codec, use the preferred one from the remote */
						if (!force_our_codec)
							break;
					}
				}
			} else {
				/* special case: add telephone-event, if supported */
				for (i = 0; codecs[i].payload_name; i++) {
					if (!!strcasecmp(codecs[i].payload_name, "telephone-event"))
						continue;
					telephone_event = codec;
					telephone_event_i = i;
					break;
				}
			}
		}
		/* codec is selected within this media, we are done */
		if (selected_codec)
			break;
	}
	if (!selected_codec) {
		LOGP(DCC, LOGL_ERROR, "No codec found in setup message that we support.\n");
		osmo_cc_free_session(*session_p);
		*session_p = NULL;
		return NULL;
	}
	osmo_cc_session_accept_codec(selected_codec, codecs[selected_codec_i].encoder, codecs[selected_codec_i].decoder);
	if (telephone_event)
		osmo_cc_session_accept_codec(telephone_event, codecs[telephone_event_i].encoder, codecs[telephone_event_i].decoder);
	osmo_cc_session_accept_media(selected_media, 0, 0, NULL, 1, 1, receiver);
	osmo_cc_rtp_open(selected_media);
	osmo_cc_rtp_connect(selected_media);
	*codec_p = selected_codec;
	if (telephone_event_p)
		*telephone_event_p = telephone_event;

	accept_sdp = osmo_cc_session_send_answer(*session_p);
	if (!accept_sdp) {
		osmo_cc_free_session(*session_p);
		*session_p = NULL;
		return NULL;
	}

	return accept_sdp;
}

int osmo_cc_helper_audio_negotiate(osmo_cc_msg_t *msg, osmo_cc_session_t **session_p, osmo_cc_session_codec_t **codec_p)
{
	return osmo_cc_helper_audio_negotiate_te(msg, session_p, codec_p, NULL);
}

int osmo_cc_helper_audio_negotiate_te(osmo_cc_msg_t *msg, osmo_cc_session_t **session_p, osmo_cc_session_codec_t **codec_p, osmo_cc_session_codec_t **telephone_event_p)
{
	char sdp[65536];
	osmo_cc_session_media_t *media;
	osmo_cc_session_codec_t *codec;
	int rc;

	if (!(*session_p)) {
		LOGP(DCC, LOGL_ERROR, "Session not set, please fix!\n");
		abort();
	}

	/* SDP IE */
	rc = osmo_cc_get_ie_sdp(msg, 0, sdp, sizeof(sdp));
	if (rc < 0)
		return 0; // no reply in this message

	rc = osmo_cc_session_receive_answer(*session_p, sdp);
	if (rc < 0)
		return rc;

	osmo_cc_session_for_each_media((*session_p)->media_list, media) {
		/* skip not accepted medias */
		if (!media->accepted)
			continue;
		/* only audio */
		if (media->description.type != osmo_cc_session_media_type_audio)
			continue;
		osmo_cc_session_for_each_codec(media->codec_list, codec) {
			/* skip not accepted codecs */
			if (!codec->accepted)
				continue;
			if (!!strcasecmp(codec->payload_name, "telephone-event")) {
				/* select first codec, if one was accpeted */
				if (!(*codec_p)) {
					LOGP(DCC, LOGL_DEBUG, "Select codec '%s'.\n", codec->payload_name);
					*codec_p = codec;
				}
			} else {
				if (telephone_event_p && !(*telephone_event_p)) {
					LOGP(DCC, LOGL_DEBUG, "select telephone event codec.\n");
					*telephone_event_p = codec;
				}
			}
		}
		if (*codec_p) {
			osmo_cc_rtp_connect(media);
			/* no more media streams */
			break;
		}
	}
	if (!(*codec_p)) {
		LOGP(DCC, LOGL_ERROR, "No codec found in setup reply message that we support.\n");
		return -EIO;
	}

	return 0;
}

