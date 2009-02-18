/* GSM Mobile Radio Interface Layer 3 messages on the A-bis interface 
 * 3GPP TS 04.08 version 7.21.0 Release 1998 / ETSI TS 100 940 V7.21.0 */

/* (C) 2008-2009 by Harald Welte <laforge@gnumonks.org>
 * (C) 2008, 2009 by Holger Hans Peter Freyther <zecke@selfish.org>
 *
 * All Rights Reserved
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <netinet/in.h>

#include <openbsc/db.h>
#include <openbsc/msgb.h>
#include <openbsc/tlv.h>
#include <openbsc/debug.h>
#include <openbsc/gsm_data.h>
#include <openbsc/gsm_subscriber.h>
#include <openbsc/gsm_04_11.h>
#include <openbsc/gsm_04_08.h>
#include <openbsc/abis_rsl.h>
#include <openbsc/chan_alloc.h>
#include <openbsc/paging.h>
#include <openbsc/signal.h>

#define GSM48_ALLOC_SIZE	1024
#define GSM48_ALLOC_HEADROOM	128

static const struct tlv_definition rsl_att_tlvdef = {
	.def = {
		[GSM48_IE_MOBILE_ID]	= { TLV_TYPE_TLV },
		[GSM48_IE_NAME_LONG]	= { TLV_TYPE_TLV },
		[GSM48_IE_NAME_SHORT]	= { TLV_TYPE_TLV },
		[GSM48_IE_UTC]		= { TLV_TYPE_TV },
		[GSM48_IE_NET_TIME_TZ]	= { TLV_TYPE_FIXED, 7 },
		[GSM48_IE_LSA_IDENT]	= { TLV_TYPE_TLV },

		[GSM48_IE_BEARER_CAP]	= { TLV_TYPE_TLV },
		[GSM48_IE_CAUSE]	= { TLV_TYPE_TLV },
		[GSM48_IE_CC_CAP]	= { TLV_TYPE_TLV },
		[GSM48_IE_ALERT]	= { TLV_TYPE_TLV },
		[GSM48_IE_FACILITY]	= { TLV_TYPE_TLV },
		[GSM48_IE_PROGR_IND]	= { TLV_TYPE_TLV },
		[GSM48_IE_AUX_STATUS]	= { TLV_TYPE_TLV },
		[GSM48_IE_KPD_FACILITY]	= { TLV_TYPE_TV },
		[GSM48_IE_SIGNAL]	= { TLV_TYPE_TV },
		[GSM48_IE_CONN_NUM]	= { TLV_TYPE_TLV },
		[GSM48_IE_CONN_SUBADDR]	= { TLV_TYPE_TLV },
		[GSM48_IE_CALLING_BCD]	= { TLV_TYPE_TLV },
		[GSM48_IE_CALLING_SUB]	= { TLV_TYPE_TLV },
		[GSM48_IE_CALLED_BCD]	= { TLV_TYPE_TLV },
		[GSM48_IE_CALLED_SUB]	= { TLV_TYPE_TLV },
		[GSM48_IE_REDIR_BCD]	= { TLV_TYPE_TLV },
		[GSM48_IE_REDIR_SUB]	= { TLV_TYPE_TLV },
		[GSM48_IE_LOWL_COMPAT]	= { TLV_TYPE_TLV },
		[GSM48_IE_HIGHL_COMPAT]	= { TLV_TYPE_TLV },
		[GSM48_IE_USER_USER]	= { TLV_TYPE_TLV },
		[GSM48_IE_SS_VERS]	= { TLV_TYPE_TLV },
		[GSM48_IE_MORE_DATA]	= { TLV_TYPE_T },
		[GSM48_IE_CLIR_SUPP]	= { TLV_TYPE_T },
		[GSM48_IE_CLIR_INVOC]	= { TLV_TYPE_T },
		[GSM48_IE_REV_C_SETUP]	= { TLV_TYPE_T },
		/* FIXME: more elements */
	},
};
		
static inline int is_ipaccess_bts(struct gsm_bts *bts)
{
	switch (bts->type) {
	case GSM_BTS_TYPE_NANOBTS_900:
	case GSM_BTS_TYPE_NANOBTS_1800:
		return 1;
	default:
		break;
	}
	return 0;
}

static int gsm48_tx_simple(struct gsm_lchan *lchan,
			   u_int8_t pdisc, u_int8_t msg_type);
static void schedule_reject(struct gsm_lchan *lchan);

struct gsm_lai {
	u_int16_t mcc;
	u_int16_t mnc;
	u_int16_t lac;
};

static int authorize_everonye = 0;
void gsm0408_allow_everyone(int everyone)
{
	printf("Allowing everyone?\n");
	authorize_everonye = everyone;
}

static int reject_cause = 0;
void gsm0408_set_reject_cause(int cause)
{
	reject_cause = cause;
}

static int authorize_subscriber(struct gsm_loc_updating_operation *loc,
				struct gsm_subscriber *subscriber)
{
	if (!subscriber)
		return 0;

	/*
	 * Do not send accept yet as more information should arrive. Some
	 * phones will not send us the information and we will have to check
	 * what we want to do with that.
	 */
	if (loc && (loc->waiting_for_imsi || loc->waiting_for_imei))
		return 0;

	if (authorize_everonye)
		return 1;

	return subscriber->authorized;
}

static void release_loc_updating_req(struct gsm_lchan *lchan)
{
	if (!lchan->loc_operation)
		return;

	del_timer(&lchan->loc_operation->updating_timer);
	free(lchan->loc_operation);
	lchan->loc_operation = 0;
	put_lchan(lchan);
}

static void allocate_loc_updating_req(struct gsm_lchan *lchan)
{
	use_lchan(lchan);
	release_loc_updating_req(lchan);

	lchan->loc_operation = (struct gsm_loc_updating_operation *)
				malloc(sizeof(*lchan->loc_operation));
	memset(lchan->loc_operation, 0, sizeof(*lchan->loc_operation));
}

static void to_bcd(u_int8_t *bcd, u_int16_t val)
{
	bcd[2] = val % 10;
	val = val / 10;
	bcd[1] = val % 10;
	val = val / 10;
	bcd[0] = val % 10;
	val = val / 10;
}

