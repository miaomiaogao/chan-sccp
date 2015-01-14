/*!
 * \file        sccp_softkeys.c
 * \brief       SCCP SoftKeys Class
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 *
 * $Date$
 * $Revision$
 */

#include <config.h>
#include "common.h"
#include "sccp_softkeys.h"
#include "sccp_pbx.h"
#include "sccp_device.h"
#include "sccp_channel.h"
#include "sccp_indicate.h"
#include "sccp_line.h"
#include "sccp_utils.h"
#include "sccp_features.h"
#include "sccp_actions.h"
#include "sccp_rtp.h"

SCCP_FILE_VERSION(__FILE__, "$Revision$");

/* =========================================================================================== Global */
/*!
 * \brief Global list of softkeys
 */
struct softKeySetConfigList softKeySetConfig;									/*!< List of SoftKeySets */

/*!
 * \brief SCCP SoftKeyMap Callback
 *
 * Used to Map Softkeys to there Handling Implementation
 */
struct sccp_softkeyMap_cb {
	uint32_t event;
	void (*softkeyEvent_cb) (const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c);
	boolean_t channelIsNecessary;
	char *uriactionstr;
};

/* =========================================================================================== Private */

/*!
 * \brief Forces Dialling before timeout
 * \n Usage: \ref sccp_sk_dial
 */
static void sccp_sk_dial(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Dial Pressed\n", DEV_ID_LOG(d));
	if (c && !PBX(getChannelPbx) (c)) {									// Prevent dialling if in an inappropriate state.
		/* Only handle this in DIALING state. AFAIK GETDIGITS is used only for call forward and related input functions. (-DD) */
		if (c->ss_action == SCCP_SS_GETFORWARDEXTEN) {
			sccp_pbx_softswitch(c);

		} else if (c->state == SCCP_CHANNELSTATE_DIGITSFOLL) {
			sccp_pbx_softswitch(c);
		}
	}
}

/*!
 * \brief Start/Stop VideoMode
 * \n Usage: \ref sccp_sk_videomode
 *
 * \todo Add doxygen entry for sccp_sk_videomode
 * \todo Implement stopping video transmission
 */
static void sccp_sk_videomode(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
#ifdef CS_SCCP_VIDEO
	if (sccp_device_isVideoSupported(d)) {
		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: We can have video, try to start vrtp\n", DEV_ID_LOG(d));
		if (!c->rtp.video.rtp && !sccp_rtp_createVideoServer(c)) {
			sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: can not start vrtp\n", DEV_ID_LOG(d));
		} else {
			sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: vrtp started\n", DEV_ID_LOG(d));
			sccp_channel_startMultiMediaTransmission(c);
		}
	}
#endif
}

/*!
 * \brief Redial last Dialed Number by this Device
 * \n Usage: \ref sk_redial
 */
static void sccp_sk_redial(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_line_t *line = NULL;

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Redial Pressed\n", DEV_ID_LOG(d));
	if (!d) {
		return;
	}
#ifdef CS_ADV_FEATURES
	char *data;

	if (d->useRedialMenu) {
		if (d->session->protocolType == SCCP_PROTOCOL) {
			if (d->protocolversion < 15) {
				data = "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"Key:Directories\"/><ExecuteItem Priority=\"0\" URL=\"Key:KeyPad3\"/></CiscoIPPhoneExecute>";
			} else {
				data = "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"Application:Cisco/PlacedCalls\"/></CiscoIPPhoneExecute>";
			}
		} else {
			data = "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"Key:Setup\"/><ExecuteItem Priority=\"0\" URL=\"Key:KeyPad1\"/><ExecuteItem Priority=\"0\" URL=\"Key:KeyPad3\"/></CiscoIPPhoneExecute>";
			//data = "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"Application:Cisco/PlacedCalls\"/></CiscoIPPhoneExecute>";
		}

		d->protocol->sendUserToDeviceDataVersionMessage(d, 0, lineInstance, 0, 0, data, 0);
		return;
	}
#endif

	if (sccp_strlen_zero(d->lastNumber)) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: No number to redial\n", d->id);
		return;
	}

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Get ready to redial number %s\n", d->id, d->lastNumber);
	if (c) {
		if (c->state == SCCP_CHANNELSTATE_OFFHOOK) {
			/* we have a offhook channel */
			sccp_copy_string(c->dialedNumber, d->lastNumber, sizeof(c->dialedNumber));
			sccp_pbx_softswitch(c);
		}
		/* here's a KEYMODE error. nothing to do */
		return;
	} else {
		if (l) {
			line = sccp_line_retain(l);
		} else {
			line = sccp_dev_get_activeline(d);
		}
		if (line) {
			AUTO_RELEASE sccp_channel_t *new_channel = NULL;

			new_channel = sccp_channel_newcall(line, d, d->lastNumber, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL);
			line = sccp_line_release(line);
		} else {
			sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Redial pressed on a device without a registered line\n", d->id);
		}
	}
}

/*!
 * \brief Initiate a New Call
 * \n Usage: \ref sk_newcall
 */
