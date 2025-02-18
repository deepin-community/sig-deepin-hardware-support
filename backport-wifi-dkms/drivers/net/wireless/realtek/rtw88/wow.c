// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2018-2019  Realtek Corporation
 */

#include "main.h"
#include "fw.h"
#include "wow.h"
#include "reg.h"
#include "debug.h"
#include "mac.h"
#include "ps.h"

static void rtw_wow_show_wakeup_reason(struct rtw_dev *rtwdev)
{
	struct cfg80211_wowlan_nd_info nd_info;
	struct cfg80211_wowlan_wakeup wakeup = {
		.pattern_idx = -1,
	};
	u8 reason;

	reason = rtw_read8(rtwdev, REG_WOWLAN_WAKE_REASON);

	switch (reason) {
	case RTW_WOW_RSN_RX_DEAUTH:
		wakeup.disconnect = true;
		rtw_dbg(rtwdev, RTW_DBG_WOW, "WOW: Rx deauth\n");
		break;
	case RTW_WOW_RSN_DISCONNECT:
		wakeup.disconnect = true;
		rtw_dbg(rtwdev, RTW_DBG_WOW, "WOW: AP is off\n");
		break;
	case RTW_WOW_RSN_RX_MAGIC_PKT:
		wakeup.magic_pkt = true;
		rtw_dbg(rtwdev, RTW_DBG_WOW, "WOW: Rx magic packet\n");
		break;
	case RTW_WOW_RSN_RX_GTK_REKEY:
		wakeup.gtk_rekey_failure = true;
		rtw_dbg(rtwdev, RTW_DBG_WOW, "WOW: Rx gtk rekey\n");
		break;
	case RTW_WOW_RSN_RX_PATTERN_MATCH:
		/* Current firmware and driver don't report pattern index
		 * Use pattern_idx to 0 defaultly.
		 */
		wakeup.pattern_idx = 0;
		rtw_dbg(rtwdev, RTW_DBG_WOW, "WOW: Rx pattern match packet\n");
		break;
	case RTW_WOW_RSN_RX_NLO:
		/* Current firmware and driver don't report ssid index.
		 * Use 0 for n_matches based on its comment.
		 */
		nd_info.n_matches = 0;
		wakeup.net_detect = &nd_info;
		rtw_dbg(rtwdev, RTW_DBG_WOW, "Rx NLO\n");
		break;
	default:
		rtw_warn(rtwdev, "Unknown wakeup reason %x\n", reason);
		ieee80211_report_wowlan_wakeup(rtwdev->wow.wow_vif, NULL,
					       GFP_KERNEL);
		return;
	}
	ieee80211_report_wowlan_wakeup(rtwdev->wow.wow_vif, &wakeup,
				       GFP_KERNEL);
}

static void rtw_wow_pattern_write_cam(struct rtw_dev *rtwdev, u8 addr,
				      u32 wdata)
{
	rtw_write32(rtwdev, REG_WKFMCAM_RWD, wdata);
	rtw_write32(rtwdev, REG_WKFMCAM_CMD, BIT_WKFCAM_POLLING_V1 |
		    BIT_WKFCAM_WE | BIT_WKFCAM_ADDR_V2(addr));

	if (!check_hw_ready(rtwdev, REG_WKFMCAM_CMD, BIT_WKFCAM_POLLING_V1, 0))
		rtw_err(rtwdev, "failed to write pattern cam\n");
}

static void rtw_wow_pattern_write_cam_ent(struct rtw_dev *rtwdev, u8 id,
					  struct rtw_wow_pattern *rtw_pattern)
{
	int i;
	u8 addr;
	u32 wdata;

	for (i = 0; i < RTW_MAX_PATTERN_MASK_SIZE / 4; i++) {
		addr = (id << 3) + i;
		wdata = rtw_pattern->mask[i * 4];
		wdata |= rtw_pattern->mask[i * 4 + 1] << 8;
		wdata |= rtw_pattern->mask[i * 4 + 2] << 16;
		wdata |= rtw_pattern->mask[i * 4 + 3] << 24;
		rtw_wow_pattern_write_cam(rtwdev, addr, wdata);
	}