void gsm0408_generate_lai(struct gsm48_loc_area_id *lai48, u_int16_t mcc, 
			 u_int16_t mnc, u_int16_t lac)
{
	u_int8_t bcd[3];

	to_bcd(bcd, mcc);
	lai48->digits[0] = bcd[0] | (bcd[1] << 4);
	lai48->digits[1] = bcd[2];

	to_bcd(bcd, mnc);
	/* FIXME: do we need three-digit MNC? See Table 10.5.3 */
#if 0
	lai48->digits[1] |= bcd[2] << 4;
	lai48->digits[2] = bcd[0] | (bcd[1] << 4);
#else
	lai48->digits[1] |= 0xf << 4;
	lai48->digits[2] = bcd[1] | (bcd[2] << 4);
#endif
	
	lai48->lac = htons(lac);
}

#define TMSI_LEN	5
#define MID_TMSI_LEN	(TMSI_LEN + 2)

int generate_mid_from_tmsi(u_int8_t *buf, u_int32_t tmsi)
{
	u_int32_t *tptr = (u_int32_t *) &buf[3];

	buf[0] = GSM48_IE_MOBILE_ID;
	buf[1] = TMSI_LEN;
	buf[2] = 0xf0 | GSM_MI_TYPE_TMSI;
	*tptr = htonl(tmsi);

	return 7;
}

static const char bcd_num_digits[] = {
	'0', '1', '2', '3', '4', '5', '6', '7', 
	'8', '9', '*', '#', 'a', 'b', 'c', '\0'
};

/* decode a 'called party BCD number' as in 10.5.4.7 */
u_int8_t decode_bcd_number(char *output, int output_len, const u_int8_t *bcd_lv)
{
	u_int8_t in_len = bcd_lv[0];
	int i;

	if (in_len < 1)
		return 0;

	for (i = 2; i <= in_len; i++) {
		/* lower nibble */
		output_len--;
		if (output_len <= 1)
			break;
		*output++ = bcd_num_digits[bcd_lv[i] & 0xf];

		/* higher nibble */
		output_len--;
		if (output_len <= 1)
			break;
		*output++ = bcd_num_digits[bcd_lv[i] >> 4];
	}
	if (output_len >= 1)
		*output++ = '\0';

	/* return number type / calling plan */
	return bcd_lv[1] & 0x3f;
}

/* convert a single ASCII character to call-control BCD */
static int asc_to_bcd(const char asc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bcd_num_digits); i++) {
		if (bcd_num_digits[i] == asc)
			return i;
	}
	return -EINVAL;
}

/* convert a ASCII phone number to 'called party BCD number' */
int encode_bcd_number(u_int8_t *bcd_lv, u_int8_t max_len,
		      u_int8_t type, const char *input)
{
	int in_len = strlen(input);
	int i;
	u_int8_t *bcd_cur = bcd_lv + 2;

	if (in_len/2 + 1 > max_len)
		return -EIO;

	/* two digits per byte, plus type byte */
	bcd_lv[0] = in_len/2 + 1;
	if (in_len % 2)
		bcd_lv[0]++;

	/* if the caller wants to create a valid 'calling party BCD
	 * number', then the extension bit MUST NOT be set.  For the
	 * 'called party BCD number' it MUST be set. *sigh */
	bcd_lv[1] = type;

	for (i = 0; i < in_len; i++) {
		int rc = asc_to_bcd(input[i]);
		if (rc < 0)
			return rc;
		if (i % 2 == 0)
			*bcd_cur = rc;	
		else
			*bcd_cur++ |= (rc << 4);
	}
	/* append padding nibble in case of odd length */
	if (i % 2)
		*bcd_cur++ |= 0xf0;

	/* return how many bytes we used */
	return (bcd_cur - bcd_lv);
}

struct msgb *gsm48_msgb_alloc(void)
{
	return msgb_alloc_headroom(GSM48_ALLOC_SIZE, GSM48_ALLOC_HEADROOM);
}

int gsm48_sendmsg(struct msgb *msg)
{
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msg->data;

	if (msg->lchan) {
		msg->trx = msg->lchan->ts->trx;

		if ((gh->proto_discr & GSM48_PDISC_MASK) == GSM48_PDISC_CC) {
			/* Send a 04.08 call control message, add transaction
			 * ID and TI flag */
			gh->proto_discr |= msg->lchan->call.transaction_id;

			/* GSM 04.07 Section 11.2.3.1.3 */
			switch (msg->lchan->call.type) {
			case GSM_CT_MO:
				gh->proto_discr |= 0x80;
				break;
			case GSM_CT_MT:
				break;
			case GSM_CT_NONE:
				break;
			}
		}
	}

	msg->l3h = msg->data;

	return rsl_data_request(msg, 0);
}


/* Chapter 9.2.14 : Send LOCATION UPDATING REJECT */
int gsm0408_loc_upd_rej(struct gsm_lchan *lchan, u_int8_t cause)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh;
	
	msg->lchan = lchan;

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh) + 1);
	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_LOC_UPD_REJECT;
	gh->data[0] = cause;

	DEBUGP(DMM, "-> LOCATION UPDATING REJECT on channel: %d\n", lchan->nr);
	
	return gsm48_sendmsg(msg);
}

/* Chapter 9.2.13 : Send LOCATION UPDATE ACCEPT */
int gsm0408_loc_upd_acc(struct gsm_lchan *lchan, u_int32_t tmsi)
{
	struct gsm_bts *bts = lchan->ts->trx->bts;
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh;
	struct gsm48_loc_area_id *lai;
	u_int8_t *mid;
	int ret;
	
	msg->lchan = lchan;

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_LOC_UPD_ACCEPT;

	lai = (struct gsm48_loc_area_id *) msgb_put(msg, sizeof(*lai));
	gsm0408_generate_lai(lai, bts->network->country_code,
		     bts->network->network_code, bts->location_area_code);

	mid = msgb_put(msg, MID_TMSI_LEN);
	generate_mid_from_tmsi(mid, tmsi);

	DEBUGP(DMM, "-> LOCATION UPDATE ACCEPT\n");

	ret = gsm48_sendmsg(msg);

	ret = gsm48_tx_mm_info(lchan);

	return ret;
}

static char bcd2char(u_int8_t bcd)
{
	if (bcd < 0xa)
		return '0' + bcd;
	else
		return 'A' + (bcd - 0xa);
}

