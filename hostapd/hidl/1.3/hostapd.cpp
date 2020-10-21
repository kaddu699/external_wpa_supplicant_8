/*
 * hidl interface for wpa_hostapd daemon
 * Copyright (c) 2004-2018, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2004-2018, Roshan Pius <rpius@google.com>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/stringprintf.h>

#include "hostapd.h"
#include "hidl_return_util.h"

extern "C"
{
#include "common/wpa_ctrl.h"
}

// The HIDL implementation for hostapd creates a hostapd.conf dynamically for
// each interface. This file can then be used to hook onto the normal config
// file parsing logic in hostapd code.  Helps us to avoid duplication of code
// in the HIDL interface.
// TOOD(b/71872409): Add unit tests for this.
namespace {
constexpr char kConfFileNameFmt[] = "/data/vendor/wifi/hostapd/hostapd_%s.conf";

using android::base::RemoveFileIfExists;
using android::base::StringPrintf;
using android::base::WriteStringToFile;
using android::hardware::wifi::hostapd::V1_3::IHostapd;
using android::hardware::wifi::hostapd::V1_3::Generation;
using android::hardware::wifi::hostapd::V1_3::Bandwidth;

std::string WriteHostapdConfig(
    const std::string& interface_name, const std::string& config)
{
	const std::string file_path =
	    StringPrintf(kConfFileNameFmt, interface_name.c_str());
	if (WriteStringToFile(
		config, file_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
		getuid(), getgid())) {
		return file_path;
	}
	// Diagnose failure
	int error = errno;
	wpa_printf(
	    MSG_ERROR, "Cannot write hostapd config to %s, error: %s",
	    file_path.c_str(), strerror(error));
	struct stat st;
	int result = stat(file_path.c_str(), &st);
	if (result == 0) {
		wpa_printf(
		    MSG_ERROR, "hostapd config file uid: %d, gid: %d, mode: %d",
		    st.st_uid, st.st_gid, st.st_mode);
	} else {
		wpa_printf(
		    MSG_ERROR,
		    "Error calling stat() on hostapd config file: %s",
		    strerror(errno));
	}
	return "";
}

/*
 * Get the op_class for a channel/band
 * The logic here is based on Table E-4 in the 802.11 Specification
 */
int getOpClassForChannel(int channel, int band, bool support11n, bool support11ac) {
	// 2GHz Band
	if ((band & IHostapd::BandMask::BAND_2_GHZ) != 0) {
		if (channel == 14) {
			return 82;
		}
		if (channel >= 1 && channel <= 13) {
			if (!support11n) {
				//20MHz channel
				return 81;
			}
			if (channel <= 9) {
				// HT40 with secondary channel above primary
				return 83;
			}
			// HT40 with secondary channel below primary
			return 84;
		}
		// Error
		return 0;
	}

	// 5GHz Band
	if ((band & IHostapd::BandMask::BAND_5_GHZ) != 0) {
		if (support11ac) {
			switch (channel) {
				case 42:
				case 58:
				case 106:
				case 122:
				case 138:
				case 155:
					// 80MHz channel
					return 128;
				case 50:
				case 114:
					// 160MHz channel
					return 129;
			}
		}

		if (!support11n) {
			if (channel >= 36 && channel <= 48) {
				return 115;
			}
			if (channel >= 52 && channel <= 64) {
				return 118;
			}
			if (channel >= 100 && channel <= 144) {
				return 121;
			}
			if (channel >= 149 && channel <= 161) {
				return 124;
			}
			if (channel >= 165 && channel <= 169) {
				return 125;
			}
		} else {
			switch (channel) {
				case 36:
				case 44:
					// HT40 with secondary channel above primary
					return 116;
				case 40:
				case 48:
					// HT40 with secondary channel below primary
					return 117;
				case 52:
				case 60:
					// HT40 with secondary channel above primary
					return  119;
				case 56:
				case 64:
					// HT40 with secondary channel below primary
					return 120;
				case 100:
				case 108:
				case 116:
				case 124:
				case 132:
				case 140:
					// HT40 with secondary channel above primary
					return 122;
				case 104:
				case 112:
				case 120:
				case 128:
				case 136:
				case 144:
					// HT40 with secondary channel below primary
					return 123;
				case 149:
				case 157:
					// HT40 with secondary channel above primary
					return 126;
				case 153:
				case 161:
					// HT40 with secondary channel below primary
					return 127;
			}
		}
		// Error
		return 0;
	}

	// 6GHz Band
	if ((band & IHostapd::BandMask::BAND_6_GHZ) != 0) {
		// Channels 1, 5. 9, 13, ...
		if ((channel & 0x03) == 0x01) {
			// 20MHz channel
			return 131;
		}
		// Channels 3, 11, 19, 27, ...
		if ((channel & 0x07) == 0x03) {
			// 40MHz channel
			return 132;
		}
		// Channels 7, 23, 39, 55, ...
		if ((channel & 0x0F) == 0x07) {
			// 80MHz channel
			return 133;
		}
		// Channels 15, 47, 69, ...
		if ((channel & 0x1F) == 0x0F) {
			// 160MHz channel
			return 134;
		}
		if (channel == 2) {
			// 20MHz channel
			return 136;
		}
		// Error
		return 0;
	}

	return 0;
}