	wdata = rtw_pattern->crc;
	addr = (id << 3) + RTW_MAX_PATTERN_MASK_SIZE / 4;

	switch (rtw_pattern->type) {
	case RTW_PATTERN_BROADCAST:
		wdata |= BIT_WKFMCAM_BC | BIT_WKFMCAM_VALID;
		break;
	case RTW_PATTERN_MULTICAST:
		wdata |= BIT_WKFMCAM_MC | BIT_WKFMCAM_VALID;
		break;
	case RTW_PATTERN_UNICAST:
		wdata |= BIT_WKFMCAM_UC | BIT_WKFMCAM_VALID;
		break;
	case RTW_PATTERN_WILDCARD:
		wdata |= BIT_WKFMCAM_VALID;
		break;
	default:
		break;
	}
	rtw_wow_pattern_write_cam(rtwdev, addr, wdata);
}

/* RTK internal CRC16 for Pattern Cam */
static u16 __rtw_cal_crc16(u8 data, u16 crc)
{
	u8 shift_in, data_bit;
	u8 crc_bit4, crc_bit11, crc_bit15;
	u16 crc_result;
	int index;

	for (index = 0; index < 8; index++) {
		crc_bit15 = ((crc & BIT(15)) ? 1 : 0);
		data_bit = (data & (BIT(0) << index) ? 1 : 0);
		shift_in = crc_bit15 ^ data_bit;

		crc_result = crc << 1;

		if (shift_in == 0)
			crc_result &= (~BIT(0));
		else
			crc_result |= BIT(0);

		crc_bit11 = ((crc & BIT(11)) ? 1 : 0) ^ shift_in;

		if (crc_bit11 == 0)
			crc_result &= (~BIT(12));
		else
			crc_result |= BIT(12);

		crc_bit4 = ((crc & BIT(4)) ? 1 : 0) ^ shift_in;

		if (crc_bit4 == 0)
			crc_result &= (~BIT(5));
		else
			crc_result |= BIT(5);

		crc = crc_result;
	}
	return crc;
}

static u16 rtw_calc_crc(u8 *pdata, int length)
{
	u16 crc = 0xffff;
	int i;

	for (i = 0; i < length; i++)
		crc = __rtw_cal_crc16(pdata[i], crc);

	/* get 1' complement */
	return ~crc;
}

static int rtw_wow_pattern_get_type(struct rtw_vif *rtwvif,
				    const u8 *pattern, u8 da_mask)
{
	u8 da[ETH_ALEN];
	u8 type;

	ether_addr_copy_mask(da, pattern, da_mask);

	/* Each pattern is divided into different kinds by DA address
	 *  a. DA is broadcast address
	 *  b. DA is multicast address
	 *  c. DA is unicast address same as dev's mac address
	 *  d. DA is unmasked. Also called wildcard type.
	 *  e. Others is invalid type.
	 */
	if (!da_mask)
		type = RTW_PATTERN_WILDCARD;
	else if (is_broadcast_ether_addr(da))
		type = RTW_PATTERN_BROADCAST;
	else if (is_multicast_ether_addr(da))
		type = RTW_PATTERN_MULTICAST;
	else if (ether_addr_equal(da, rtwvif->mac_addr) &&
		 (da_mask == GENMASK(5, 0)))
		type = RTW_PATTERN_UNICAST;
	else
		type = RTW_PATTERN_INVALID;

	return type;
}

static int rtw_wow_pattern_generate(struct rtw_dev *rtwdev,
				    struct rtw_vif *rtwvif,
				    const struct cfg80211_pkt_pattern *pkt_pattern,
				    struct rtw_wow_pattern *rtw_pattern)
{
	const u8 *mask;
	const u8 *pattern;
	u8 mask_hw[RTW_MAX_PATTERN_MASK_SIZE] = {0};
	u8 content[RTW_MAX_PATTERN_SIZE] = {0};
	u8 mask_len;
	u8 type;
	u16 count;
	int len;
	int i;

	pattern = pkt_pattern->pattern;
	len = pkt_pattern->pattern_len;
	mask = pkt_pattern->mask;
	mask_len = DIV_ROUND_UP(len, 8);
	memset(rtw_pattern, 0, sizeof(*rtw_pattern));