/* Convert Mobile Identity (10.5.1.4) to string */
static int mi_to_string(char *string, int str_len, u_int8_t *mi, int mi_len)
{
	int i;
	u_int8_t mi_type;
	char *str_cur = string;
	u_int32_t tmsi;

	mi_type = mi[0] & GSM_MI_TYPE_MASK;

	switch (mi_type) {
	case GSM_MI_TYPE_NONE:
		break;
	case GSM_MI_TYPE_TMSI:
		/* Table 10.5.4.3, reverse generate_mid_from_tmsi */
		if (mi_len == TMSI_LEN && mi[0] == (0xf0 | GSM_MI_TYPE_TMSI)) {
			memcpy(&tmsi, &mi[1], 4);
			tmsi = ntohl(tmsi);
			return snprintf(string, str_len, "%u", tmsi);
		}
		break;
	case GSM_MI_TYPE_IMSI:
	case GSM_MI_TYPE_IMEI:
	case GSM_MI_TYPE_IMEISV:
		*str_cur++ = bcd2char(mi[0] >> 4);
		
                for (i = 1; i < mi_len; i++) {
			if (str_cur + 2 >= string + str_len)
				return str_cur - string;
			*str_cur++ = bcd2char(mi[i] & 0xf);
			/* skip last nibble in last input byte when GSM_EVEN */
			if( (i != mi_len-1) || (mi[0] & GSM_MI_ODD))
				*str_cur++ = bcd2char(mi[i] >> 4);
		}
		break;
	default:
		break;
	}
	*str_cur++ = '\0';

	return str_cur - string;
}

/* Transmit Chapter 9.2.10 Identity Request */
static int mm_tx_identity_req(struct gsm_lchan *lchan, u_int8_t id_type)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh;

	msg->lchan = lchan;

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh) + 1);
	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_ID_REQ;
	gh->data[0] = id_type;

	return gsm48_sendmsg(msg);
}

#define MI_SIZE 32

/* Parse Chapter 9.2.11 Identity Response */
static int mm_rx_id_resp(struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm_lchan *lchan = msg->lchan;
	u_int8_t mi_type = gh->data[1] & GSM_MI_TYPE_MASK;
	char mi_string[MI_SIZE];
	u_int32_t tmsi;

	mi_to_string(mi_string, sizeof(mi_string), &gh->data[1], gh->data[0]);
	DEBUGP(DMM, "IDENTITY RESPONSE: mi_type=0x%02x MI(%s)\n",
		mi_type, mi_string);

	/*
	 * Rogue messages could trick us but so is life
	 */
	put_lchan(lchan);

	switch (mi_type) {
	case GSM_MI_TYPE_IMSI:
		if (!lchan->subscr)
			lchan->subscr = db_create_subscriber(mi_string);
		if (lchan->loc_operation)
			lchan->loc_operation->waiting_for_imsi = 0;
		break;
	case GSM_MI_TYPE_IMEI:
	case GSM_MI_TYPE_IMEISV:
		/* update subscribe <-> IMEI mapping */
		if (lchan->subscr)
			db_subscriber_assoc_imei(lchan->subscr, mi_string);
		if (lchan->loc_operation)
			lchan->loc_operation->waiting_for_imei = 0;
		break;
	}

	/* Check if we can let the mobile station enter */
	if (authorize_subscriber(lchan->loc_operation, lchan->subscr)) {
		db_subscriber_alloc_tmsi(lchan->subscr);
		tmsi = strtoul(lchan->subscr->tmsi, NULL, 10);
		release_loc_updating_req(lchan);
		return gsm0408_loc_upd_acc(msg->lchan, tmsi);
	}

	return 0;
}


static void loc_upd_rej_cb(void *data)
{
	struct gsm_lchan *lchan = data;

	release_loc_updating_req(lchan);
	gsm0408_loc_upd_rej(lchan, reject_cause);
	lchan_auto_release(lchan);
}

static void schedule_reject(struct gsm_lchan *lchan)
{
	lchan->loc_operation->updating_timer.cb = loc_upd_rej_cb;
	lchan->loc_operation->updating_timer.data = lchan;
	schedule_timer(&lchan->loc_operation->updating_timer, 5, 0);
}

#define MI_SIZE 32
/* Chapter 9.2.15: Receive Location Updating Request */
static int mm_rx_loc_upd_req(struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm_bts *bts = msg->trx->bts;
	struct gsm48_loc_upd_req *lu;
	struct gsm_subscriber *subscr;
	struct gsm_lchan *lchan = msg->lchan;
	u_int8_t mi_type;
	u_int32_t tmsi;
	char mi_string[MI_SIZE];
	int rc;

 	lu = (struct gsm48_loc_upd_req *) gh->data;

	mi_type = lu->mi[0] & GSM_MI_TYPE_MASK;

	mi_to_string(mi_string, sizeof(mi_string), lu->mi, lu->mi_len);

	DEBUGP(DMM, "LUPDREQ: mi_type=0x%02x MI(%s)\n", mi_type, mi_string);

	allocate_loc_updating_req(lchan);

	switch (mi_type) {
	case GSM_MI_TYPE_IMSI:
		/* we always want the IMEI, too */
		use_lchan(lchan);
		rc = mm_tx_identity_req(lchan, GSM_MI_TYPE_IMEISV);
		lchan->loc_operation->waiting_for_imei = 1;

		/* look up subscriber based on IMSI */
		subscr = db_create_subscriber(mi_string);
		break;
	case GSM_MI_TYPE_TMSI:
		/* we always want the IMEI, too */
		use_lchan(lchan);
		rc = mm_tx_identity_req(lchan, GSM_MI_TYPE_IMEISV);
		lchan->loc_operation->waiting_for_imei = 1;

		/* look up the subscriber based on TMSI, request IMSI if it fails */
		subscr = subscr_get_by_tmsi(mi_string);
		if (!subscr) {
			/* send IDENTITY REQUEST message to get IMSI */
			use_lchan(lchan);
			rc = mm_tx_identity_req(lchan, GSM_MI_TYPE_IMSI);
			lchan->loc_operation->waiting_for_imsi = 1;
		}
		break;
	case GSM_MI_TYPE_IMEI:
	case GSM_MI_TYPE_IMEISV:
		/* no sim card... FIXME: what to do ? */
		fprintf(stderr, "Unimplemented mobile identity type\n");
		break;
	default:	
		fprintf(stderr, "Unknown mobile identity type\n");
		break;
	}

	lchan->subscr = subscr;

	/*
	 * Schedule the reject timer and check if we can let the
	 * subscriber into our network immediately or if we need to wait
	 * for identity responses.
	 */
	schedule_reject(lchan);
	if (!authorize_subscriber(lchan->loc_operation, subscr))
		return 0;

	db_subscriber_alloc_tmsi(subscr);
	subscr_update(subscr, bts);

	tmsi = strtoul(subscr->tmsi, NULL, 10);

	release_loc_updating_req(lchan);
	return gsm0408_loc_upd_acc(lchan, tmsi);
}