bool validatePassphrase(int passphrase_len, int min_len, int max_len)
{
	if (min_len != -1 && passphrase_len < min_len) return false;
	if (max_len != -1 && passphrase_len > max_len) return false;
	return true;
}

std::string CreateHostapdConfig(
    const IHostapd::IfaceParams& iface_params,
    const IHostapd::NetworkParams& nw_params)
{
	if (nw_params.V1_2.V1_0.ssid.size() >
	    static_cast<uint32_t>(
		IHostapd::ParamSizeLimits::SSID_MAX_LEN_IN_BYTES)) {
		wpa_printf(
		    MSG_ERROR, "Invalid SSID size: %zu", nw_params.V1_2.V1_0.ssid.size());
		return "";
	}

	// SSID string
	std::stringstream ss;
	ss << std::hex;
	ss << std::setfill('0');
	for (uint8_t b : nw_params.V1_2.V1_0.ssid) {
		ss << std::setw(2) << static_cast<unsigned int>(b);
	}
	const std::string ssid_as_string = ss.str();

	// Encryption config string
	uint32_t band = 0;
	band |= iface_params.channelParams.bandMask;
	bool is_6Ghz_band_only = band == static_cast<uint32_t>(IHostapd::BandMask::BAND_6_GHZ);
	std::string encryption_config_as_string;
	switch (nw_params.V1_2.encryptionType) {
	case IHostapd::EncryptionType::NONE:
		// no security params
		break;
	case IHostapd::EncryptionType::WPA:
		if (!validatePassphrase(
		    nw_params.V1_2.passphrase.size(),
		    static_cast<uint32_t>(IHostapd::ParamSizeLimits::
				WPA2_PSK_PASSPHRASE_MIN_LEN_IN_BYTES),
		    static_cast<uint32_t>(IHostapd::ParamSizeLimits::
				WPA2_PSK_PASSPHRASE_MAX_LEN_IN_BYTES))) {
			return "";
		}
		encryption_config_as_string = StringPrintf(
		    "wpa=3\n"
		    "wpa_pairwise=TKIP CCMP\n"
		    "wpa_passphrase=%s",
		    nw_params.V1_2.passphrase.c_str());
		break;
	case IHostapd::EncryptionType::WPA2:
		if (!validatePassphrase(
		    nw_params.V1_2.passphrase.size(),
		    static_cast<uint32_t>(IHostapd::ParamSizeLimits::
				WPA2_PSK_PASSPHRASE_MIN_LEN_IN_BYTES),
		    static_cast<uint32_t>(IHostapd::ParamSizeLimits::
				WPA2_PSK_PASSPHRASE_MAX_LEN_IN_BYTES))) {
			return "";
		}
		encryption_config_as_string = StringPrintf(
		    "wpa=2\n"
		    "rsn_pairwise=CCMP\n"
		    "wpa_passphrase=%s",
		    nw_params.V1_2.passphrase.c_str());
		break;
	case IHostapd::EncryptionType::WPA3_SAE_TRANSITION:
		if (!validatePassphrase(
		    nw_params.V1_2.passphrase.size(),
		    static_cast<uint32_t>(IHostapd::ParamSizeLimits::
				WPA2_PSK_PASSPHRASE_MIN_LEN_IN_BYTES),
		    static_cast<uint32_t>(IHostapd::ParamSizeLimits::
				WPA2_PSK_PASSPHRASE_MAX_LEN_IN_BYTES))) {
			return "";
		}
		encryption_config_as_string = StringPrintf(
		    "wpa=2\n"
		    "rsn_pairwise=CCMP\n"
		    "wpa_key_mgmt=WPA-PSK SAE\n"
		    "ieee80211w=1\n"
		    "sae_require_mfp=1\n"
		    "wpa_passphrase=%s\n"
		    "sae_password=%s",
		    nw_params.V1_2.passphrase.c_str(),
		    nw_params.V1_2.passphrase.c_str());
		break;
	case IHostapd::EncryptionType::WPA3_SAE:
		if (!validatePassphrase(nw_params.V1_2.passphrase.size(), 1, -1)) {
			return "";
		}
		encryption_config_as_string = StringPrintf(
		    "wpa=2\n"
		    "rsn_pairwise=CCMP\n"
		    "wpa_key_mgmt=SAE\n"
		    "ieee80211w=2\n"
		    "sae_require_mfp=2\n"
		    "sae_pwe=%d\n"
		    "sae_password=%s",
		    is_6Ghz_band_only ? 1 : 2,
		    nw_params.V1_2.passphrase.c_str());
		break;
	default:
		wpa_printf(MSG_ERROR, "Unknown encryption type");
		return "";
	}

	std::string channel_config_as_string;
	bool isFirst = true;
	if (iface_params.V1_1.V1_0.channelParams.enableAcs) {
		std::string freqList_as_string;
		for (const auto &range :
		    iface_params.channelParams.acsChannelFreqRangesMhz) {
			if (!isFirst) {
				freqList_as_string += ",";
			}
			isFirst = false;

			if (range.start != range.end) {
				freqList_as_string +=
				    StringPrintf("%d-%d", range.start, range.end);
			} else {
				freqList_as_string += StringPrintf("%d", range.start);
			}
		}
		channel_config_as_string = StringPrintf(
		    "channel=0\n"
		    "acs_exclude_dfs=%d\n"
		    "freqlist=%s",
		    iface_params.V1_1.V1_0.channelParams.acsShouldExcludeDfs,
		    freqList_as_string.c_str());
	} else {
		int op_class = getOpClassForChannel(
		    iface_params.V1_1.V1_0.channelParams.channel,
		    band,
		    iface_params.V1_1.V1_0.hwModeParams.enable80211N,
		    iface_params.V1_1.V1_0.hwModeParams.enable80211AC);
		channel_config_as_string = StringPrintf(
		    "channel=%d\n"
		    "op_class=%d",
		    iface_params.V1_1.V1_0.channelParams.channel, op_class);
	}

	std::string hw_mode_as_string;
	std::string ht_cap_vht_oper_chwidth_as_string;

	if ((band & IHostapd::BandMask::BAND_2_GHZ) != 0) {
		if (((band & IHostapd::BandMask::BAND_5_GHZ) != 0)
		    || ((band & IHostapd::BandMask::BAND_6_GHZ) != 0)) {
			hw_mode_as_string = "hw_mode=any";
			if (iface_params.V1_1.V1_0.channelParams.enableAcs) {
				ht_cap_vht_oper_chwidth_as_string =
				    "ht_capab=[HT40+]\n"
				    "vht_oper_chwidth=1";
			}
		} else {
			hw_mode_as_string = "hw_mode=g";
		}
	} else {
		if (((band & IHostapd::BandMask::BAND_5_GHZ) != 0)
		    || ((band & IHostapd::BandMask::BAND_6_GHZ) != 0)) {
			hw_mode_as_string = "hw_mode=a";
			if (iface_params.V1_1.V1_0.channelParams.enableAcs) {
				ht_cap_vht_oper_chwidth_as_string =
				    "ht_capab=[HT40+]\n"
				    "vht_oper_chwidth=1";
			}
		} else {
			wpa_printf(MSG_ERROR, "Invalid band");
			return "";
		}
	}

	std::string he_params_as_string;
#ifdef CONFIG_IEEE80211AX
	if (iface_params.hwModeParams.enable80211AX) {
		he_params_as_string = StringPrintf(
		    "ieee80211ax=1\n"
		    "he_su_beamformer=%d\n"
		    "he_su_beamformee=%d\n"
		    "he_mu_beamformer=%d\n"
		    "he_twt_required=%d\n",
		    iface_params.hwModeParams.enableHeSingleUserBeamformer ? 1 : 0,
		    iface_params.hwModeParams.enableHeSingleUserBeamformee ? 1 : 0,
		    iface_params.hwModeParams.enableHeMultiUserBeamformer ? 1 : 0,
		    iface_params.hwModeParams.enableHeTargetWakeTime ? 1 : 0);
	} else {
		he_params_as_string = "ieee80211ax=0";
	}
#endif /* CONFIG_IEEE80211AX */

#ifdef CONFIG_INTERWORKING
	std::string access_network_params_as_string;
	if (nw_params.isMetered) {
		access_network_params_as_string = StringPrintf(
		    "interworking=1\n"
		    "access_network_type=2\n"); // CHARGEABLE_PUBLIC_NETWORK
	} else {
	    access_network_params_as_string = StringPrintf(
		    "interworking=0\n");
	}
#endif /* CONFIG_INTERWORKING */

	return StringPrintf(
	    "interface=%s\n"
	    "driver=nl80211\n"
	    "ctrl_interface=/data/vendor/wifi/hostapd/ctrl\n"
	    // ssid2 signals to hostapd that the value is not a literal value
	    // for use as a SSID.  In this case, we're giving it a hex
	    // std::string and hostapd needs to expect that.
	    "ssid2=%s\n"
	    "%s\n"
	    "ieee80211n=%d\n"
	    "ieee80211ac=%d\n"
	    "%s\n"
	    "%s\n"
	    "%s\n"
	    "ignore_broadcast_ssid=%d\n"
	    "wowlan_triggers=any\n"
#ifdef CONFIG_INTERWORKING
	    "%s\n"
#endif /* CONFIG_INTERWORKING */
	    "%s\n",
	    iface_params.V1_1.V1_0.ifaceName.c_str(), ssid_as_string.c_str(),
	    channel_config_as_string.c_str(),
	    iface_params.V1_1.V1_0.hwModeParams.enable80211N ? 1 : 0,
	    iface_params.V1_1.V1_0.hwModeParams.enable80211AC ? 1 : 0,
	    he_params_as_string.c_str(),
	    hw_mode_as_string.c_str(), ht_cap_vht_oper_chwidth_as_string.c_str(),
	    nw_params.V1_2.V1_0.isHidden ? 1 : 0,
#ifdef CONFIG_INTERWORKING
            access_network_params_as_string.c_str(),
#endif /* CONFIG_INTERWORKING */
            encryption_config_as_string.c_str());
}