	type = rtw_wow_pattern_get_type(rtwvif, pattern,
					mask[0] & GENMASK(5, 0));
	if (type == RTW_PATTERN_INVALID)
		return -EPERM;

	rtw_pattern->type = type;

	/* translate mask from os to mask for hw
	 * pattern from OS uses 'ethenet frame', like this:
	 * |    6   |    6   |   2  |     20    |  Variable  |  4  |
	 * |--------+--------+------+-----------+------------+-----|
	 * |    802.3 Mac Header    | IP Header | TCP Packet | FCS |
	 * |   DA   |   SA   | Type |
	 *
	 * BUT, packet catched by our HW is in '802.11 frame', begin from LLC
	 * |     24 or 30      |    6   |   2  |     20    |  Variable  |  4  |
	 * |-------------------+--------+------+-----------+------------+-----|
	 * | 802.11 MAC Header |       LLC     | IP Header | TCP Packet | FCS |
	 *		       | Others | Tpye |
	 *
	 * Therefore, we need translate mask_from_OS to mask_to_hw.
	 * We should left-shift mask by 6 bits, then set the new bit[0~5] = 0,
	 * because new mask[0~5] means 'SA', but our HW packet begins from LLC,
	 * bit[0~5] corresponds to first 6 Bytes in LLC, they just don't match.
	 */

	/* Shift 6 bits */
	for (i = 0; i < mask_len - 1; i++) {
		mask_hw[i] = u8_get_bits(mask[i], GENMASK(7, 6));
		mask_hw[i] |= u8_get_bits(mask[i + 1], GENMASK(5, 0)) << 2;
	}
	mask_hw[i] = u8_get_bits(mask[i], GENMASK(7, 6));

	/* Set bit 0-5 to zero */
	mask_hw[0] &= (~GENMASK(5, 0));

	memcpy(rtw_pattern->mask, mask_hw, RTW_MAX_PATTERN_MASK_SIZE);

	/* To get the wake up pattern from the mask.
	 * We do not count first 12 bits which means
	 * DA[6] and SA[6] in the pattern to match HW design.
	 */
	count = 0;
	for (i = 12; i < len; i++) {
		if ((mask[i / 8] >> (i % 8)) & 0x01) {
			content[count] = pattern[i];
			count++;
		}
	}

	rtw_pattern->crc = rtw_calc_crc(content, count);

	return 0;
}

static int rtw_wow_parse_patterns(struct rtw_dev *rtwdev,
				  struct rtw_vif *rtwvif,
				  struct cfg80211_wowlan *wowlan)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;
	struct rtw_wow_pattern *rtw_patterns = rtw_wow->patterns;
	int i;
	int ret;

	if (!wowlan->n_patterns || !wowlan->patterns)
		return 0;

	for (i = 0; i < wowlan->n_patterns; i++) {
		ret = rtw_wow_pattern_generate(rtwdev, rtwvif,
					       wowlan->patterns + i,
					       rtw_patterns + i);
		if (ret) {
			rtw_err(rtwdev, "failed to generate pattern(%d)\n", i);
			rtw_wow->pattern_cnt = 0;
			return ret;
		}
	}
	rtw_wow->pattern_cnt = wowlan->n_patterns;

	return 0;
}

static void rtw_wow_pattern_clear_cam(struct rtw_dev *rtwdev)
{
	bool ret;

	rtw_write32(rtwdev, REG_WKFMCAM_CMD, BIT_WKFCAM_POLLING_V1 |
		    BIT_WKFCAM_CLR_V1);

	ret = check_hw_ready(rtwdev, REG_WKFMCAM_CMD, BIT_WKFCAM_POLLING_V1, 0);
	if (!ret)
		rtw_err(rtwdev, "failed to clean pattern cam\n");
}

static void rtw_wow_pattern_write(struct rtw_dev *rtwdev)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;
	struct rtw_wow_pattern *rtw_pattern = rtw_wow->patterns;
	int i = 0;

	for (i = 0; i < rtw_wow->pattern_cnt; i++)
		rtw_wow_pattern_write_cam_ent(rtwdev, i, rtw_pattern + i);
}

