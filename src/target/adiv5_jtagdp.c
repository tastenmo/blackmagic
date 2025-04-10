/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
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

/* This file implements the JTAG-DP specific functions of the
 * ARM Debug Interface v5 Architecture Specification, ARM doc IHI0031A.
 */

#include "general.h"
#include "exception.h"
#include "adiv5.h"
#include "jtag_scan.h"
#include "jtagtap.h"
#include "morse.h"

#define JTAGDP_ACK_OK   0x02U
#define JTAGDP_ACK_WAIT 0x01U

/* 35-bit registers that control the ADIv5 DP */
#define IR_ABORT 0x8U
#define IR_DPACC 0xaU
#define IR_APACC 0xbU

static uint32_t adiv5_jtagdp_error(adiv5_debug_port_s *dp, bool protocol_recovery);

void adiv5_jtag_dp_handler(uint8_t jd_index)
{
	adiv5_debug_port_s *dp = calloc(1, sizeof(*dp));
	if (!dp) { /* calloc failed: heap exhaustion */
		DEBUG_WARN("calloc: failed in %s\n", __func__);
		return;
	}

	dp->dp_jd_index = jd_index;

	dp->dp_read = fw_adiv5_jtagdp_read;
	dp->error = adiv5_jtagdp_error;
	dp->low_access = fw_adiv5_jtagdp_low_access;
	dp->abort = adiv5_jtagdp_abort;
#if PC_HOSTED == 1
	platform_jtag_dp_init(dp);
#endif

	adiv5_dp_init(dp, jtag_devs[jd_index].jd_idcode);
}

uint32_t fw_adiv5_jtagdp_read(adiv5_debug_port_s *dp, uint16_t addr)
{
	fw_adiv5_jtagdp_low_access(dp, ADIV5_LOW_READ, addr, 0);
	return fw_adiv5_jtagdp_low_access(dp, ADIV5_LOW_READ, ADIV5_DP_RDBUFF, 0);
}

static uint32_t adiv5_jtagdp_error(adiv5_debug_port_s *dp, const bool protocol_recovery)
{
	(void)protocol_recovery;
	const uint32_t status = fw_adiv5_jtagdp_read(dp, ADIV5_DP_CTRLSTAT) & ADIV5_DP_CTRLSTAT_ERRMASK;
	dp->fault = 0;
	return fw_adiv5_jtagdp_low_access(dp, ADIV5_LOW_WRITE, ADIV5_DP_CTRLSTAT, status) & 0x32U;
}

uint32_t fw_adiv5_jtagdp_low_access(adiv5_debug_port_s *dp, uint8_t RnW, uint16_t addr, uint32_t value)
{
	const bool APnDP = addr & ADIV5_APnDP;
	addr &= 0xffU;

	const uint64_t request = ((uint64_t)value << 3U) | ((addr >> 1U) & 0x06U) | (RnW ? 1U : 0U);

	uint32_t result;
	uint8_t ack;

	jtag_dev_write_ir(dp->dp_jd_index, APnDP ? IR_APACC : IR_DPACC);

	platform_timeout_s timeout;
	platform_timeout_set(&timeout, 250);
	do {
		uint64_t response;
		jtag_dev_shift_dr(dp->dp_jd_index, (uint8_t *)&response, (uint8_t *)&request, 35);
		result = response >> 3U;
		ack = response & 0x07U;
	} while (!platform_timeout_is_expired(&timeout) && ack == JTAGDP_ACK_WAIT);

	if (ack == JTAGDP_ACK_WAIT) {
		DEBUG_WARN("JTAG access resulted in wait, aborting\n");
		dp->abort(dp, ADIV5_DP_ABORT_DAPABORT);
		dp->fault = 1;
		return 0;
	}

	if (ack != JTAGDP_ACK_OK) {
		DEBUG_WARN("JTAG access resulted in: %" PRIx32 ":%x\n", result, ack);
		raise_exception(EXCEPTION_ERROR, "JTAG-DP invalid ACK");
	}

	return result;
}

void adiv5_jtagdp_abort(adiv5_debug_port_s *dp, uint32_t abort)
{
	uint64_t request = (uint64_t)abort << 3U;
	jtag_dev_write_ir(dp->dp_jd_index, IR_ABORT);
	jtag_dev_shift_dr(dp->dp_jd_index, NULL, (const uint8_t *)&request, 35);
}