Generation getGeneration(hostapd_hw_modes *current_mode)
{
	wpa_printf(MSG_DEBUG, "getGeneration hwmode=%d, ht_enabled=%d, vht_enabled=%d",
		   current_mode->mode, current_mode->ht_capab != 0,
		   current_mode->vht_capab != 0);
	switch (current_mode->mode) {
	case HOSTAPD_MODE_IEEE80211B:
		return Generation::WIFI_STANDARD_LEGACY;
	case HOSTAPD_MODE_IEEE80211G:
		return current_mode->ht_capab == 0 ?
		       Generation::WIFI_STANDARD_LEGACY : Generation::WIFI_STANDARD_11N;
	case HOSTAPD_MODE_IEEE80211A:
		return current_mode->vht_capab == 0 ?
		       Generation::WIFI_STANDARD_11N : Generation::WIFI_STANDARD_11AC;
        // TODO: b/162484222 miss HOSTAPD_MODE_IEEE80211AX definition.
	default:
		return Generation::WIFI_STANDARD_UNKNOWN;
	}
}

Bandwidth getBandwidth(struct hostapd_config *iconf)
{
	wpa_printf(MSG_DEBUG, "getBandwidth %d, isHT=%d, isHT40=%d",
		   iconf->vht_oper_chwidth, iconf->ieee80211n,
		   iconf->secondary_channel);
	switch (iconf->vht_oper_chwidth) {
	case CHANWIDTH_80MHZ:
		return Bandwidth::WIFI_BANDWIDTH_80;
	case CHANWIDTH_80P80MHZ:
		return Bandwidth::WIFI_BANDWIDTH_80P80;
		break;
	case CHANWIDTH_160MHZ:
		return Bandwidth::WIFI_BANDWIDTH_160;
		break;
	case CHANWIDTH_USE_HT:
		if (iconf->ieee80211n) {
			return iconf->secondary_channel != 0 ?
				Bandwidth::WIFI_BANDWIDTH_40 : Bandwidth::WIFI_BANDWIDTH_20;
		}
		return Bandwidth::WIFI_BANDWIDTH_20_NOHT;
	default:
		return Bandwidth::WIFI_BANDWIDTH_INVALID;
	}
}