static void sccp_sk_newcall(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	char *adhocNumber = NULL;
	sccp_speed_t k;
	sccp_line_t *line = NULL;

	uint8_t instance = l ? sccp_device_find_index_for_line(d, l->name) : 0;					/*!< we use this instance, do determine if this should be a speeddial or a linerequest */

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey NewCall Pressed\n", DEV_ID_LOG(d));
	if (!l || instance != lineInstance) {
		/* handle dummy speeddial */
		sccp_dev_speed_find_byindex(d, lineInstance, TRUE, &k);
		if (strlen(k.ext) > 0) {
			adhocNumber = k.ext;
		}

		line = l;											/*!< use l as line to dialout */

		/* use default line if it is set */
		if (!line && d && d->defaultLineInstance > 0) {
			sccp_log((DEBUGCAT_SOFTKEY + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "using default line with instance: %u", d->defaultLineInstance);
			line = sccp_line_find_byid(d, d->defaultLineInstance);
		}
	} else {
		line = sccp_line_retain(l);
	}

	if (!line && d && d->currentLine) {
		line = sccp_dev_get_activeline(d);
	}
	if (!line) {
		sccp_dev_starttone(d, SKINNY_TONE_ZIPZIP, 0, 0, 1);
		sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_NO_LINE_AVAILABLE, GLOB(digittimeout));
	} else {
		if (!adhocNumber && (strlen(line->adhocNumber) > 0)) {
			adhocNumber = line->adhocNumber;
		}

		/* check if we have an active channel on an other line, that does not have any dialed number 
		 * (Can't select line after already off-hook - https://sourceforge.net/p/chan-sccp-b/discussion/652060/thread/878fe455/?limit=25#c06e/6006/a54d) 
		 */
		sccp_channel_t *activeChannel = NULL;

		if (!adhocNumber && (activeChannel = sccp_device_getActiveChannel(d))) {
			if (activeChannel->line != l && strlen(activeChannel->dialedNumber) == 0) {
				sccp_channel_endcall(activeChannel);
			}
			sccp_channel_release(activeChannel);
		}
		/* done */

		AUTO_RELEASE sccp_channel_t *new_channel = NULL;

		new_channel = sccp_channel_newcall(line, d, adhocNumber, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL);
		line = sccp_line_release(line);
	}
}

/*!
 * \brief Hold Call on Current Line
 * \n Usage: \ref sk_hold
 */
static void sccp_sk_hold(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Hold Pressed\n", DEV_ID_LOG(d));
	if (!c) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: No call to put on hold, check your softkeyset, hold should not be available in this situation.\n", DEV_ID_LOG(d));
		sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_NO_ACTIVE_CALL_TO_PUT_ON_HOLD, SCCP_DISPLAYSTATUS_TIMEOUT);
		return;
	}
	sccp_channel_hold(c);
}

/*!
 * \brief Resume Call on Current Line
 * \n Usage: \ref sk_resume
 */
static void sccp_sk_resume(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Resume Pressed\n", DEV_ID_LOG(d));
	if (!c) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: No call to resume. Ignoring\n", d->id);
		return;
	}
	sccp_channel_resume(d, c, TRUE);
}

/*!
 * \brief Transfer Call on Current Line
 * \n Usage: \ref sk_transfer
 *
 * \todo discus Marcello's transfer experiment
 */
static void sccp_sk_transfer(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_channel_transfer(c, d);
}

/*!
 * \brief End Call on Current Line
 * \n Usage: \ref sk_endcall
 */
static void sccp_sk_endcall(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey EndCall Pressed\n", DEV_ID_LOG(d));
	if (!c) {
		pbx_log(LOG_NOTICE, "%s: Endcall with no call in progress\n", d->id);
		return;
	}

	if (c->calltype == SKINNY_CALLTYPE_INBOUND && 1 < c->subscribers--) {					// && pbx_channel_state(c->owner) != AST_STATE_UP) {
		if (d && d->indicate && d->indicate->onhook) {
			d->indicate->onhook(d, lineInstance, c->callid);
		}
	} else {
		sccp_channel_endcall(c);
	}

#if 0														/* new */
	if (!(c->calltype == SKINNY_CALLTYPE_INBOUND && 1 < c->subscribers--)) {
		sccp_channel_endcall(c);
	}
	if (d && d->indicate && d->indicate->onhook) {
		d->indicate->onhook(d, lineInstance, c->callid);
	}
#endif
}

/*!
 * \brief Set DND on Current Line if Line is Active otherwise set on Device
 * \n Usage: \ref sk_dnd
 *
 * \todo The line param is not used 
 */
static void sccp_sk_dnd(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	if (!d) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: sccp_sk_dnd function called without specifying a device\n");
		return;
	}

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey DND Pressed (Current Status: %s, Feature enabled: %s)\n", DEV_ID_LOG(d), sccp_dndmode2str(d->dndFeature.status), d->dndFeature.enabled ? "YES" : "NO");

	if (!d->dndFeature.enabled) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: SoftKey DND Feature disabled\n", DEV_ID_LOG(d));
		sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_DND " " SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
		return;
	}

	if (!strcasecmp(d->dndFeature.configOptions, "reject")) {
		/* config is set to: dnd=reject */
		if (d->dndFeature.status == SCCP_DNDMODE_OFF) {
			d->dndFeature.status = SCCP_DNDMODE_REJECT;
		} else {
			d->dndFeature.status = SCCP_DNDMODE_OFF;
		}
	} else if (!strcasecmp(d->dndFeature.configOptions, "silent")) {
		/* config is set to: dnd=silent */
		if (d->dndFeature.status == SCCP_DNDMODE_OFF) {
			d->dndFeature.status = SCCP_DNDMODE_SILENT;
		} else {
			d->dndFeature.status = SCCP_DNDMODE_OFF;
		}
	} else {
		/* for all other config us the toggle mode */
		switch (d->dndFeature.status) {
			case SCCP_DNDMODE_OFF:
				d->dndFeature.status = SCCP_DNDMODE_REJECT;
				break;
			case SCCP_DNDMODE_REJECT:
				d->dndFeature.status = SCCP_DNDMODE_SILENT;
				break;
			case SCCP_DNDMODE_SILENT:
				d->dndFeature.status = SCCP_DNDMODE_OFF;
				break;
			default:
				d->dndFeature.status = SCCP_DNDMODE_OFF;
				break;
		}
	}

	sccp_feat_changed(d, NULL, SCCP_FEATURE_DND);								/* notify the modules the the DND-feature changed state */
	sccp_dev_check_displayprompt(d);									/*! \todo we should use the feature changed event to check displayprompt */
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey DND Pressed (New Status: %s, Feature enabled: %s)\n", DEV_ID_LOG(d), sccp_dndmode2str(d->dndFeature.status), d->dndFeature.enabled ? "YES" : "NO");
}