static void rtw_wow_pattern_clear(struct rtw_dev *rtwdev)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;

	rtw_wow_pattern_clear_cam(rtwdev);

	rtw_wow->pattern_cnt = 0;
	memset(rtw_wow->patterns, 0, sizeof(rtw_wow->patterns));
}

static void rtw_wow_bb_stop(struct rtw_dev *rtwdev)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;

	/* wait 100ms for firmware to finish TX */
	msleep(100);

	if (!rtw_read32_mask(rtwdev, REG_BCNQ_INFO, BIT_MGQ_CPU_EMPTY))
		rtw_warn(rtwdev, "Wrong status of MGQ_CPU empty!\n");

	rtw_wow->txpause = rtw_read8(rtwdev, REG_TXPAUSE);
	rtw_write8(rtwdev, REG_TXPAUSE, 0xff);
	rtw_write8_clr(rtwdev, REG_SYS_FUNC_EN, BIT_FEN_BB_RSTB);
}

static void rtw_wow_bb_start(struct rtw_dev *rtwdev)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;

	rtw_write8_set(rtwdev, REG_SYS_FUNC_EN, BIT_FEN_BB_RSTB);
	rtw_write8(rtwdev, REG_TXPAUSE, rtw_wow->txpause);
}

static void rtw_wow_rx_dma_stop(struct rtw_dev *rtwdev)
{
	/* wait 100ms for HW to finish rx dma */
	msleep(100);

	rtw_write32_set(rtwdev, REG_RXPKT_NUM, BIT_RW_RELEASE);

	if (!check_hw_ready(rtwdev, REG_RXPKT_NUM, BIT_RXDMA_IDLE, 1))
		rtw_err(rtwdev, "failed to stop rx dma\n");
}

static void rtw_wow_rx_dma_start(struct rtw_dev *rtwdev)
{
	rtw_write32_clr(rtwdev, REG_RXPKT_NUM, BIT_RW_RELEASE);
}

static int rtw_wow_check_fw_status(struct rtw_dev *rtwdev, bool wow_enable)
{
	int ret;
	u8 check;
	u32 check_dis;

	if (wow_enable) {
		ret = read_poll_timeout(rtw_read8, check, !check, 1000,
					100000, true, rtwdev,
					REG_WOWLAN_WAKE_REASON);
		if (ret)
			goto wow_fail;
	} else {
		ret = read_poll_timeout(rtw_read32_mask, check_dis,
					!check_dis, 1000, 100000, true, rtwdev,
					REG_FE1IMR, BIT_FS_RXDONE);
		if (ret)
			goto wow_fail;
		ret = read_poll_timeout(rtw_read32_mask, check_dis,
					!check_dis, 1000, 100000, false, rtwdev,
					REG_RXPKT_NUM, BIT_RW_RELEASE);
		if (ret)
			goto wow_fail;
	}

	return 0;

wow_fail:
	rtw_err(rtwdev, "failed to check wow status %s\n",
		wow_enable ? "enabled" : "disabled");
	return -EBUSY;
}

static void rtw_wow_fw_security_type_iter(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif,
					  struct ieee80211_sta *sta,
					  struct ieee80211_key_conf *key,
					  void *data)
{
	struct rtw_fw_key_type_iter_data *iter_data = data;
	struct rtw_dev *rtwdev = hw->priv;
	u8 hw_key_type;

	if (vif != rtwdev->wow.wow_vif)
		return;

	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		hw_key_type = RTW_CAM_WEP40;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		hw_key_type = RTW_CAM_WEP104;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		hw_key_type = RTW_CAM_TKIP;
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIC;
		break;
	case WLAN_CIPHER_SUITE_CCMP:
		hw_key_type = RTW_CAM_AES;
		key->flags |= IEEE80211_KEY_FLAG_SW_MGMT_TX;
		break;
	default:
		rtw_err(rtwdev, "Unsupported key type for wowlan mode: %#x\n",
			key->cipher);
		hw_key_type = 0;
		break;
	}

	if (sta)
		iter_data->pairwise_key_type = hw_key_type;
	else
		iter_data->group_key_type = hw_key_type;
}

