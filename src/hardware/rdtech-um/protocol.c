/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2018-2020 Andreas Sandberg <andreas@sandberg.pp.se>
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

#include <config.h>

#include <glib.h>
#include <libsigrok/libsigrok.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "libsigrok-internal.h"
#include "protocol.h"

#define SERIAL_WRITE_TIMEOUT_MS 1

#define UM_POLL_LEN 130
#define UM_POLL_PERIOD_MS 100
#define UM_TIMEOUT_MS 1000

#define UM_CMD_POLL 0xf0

static const struct binary_analog_channel rdtech_default_channels[] = {
	{ "V", { 2, BVT_BE_UINT16, 0.01, }, 2, SR_MQ_VOLTAGE, SR_UNIT_VOLT },
	{ "I", { 4, BVT_BE_UINT16, 0.001, }, 3, SR_MQ_CURRENT, SR_UNIT_AMPERE },
	{ "D+", { 96, BVT_BE_UINT16, 0.01, }, 2, SR_MQ_VOLTAGE, SR_UNIT_VOLT },
	{ "D-", { 98, BVT_BE_UINT16, 0.01, }, 2, SR_MQ_VOLTAGE, SR_UNIT_VOLT },
	{ "T", { 10, BVT_BE_UINT16, 1.0, }, 0, SR_MQ_TEMPERATURE, SR_UNIT_CELSIUS },
	/* Threshold-based recording (mWh) */
	{ "E", { 106, BVT_BE_UINT32, 0.001, }, 3, SR_MQ_ENERGY, SR_UNIT_WATT_HOUR },
	ALL_ZERO,
};

static const struct binary_analog_channel rdtech_um25c_channels[] = {
	{ "V", { 2, BVT_BE_UINT16, 0.001, }, 3, SR_MQ_VOLTAGE, SR_UNIT_VOLT },
	{ "I", { 4, BVT_BE_UINT16, 0.0001, }, 4, SR_MQ_CURRENT, SR_UNIT_AMPERE },
	{ "D+", { 96, BVT_BE_UINT16, 0.01, }, 2, SR_MQ_VOLTAGE, SR_UNIT_VOLT },
	{ "D-", { 98, BVT_BE_UINT16, 0.01, }, 2, SR_MQ_VOLTAGE, SR_UNIT_VOLT },
	{ "T", { 10, BVT_BE_UINT16, 1.0, }, 0, SR_MQ_TEMPERATURE, SR_UNIT_CELSIUS },
	/* Threshold-based recording (mWh) */
	{ "E", { 106, BVT_BE_UINT32, 0.001, }, 3, SR_MQ_ENERGY, SR_UNIT_WATT_HOUR },
	ALL_ZERO,
};

static gboolean csum_ok_fff1(const uint8_t *buf, size_t len)
{
	uint16_t csum_recv;

	if (len != UM_POLL_LEN)
		return FALSE;

	csum_recv = read_u16be(&buf[len - sizeof(uint16_t)]);
	if (csum_recv != 0xfff1)
		return FALSE;

	return TRUE;
}

static gboolean csum_ok_um34c(const uint8_t *buf, size_t len)
{
	static const int positions[] = {
		1, 3, 7, 9, 15, 17, 19, 23, 31, 39, 41, 45, 49, 53,
		55, 57, 59, 63, 67, 69, 73, 79, 83, 89, 97, 99, 109,
		111, 113, 119, 121, 127,
	};

	size_t i;
	uint8_t csum_calc, csum_recv;

	if (len != UM_POLL_LEN)
		return FALSE;

	csum_calc = 0;
	for (i = 0; i < ARRAY_SIZE(positions); i++)
		csum_calc ^= buf[positions[i]];
	csum_recv = read_u8(&buf[len - sizeof(uint8_t)]);
	if (csum_recv != csum_calc)
		return FALSE;

	return TRUE;
}

static const struct rdtech_um_profile um_profiles[] = {
	{ "UM24C", RDTECH_UM24C, rdtech_default_channels, csum_ok_fff1, },
	{ "UM25C", RDTECH_UM25C, rdtech_um25c_channels, csum_ok_fff1, },
	{ "UM34C", RDTECH_UM34C, rdtech_default_channels, csum_ok_um34c, },
};

static const struct rdtech_um_profile *find_profile(uint16_t id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(um_profiles); i++) {
		if (um_profiles[i].model_id == id)
			return &um_profiles[i];
	}
	return NULL;
}