/*!
 * \brief BackSpace Last Entered Number
 * \n Usage: \ref sk_backspace
 */
static void sccp_sk_backspace(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Backspace Pressed\n", DEV_ID_LOG(d));
	int len;

	if (((c->state != SCCP_CHANNELSTATE_DIALING) && (c->state != SCCP_CHANNELSTATE_DIGITSFOLL) && (c->state != SCCP_CHANNELSTATE_OFFHOOK) && (c->state != SCCP_CHANNELSTATE_GETDIGITS)) || PBX(getChannelPbx) (c)) {
		return;
	}

	len = strlen(c->dialedNumber);

	/* we have no number, so nothing to process */
	if (!len) {
		return;
	}

	if (len >= 1) {
		c->dialedNumber[len - 1] = '\0';
		sccp_channel_schedule_digittimout(c, (len >= 1) ? GLOB(digittimeout) : GLOB(digittimeout));
	}
	// sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: backspacing dial number %s\n", c->device->id, c->dialedNumber);
	sccp_handle_dialtone(c);
	sccp_handle_backspace(d, lineInstance, c->callid);
}

/*!
 * \brief Answer Incoming Call
 * \n Usage: \ref sk_answer
 */
static void sccp_sk_answer(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	if (!c) {
		if (l) {
			ast_log(LOG_WARNING, "%s: (sccp_sk_answer) Pressed the answer key without any channel on line %s\n", d->id, l->name);
		}
		return;
	}
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Answer Pressed, instance: %d\n", DEV_ID_LOG(d), lineInstance);

	/* taking the reference during a locked ast channel allows us to call sccp_channel_answer unlock without the risk of loosing the channel */
	if (c->owner) {
		pbx_channel_lock(c->owner);
		PBX_CHANNEL_TYPE *pbx_channel = pbx_channel_ref(c->owner);

		pbx_channel_unlock(c->owner);

		if (pbx_channel) {
			sccp_channel_answer(d, c);
			pbx_channel_unref(pbx_channel);
		}
	}
}

/*!
 * \brief Bridge two selected channels
 * \n Usage: \ref sk_dirtrfr
 */
static void sccp_sk_dirtrfr(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Direct Transfer Pressed\n", DEV_ID_LOG(d));

	AUTO_RELEASE sccp_device_t *device = sccp_device_retain(d);

	if (!device) {
		return;
	}

	AUTO_RELEASE sccp_channel_t *chan1 = NULL, *chan2 = NULL;
	sccp_channel_t *tmp = NULL;

	if ((sccp_device_selectedchannels_count(device)) == 2) {
		sccp_selectedchannel_t *x = NULL;

		SCCP_LIST_LOCK(&device->selectedChannels);
		x = SCCP_LIST_FIRST(&device->selectedChannels);
		chan1 = sccp_channel_retain(x->channel);
		chan2 = SCCP_LIST_NEXT(x, list)->channel;
		chan2 = sccp_channel_retain(chan2);
		SCCP_LIST_UNLOCK(&device->selectedChannels);
	} else {
		if (SCCP_RWLIST_GETSIZE(&l->channels) == 2) {
			SCCP_LIST_LOCK(&l->channels);
			if ((tmp = SCCP_LIST_FIRST(&l->channels))) {
				chan1 = sccp_channel_retain(tmp);
				if ((tmp = SCCP_LIST_NEXT(tmp, list))) {
					chan2 = sccp_channel_retain(tmp);
				}
			}
			SCCP_LIST_UNLOCK(&l->channels);
		} else if (SCCP_RWLIST_GETSIZE(&l->channels) < 2) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Not enough channels to transfer\n", device->id);
			sccp_dev_displayprompt(device, lineInstance, c->callid, SKINNY_DISP_NOT_ENOUGH_CALLS_TO_TRANSFER, SCCP_DISPLAYSTATUS_TIMEOUT);
			return;
		} else {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: More than 2 channels to transfer, please use softkey select\n", device->id);
			sccp_dev_displayprompt(device, lineInstance, c->callid, SKINNY_DISP_MORE_THAN_TWO_CALLS ", " SKINNY_DISP_USE " " SKINNY_DISP_SELECT, SCCP_DISPLAYSTATUS_TIMEOUT);
			return;
		}
	}

	if (chan1 && chan2) {
		//for using the sccp_channel_transfer_complete function
		//chan2 must be in RINGOUT or CONNECTED state
		sccp_dev_displayprompt(device, lineInstance, c->callid, SKINNY_DISP_CALL_TRANSFER, SCCP_DISPLAYSTATUS_TIMEOUT);
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: (sccp_sk_dirtrfr) First Channel Status (%d), Second Channel Status (%d)\n", DEV_ID_LOG(device), chan1->state, chan2->state);
		if (chan2->state != SCCP_CHANNELSTATE_CONNECTED && chan1->state == SCCP_CHANNELSTATE_CONNECTED) {
			tmp = chan1;
			chan1 = chan2;
			chan2 = tmp;
		} else if (chan1->state == SCCP_CHANNELSTATE_HOLD && chan2->state == SCCP_CHANNELSTATE_HOLD) {
			//resume chan2 if both channels are on hold
			sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: (sccp_sk_dirtrfr) Resuming Second Channel (%d)\n", DEV_ID_LOG(device), chan2->state);
			sccp_channel_resume(device, chan2, TRUE);
			sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: (sccp_sk_dirtrfr) Resumed Second Channel (%d)\n", DEV_ID_LOG(device), chan2->state);
		}
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: (sccp_sk_dirtrfr) First Channel Status (%d), Second Channel Status (%d)\n", DEV_ID_LOG(device), chan1->state, chan2->state);
		device->transferChannels.transferee = sccp_channel_retain(chan1);
		device->transferChannels.transferer = sccp_channel_retain(chan2);

		sccp_channel_transfer_complete(chan2);
	}
}