static void rtw_wow_fw_security_type(struct rtw_dev *rtwdev)
{
	struct rtw_fw_key_type_iter_data data = {};
	struct ieee80211_vif *wow_vif = rtwdev->wow.wow_vif;

	data.rtwdev = rtwdev;
	rtw_iterate_keys(rtwdev, wow_vif,
			 rtw_wow_fw_security_type_iter, &data);
	rtw_fw_set_aoac_global_info_cmd(rtwdev, data.pairwise_key_type,
					data.group_key_type);
}

static int rtw_wow_fw_start(struct rtw_dev *rtwdev)
{
	if (rtw_wow_mgd_linked(rtwdev)) {
		rtw_send_rsvd_page_h2c(rtwdev);
		rtw_wow_pattern_write(rtwdev);
		rtw_wow_fw_security_type(rtwdev);
		rtw_fw_set_disconnect_decision_cmd(rtwdev, true);
		rtw_fw_set_keep_alive_cmd(rtwdev, true);
	} else if (rtw_wow_no_link(rtwdev)) {
		rtw_fw_set_nlo_info(rtwdev, true);
		rtw_fw_update_pkt_probe_req(rtwdev, NULL);
		rtw_fw_channel_switch(rtwdev, true);
	}

	rtw_fw_set_wowlan_ctrl_cmd(rtwdev, true);
	rtw_fw_set_remote_wake_ctrl_cmd(rtwdev, true);

	return rtw_wow_check_fw_status(rtwdev, true);
}

static int rtw_wow_fw_stop(struct rtw_dev *rtwdev)
{
	if (rtw_wow_mgd_linked(rtwdev)) {
		rtw_fw_set_disconnect_decision_cmd(rtwdev, false);
		rtw_fw_set_keep_alive_cmd(rtwdev, false);
		rtw_wow_pattern_clear(rtwdev);
	} else if (rtw_wow_no_link(rtwdev)) {
		rtw_fw_channel_switch(rtwdev, false);
		rtw_fw_set_nlo_info(rtwdev, false);
	}

	rtw_fw_set_wowlan_ctrl_cmd(rtwdev, false);
	rtw_fw_set_remote_wake_ctrl_cmd(rtwdev, false);

	return rtw_wow_check_fw_status(rtwdev, false);
}

static void rtw_wow_avoid_reset_mac(struct rtw_dev *rtwdev)
{
	/* When resuming from wowlan mode, some hosts issue signal
	 * (PCIE: PREST, USB: SE0RST) to device, and lead to reset
	 * mac core. If it happens, the connection to AP will be lost.
	 * Setting REG_RSV_CTRL Register can avoid this process.
	 */
	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
	case RTW_HCI_TYPE_USB:
		rtw_write8(rtwdev, REG_RSV_CTRL, BIT_WLOCK_1C_B6);
		rtw_write8(rtwdev, REG_RSV_CTRL,
			   BIT_WLOCK_1C_B6 | BIT_R_DIS_PRST);
		break;
	default:
		rtw_warn(rtwdev, "Unsupported hci type to disable reset MAC\n");
		break;
	}
}

static void rtw_wow_fw_media_status_iter(void *data, struct ieee80211_sta *sta)
{
	struct rtw_sta_info *si = (struct rtw_sta_info *)sta->drv_priv;
	struct rtw_fw_media_status_iter_data *iter_data = data;
	struct rtw_dev *rtwdev = iter_data->rtwdev;

	rtw_fw_media_status_report(rtwdev, si->mac_id, iter_data->connect);
}

static void rtw_wow_fw_media_status(struct rtw_dev *rtwdev, bool connect)
{
	struct rtw_fw_media_status_iter_data data;

	data.rtwdev = rtwdev;
	data.connect = connect;

	rtw_iterate_stas_atomic(rtwdev, rtw_wow_fw_media_status_iter, &data);
}

static int rtw_wow_config_wow_fw_rsvd_page(struct rtw_dev *rtwdev)
{
	struct ieee80211_vif *wow_vif = rtwdev->wow.wow_vif;
	struct rtw_vif *rtwvif = (struct rtw_vif *)wow_vif->drv_priv;

	rtw_remove_rsvd_page(rtwdev, rtwvif);

	if (rtw_wow_no_link(rtwdev))
		rtw_add_rsvd_page_pno(rtwdev, rtwvif);
	else
		rtw_add_rsvd_page_sta(rtwdev, rtwvif);

	return rtw_fw_download_rsvd_page(rtwdev);
}