/* 9.1.5 Channel mode modify */
int gsm48_tx_chan_mode_modify(struct gsm_lchan *lchan, u_int8_t mode)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	struct gsm48_chan_mode_modify *cmm =
		(struct gsm48_chan_mode_modify *) msgb_put(msg, sizeof(*cmm));
	u_int16_t arfcn = lchan->ts->trx->arfcn;

	DEBUGP(DRR, "-> CHANNEL MODE MODIFY\n");

	msg->lchan = lchan;
	gh->proto_discr = GSM48_PDISC_RR;
	gh->msg_type = GSM48_MT_RR_CHAN_MODE_MODIF;

	/* fill the channel information element, this code
	 * should probably be shared with rsl_rx_chan_rqd() */
	cmm->chan_desc.chan_nr = lchan2chan_nr(lchan);
	cmm->chan_desc.h0.h = 0;
	cmm->chan_desc.h0.arfcn_high = arfcn >> 8;
	cmm->chan_desc.h0.arfcn_low = arfcn & 0xff;
	cmm->mode = mode;

	return gsm48_sendmsg(msg);
}

/* Section 9.2.15a */
int gsm48_tx_mm_info(struct gsm_lchan *lchan)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh;
	struct gsm_network *net = lchan->ts->trx->bts->network;
	u_int8_t *ptr8;
	u_int16_t *ptr16;
	int name_len;
	int i;

	msg->lchan = lchan;

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_INFO;

	if (net->name_long) {
		name_len = strlen(net->name_long);
		/* 10.5.3.5a */
		ptr8 = msgb_put(msg, 3);
		ptr8[0] = GSM48_IE_NAME_LONG;
		ptr8[1] = name_len*2 +1;
		ptr8[2] = 0x90; /* UCS2, no spare bits, no CI */

		ptr16 = (u_int16_t *) msgb_put(msg, name_len*2);
		for (i = 0; i < name_len; i++)
			ptr16[i] = htons(net->name_long[i]);

		/* FIXME: Use Cell Broadcast, not UCS-2, since
		 * UCS-2 is only supported by later revisions of the spec */
	}

	if (net->name_short) {
		name_len = strlen(net->name_short);
		/* 10.5.3.5a */
		ptr8 = (u_int8_t *) msgb_put(msg, 3);
		ptr8[0] = GSM48_IE_NAME_LONG;
		ptr8[1] = name_len*2 + 1;
		ptr8[2] = 0x90; /* UCS2, no spare bits, no CI */

		ptr16 = (u_int16_t *) msgb_put(msg, name_len*2);
		for (i = 0; i < name_len; i++)
			ptr16[i] = htons(net->name_short[i]);
	}

#if 0
	/* move back to the top */
	time_t cur_t;
	struct tm* cur_time;
	int tz15min;
	/* Section 10.5.3.9 */
	cur_t = time(NULL);
	cur_time = gmtime(cur_t);
	ptr8 = msgb_put(msg, 8);
	ptr8[0] = GSM48_IE_NET_TIME_TZ;
	ptr8[1] = to_bcd8(cur_time->tm_year % 100);
	ptr8[2] = to_bcd8(cur_time->tm_mon);
	ptr8[3] = to_bcd8(cur_time->tm_mday);
	ptr8[4] = to_bcd8(cur_time->tm_hour);
	ptr8[5] = to_bcd8(cur_time->tm_min);
	ptr8[6] = to_bcd8(cur_time->tm_sec);
	/* 02.42: coded as BCD encoded signed value in units of 15 minutes */
	tz15min = (cur_time->tm_gmtoff)/(60*15);
	ptr8[6] = to_bcd8(tz15min);
	if (tz15min < 0)
		ptr8[6] |= 0x80;
#endif

	return gsm48_sendmsg(msg);
}

static int gsm48_tx_mm_serv_ack(struct gsm_lchan *lchan)
{
	DEBUGP(DMM, "-> CM SERVICE ACK\n");
	return gsm48_tx_simple(lchan, GSM48_PDISC_MM, GSM48_MT_MM_CM_SERV_ACC);
}

/* 9.2.6 CM service reject */
static int gsm48_tx_mm_serv_rej(struct gsm_lchan *lchan,
				enum gsm48_reject_value value)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh;

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh) + 1);

	msg->lchan = lchan;
	use_lchan(lchan);

	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_CM_SERV_REJ;
	gh->data[0] = value;
	DEBUGP(DMM, "-> CM SERVICE Reject cause: %d\n", value);

	return gsm48_sendmsg(msg);
}


/*
 * Handle CM Service Requests
 * a) Verify that the packet is long enough to contain the information
 *    we require otherwsie reject with INCORRECT_MESSAGE
 * b) Try to parse the TMSI. If we do not have one reject
 * c) Check that we know the subscriber with the TMSI otherwise reject
 *    with a HLR cause
 * d) Set the subscriber on the gsm_lchan and accept
 */
static int gsm48_rx_mm_serv_req(struct msgb *msg)
{
	u_int8_t mi_type;
	char mi_string[MI_SIZE];

	struct gsm_subscriber *subscr;
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm48_service_request *req =
			(struct gsm48_service_request *)gh->data;

	if (msg->data_len < sizeof(struct gsm48_service_request*)) {
		DEBUGP(DMM, "<- CM SERVICE REQUEST wrong sized message\n");
		return gsm48_tx_mm_serv_rej(msg->lchan,
					    GSM48_REJECT_INCORRECT_MESSAGE);
	}

	if (msg->data_len < req->mi_len + 6) {
		DEBUGP(DMM, "<- CM SERVICE REQUEST MI does not fit in package\n");
		return gsm48_tx_mm_serv_rej(msg->lchan,
					    GSM48_REJECT_INCORRECT_MESSAGE);
	}

	mi_type = req->mi[0] & GSM_MI_TYPE_MASK;
	if (mi_type != GSM_MI_TYPE_TMSI) {
		DEBUGP(DMM, "<- CM SERVICE REQUEST mi type is not TMSI: %d\n", mi_type);
		return gsm48_tx_mm_serv_rej(msg->lchan,
					    GSM48_REJECT_INCORRECT_MESSAGE);
	}

	mi_to_string(mi_string, sizeof(mi_string), req->mi, req->mi_len);
	subscr = subscr_get_by_tmsi(mi_string);
	DEBUGP(DMM, "<- CM SERVICE REQUEST serv_type=0x%02x mi_type=0x%02x M(%s)\n",
		req->cm_service_type, mi_type, mi_string);

	if (!subscr)
		return gsm48_tx_mm_serv_rej(msg->lchan,
					    GSM48_REJECT_IMSI_UNKNOWN_IN_HLR);

	if (!msg->lchan->subscr)
		msg->lchan->subscr = subscr;
	else if (msg->lchan->subscr != subscr) {
		DEBUGP(DMM, "<- CM Channel already owned by someone else?\n");
		subscr_put(subscr);
	}

	return gsm48_tx_mm_serv_ack(msg->lchan);
}