/*!
 * \brief Select a Line for further processing by for example DirTrfr
 * \n Usage: \ref sk_select
 */
static void sccp_sk_select(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Select Pressed\n", DEV_ID_LOG(d));
	sccp_selectedchannel_t *x = NULL;
	sccp_msg_t *msg;
	uint8_t numSelectedChannels = 0, status = 0;

	if (!d) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: (sccp_sk_select) Can't select a channel without a device\n");
		return;
	}
	if (!c) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: (sccp_sk_select) No channel to be selected\n", DEV_ID_LOG(d));
		return;
	}

	if ((x = sccp_device_find_selectedchannel(d, c))) {
		SCCP_LIST_LOCK(&d->selectedChannels);
		x = SCCP_LIST_REMOVE(&d->selectedChannels, x, list);
		SCCP_LIST_UNLOCK(&d->selectedChannels);
		sccp_free(x);
	} else {
		x = sccp_malloc(sizeof(sccp_selectedchannel_t));
		if (x != NULL) {
			x->channel = c;
			SCCP_LIST_LOCK(&d->selectedChannels);
			SCCP_LIST_INSERT_HEAD(&d->selectedChannels, x, list);
			SCCP_LIST_UNLOCK(&d->selectedChannels);
			status = 1;
		}
	}
	numSelectedChannels = sccp_device_selectedchannels_count(d);

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: (sccp_sk_select) '%d' channels selected\n", DEV_ID_LOG(d), numSelectedChannels);

	REQ(msg, CallSelectStatMessage);
	msg->data.CallSelectStatMessage.lel_status = htolel(status);
	msg->data.CallSelectStatMessage.lel_lineInstance = htolel(lineInstance);
	msg->data.CallSelectStatMessage.lel_callReference = htolel(c->callid);
	sccp_dev_send(d, msg);
}

/*!
 * \brief Set Call Forward All on Current Line
 * \n Usage: \ref sk_cfwdall
 */
static void sccp_sk_cfwdall(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_line_t *line = NULL;

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Call Forward All Pressed, line: %s, instance: %d, channel: %d\n", DEV_ID_LOG(d), l ? l->name : "(NULL)", lineInstance, c ? c->callid : -1);
	if (!l && d) {

		if (d->defaultLineInstance > 0) {
			line = sccp_line_find_byid(d, d->defaultLineInstance);
		}
		if (!line) {
			line = sccp_dev_get_activeline(d);
		}
		if (!line) {
			line = sccp_line_find_byid(d, 1);
		}
	} else {
		line = sccp_line_retain(l);
	}

	if (line) {
		sccp_feat_handle_callforward(line, d, SCCP_CFWD_ALL);
		line = sccp_line_release(line);
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: No line found\n", DEV_ID_LOG(d));
	}
}

/*!
 * \brief Set Call Forward when Busy on Current Line
 * \n Usage: \ref sk_cfwdbusy
 */
static void sccp_sk_cfwdbusy(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_line_t *line = NULL;

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Call Forward Busy Pressed\n", DEV_ID_LOG(d));
	if (!l && d) {
		line = sccp_line_find_byid(d, 1);
	} else {
		line = sccp_line_retain(l);
	}

	if (line) {
		sccp_feat_handle_callforward(line, d, SCCP_CFWD_BUSY);
		line = sccp_line_release(line);
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: No line found\n", DEV_ID_LOG(d));
	}
}

/*!
 * \brief Set Call Forward when No Answer on Current Line
 * \n Usage: \ref sk_cfwdnoanswer
 */
static void sccp_sk_cfwdnoanswer(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_line_t *line = NULL;

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Call Forward NoAnswer Pressed\n", DEV_ID_LOG(d));
	if (!l && d) {
		line = sccp_line_find_byid(d, 1);
	} else {
		line = sccp_line_retain(l);
	}

	if (line) {
		sccp_feat_handle_callforward(line, d, SCCP_CFWD_NOANSWER);
		line = sccp_line_release(line);
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: No line found\n", DEV_ID_LOG(d));
	}
}

/*!
 * \brief Park Call on Current Line
 * \n Usage: \ref sk_park
 */