// hostapd core functions accept "C" style function pointers, so use global
// functions to pass to the hostapd core function and store the corresponding
// std::function methods to be invoked.
//
// NOTE: Using the pattern from the vendor HAL (wifi_legacy_hal.cpp).
//
// Callback to be invoked once setup is complete
std::function<void(struct hostapd_data*)> on_setup_complete_internal_callback;
void onAsyncSetupCompleteCb(void* ctx)
{
	struct hostapd_data* iface_hapd = (struct hostapd_data*)ctx;
	if (on_setup_complete_internal_callback) {
		on_setup_complete_internal_callback(iface_hapd);
		// Invalidate this callback since we don't want this firing
		// again.
		on_setup_complete_internal_callback = nullptr;
	}
}

// Callback to be invoked on hotspot client connection/disconnection
std::function<void(struct hostapd_data*, const u8 *mac_addr, int authorized,
		   const u8 *p2p_dev_addr)> on_sta_authorized_internal_callback;
void onAsyncStaAuthorizedCb(void* ctx, const u8 *mac_addr, int authorized,
			    const u8 *p2p_dev_addr)
{
	struct hostapd_data* iface_hapd = (struct hostapd_data*)ctx;
	if (on_sta_authorized_internal_callback) {
		on_sta_authorized_internal_callback(iface_hapd, mac_addr,
			authorized, p2p_dev_addr);
	}
}