SR_PRIV const struct rdtech_um_profile *rdtech_um_probe(struct sr_serial_dev_inst *serial)
{
	const struct rdtech_um_profile *p;
	uint8_t request;
	uint8_t buf[RDTECH_UM_BUFSIZE];
	int len;

	request = UM_CMD_POLL;
	if (serial_write_blocking(serial, &request, sizeof(request),
			SERIAL_WRITE_TIMEOUT_MS) < 0) {
		sr_err("Unable to send probe request.");
		return NULL;
	}

	len = serial_read_blocking(serial, buf, UM_POLL_LEN, UM_TIMEOUT_MS);
	if (len != UM_POLL_LEN) {
		sr_err("Failed to read probe response.");
		return NULL;
	}

	p = find_profile(RB16(&buf[0]));
	if (!p) {
		sr_err("Unrecognized UM device (0x%.4" PRIx16 ")!", RB16(&buf[0]));
		return NULL;
	}

	if (!p->csum_ok(buf, len)) {
		sr_err("Probe response contains illegal checksum or end marker.\n");
		return NULL;
	}

	return p;
}

SR_PRIV int rdtech_um_poll(const struct sr_dev_inst *sdi, gboolean force)
{
	struct dev_context *devc;
	int64_t now, elapsed;
	struct sr_serial_dev_inst *serial;
	uint8_t request;

	/* Check for expired intervals or forced requests. */
	devc = sdi->priv;
	now = g_get_monotonic_time() / 1000;
	elapsed = now - devc->cmd_sent_at;
	if (!force && elapsed < UM_POLL_PERIOD_MS)
		return SR_OK;

	/* Send another poll request. Update interval only on success. */
	serial = sdi->conn;
	request = UM_CMD_POLL;
	if (serial_write_blocking(serial, &request, sizeof(request),
			SERIAL_WRITE_TIMEOUT_MS) < 0) {
		sr_err("Unable to send poll request.");
		return SR_ERR;
	}
	devc->cmd_sent_at = now;

	return SR_OK;
}

static void handle_poll_data(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int i;
	GSList *ch;

	devc = sdi->priv;
	sr_spew("Received poll packet (len: %zu).", devc->buflen);
	if (devc->buflen != UM_POLL_LEN) {
		sr_err("Unexpected poll packet length: %zu", devc->buflen);
		return;
	}

	for (ch = sdi->channels, i = 0; ch; ch = g_slist_next(ch), i++) {
		bv_send_analog_channel(sdi, ch->data,
			&devc->profile->channels[i],
			devc->buf, devc->buflen);
	}

	sr_sw_limits_update_samples_read(&devc->limits, 1);
}

static void recv_poll_data(struct sr_dev_inst *sdi, struct sr_serial_dev_inst *serial)
{
	struct dev_context *devc;
	const struct rdtech_um_profile *p;
	int len;

	/* Serial data arrived. */
	devc = sdi->priv;
	p = devc->profile;
	while (devc->buflen < UM_POLL_LEN) {
		len = serial_read_nonblocking(serial, devc->buf + devc->buflen, 1);
		if (len < 1)
			return;

		devc->buflen++;

		/* Check if the poll model ID matches the profile. */
		if (devc->buflen == 2 && RB16(devc->buf) != p->model_id) {
			sr_warn("Illegal model ID in poll response (0x%.4" PRIx16 "),"
				" skipping 1 byte.",
				RB16(devc->buf));
			devc->buflen--;
			memmove(devc->buf, devc->buf + 1, devc->buflen);
		}
	}

	if (devc->buflen == UM_POLL_LEN && p->csum_ok(devc->buf, devc->buflen))
		handle_poll_data(sdi);
	else
		sr_warn("Skipping packet with illegal checksum / end marker.");

	devc->buflen = 0;
}

SR_PRIV int rdtech_um_receive_data(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;

	(void)fd;

	if (!(sdi = cb_data))
		return TRUE;

	if (!(devc = sdi->priv))
		return TRUE;

	serial = sdi->conn;
	if (revents == G_IO_IN)
		recv_poll_data(sdi, serial);

	if (sr_sw_limits_check(&devc->limits)) {
		sr_dev_acquisition_stop(sdi);
		return TRUE;
	}

	(void)rdtech_um_poll(sdi, FALSE);

	return TRUE;
}