static void sccp_sk_park(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Park Pressed\n", DEV_ID_LOG(d));
#ifdef CS_SCCP_PARK
	sccp_channel_park(c);
#else
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "### Native park was not compiled in\n");
#endif
}

/*!
 * \brief Transfer to VoiceMail on Current Line
 * \n Usage: \ref sk_transfer
 */
static void sccp_sk_trnsfvm(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Transfer Voicemail Pressed\n", DEV_ID_LOG(d));
	sccp_feat_idivert(d, l, c);
}

/*!
 * \brief Initiate Private Call on Current Line
 * \n Usage: \ref sk_private
 *
 */
static void sccp_sk_private(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * line, const uint32_t lineInstance, sccp_channel_t * channel)
{
	AUTO_RELEASE sccp_channel_t *c;
	uint8_t instance;

	if (!d) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: sccp_sk_private function called without specifying a device\n");
		return;
	}

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Private Pressed\n", DEV_ID_LOG(d));

	if (!d->privacyFeature.enabled) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: private function is not active on this device\n", d->id);
		sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_PRIVATE_FEATURE_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
		return;
	}

	if (channel) {
		c = sccp_channel_retain(channel);
		instance = lineInstance;
	} else {
		AUTO_RELEASE sccp_line_t *l;

		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Creating new PRIVATE channel\n", d->id);
		if (line) {
			l = sccp_line_retain(line);
		} else {
			instance = (d->defaultLineInstance > 0) ? d->defaultLineInstance : SCCP_FIRST_LINEINSTANCE;
			l = sccp_line_find_byid(d, instance);
		}
		if (!l) {
			sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_PRIVATE_WITHOUT_LINE_CHANNEL, SCCP_DISPLAYSTATUS_TIMEOUT);
			return;
		}
		instance = sccp_device_find_index_for_line(d, l->name);
		sccp_dev_set_activeline(d, l);
		sccp_dev_set_cplane(d, instance, 1);
		c = sccp_channel_newcall(l, d, NULL, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL);
	}

	if (!c) {
		sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_PRIVATE_WITHOUT_LINE_CHANNEL, SCCP_DISPLAYSTATUS_TIMEOUT);
		return;
	}
	// check device->privacyFeature.status before toggeling

	// toggle
	c->privacy = (c->privacy) ? FALSE : TRUE;

	// update device->privacyFeature.status  using sccp_feat_changed
	//sccp_feat_changed(d, NULL, SCCP_FEATURE_PRIVACY);

	// Should actually use the messageStack instead of using displayprompt directly
	if (c->privacy) {
		//sccp_device_addMessageToStack(d, SCCP_MESSAGE_PRIORITY_PRIVACY, SKINNY_DISP_PRIVATE);
		sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_PRIVATE, 300);			// replaced with 5 min instead of always, just to make sure we return
		c->callInfo.presentation = 0;
	} else {
		sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_ENTER_NUMBER, 1);
		//sccp_device_clearMessageFromStack(d, SCCP_MESSAGE_PRIORITY_PRIVACY);
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Private %s on call %d\n", d->id, c->privacy ? "enabled" : "disabled", c->callid);
}

/*!
 * \brief Monitor Current Line
 * \n Usage: \ref sk_monitor
 */
static void sccp_sk_monitor(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Monitor Pressed\n", DEV_ID_LOG(d));
	sccp_feat_monitor(d, l, lineInstance, c);
}

/*!
 * \brief Put Current Line into Conference
 * \n Usage: \ref sk_conference
 * \todo Conferencing option needs to be build and implemented
 *       Using and External Conference Application Instead of Meetme makes it possible to use app_Conference, app_MeetMe, app_Konference and/or others
 */
static void sccp_sk_conference(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Conference Pressed\n", DEV_ID_LOG(d));
#ifdef CS_SCCP_CONFERENCE
	sccp_feat_handle_conference(d, l, lineInstance, c);
#else
	sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "### Conference was not compiled in\n");
#endif
}

/*!
 * \brief Show Participant List of Current Conference
 * \n Usage: \ref sk_conflist
 * \todo Conferencing option is under development.
 */
static void sccp_sk_conflist(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Conflist Pressed\n", DEV_ID_LOG(d));
#ifdef CS_SCCP_CONFERENCE
	sccp_feat_conflist(d, l, lineInstance, c);
#else
	sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "### Conference was not compiled in\n");
#endif
}

/*!
 * \brief Join Current Line to Conference
 * \n Usage: \ref sk_join
 * \todo Conferencing option needs to be build and implemented
 */
static void sccp_sk_join(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Join Pressed\n", DEV_ID_LOG(d));
#ifdef CS_SCCP_CONFERENCE
	sccp_feat_join(d, l, lineInstance, c);
#else
	sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "### Conference was not compiled in\n");
#endif
}

/*!
 * \brief Barge into Call on the Current Line
 * \n Usage: \ref sk_barge
 */
static void sccp_sk_barge(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_line_t *line = NULL;

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Barge Pressed\n", DEV_ID_LOG(d));
	if (!l && d) {
		line = sccp_line_find_byid(d, 1);
	} else {
		line = sccp_line_retain(l);
	}
	if (line) {
		sccp_feat_handle_barge(line, lineInstance, d);
		line = sccp_line_release(line);
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: No line found\n", DEV_ID_LOG(d));
	}
}

/*!
 * \brief Barge into Call on the Current Line
 * \n Usage: \ref sk_cbarge
 */