/* Receive a GSM 04.08 Mobility Management (MM) message */
static int gsm0408_rcv_mm(struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	int rc;

	switch (gh->msg_type & 0xbf) {
	case GSM48_MT_MM_LOC_UPD_REQUEST:
		DEBUGP(DMM, "LOCATION UPDATING REQUEST\n");
		rc = mm_rx_loc_upd_req(msg);
		break;
	case GSM48_MT_MM_ID_RESP:
		rc = mm_rx_id_resp(msg);
		break;
	case GSM48_MT_MM_CM_SERV_REQ:
		rc = gsm48_rx_mm_serv_req(msg);
		break;
	case GSM48_MT_MM_STATUS:
		DEBUGP(DMM, "MM STATUS: FIXME parse error cond.\n");
		break;
	case GSM48_MT_MM_TMSI_REALL_COMPL:
		DEBUGP(DMM, "TMSI Reallocation Completed. Subscriber: %s\n",
		       msg->lchan->subscr ?
				msg->lchan->subscr->imsi :
				"unknown subscriber");
		break;
	case GSM48_MT_MM_CM_REEST_REQ:
	case GSM48_MT_MM_AUTH_RESP:
	case GSM48_MT_MM_IMSI_DETACH_IND:
		fprintf(stderr, "Unimplemented GSM 04.08 MM msg type 0x%02x\n",
			gh->msg_type);
		break;
	default:
		fprintf(stderr, "Unknown GSM 04.08 MM msg type 0x%02x\n",
			gh->msg_type);
		break;
	}

	return rc;
}

/* Receive a PAGING RESPONSE message from the MS */
static int gsm48_rr_rx_pag_resp(struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm48_paging_response *pr =
			(struct gsm48_paging_response *) gh->data;
	u_int8_t mi_type = pr->mi[0] & GSM_MI_TYPE_MASK;
	char mi_string[MI_SIZE];
	struct gsm_subscriber *subscr;
	struct paging_signal_data sig_data;
	int rc = 0;

	mi_to_string(mi_string, sizeof(mi_string), &pr->mi[0], pr->mi_len);
	DEBUGP(DRR, "PAGING RESPONSE: mi_type=0x%02x MI(%s)\n",
		mi_type, mi_string);
	subscr = subscr_get_by_tmsi(mi_string);

	if (!subscr) {
		DEBUGP(DRR, "<- Can't find any subscriber for this ID\n");
		/* FIXME: request id? close channel? */
		return -EINVAL;
	}
	DEBUGP(DRR, "<- Channel was requested by %s\n",
		subscr->name ? subscr->name : subscr->imsi);

	if (!msg->lchan->subscr)
		msg->lchan->subscr = subscr;
	else if (msg->lchan->subscr != subscr) {
		DEBUGP(DRR, "<- Channel already owned by someone else?\n");
		subscr_put(subscr);
	}

	sig_data.subscr = subscr;
	sig_data.bts	= msg->lchan->ts->trx->bts;
	sig_data.lchan	= msg->lchan;

	dispatch_signal(SS_PAGING, S_PAGING_COMPLETED, &sig_data);
	paging_request_stop(msg->trx->bts, subscr, msg->lchan);

	/* FIXME: somehow signal the completion of the PAGING to
	 * the entity that requested the paging */

	return rc;
}

/* Receive a GSM 04.08 Radio Resource (RR) message */
static int gsm0408_rcv_rr(struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	int rc = 0;

	switch (gh->msg_type) {
	case GSM48_MT_RR_CLSM_CHG:
		DEBUGP(DRR, "CLASSMARK CHANGE\n");
		/* FIXME: what to do ?!? */
		break;
	case GSM48_MT_RR_GPRS_SUSP_REQ:
		DEBUGP(DRR, "GRPS SUSPEND REQUEST\n");
		break;
	case GSM48_MT_RR_PAG_RESP:
		rc = gsm48_rr_rx_pag_resp(msg);
		break;
	case GSM48_MT_RR_CHAN_MODE_MODIF_ACK:
		DEBUGP(DRR, "CHANNEL MODE MODIFY ACK\n");
		rc = rsl_chan_mode_modify_req(msg->lchan);
		break;
	default:
		fprintf(stderr, "Unimplemented GSM 04.08 RR msg type 0x%02x\n",
			gh->msg_type);
		break;
	}

	return rc;
}

/* 7.1.7 and 9.1.7 Channel release*/
int gsm48_send_rr_release(struct gsm_lchan *lchan)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	u_int8_t *cause;

	msg->lchan = lchan;
	gh->proto_discr = GSM48_PDISC_RR;
	gh->msg_type = GSM48_MT_RR_CHAN_REL;

	cause = msgb_put(msg, 1);
	cause[0] = GSM48_RR_CAUSE_NORMAL;

	DEBUGP(DRR, "Sending Channel Release: Chan: Number: %d Type: %d\n",
		lchan->nr, lchan->type);

	return gsm48_sendmsg(msg);
}

/* Call Control */

/* The entire call control code is written in accordance with Figure 7.10c
 * for 'very early assignment', i.e. we allocate a TCH/F during IMMEDIATE
 * ASSIGN, then first use that TCH/F for signalling and later MODE MODIFY
 * it for voice */

static int gsm48_cc_tx_status(struct gsm_lchan *lchan)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	u_int8_t *cause, *call_state;

	gh->proto_discr = GSM48_PDISC_CC;

	msg->lchan = lchan;

	gh->msg_type = GSM48_MT_CC_STATUS;

	cause = msgb_put(msg, 3);
	cause[0] = 2;
	cause[1] = GSM48_CAUSE_CS_GSM | GSM48_CAUSE_LOC_USER;
	cause[2] = 0x80 | 30;	/* response to status inquiry */

	call_state = msgb_put(msg, 1);
	call_state[0] = 0xc0 | 0x00;

	return gsm48_sendmsg(msg);
}