std::function<void(struct hostapd_data*, int level,
                   enum wpa_msg_type type, const char *txt,
                   size_t len)> on_wpa_msg_internal_callback;

void onAsyncWpaEventCb(void *ctx, int level,
                   enum wpa_msg_type type, const char *txt,
                   size_t len)
{
	struct hostapd_data* iface_hapd = (struct hostapd_data*)ctx;
	if (on_wpa_msg_internal_callback) {
		on_wpa_msg_internal_callback(iface_hapd, level,
					       type, txt, len);
	}
}


}  // namespace

namespace android {
namespace hardware {
namespace wifi {
namespace hostapd {
namespace V1_3 {
namespace implementation {
using hidl_return_util::call;
using namespace android::hardware::wifi::hostapd::V1_0;

Hostapd::Hostapd(struct hapd_interfaces* interfaces)
    : interfaces_(interfaces), death_notifier_(sp<DeathNotifier>::make())
{}

Return<void> Hostapd::addAccessPoint(
    const V1_0::IHostapd::IfaceParams& iface_params,
    const V1_0::IHostapd::NetworkParams& nw_params, addAccessPoint_cb _hidl_cb)
{
	return call(
	    this, &Hostapd::addAccessPointInternal, _hidl_cb, iface_params,
	    nw_params);
}

Return<void> Hostapd::addAccessPoint_1_1(
    const V1_1::IHostapd::IfaceParams& iface_params,
    const V1_0::IHostapd::NetworkParams& nw_params, addAccessPoint_cb _hidl_cb)
{
	return call(
	    this, &Hostapd::addAccessPointInternal_1_1, _hidl_cb, iface_params,
	    nw_params);
}

Return<void> Hostapd::addAccessPoint_1_2(
    const V1_2::IHostapd::IfaceParams& iface_params,
    const V1_2::IHostapd::NetworkParams& nw_params,
    addAccessPoint_1_2_cb _hidl_cb)
{
	return call(
	    this, &Hostapd::addAccessPointInternal_1_2, _hidl_cb, iface_params,
	    nw_params);
}

Return<void> Hostapd::addAccessPoint_1_3(
    const V1_3::IHostapd::IfaceParams& iface_params,
    const V1_3::IHostapd::NetworkParams& nw_params,
    addAccessPoint_1_3_cb _hidl_cb)
{
        return call(
            this, &Hostapd::addAccessPointInternal_1_3, _hidl_cb, iface_params,
            nw_params);
}

Return<void> Hostapd::removeAccessPoint(
    const hidl_string& iface_name, removeAccessPoint_cb _hidl_cb)
{
	return call(
	    this, &Hostapd::removeAccessPointInternal, _hidl_cb, iface_name);
}

Return<void> Hostapd::terminate()
{
	wpa_printf(MSG_INFO, "Terminating...");
	eloop_terminate();
	return Void();
}

Return<void> Hostapd::registerCallback(
    const sp<V1_1::IHostapdCallback>& callback, registerCallback_cb _hidl_cb)
{
	return call(
	    this, &Hostapd::registerCallbackInternal, _hidl_cb, callback);
}

Return<void> Hostapd::registerCallback_1_3(
    const sp<V1_3::IHostapdCallback>& callback, registerCallback_1_3_cb _hidl_cb)
{
	return call(
	    this, &Hostapd::registerCallbackInternal_1_3, _hidl_cb, callback);
}

Return<void> Hostapd::forceClientDisconnect(
    const hidl_string& iface_name, const hidl_array<uint8_t, 6>& client_address,
    V1_2::Ieee80211ReasonCode reason_code, forceClientDisconnect_cb _hidl_cb)
{
	return call(
	    this, &Hostapd::forceClientDisconnectInternal, _hidl_cb, iface_name,
	    client_address, reason_code);
}

Return<void> Hostapd::setDebugParams(
     V1_2::DebugLevel level, setDebugParams_cb _hidl_cb)
{
	return call(
	    this, &Hostapd::setDebugParamsInternal, _hidl_cb, level);
}

V1_0::HostapdStatus Hostapd::addAccessPointInternal(
    const V1_0::IHostapd::IfaceParams& iface_params,
    const V1_0::IHostapd::NetworkParams& nw_params)
{
	return {V1_0::HostapdStatusCode::FAILURE_UNKNOWN, ""};
}

V1_0::HostapdStatus Hostapd::addAccessPointInternal_1_1(
    const V1_1::IHostapd::IfaceParams& iface_params,
    const V1_1::IHostapd::NetworkParams& nw_params)
{
	return {V1_0::HostapdStatusCode::FAILURE_UNKNOWN, ""};
}

V1_2::HostapdStatus Hostapd::addAccessPointInternal_1_2(
    const V1_2::IHostapd::IfaceParams& iface_params,
    const V1_2::IHostapd::NetworkParams& nw_params) {
	return {V1_2::HostapdStatusCode::FAILURE_UNKNOWN, ""};
}

V1_2::HostapdStatus Hostapd::addAccessPointInternal_1_3(
    const V1_2::IHostapd::IfaceParams& iface_params,
    const V1_3::IHostapd::NetworkParams& nw_params)
{
	if (hostapd_get_iface(interfaces_, iface_params.V1_1.V1_0.ifaceName.c_str())) {
		wpa_printf(
		    MSG_ERROR, "Interface %s already present",
		    iface_params.V1_1.V1_0.ifaceName.c_str());
		return {V1_2::HostapdStatusCode::FAILURE_IFACE_EXISTS, ""};
	}
	const auto conf_params = CreateHostapdConfig(iface_params, nw_params);
	if (conf_params.empty()) {
		wpa_printf(MSG_ERROR, "Failed to create config params");
		return {V1_2::HostapdStatusCode::FAILURE_ARGS_INVALID, ""};
	}
	const auto conf_file_path =
	    WriteHostapdConfig(iface_params.V1_1.V1_0.ifaceName, conf_params);
	if (conf_file_path.empty()) {
		wpa_printf(MSG_ERROR, "Failed to write config file");
		return {V1_2::HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}
	std::string add_iface_param_str = StringPrintf(
	    "%s config=%s", iface_params.V1_1.V1_0.ifaceName.c_str(),
	    conf_file_path.c_str());
	std::vector<char> add_iface_param_vec(
	    add_iface_param_str.begin(), add_iface_param_str.end() + 1);
	if (hostapd_add_iface(interfaces_, add_iface_param_vec.data()) < 0) {
		wpa_printf(
		    MSG_ERROR, "Adding interface %s failed",
		    add_iface_param_str.c_str());
		return {V1_2::HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}
	struct hostapd_data* iface_hapd =
	    hostapd_get_iface(interfaces_, iface_params.V1_1.V1_0.ifaceName.c_str());
	WPA_ASSERT(iface_hapd != nullptr && iface_hapd->iface != nullptr);
	// Register the setup complete callbacks
	on_setup_complete_internal_callback =
	    [this](struct hostapd_data* iface_hapd) {
		    wpa_printf(
			MSG_DEBUG, "AP interface setup completed - state %s",
			hostapd_state_text(iface_hapd->iface->state));
		    if (iface_hapd->iface->state == HAPD_IFACE_DISABLED) {
			    // Invoke the failure callback on all registered
			    // clients.
			    for (const auto& callback : callbacks_) {
				    callback->onFailure(
					iface_hapd->conf->iface);
			    }
		    }
	    };

	// Rgegister for new client connect/disconnect indication.
	on_sta_authorized_internal_callback =
	    [this](struct hostapd_data* iface_hapd, const u8 *mac_addr,
		   int authorized, const u8 *p2p_dev_addr) {
		wpa_printf(MSG_DEBUG, "notify client " MACSTR " %s",
			   MAC2STR(mac_addr),
			   (authorized) ? "Connected" : "Disconnected");

		for (const auto &callback : callbacks_) {
		    // TODO: The iface need to separate to iface and ap instance
		    // identify for AP+AP case.
		    callback->onConnectedClientsChanged(iface_hapd->conf->iface,
			    iface_hapd->conf->iface, mac_addr, authorized);
		}
	    };

	// Register for wpa_event which used to get channel switch event
	on_wpa_msg_internal_callback =
	    [this](struct hostapd_data* iface_hapd, int level,
		   enum wpa_msg_type type, const char *txt,
		   size_t len) {
		wpa_printf(MSG_DEBUG, "Receive wpa msg : %s", txt);
		if (os_strncmp(txt, WPA_EVENT_CHANNEL_SWITCH,
			       strlen(WPA_EVENT_CHANNEL_SWITCH)) == 0) {
		    for (const auto &callback : callbacks_) {
			callback->onApInstanceInfoChanged(
				iface_hapd->conf->iface, iface_hapd->conf->iface,
				iface_hapd->iface->freq, getBandwidth(iface_hapd->iconf),
				getGeneration(iface_hapd->iface->current_mode),
				iface_hapd->own_addr);
		    }
		}
	    };

	// Setup callback
	iface_hapd->setup_complete_cb = onAsyncSetupCompleteCb;
	iface_hapd->setup_complete_cb_ctx = iface_hapd;
	iface_hapd->sta_authorized_cb = onAsyncStaAuthorizedCb;
	iface_hapd->sta_authorized_cb_ctx = iface_hapd;
	wpa_msg_register_cb(onAsyncWpaEventCb);

	if (hostapd_enable_iface(iface_hapd->iface) < 0) {
		wpa_printf(
		    MSG_ERROR, "Enabling interface %s failed",
		    iface_params.V1_1.V1_0.ifaceName.c_str());
		return {V1_2::HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}
	return {V1_2::HostapdStatusCode::SUCCESS, ""};
}

V1_0::HostapdStatus Hostapd::removeAccessPointInternal(const std::string& iface_name)
{
	std::vector<char> remove_iface_param_vec(
	    iface_name.begin(), iface_name.end() + 1);
	if (hostapd_remove_iface(interfaces_, remove_iface_param_vec.data()) <
	    0) {
		wpa_printf(
		    MSG_ERROR, "Removing interface %s failed",
		    iface_name.c_str());
		return {V1_0::HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}
	return {V1_0::HostapdStatusCode::SUCCESS, ""};
}

V1_0::HostapdStatus Hostapd::registerCallbackInternal(
    const sp<V1_1::IHostapdCallback>& callback)
{
	return {V1_0::HostapdStatusCode::FAILURE_UNKNOWN, ""};
}

V1_2::HostapdStatus Hostapd::registerCallbackInternal_1_3(
    const sp<V1_3::IHostapdCallback>& callback)
{
	if (!callback->linkToDeath(death_notifier_, 0)) {
		wpa_printf(
		    MSG_ERROR,
		    "Error registering for death notification for "
		    "hostapd callback object");
		return {V1_2::HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}
	callbacks_.push_back(callback);
	return {V1_2::HostapdStatusCode::SUCCESS, ""};
}

V1_2::HostapdStatus Hostapd::forceClientDisconnectInternal(const std::string& iface_name,
    const std::array<uint8_t, 6>& client_address, V1_2::Ieee80211ReasonCode reason_code)
{
	struct hostapd_data *hapd = hostapd_get_iface(interfaces_, iface_name.c_str());
	struct sta_info *sta;
	if (!hapd) {
		wpa_printf(MSG_ERROR, "Interface %s doesn't exist", iface_name.c_str());
		return {V1_2::HostapdStatusCode::FAILURE_IFACE_UNKNOWN, ""};
	}
	for (sta = hapd->sta_list; sta; sta = sta->next) {
		int res;
		res = memcmp(sta->addr, client_address.data(), ETH_ALEN);
		if (res == 0) {
			wpa_printf(MSG_INFO, "Force client:" MACSTR " disconnect with reason: %d",
			    MAC2STR(client_address.data()), (uint16_t) reason_code);
			ap_sta_disconnect(hapd, sta, sta->addr, (uint16_t) reason_code);
			return {V1_2::HostapdStatusCode::SUCCESS, ""};
		}
	}
	return {V1_2::HostapdStatusCode::FAILURE_CLIENT_UNKNOWN, ""};
}

V1_2::HostapdStatus Hostapd::setDebugParamsInternal(V1_2::DebugLevel level)
{
	wpa_debug_level = static_cast<uint32_t>(level);
	return {V1_2::HostapdStatusCode::SUCCESS, ""};
}

}  // namespace implementation
}  // namespace V1_3
}  // namespace hostapd
}  // namespace wifi
}  // namespace hardware
}  // namespace android