static void sccp_sk_cbarge(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_line_t *line = NULL;

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey cBarge Pressed\n", DEV_ID_LOG(d));
	if (!l && d) {
		line = sccp_line_find_byid(d, 1);
	} else {
		line = sccp_line_retain(l);
	}
	if (line) {
		sccp_feat_handle_cbarge(line, lineInstance, d);
		line = sccp_line_release(line);
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: No line found\n", DEV_ID_LOG(d));
	}
}

/*!
 * \brief Put Current Line in to Meetme Conference
 * \n Usage: \ref sk_meetme
 * \todo Conferencing option needs to be build and implemented
 *       Using and External Conference Application Instead of Meetme makes it possible to use app_Conference, app_MeetMe, app_Konference and/or others
 */
static void sccp_sk_meetme(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_line_t *line = NULL;

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Meetme Pressed\n", DEV_ID_LOG(d));
	if (!l && d) {
		line = sccp_line_find_byid(d, 1);
	} else {
		line = sccp_line_retain(l);
	}
	if (line) {
		sccp_feat_handle_meetme(line, lineInstance, d);
		line = sccp_line_release(line);
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: No line found\n", DEV_ID_LOG(d));
	}
}

/*!
 * \brief Pickup Parked Call
 * \n Usage: \ref sk_pickup
 */
static void sccp_sk_pickup(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Pickup Pressed\n", DEV_ID_LOG(d));
#ifndef CS_SCCP_PICKUP
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "### Native EXTENSION PICKUP was not compiled in\n");
#else
	sccp_line_t *line = NULL;

	if (!l && d) {
		line = sccp_line_find_byid(d, 1);
	} else {
		line = sccp_line_retain(l);
	}
	if (line) {
		sccp_feat_handle_directed_pickup(line, lineInstance, d);
		line = sccp_line_release(line);
		if (c) {
			sccp_channel_stop_schedule_digittimout(c);
		}
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: No line found\n", DEV_ID_LOG(d));
	}
#endif
}

/*!
 * \brief Pickup Ringing Line from Pickup Group
 * \n Usage: \ref sk_gpickup
 */
static void sccp_sk_gpickup(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Group Pickup Pressed\n", DEV_ID_LOG(d));
#ifndef CS_SCCP_PICKUP
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "### Native GROUP PICKUP was not compiled in\n");
#else
	sccp_line_t *line = NULL;

	if (!l && d) {
		line = sccp_line_find_byid(d, 1);
	} else {
		line = sccp_line_retain(l);
	}
	if (line) {
		sccp_feat_grouppickup(line, d);
		line = sccp_line_release(line);
		if (c) {
			sccp_channel_stop_schedule_digittimout(c);
		}
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: No line found\n", DEV_ID_LOG(d));
	}
#endif
}

#if CS_EXPERIMENTAL
/*!
 * \brief Execute URI(s) 
 * \n Usage: \ref sk_uriaction
 */
static void sccp_sk_uriaction(const sccp_softkeyMap_cb_t * softkeyMap_cb, sccp_device_t * d, sccp_line_t * l, const uint32_t lineInstance, sccp_channel_t * c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: SoftKey Pressed\n", DEV_ID_LOG(d));
	if (!d) {
		return;
	}
	unsigned int transactionID = random();

	/* build parameters */
	struct ast_str *paramStr = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);

	ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "name=%s", d->id);
	ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;softkey=%s", label2str(softkeyMap_cb->event));
	if (l) {
		ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;line=%s", l->name);
	}
	if (lineInstance) {
		ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;lineInstance=%d", lineInstance);
	}
	if (c) {
		ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;channel=%s", c->designator);
		ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;callid=%d", c->callid);
		if (c->owner) {
			ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;linkedid=%s", PBX(getChannelLinkedId) (c));
			ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;uniqueid=%s", pbx_channel_uniqueid(c->owner));
		}
	}
	ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;appID=%d", APPID_URIHOOK);
	ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;transactionID=%d", transactionID);

	/* build xmlStr */
	struct ast_str *xmlStr = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);

	ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "%s", "<CiscoIPPhoneExecute>");

	char delims[] = ",";
	char *uris = strdupa(softkeyMap_cb->uriactionstr);
	char *token = strtok(uris, delims);

	while (token) {
		token = sccp_trimwhitespace(token);
		if (!strncasecmp("http:", token, 5)) {
			if (!strchr(token, '?')) {
				ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "<ExecuteItem Priority=\"0\" URL=\"%s?%s\"/>", token, pbx_str_buffer(paramStr));
			} else {
				ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "<ExecuteItem Priority=\"0\" URL=\"%s&amp;%s\"/>", token, pbx_str_buffer(paramStr));
			}
		} else {
			ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "<ExecuteItem Priority=\"0\" URL=\"%s\"/>", token);
		}
		token = strtok(NULL, delims);
	}
	ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "%s", "</CiscoIPPhoneExecute>");

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Send '%s' to Phone\n", DEV_ID_LOG(d), pbx_str_buffer(xmlStr));
	d->protocol->sendUserToDeviceDataVersionMessage(d, APPID_URIHOOK, lineInstance, c ? c->callid : 0, transactionID, pbx_str_buffer(xmlStr), 0);
}
#endif

/*!
 * \brief Softkey Function Callback by SKINNY LABEL
 */