static int gsm48_tx_simple(struct gsm_lchan *lchan,
			   u_int8_t pdisc, u_int8_t msg_type)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	msg->lchan = lchan;

	gh->proto_discr = pdisc;
	gh->msg_type = msg_type;

	return gsm48_sendmsg(msg);
}

/* call-back from paging the B-end of the connection */
static int setup_trig_pag_evt(unsigned int hooknum, unsigned int event,
			      struct msgb *msg, void *_lchan, void *param)
{
	struct gsm_lchan *lchan = _lchan;
	struct gsm_call *remote_call = param;
	struct gsm_call *call = &lchan->call;
	int rc = 0;

	if (hooknum != GSM_HOOK_RR_PAGING)
		return -EINVAL;

	switch (event) {
	case GSM_PAGING_SUCCEEDED:
		DEBUGP(DCC, "paging succeeded!\n");
		remote_call->remote_lchan = lchan;
		call->remote_lchan = remote_call->local_lchan;
		/* send SETUP request to called party */
		rc = gsm48_cc_tx_setup(lchan, call->remote_lchan->subscr);
		if (is_ipaccess_bts(lchan->ts->trx->bts))
			rsl_ipacc_bind(lchan);
		break;
	case GSM_PAGING_EXPIRED:
		DEBUGP(DCC, "paging expired!\n");
		/* notify caller that we cannot reach called party */
		/* FIXME: correct cause, etc */
		rc = gsm48_tx_simple(remote_call->local_lchan, GSM48_PDISC_CC,
				     GSM48_MT_CC_RELEASE_COMPL);
		break;
	}
	return rc;
}

static int gsm48_cc_rx_status_enq(struct msgb *msg)
{
	return gsm48_cc_tx_status(msg->lchan);
}

static int gsm48_cc_rx_setup(struct msgb *msg)
{
	struct gsm_call *call = &msg->lchan->call;
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct gsm_subscriber *called_subscr;
	char called_number[(43-2)*2 + 1] = "\0";
	struct tlv_parsed tp;
	u_int8_t num_type;
	int ret;

	if (call->state == GSM_CSTATE_NULL ||
	    call->state == GSM_CSTATE_RELEASE_REQ)
		use_lchan(msg->lchan);

	call->type = GSM_CT_MO;
	call->state = GSM_CSTATE_INITIATED;
	call->local_lchan = msg->lchan;
	call->transaction_id = gh->proto_discr & 0xf0;

	tlv_parse(&tp, &rsl_att_tlvdef, gh->data, payload_len);
	if (!TLVP_PRESENT(&tp, GSM48_IE_CALLED_BCD))
		goto err;

	/* Parse the number that was dialed and lookup subscriber */
	num_type = decode_bcd_number(called_number, sizeof(called_number),
				     TLVP_VAL(&tp, GSM48_IE_CALLED_BCD)-1);

	DEBUGP(DCC, "A -> SETUP(tid=0x%02x,number='%s')\n", call->transaction_id,
		called_number);

	called_subscr = subscr_get_by_extension(called_number);
	if (!called_subscr) {
		DEBUGP(DCC, "could not find subscriber, RELEASE\n");
		put_lchan(msg->lchan);
		return gsm48_tx_simple(msg->lchan, GSM48_PDISC_CC,
				GSM48_MT_CC_RELEASE_COMPL);
	}

	subscr_get(msg->lchan->subscr);
	call->called_subscr = called_subscr;

	/* start paging of the receiving end of the call */
	paging_request(msg->trx->bts, called_subscr, RSL_CHANNEED_TCH_F,
			setup_trig_pag_evt, call);

	/* send a CALL PROCEEDING message to the MO */
	ret = gsm48_tx_simple(msg->lchan, GSM48_PDISC_CC,
			       GSM48_MT_CC_CALL_PROC);

	if (is_ipaccess_bts(msg->trx->bts))
		rsl_ipacc_bind(msg->lchan);

	/* change TCH/F mode to voice */ 
	return gsm48_tx_chan_mode_modify(msg->lchan, GSM48_CMODE_SPEECH_EFR);

err:
	/* FIXME: send some kind of RELEASE */
	return 0;
}

static int gsm48_cc_rx_alerting(struct msgb *msg)
{
	struct gsm_call *call = &msg->lchan->call;

	DEBUGP(DCC, "A -> ALERTING\n");

	/* forward ALERTING to other party */
	if (!call->remote_lchan)
		return -EIO;

	DEBUGP(DCC, "B <- ALERTING\n");
	return gsm48_tx_simple(call->remote_lchan, GSM48_PDISC_CC,
			       GSM48_MT_CC_ALERTING);
}

/* map two ipaccess RTP streams onto each other */
static int ipacc_map(struct gsm_lchan *lchan, struct gsm_lchan *remote_lchan)
{
	struct gsm_bts_trx_ts *ts;

	ts = remote_lchan->ts;
	rsl_ipacc_connect(lchan, ts->abis_ip.bound_ip, ts->abis_ip.bound_port,
			  lchan->ts->abis_ip.attr_f8, ts->abis_ip.attr_fc);
	
	ts = lchan->ts;
	rsl_ipacc_connect(remote_lchan, ts->abis_ip.bound_ip, ts->abis_ip.bound_port,
			  remote_lchan->ts->abis_ip.attr_f8, ts->abis_ip.attr_fc);

	return 0;
}

static int gsm48_cc_rx_connect(struct msgb *msg)
{
	struct gsm_call *call = &msg->lchan->call;
	int rc;

	DEBUGP(DCC, "A -> CONNECT\n");
	DEBUGP(DCC, "A <- CONNECT ACK\n");
	/* MT+MO: need to respond with CONNECT_ACK and pass on */
	rc = gsm48_tx_simple(msg->lchan, GSM48_PDISC_CC,
			     GSM48_MT_CC_CONNECT_ACK);

	if (!call->remote_lchan)
		return -EIO;

	if (is_ipaccess_bts(msg->trx->bts))
		ipacc_map(msg->lchan, call->remote_lchan);

	/* forward CONNECT to other party */
	DEBUGP(DCC, "B <- CONNECT\n");
	return gsm48_tx_simple(call->remote_lchan, GSM48_PDISC_CC,
			       GSM48_MT_CC_CONNECT);
}