static int rtw_wow_config_normal_fw_rsvd_page(struct rtw_dev *rtwdev)
{
	struct ieee80211_vif *wow_vif = rtwdev->wow.wow_vif;
	struct rtw_vif *rtwvif = (struct rtw_vif *)wow_vif->drv_priv;

	rtw_remove_rsvd_page(rtwdev, rtwvif);
	rtw_add_rsvd_page_sta(rtwdev, rtwvif);

	if (rtw_wow_no_link(rtwdev))
		return 0;

	return rtw_fw_download_rsvd_page(rtwdev);
}

static int rtw_wow_swap_fw(struct rtw_dev *rtwdev, enum rtw_fw_type type)
{
	struct rtw_fw_state *fw;
	int ret;

	switch (type) {
	case RTW_WOWLAN_FW:
		fw = &rtwdev->wow_fw;
		break;

	case RTW_NORMAL_FW:
		fw = &rtwdev->fw;
		break;

	default:
		rtw_warn(rtwdev, "unsupported firmware type to swap\n");
		return -ENOENT;
	}

	ret = rtw_download_firmware(rtwdev, fw);
	if (ret)
		goto out;

	rtw_fw_send_general_info(rtwdev);
	rtw_fw_send_phydm_info(rtwdev);
	rtw_wow_fw_media_status(rtwdev, true);

out:
	return ret;
}

static void rtw_wow_check_pno(struct rtw_dev *rtwdev,
			      struct cfg80211_sched_scan_request *nd_config)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;
	struct rtw_pno_request *pno_req = &rtw_wow->pno_req;
	struct ieee80211_channel *channel;
	int i, size;

	if (!nd_config->n_match_sets || !nd_config->n_channels)
		goto err;

	pno_req->match_set_cnt = nd_config->n_match_sets;
	size = sizeof(*pno_req->match_sets) * pno_req->match_set_cnt;
	pno_req->match_sets = kmemdup(nd_config->match_sets, size, GFP_KERNEL);
	if (!pno_req->match_sets)
		goto err;

	pno_req->channel_cnt = nd_config->n_channels;
	size = sizeof(*nd_config->channels[0]) * nd_config->n_channels;
	pno_req->channels = kmalloc(size, GFP_KERNEL);
	if (!pno_req->channels)
		goto channel_err;

	for (i = 0 ; i < pno_req->channel_cnt; i++) {
		channel = pno_req->channels + i;
		memcpy(channel, nd_config->channels[i], sizeof(*channel));
	}

	pno_req->scan_plan = *nd_config->scan_plans;
	pno_req->inited = true;

	rtw_dbg(rtwdev, RTW_DBG_WOW, "WOW: net-detect is enabled\n");

	return;

channel_err:
	kfree(pno_req->match_sets);

err:
	rtw_dbg(rtwdev, RTW_DBG_WOW, "WOW: net-detect is disabled\n");
}

static int rtw_wow_leave_linked_ps(struct rtw_dev *rtwdev)
{
	if (!test_bit(RTW_FLAG_WOWLAN, rtwdev->flags))
		cancel_delayed_work_sync(&rtwdev->watch_dog_work);

	rtw_leave_lps(rtwdev);

	return 0;
}