static const struct sccp_softkeyMap_cb softkeyCbMap[] = {
	{SKINNY_LBL_NEWCALL, sccp_sk_newcall, FALSE, NULL},
	{SKINNY_LBL_REDIAL, sccp_sk_redial, FALSE, NULL},
	{SKINNY_LBL_MEETME, sccp_sk_meetme, TRUE, NULL},
	{SKINNY_LBL_BARGE, sccp_sk_barge, TRUE, NULL},
	{SKINNY_LBL_CBARGE, sccp_sk_cbarge, TRUE, NULL},
	{SKINNY_LBL_HOLD, sccp_sk_hold, TRUE, NULL},
	{SKINNY_LBL_TRANSFER, sccp_sk_transfer, TRUE, NULL},
	{SKINNY_LBL_CFWDALL, sccp_sk_cfwdall, FALSE, NULL},
	{SKINNY_LBL_CFWDBUSY, sccp_sk_cfwdbusy, FALSE, NULL},
	{SKINNY_LBL_CFWDNOANSWER, sccp_sk_cfwdnoanswer, FALSE, NULL},
	{SKINNY_LBL_BACKSPACE, sccp_sk_backspace, TRUE, NULL},
	{SKINNY_LBL_ENDCALL, sccp_sk_endcall, TRUE, NULL},
	{SKINNY_LBL_RESUME, sccp_sk_resume, TRUE, NULL},
	{SKINNY_LBL_ANSWER, sccp_sk_answer, TRUE, NULL},
	{SKINNY_LBL_TRNSFVM, sccp_sk_trnsfvm, TRUE, NULL},
	{SKINNY_LBL_IDIVERT, sccp_sk_trnsfvm, TRUE, NULL},
	{SKINNY_LBL_DND, sccp_sk_dnd, FALSE, NULL},
	{SKINNY_LBL_DIRTRFR, sccp_sk_dirtrfr, TRUE, NULL},
	{SKINNY_LBL_SELECT, sccp_sk_select, TRUE, NULL},
	{SKINNY_LBL_PRIVATE, sccp_sk_private, FALSE, NULL},
	{SKINNY_LBL_MONITOR, sccp_sk_monitor, TRUE, NULL},
	{SKINNY_LBL_INTRCPT, sccp_sk_resume, TRUE, NULL},
	{SKINNY_LBL_DIAL, sccp_sk_dial, TRUE, NULL},
	{SKINNY_LBL_VIDEO_MODE, sccp_sk_videomode, TRUE, NULL},
	{SKINNY_LBL_PARK, sccp_sk_park, TRUE, NULL},
	{SKINNY_LBL_PICKUP, sccp_sk_pickup, FALSE, NULL},
	{SKINNY_LBL_GPICKUP, sccp_sk_gpickup, FALSE, NULL},
	{SKINNY_LBL_CONFRN, sccp_sk_conference, TRUE, NULL},
	{SKINNY_LBL_JOIN, sccp_sk_join, TRUE, NULL},
	{SKINNY_LBL_CONFLIST, sccp_sk_conflist, TRUE, NULL},
};

/*!
 * \brief Get SoftkeyMap by Event
 */
gcc_inline static const sccp_softkeyMap_cb_t *sccp_getSoftkeyMap_by_SoftkeyEvent(sccp_device_t * d, uint32_t event)
{
	uint8_t i;

	const sccp_softkeyMap_cb_t *mySoftkeyCbMap = softkeyCbMap;

#ifdef CS_EXPERIMENTAL
	if (d->softkeyset && d->softkeyset->softkeyCbMap) {
		mySoftkeyCbMap = d->softkeyset->softkeyCbMap;
	}
	sccp_log(DEBUGCAT_SOFTKEY) (VERBOSE_PREFIX_3 "%s: (sccp_getSoftkeyMap_by_SoftkeyEvent) default: %p, softkeyset: %p, softkeyCbMap: %p\n", d->id, softkeyCbMap, d->softkeyset, d->softkeyset ? d->softkeyset->softkeyCbMap : NULL);
#endif

	for (i = 0; i < ARRAY_LEN(softkeyCbMap); i++) {
		if (mySoftkeyCbMap[i].event == event) {
			return &mySoftkeyCbMap[i];
		}
	}
	return NULL;
}

/* =========================================================================================== Public */

/*!
 * \brief Softkey Pre Reload
 *
 */
void sccp_softkey_pre_reload(void)
{
#ifdef CS_EXPERIMENTAL
	sccp_softkey_clear();
#endif
}

/*!
 * \brief Softkey Post Reload
 */
void sccp_softkey_post_reload(void)
{
#ifdef CS_EXPERIMENTAL
	/* only required because softkeys are parsed after devices */
	/* incase softkeysets have changed but device was not reloaded, then d->softkeyset needs to be fixed up */
	sccp_softKeySetConfiguration_t *softkeyset;
	sccp_device_t *d;

	SCCP_LIST_LOCK(&softKeySetConfig);
	SCCP_LIST_TRAVERSE(&softKeySetConfig, softkeyset, list) {
		SCCP_RWLIST_WRLOCK(&GLOB(devices));
		SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
			if (d->softkeyDefinition && !strcasecmp(d->softkeyDefinition, softkeyset->name)) {
				d->softkeyset = softkeyset;
			}
		}
		SCCP_RWLIST_WRLOCK(&GLOB(devices));
	}
	SCCP_LIST_UNLOCK(&softKeySetConfig);
#endif
}

/*!
 * \brief Softkey Clear (Unload/Reload)
 */