static int gsm48_cc_rx_disconnect(struct msgb *msg)
{
	struct gsm_call *call = &msg->lchan->call;
	int rc;


	/* Section 5.4.3.2 */
	DEBUGP(DCC, "A -> DISCONNECT (state->RELEASE_REQ)\n");
	call->state = GSM_CSTATE_RELEASE_REQ;
	if (call->state != GSM_CSTATE_NULL)
		put_lchan(msg->lchan);
	/* FIXME: clear the network connection */
	DEBUGP(DCC, "A <- RELEASE\n");
	rc = gsm48_tx_simple(msg->lchan, GSM48_PDISC_CC,
			     GSM48_MT_CC_RELEASE);

	/* forward DISCONNECT to other party */
	if (!call->remote_lchan)
		return -EIO;

	DEBUGP(DCC, "B <- DISCONNECT\n");
	return gsm48_tx_simple(call->remote_lchan, GSM48_PDISC_CC,
			       GSM48_MT_CC_DISCONNECT);
}

static const u_int8_t calling_bcd[] = { 0xb9, 0x32, 0x24 };

int gsm48_cc_tx_setup(struct gsm_lchan *lchan, 
		      struct gsm_subscriber *calling_subscr)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh;
	struct gsm_call *call = &lchan->call;
	u_int8_t bcd_lv[19];

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	call->type = GSM_CT_MT;

	call->local_lchan = msg->lchan = lchan;
	use_lchan(lchan);

	gh->proto_discr = GSM48_PDISC_CC;
	gh->msg_type = GSM48_MT_CC_SETUP;

	msgb_tv_put(msg, GSM48_IE_SIGNAL, GSM48_SIGNAL_DIALTONE);
	if (calling_subscr) {
		encode_bcd_number(bcd_lv, sizeof(bcd_lv), 0xb9,
				  calling_subscr->extension);
		msgb_tlv_put(msg, GSM48_IE_CALLING_BCD,
			     bcd_lv[0], bcd_lv+1);
	}
	if (lchan->subscr) {
		encode_bcd_number(bcd_lv, sizeof(bcd_lv), 0xb9,
				  lchan->subscr->extension);
		msgb_tlv_put(msg, GSM48_IE_CALLED_BCD,
			     bcd_lv[0], bcd_lv+1);
	}

	DEBUGP(DCC, "B <- SETUP\n");

	return gsm48_sendmsg(msg);
}

static int gsm0408_rcv_cc(struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	u_int8_t msg_type = gh->msg_type & 0xbf;
	struct gsm_call *call = &msg->lchan->call;
	int rc = 0;

	switch (msg_type) {
	case GSM48_MT_CC_CALL_CONF:
		/* Response to SETUP */
		DEBUGP(DCC, "-> CALL CONFIRM\n");
		/* we now need to MODIFY the channel */
		rc = gsm48_tx_chan_mode_modify(msg->lchan, GSM48_CMODE_SPEECH_EFR);
		break;
	case GSM48_MT_CC_RELEASE_COMPL:
		/* Answer from MS to RELEASE */
		DEBUGP(DCC, "-> RELEASE COMPLETE (state->NULL)\n");
		call->state = GSM_CSTATE_NULL;
		break;
	case GSM48_MT_CC_ALERTING:
		rc = gsm48_cc_rx_alerting(msg);
		break;
	case GSM48_MT_CC_CONNECT:
		rc = gsm48_cc_rx_connect(msg);
		break;
	case GSM48_MT_CC_CONNECT_ACK:
		/* MO: Answer to CONNECT */
		call->state = GSM_CSTATE_ACTIVE;
		DEBUGP(DCC, "-> CONNECT_ACK (state->ACTIVE)\n");
		break;
	case GSM48_MT_CC_RELEASE:
		DEBUGP(DCC, "-> RELEASE\n");
		DEBUGP(DCC, "<- RELEASE_COMPLETE\n");
		/* need to respond with RELEASE_COMPLETE */
		rc = gsm48_tx_simple(msg->lchan, GSM48_PDISC_CC,
				     GSM48_MT_CC_RELEASE_COMPL);
		put_lchan(msg->lchan);
                call->state = GSM_CSTATE_NULL;
		break;
	case GSM48_MT_CC_STATUS_ENQ:
		rc = gsm48_cc_rx_status_enq(msg);
		break;
	case GSM48_MT_CC_DISCONNECT:
		rc = gsm48_cc_rx_disconnect(msg);
		break;
	case GSM48_MT_CC_SETUP:
		rc = gsm48_cc_rx_setup(msg);
		break;
	case GSM48_MT_CC_EMERG_SETUP:
		DEBUGP(DCC, "-> EMERGENCY SETUP\n");
		/* FIXME: continue with CALL_PROCEEDING, ALERTING, CONNECT, RELEASE_COMPLETE */
		break;
	default:
		fprintf(stderr, "Unimplemented GSM 04.08 CC msg type 0x%02x\n",
			msg_type);
		break;
	}

	return rc;
}

/* here we pass in a msgb from the RSL->RLL.  We expect the l3 pointer to be set */
int gsm0408_rcvmsg(struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	u_int8_t pdisc = gh->proto_discr & 0x0f;
	int rc = 0;
	
	switch (pdisc) {
	case GSM48_PDISC_CC:
		rc = gsm0408_rcv_cc(msg);
		break;
	case GSM48_PDISC_MM:
		rc = gsm0408_rcv_mm(msg);
		break;
	case GSM48_PDISC_RR:
		rc = gsm0408_rcv_rr(msg);
		break;
	case GSM48_PDISC_SMS:
		rc = gsm0411_rcv_sms(msg);
		break;
	case GSM48_PDISC_MM_GPRS:
	case GSM48_PDISC_SM_GPRS:
		fprintf(stderr, "Unimplemented GSM 04.08 discriminator 0x%02d\n",
			pdisc);
		break;
	default:
		fprintf(stderr, "Unknown GSM 04.08 discriminator 0x%02d\n",
			pdisc);
		break;
	}

	return rc;
}