static int rtw_wow_leave_no_link_ps(struct rtw_dev *rtwdev)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;
	int ret = 0;

	if (test_bit(RTW_FLAG_WOWLAN, rtwdev->flags)) {
		if (rtw_get_lps_deep_mode(rtwdev) != LPS_DEEP_MODE_NONE)
			rtw_leave_lps_deep(rtwdev);
	} else {
		if (!test_bit(RTW_FLAG_POWERON, rtwdev->flags)) {
			rtw_wow->ips_enabled = true;
			ret = rtw_leave_ips(rtwdev);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int rtw_wow_leave_ps(struct rtw_dev *rtwdev)
{
	int ret = 0;

	if (rtw_wow_mgd_linked(rtwdev))
		ret = rtw_wow_leave_linked_ps(rtwdev);
	else if (rtw_wow_no_link(rtwdev))
		ret = rtw_wow_leave_no_link_ps(rtwdev);

	return ret;
}

static int rtw_wow_restore_ps(struct rtw_dev *rtwdev)
{
	int ret = 0;

	if (rtw_wow_no_link(rtwdev) && rtwdev->wow.ips_enabled)
		ret = rtw_enter_ips(rtwdev);

	return ret;
}

static int rtw_wow_enter_linked_ps(struct rtw_dev *rtwdev)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;
	struct ieee80211_vif *wow_vif = rtw_wow->wow_vif;
	struct rtw_vif *rtwvif = (struct rtw_vif *)wow_vif->drv_priv;

	rtw_enter_lps(rtwdev, rtwvif->port);

	return 0;
}

static int rtw_wow_enter_no_link_ps(struct rtw_dev *rtwdev)
{
	/* firmware enters deep ps by itself if supported */
	set_bit(RTW_FLAG_LEISURE_PS_DEEP, rtwdev->flags);

	return 0;
}

static int rtw_wow_enter_ps(struct rtw_dev *rtwdev)
{
	int ret = 0;

	if (rtw_wow_mgd_linked(rtwdev))
		ret = rtw_wow_enter_linked_ps(rtwdev);
	else if (rtw_wow_no_link(rtwdev) &&
		 rtw_get_lps_deep_mode(rtwdev) != LPS_DEEP_MODE_NONE)
		ret = rtw_wow_enter_no_link_ps(rtwdev);

	return ret;
}

static void rtw_wow_stop_trx(struct rtw_dev *rtwdev)
{
	rtw_wow_bb_stop(rtwdev);
	rtw_wow_rx_dma_stop(rtwdev);
}

static int rtw_wow_start(struct rtw_dev *rtwdev)
{
	int ret;

	ret = rtw_wow_fw_start(rtwdev);
	if (ret)
		goto out;

	rtw_hci_stop(rtwdev);
	rtw_wow_bb_start(rtwdev);
	rtw_wow_avoid_reset_mac(rtwdev);

out:
	return ret;
}

static int rtw_wow_enable(struct rtw_dev *rtwdev)
{
	int ret = 0;

	rtw_wow_stop_trx(rtwdev);

	ret = rtw_wow_swap_fw(rtwdev, RTW_WOWLAN_FW);
	if (ret) {
		rtw_err(rtwdev, "failed to swap wow fw\n");
		goto error;
	}

	set_bit(RTW_FLAG_WOWLAN, rtwdev->flags);

	ret = rtw_wow_config_wow_fw_rsvd_page(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to download wowlan rsvd page\n");
		goto error;
	}

	ret = rtw_wow_start(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to start wow\n");
		goto error;
	}

	return ret;

error:
	clear_bit(RTW_FLAG_WOWLAN, rtwdev->flags);
	return ret;
}

static int rtw_wow_stop(struct rtw_dev *rtwdev)
{
	int ret;

	/* some HCI related registers will be reset after resume,
	 * need to set them again.
	 */
	ret = rtw_hci_setup(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to setup hci\n");
		return ret;
	}

	ret = rtw_hci_start(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to start hci\n");
		return ret;
	}

	ret = rtw_wow_fw_stop(rtwdev);
	if (ret)
		rtw_err(rtwdev, "failed to stop wowlan fw\n");

	rtw_wow_bb_stop(rtwdev);

	return ret;
}

static void rtw_wow_resume_trx(struct rtw_dev *rtwdev)
{
	rtw_wow_rx_dma_start(rtwdev);
	rtw_wow_bb_start(rtwdev);
	ieee80211_queue_delayed_work(rtwdev->hw, &rtwdev->watch_dog_work,
				     RTW_WATCH_DOG_DELAY_TIME);
}

static int rtw_wow_disable(struct rtw_dev *rtwdev)
{
	int ret;

	clear_bit(RTW_FLAG_WOWLAN, rtwdev->flags);

	ret = rtw_wow_stop(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to stop wow\n");
		goto out;
	}

	ret = rtw_wow_swap_fw(rtwdev, RTW_NORMAL_FW);
	if (ret) {
		rtw_err(rtwdev, "failed to swap normal fw\n");
		goto out;
	}

	ret = rtw_wow_config_normal_fw_rsvd_page(rtwdev);
	if (ret)
		rtw_err(rtwdev, "failed to download normal rsvd page\n");

out:
	rtw_wow_resume_trx(rtwdev);
	return ret;
}

static void rtw_wow_vif_iter(void *data, u8 *mac, struct ieee80211_vif *vif)
{
	struct rtw_dev *rtwdev = data;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;

	/* Current wowlan function support setting of only one STATION vif.
	 * So when one suitable vif is found, stop the iteration.
	 */
	if (rtw_wow->wow_vif || vif->type != NL80211_IFTYPE_STATION)
		return;

	switch (rtwvif->net_type) {
	case RTW_NET_MGD_LINKED:
		rtw_wow->wow_vif = vif;
		break;
	case RTW_NET_NO_LINK:
		if (rtw_wow->pno_req.inited)
			rtwdev->wow.wow_vif = vif;
		break;
	default:
		break;
	}
}

static int rtw_wow_set_wakeups(struct rtw_dev *rtwdev,
			       struct cfg80211_wowlan *wowlan)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;
	struct rtw_vif *rtwvif;

	if (wowlan->disconnect)
		set_bit(RTW_WOW_FLAG_EN_DISCONNECT, rtw_wow->flags);
	if (wowlan->magic_pkt)
		set_bit(RTW_WOW_FLAG_EN_MAGIC_PKT, rtw_wow->flags);
	if (wowlan->gtk_rekey_failure)
		set_bit(RTW_WOW_FLAG_EN_REKEY_PKT, rtw_wow->flags);

	if (wowlan->nd_config)
		rtw_wow_check_pno(rtwdev, wowlan->nd_config);

	rtw_iterate_vifs_atomic(rtwdev, rtw_wow_vif_iter, rtwdev);
	if (!rtw_wow->wow_vif)
		return -EPERM;

	rtwvif = (struct rtw_vif *)rtw_wow->wow_vif->drv_priv;
	return rtw_wow_parse_patterns(rtwdev, rtwvif, wowlan);
}