void sccp_softkey_clear(void)
{
	sccp_softKeySetConfiguration_t *k;
	uint8_t i;

	SCCP_LIST_LOCK(&softKeySetConfig);
	while ((k = SCCP_LIST_REMOVE_HEAD(&softKeySetConfig, list))) {
		for (i = 0; i < (sizeof(SoftKeyModes) / sizeof(softkey_modes)); i++) {
			if (k->modes[i].ptr) {
				sccp_free(k->modes[i].ptr);
			}
		}
#ifdef CS_EXPERIMENTAL
		if (k->softkeyCbMap) {
			for (i = 0; i < ARRAY_LEN(softkeyCbMap); i++) {
				if (!sccp_strlen_zero(k->softkeyCbMap[i].uriactionstr)) {
					sccp_free(k->softkeyCbMap[i].uriactionstr);
				}
			}
			sccp_free(k->softkeyCbMap);
		}
#endif
		sccp_free(k);
	}
	SCCP_LIST_UNLOCK(&softKeySetConfig);
}

/*!
 * \brief Return a Copy of the statically defined SoftkeyMap
 * \note malloc, needs to be freed
 */
sccp_softkeyMap_cb_t __attribute__ ((malloc)) * sccp_softkeyMap_copyStaticallyMapped(void)
{
	sccp_softkeyMap_cb_t *newSoftKeyMap = sccp_malloc(ARRAY_LEN(softkeyCbMap) * sizeof(sccp_softkeyMap_cb_t));

	if (!newSoftKeyMap) {
		return NULL;
	}
	memcpy(newSoftKeyMap, softkeyCbMap, ARRAY_LEN(softkeyCbMap) * sizeof(sccp_softkeyMap_cb_t));
	sccp_log(DEBUGCAT_SOFTKEY) (VERBOSE_PREFIX_3 "SCCP: (sccp_softkeyMap_copyIfStaticallyMapped) Created copy of static version, returning: %p\n", newSoftKeyMap);
	return newSoftKeyMap;
}

#if CS_EXPERIMENTAL
/*!
 * \brief Replace a specific softkey callback entry by sccp_sk_uriaction and fill it's uriactionstr
 * \note strdup, needs to be freed
 */
boolean_t sccp_softkeyMap_replaceCallBackByUriAction(sccp_softkeyMap_cb_t * softkeyMap, uint32_t event, char *uriactionstr)
{
	int i;

	sccp_log(DEBUGCAT_SOFTKEY) (VERBOSE_PREFIX_3 "SCCP: (sccp_softkeyMap_replaceCallBackByUriHook) %p, event: %s, uriactionstr: %s\n", softkeyMap, label2str(event), uriactionstr);
	for (i = 0; i < ARRAY_LEN(softkeyCbMap); i++) {
		if (event == softkeyMap[i].event) {
			softkeyMap[i].softkeyEvent_cb = sccp_sk_uriaction;
			softkeyMap[i].uriactionstr = strdup(sccp_trimwhitespace(uriactionstr));
			return TRUE;
		}
	}
	return FALSE;
}
#endif

/*!
 * \brief Execute Softkey Callback by SofkeyEvent
 */
boolean_t sccp_SoftkeyMap_execCallbackByEvent(sccp_device_t * d, sccp_line_t * l, uint32_t lineInstance, sccp_channel_t * c, uint32_t event)
{
	if (!d || !event) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_execSoftkeyMapCb_by_SoftkeyEvent) no device or event provided\n");
		return FALSE;
	}
	const sccp_softkeyMap_cb_t *softkeyMap_cb = sccp_getSoftkeyMap_by_SoftkeyEvent(d, event);

	if (!softkeyMap_cb) {
		pbx_log(LOG_WARNING, "%s: Don't know how to handle keypress %d\n", d->id, event);
		return FALSE;
	}
	if (softkeyMap_cb->channelIsNecessary == TRUE && !c) {
		pbx_log(LOG_WARNING, "%s: Channel required to handle keypress %d\n", d->id, event);
		return FALSE;
	}
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Handling Softkey: %s on line: %s and channel: %s\n", d->id, label2str(event), l ? l->name : "UNDEF", c ? sccp_channel_toString(c) : "UNDEF");
	softkeyMap_cb->softkeyEvent_cb(softkeyMap_cb, d, l, lineInstance, c);
	return TRUE;
}

/*!
 * \brief Enable or Disable one softkey on a specific softKeySet
 */
void sccp_softkey_setSoftkeyState(sccp_device_t * device, uint8_t softKeySet, uint8_t softKey, boolean_t enable)
{
	uint8_t i;

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: softkey '%s' on %s to %s\n", DEV_ID_LOG(device), label2str(softKey), skinny_keymode2str(softKeySet), enable ? "on" : "off");

	if (!device) {
		return;
	}

	/* find softkey */
	for (i = 0; i < device->softKeyConfiguration.modes[softKeySet].count; i++) {
		if (device->softKeyConfiguration.modes[softKeySet].ptr[i] == softKey) {
			sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: found softkey '%s' at %d\n", DEV_ID_LOG(device), label2str(device->softKeyConfiguration.modes[softKeySet].ptr[i]), i);

			if (enable) {
				device->softKeyConfiguration.activeMask[softKeySet] |= (1 << i);
			} else {
				device->softKeyConfiguration.activeMask[softKeySet] &= (~(1 << i));
			}
		}
	}
}

// kate: indent-width 8; replace-tabs off; indent-mode cstyle; auto-insert-doxygen on; line-numbers on; tab-indents on; keep-extra-spaces off; auto-brackets off;