enum chreq_type {
	CHREQ_T_EMERG_CALL,
	CHREQ_T_CALL_REEST_TCH_F,
	CHREQ_T_CALL_REEST_TCH_H,
	CHREQ_T_CALL_REEST_TCH_H_DBL,
	CHREQ_T_SDCCH,
	CHREQ_T_TCH_F,
	CHREQ_T_VOICE_CALL_TCH_H,
	CHREQ_T_DATA_CALL_TCH_H,
	CHREQ_T_LOCATION_UPD,
	CHREQ_T_PAG_R_ANY,
	CHREQ_T_PAG_R_TCH_F,
	CHREQ_T_PAG_R_TCH_FH,
};

/* Section 9.1.8 / Table 9.9 */
struct chreq {
	u_int8_t val;
	u_int8_t mask;
	enum chreq_type type;
};

/* If SYSTEM INFORMATION TYPE 4 NECI bit == 1 */
static const struct chreq chreq_type_neci1[] = {
	{ 0xa0, 0xe0, CHREQ_T_EMERG_CALL },
	{ 0xc0, 0xe0, CHREQ_T_CALL_REEST_TCH_F },
	{ 0x68, 0xfc, CHREQ_T_CALL_REEST_TCH_H },
	{ 0x6c, 0xfc, CHREQ_T_CALL_REEST_TCH_H_DBL },
	{ 0xe0, 0xe0, CHREQ_T_SDCCH },
	{ 0x40, 0xf0, CHREQ_T_VOICE_CALL_TCH_H },
	{ 0x50, 0xf0, CHREQ_T_DATA_CALL_TCH_H },
	{ 0x00, 0xf0, CHREQ_T_LOCATION_UPD },
	{ 0x10, 0xf0, CHREQ_T_SDCCH },
	{ 0x80, 0xe0, CHREQ_T_PAG_R_ANY },
	{ 0x20, 0xf0, CHREQ_T_PAG_R_TCH_F },
	{ 0x30, 0xf0, CHREQ_T_PAG_R_TCH_FH },
};

/* If SYSTEM INFORMATION TYPE 4 NECI bit == 0 */
static const struct chreq chreq_type_neci0[] = {
	{ 0xa0, 0xe0, CHREQ_T_EMERG_CALL },
	{ 0xc0, 0xe0, CHREQ_T_CALL_REEST_TCH_H },
	{ 0xe0, 0xe0, CHREQ_T_TCH_F },
	{ 0x50, 0xf0, CHREQ_T_DATA_CALL_TCH_H },
	{ 0x00, 0xe0, CHREQ_T_LOCATION_UPD },
	{ 0x80, 0xe0, CHREQ_T_PAG_R_ANY },
	{ 0x20, 0xf0, CHREQ_T_PAG_R_TCH_F },
	{ 0x30, 0xf0, CHREQ_T_PAG_R_TCH_FH },
};

static const enum gsm_chan_t ctype_by_chreq[] = {
	[CHREQ_T_EMERG_CALL]		= GSM_LCHAN_TCH_F,
	[CHREQ_T_CALL_REEST_TCH_F]	= GSM_LCHAN_TCH_F,
	[CHREQ_T_CALL_REEST_TCH_H]	= GSM_LCHAN_TCH_H,
	[CHREQ_T_CALL_REEST_TCH_H_DBL]	= GSM_LCHAN_TCH_H,
	[CHREQ_T_SDCCH]			= GSM_LCHAN_SDCCH,
	[CHREQ_T_TCH_F]			= GSM_LCHAN_TCH_F,
	[CHREQ_T_VOICE_CALL_TCH_H]	= GSM_LCHAN_TCH_H,
	[CHREQ_T_DATA_CALL_TCH_H]	= GSM_LCHAN_TCH_H,
	[CHREQ_T_LOCATION_UPD]		= GSM_LCHAN_SDCCH,
	[CHREQ_T_PAG_R_ANY]		= GSM_LCHAN_SDCCH,
	[CHREQ_T_PAG_R_TCH_F]		= GSM_LCHAN_TCH_F,
	[CHREQ_T_PAG_R_TCH_FH]		= GSM_LCHAN_TCH_F,
};

static const enum gsm_chreq_reason_t reason_by_chreq[] = {
	[CHREQ_T_EMERG_CALL]		= GSM_CHREQ_REASON_EMERG,
	[CHREQ_T_CALL_REEST_TCH_F]	= GSM_CHREQ_REASON_CALL,
	[CHREQ_T_CALL_REEST_TCH_H]	= GSM_CHREQ_REASON_CALL,
	[CHREQ_T_CALL_REEST_TCH_H_DBL]	= GSM_CHREQ_REASON_CALL,
	[CHREQ_T_SDCCH]			= GSM_CHREQ_REASON_OTHER,
	[CHREQ_T_TCH_F]			= GSM_CHREQ_REASON_OTHER,
	[CHREQ_T_VOICE_CALL_TCH_H]	= GSM_CHREQ_REASON_OTHER,
	[CHREQ_T_DATA_CALL_TCH_H]	= GSM_CHREQ_REASON_OTHER,
	[CHREQ_T_LOCATION_UPD]		= GSM_CHREQ_REASON_LOCATION_UPD,
	[CHREQ_T_PAG_R_ANY]		= GSM_CHREQ_REASON_PAG,
	[CHREQ_T_PAG_R_TCH_F]		= GSM_CHREQ_REASON_PAG,
	[CHREQ_T_PAG_R_TCH_FH]		= GSM_CHREQ_REASON_PAG,
};

enum gsm_chan_t get_ctype_by_chreq(struct gsm_bts *bts, u_int8_t ra)
{
	int i;
	/* FIXME: determine if we set NECI = 0 in the BTS SI4 */

	for (i = 0; i < ARRAY_SIZE(chreq_type_neci0); i++) {
		const struct chreq *chr = &chreq_type_neci0[i];
		if ((ra & chr->mask) == chr->val)
			return ctype_by_chreq[chr->type];
	}
	fprintf(stderr, "Unknown CHANNEL REQUEST RQD 0x%02x\n", ra);
	return GSM_LCHAN_SDCCH;
}

enum gsm_chreq_reason_t get_reason_by_chreq(struct gsm_bts *bts, u_int8_t ra)
{
	int i;
	/* FIXME: determine if we set NECI = 0 in the BTS SI4 */

	for (i = 0; i < ARRAY_SIZE(chreq_type_neci0); i++) {
		const struct chreq *chr = &chreq_type_neci0[i];
		if ((ra & chr->mask) == chr->val)
			return reason_by_chreq[chr->type];
	}
	fprintf(stderr, "Unknown CHANNEL REQUEST REASON 0x%02x\n", ra);
	return GSM_CHREQ_REASON_OTHER;
}