static void rtw_wow_clear_wakeups(struct rtw_dev *rtwdev)
{
	struct rtw_wow_param *rtw_wow = &rtwdev->wow;
	struct rtw_pno_request *pno_req = &rtw_wow->pno_req;

	if (pno_req->inited) {
		kfree(pno_req->channels);
		kfree(pno_req->match_sets);
	}

	memset(rtw_wow, 0, sizeof(rtwdev->wow));
}

int rtw_wow_suspend(struct rtw_dev *rtwdev, struct cfg80211_wowlan *wowlan)
{
	int ret = 0;

	ret = rtw_wow_set_wakeups(rtwdev, wowlan);
	if (ret) {
		rtw_err(rtwdev, "failed to set wakeup event\n");
		goto out;
	}

	ret = rtw_wow_leave_ps(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to leave ps from normal mode\n");
		goto out;
	}

	ret = rtw_wow_enable(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to enable wow\n");
		rtw_wow_restore_ps(rtwdev);
		goto out;
	}

	ret = rtw_wow_enter_ps(rtwdev);
	if (ret)
		rtw_err(rtwdev, "failed to enter ps for wow\n");

out:
	return ret;
}

int rtw_wow_resume(struct rtw_dev *rtwdev)
{
	int ret;

	/* If wowlan mode is not enabled, do nothing */
	if (!test_bit(RTW_FLAG_WOWLAN, rtwdev->flags)) {
		rtw_err(rtwdev, "wow is not enabled\n");
		ret = -EPERM;
		goto out;
	}

	ret = rtw_wow_leave_ps(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to leave ps from wowlan mode\n");
		goto out;
	}

	rtw_wow_show_wakeup_reason(rtwdev);

	ret = rtw_wow_disable(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to disable wow\n");
		goto out;
	}

	ret = rtw_wow_restore_ps(rtwdev);
	if (ret)
		rtw_err(rtwdev, "failed to restore ps to normal mode\n");

out:
	rtw_wow_clear_wakeups(rtwdev);
	return ret;
}
