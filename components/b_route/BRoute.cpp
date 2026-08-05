#include "BRoute.h"
#include <esphome/core/application.h>
#include <cmath>
#include <cstring>
#include "echonet_lite.h"

namespace esphome::b_route {

using j11::Driver;
using j11::Frame;
namespace cmd = j11::req;
namespace notif = j11::notif;
namespace echo = echonet_lite;
namespace meter = echonet_lite::props::lowv_smart_meter;

namespace {

constexpr uint32_t BOOT_WAIT = 5'000;
constexpr uint32_t RESP_TIMEOUT = 5'000;
constexpr uint32_t SCAN_TIMEOUT = 30'000;
constexpr uint32_t BROUTE_START_TIMEOUT = 10'000;
constexpr uint32_t PANA_RESULT_TIMEOUT = 120'000;
constexpr uint32_t RESTART_DELAY = 5'000;

constexpr uint32_t SEND_RETRY_INTERVAL = 2'000;
constexpr uint32_t REQUEST_RETRY_INTERVAL = 5'000;

constexpr uint8_t SCAN_TIME = 0x06;
// Channels 4..17 → bits 4..17 set = 0x0003FFF0 (big endian)
constexpr uint8_t SCAN_CHANNELS[4] = {0x00, 0x03, 0xFF, 0xF0};

constexpr const char* power_task = "power";
constexpr const char* energy_task = "energy";
constexpr const char* params_task = "params";

constexpr std::array PROPS_MOMENTARY_POWER{meter::MOMENTARY_POWER};
constexpr std::array PROPS_ENERGY_PARAMS{meter::ENERGY_COEFF, meter::ENERGY_UNIT};
constexpr std::array PROPS_INTEGRAL_ENERGY{meter::INTEGRAL_ENERGY_FWD};

uint16_t
be16(const uint8_t* p) {
	return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

}  // namespace

BRoute::BRoute() {}

const char*
BRoute::state_name(state_t s) {
	switch (s) {
		case state_t::init:
			return "init";
		case state_t::set_mode:
			return "set_mode";
		case state_t::set_auth:
			return "set_auth";
		case state_t::scan:
			return "scan";
		case state_t::set_channel:
			return "set_channel";
		case state_t::broute_start:
			return "broute_start";
		case state_t::open_udp:
			return "open_udp";
		case state_t::pana_start:
			return "pana_start";
		case state_t::pana_wait:
			return "pana_wait";
		case state_t::running:
			return "running";
		case state_t::restarting:
			return "restarting";
		default:
			return "unknown";
	}
}

void
BRoute::set_state(state_t s, uint32_t timeout) {
	if (state == state_t::restarting) {
		return;
	}
	state = s;
	state_timeout = timeout;
	state_started = esphome::millis();
	request_sent = false;
}

void
BRoute::setup() {
	if (rb_id == nullptr || rb_password == nullptr) {
		ESP_LOGE(TAG, "Route B ID/Password not set");
		mark_failed();
		return;
	}
	if (std::strlen(rb_id) != 32) {
		ESP_LOGE(TAG, "Route B ID must be 32 chars");
		mark_failed();
		return;
	}
	if (power_sensor || energy_sensor) {
		request_energy_parameters();
	}
	if (power_sensor && power_sensor_interval) {
		set_interval(power_sensor_interval, [this] { request_momentary_power(); });
	}
	if (energy_sensor && energy_sensor_interval) {
		set_interval(energy_sensor_interval, [this] { request_integral_energy(); });
	}
	if (reset_pin != nullptr) {
		reset_pin->setup();              // configure as output
		reset_pin->digital_write(true);  // idle HIGH (RESETN de-asserted)
	}
	reset_timers();
}

void
BRoute::do_hardware_reset() {
	if (reset_pin == nullptr) {
		return;
	}
	// RESETN is active-low: pulse LOW then release HIGH
	reset_pin->digital_write(false);
	esphome::delay(100);
	reset_pin->digital_write(true);
	esphome::delay(500);
	ESP_LOGI(TAG, "RESETN pulse");
}

bool
BRoute::send_initial_setting(uint8_t ch) {
	const uint8_t data[4] = {j11::MODE_DUAL, j11::HAN_SLEEP_DISABLE, ch, j11::TX_POWER_20MW};
	ESP_LOGD(TAG, "INITIAL_SETTING channel=%02X", ch);
	return driver.send_request(cmd::INITIAL_SETTING, data, sizeof(data));
}

bool
BRoute::send_broute_auth() {
	uint8_t data[44];
	std::memcpy(data, rb_id, 32);
	size_t pwlen = std::strlen(rb_password);
	if (pwlen > 12) {
		pwlen = 12;
	}
	std::memcpy(data + 32, rb_password, pwlen);
	std::memset(data + 32 + pwlen, 0, 12 - pwlen);
	ESP_LOGD(TAG, "BROUTE_SET_AUTH");
	return driver.send_request(cmd::BROUTE_SET_AUTH, data, sizeof(data));
}

bool
BRoute::send_active_scan() {
	uint8_t data[1 + 4 + 1 + 8];
	data[0] = SCAN_TIME;
	std::memcpy(&data[1], SCAN_CHANNELS, 4);
	data[5] = 0x01;                        // Paring ID あり
	std::memcpy(&data[6], rb_id + 24, 8);  // Bルート認証ID 末尾8文字
	channel_found = false;
	ESP_LOGD(TAG, "ACTIVE_SCAN");
	return driver.send_request(cmd::ACTIVE_SCAN, data, sizeof(data));
}

bool
BRoute::send_broute_start() {
	ESP_LOGD(TAG, "BROUTE_START");
	return driver.send_request(cmd::BROUTE_START);
}

bool
BRoute::send_udp_open() {
	const uint8_t data[2] = {static_cast<uint8_t>(UDP_PORT_ECHONET >> 8), static_cast<uint8_t>(UDP_PORT_ECHONET & 0xFF)};
	ESP_LOGD(TAG, "UDP_PORT_OPEN %u", UDP_PORT_ECHONET);
	return driver.send_request(cmd::UDP_PORT_OPEN, data, sizeof(data));
}

bool
BRoute::send_pana_start() {
	ESP_LOGD(TAG, "BROUTE_PANA_START");
	return driver.send_request(cmd::BROUTE_PANA_START);
}

void
BRoute::restart_connection(bool with_scan) {
	ESP_LOGW(TAG, "Reconnect (rescan=%d)", with_scan ? 1 : 0);
	need_scan_ = with_scan;
	if (with_scan) {
		channel = j11::CHANNEL_UNSPEC;
		channel_found = false;
	}
	// init 状態でリセット(RESETN パルス + 0x00D9)→起動完了通知待ち→初期設定から再開
	set_state(state_t::init, BOOT_WAIT);
}

bool
BRoute::handle_simple_response(const Frame& frame, state_t ok_state) {
	if (frame.command != expected_resp) {
		return false;
	}
	if (frame.result() == j11::RESULT_OK) {
		set_state(ok_state);
		return true;
	}
	ESP_LOGE(TAG, "%s: result=%02X", state_name(state), frame.result());
	restart_connection(false);
	return true;
}

template <size_t N>
bool
BRoute::request_property_impl(std::array<uint8_t, N> props) {
	if (state != state_t::running) {
		return false;
	}
	if (rejoin_miss_count && miss_count >= rejoin_miss_count) {
		ESP_LOGW(TAG, "No response for %u requests, reconnect", miss_count);
		miss_count = 0;
		restart_connection(false);
		return false;
	}
	if (property_requested && esphome::millis() - property_requested < REQUEST_PROPERTY_INTERVAL) {
		return false;
	}
	size_t elen = echo::Codec::encode_property_get(out_buffer, EOJ_CONTROLLER, EOJ_LOWV_SMART_METER, props);
	if (elen > std::size(out_buffer)) {
		ESP_LOGE(TAG, "Get property encode overflow");
		return false;
	}
	// SEND_DATA payload: dst_ipv6(16) + src_port(2) + dst_port(2) + size(2) + data
	constexpr size_t HDR = 16 + 2 + 2 + 2;
	if (HDR + elen > std::size(tx_buffer)) {
		return false;
	}
	size_t p = 0;
	std::memcpy(&tx_buffer[p], meter_ipv6, 16);
	p += 16;
	tx_buffer[p++] = static_cast<uint8_t>(UDP_PORT_ECHONET >> 8);
	tx_buffer[p++] = static_cast<uint8_t>(UDP_PORT_ECHONET & 0xFF);
	tx_buffer[p++] = static_cast<uint8_t>(UDP_PORT_ECHONET >> 8);
	tx_buffer[p++] = static_cast<uint8_t>(UDP_PORT_ECHONET & 0xFF);
	tx_buffer[p++] = static_cast<uint8_t>(elen >> 8);
	tx_buffer[p++] = static_cast<uint8_t>(elen & 0xFF);
	std::memcpy(&tx_buffer[p], out_buffer.data(), elen);
	p += elen;
	if (!driver.send_request(cmd::SEND_DATA, tx_buffer.data(), p)) {
		return false;
	}
	property_requested = esphome::millis();
	++miss_count;
	return true;
}

void
BRoute::request_energy_parameters() {
	auto rc = request_property_impl(PROPS_ENERGY_PARAMS);
	if (rc) {
		ESP_LOGD(TAG, "Energy params requested");
	}
	App.scheduler.set_timeout(this, params_task, rc ? REQUEST_RETRY_INTERVAL : SEND_RETRY_INTERVAL,
	                          [this] { request_energy_parameters(); });
}

void
BRoute::request_momentary_power() {
	auto rc = request_property_impl(PROPS_MOMENTARY_POWER);
	if (rc) {
		ESP_LOGD(TAG, "POWER requested");
	}
	App.scheduler.set_timeout(this, power_task, rc ? REQUEST_RETRY_INTERVAL : SEND_RETRY_INTERVAL,
	                          [this] { request_momentary_power(); });
}

void
BRoute::request_integral_energy() {
	auto rc = energy_params_received() && request_property_impl(PROPS_INTEGRAL_ENERGY);
	if (rc) {
		ESP_LOGD(TAG, "ENERGY requested");
	}
	App.scheduler.set_timeout(this, energy_task, rc ? REQUEST_RETRY_INTERVAL : SEND_RETRY_INTERVAL,
	                          [this] { request_integral_energy(); });
}

void
BRoute::handle_property_response(const std::byte* raw, const echo::Packet& pkt) {
	for (int i = 0; i < pkt.opc; i++) {
		auto& prop = pkt.properties[i];
		if (prop.epc == meter::ENERGY_COEFF) {
			ESP_LOGD(TAG, "coeff received");
			if (pkt.esv == static_cast<uint8_t>(echo::ESV::Get_SNA) && prop.pdc == 0) {
				energy_coeff = 1;
				miss_count = 0;
			} else {
				if (prop.pdc != sizeof(energy_coeff)) {
					ESP_LOGW(TAG, "property(coeff) len mismatch %u != %u", prop.pdc, sizeof(energy_coeff));
					continue;
				}
				miss_count = 0;
				energy_coeff = echo::Codec::get_signed_long(raw + prop.offset);
			}
			if (energy_params_received()) {
				App.scheduler.cancel_timeout(this, params_task);
			}
			continue;
		} else if (prop.epc == meter::ENERGY_UNIT) {
			ESP_LOGD(TAG, "unit received");
			if (prop.pdc != 1) {
				ESP_LOGW(TAG, "Property(unit) len mismatch %u != 1", prop.pdc);
				continue;
			}
			miss_count = 0;
			auto v = std::to_integer<int8_t>(raw[prop.offset]);
			energy_unit = v > 10 ? std::pow(10.0f, v - 9) : std::pow(10.0f, -v);
			if (energy_params_received()) {
				App.scheduler.cancel_timeout(this, params_task);
				if (energy_sensor) {
					int8_t prec = 0;
					if (v < 10) {
						prec = static_cast<int>(std::ceil(v - std::log10(static_cast<float>(energy_coeff))));
						energy_sensor->set_accuracy_decimals(prec);
					}
				}
			}
			continue;
		}
		if (pkt.esv == static_cast<uint8_t>(echo::ESV::Get_SNA)) {
			continue;
		}
		if (prop.epc == meter::MOMENTARY_POWER) {
			ESP_LOGD(TAG, "POWER received");
			App.scheduler.cancel_timeout(this, power_task);
			int32_t power;
			if (prop.pdc != sizeof(power)) {
				ESP_LOGW(TAG, "Property(momentary power) len mismatch %u != %u", prop.pdc, sizeof(power));
				continue;
			}
			reset_timers();
			miss_count = 0;
			if (power_sensor) {
				power = echo::Codec::get_signed_long(raw + prop.offset);
				power_sensor->publish_state(power);
			}
		} else if (prop.epc == meter::SCHEDULED_INTEGRAL_ENERGY_FWD) {
			ESP_LOGD(TAG, "Scheduled ENERGY received");
			echo::IntegralPowerWithDateTime data;
			if (prop.pdc != sizeof(data)) {
				ESP_LOGW(TAG, "Property(sched integral energy) len mismatch %u != %u", prop.pdc, sizeof(data));
				continue;
			}
			reset_timers();
			data.year = echo::Codec::get_unsigned_short(raw + prop.offset);
			std::copy(raw + prop.offset + offsetof(echo::IntegralPowerWithDateTime, mon),
			          raw + prop.offset + offsetof(echo::IntegralPowerWithDateTime, value),
			          reinterpret_cast<std::byte*>(&data) + offsetof(echo::IntegralPowerWithDateTime, mon));
			data.value = echo::Codec::get_unsigned_long(raw + prop.offset + offsetof(echo::IntegralPowerWithDateTime, value));
			ESP_LOGI(TAG, "Integral data of %02u:%02u received", data.hour, data.min);
		} else if (prop.epc == meter::INTEGRAL_ENERGY_FWD) {
			ESP_LOGD(TAG, "ENERGY received");
			App.scheduler.cancel_timeout(this, energy_task);
			uint32_t evalue;
			if (prop.pdc != sizeof(evalue)) {
				ESP_LOGW(TAG, "Property(integral energy fwd) len mismatch %u != %u", prop.pdc, sizeof(evalue));
				continue;
			}
			reset_timers();
			miss_count = 0;
			if (energy_sensor) {
				evalue = echo::Codec::get_unsigned_long(raw + prop.offset);
				auto fenergy = energy_unit * evalue * energy_coeff;
				ESP_LOGV(TAG, "Energy %.3f = %.4f(kWh) * %u * %d, prec=%d", fenergy, energy_unit, evalue, energy_coeff,
				         energy_sensor->get_accuracy_decimals());
				energy_sensor->publish_state(fenergy);
			}
		} else {
			ESP_LOGD(TAG, "Drop property response %02X", prop.epc);
		}
	}
}

void
BRoute::handle_data_rx(const Frame& frame) {
	// 0x6018: sender_ipv6(16) + srcport(2) + dstport(2) + panid(2) + addrtype(1) + enc(1) + rssi(1) + size(2) + data
	constexpr size_t HDR = 16 + 2 + 2 + 2 + 1 + 1 + 1 + 2;
	if (frame.data_len < HDR) {
		ESP_LOGW(TAG, "DATA_RX too short");
		return;
	}
	const uint16_t dst_port = be16(frame.data + 18);
	if (dst_port != UDP_PORT_ECHONET) {
		ESP_LOGD(TAG, "DATA_RX port %u not for ECHONET", dst_port);
		return;
	}
	const size_t dsize = be16(frame.data + 25);
	if (HDR + dsize > frame.data_len) {
		ESP_LOGW(TAG, "DATA_RX size mismatch");
		return;
	}
	const auto* raw = reinterpret_cast<const std::byte*>(frame.data + HDR);
	echo::Packet pkt;
	if (!echo::Codec::decode_packet(raw, dsize, pkt)) {
		ESP_LOGW(TAG, "Failed to decode echonet packet");
		return;
	}
	if (pkt.ehd1 != echo::EHD1 || pkt.ehd2 != echo::EHD2_Format1) {
		return;
	}
	if (pkt.seoj.X1 != 0x02 || pkt.seoj.X2 != 0x88) {  // low voltage smart meter
		return;
	}
	if (pkt.esv == static_cast<uint8_t>(echo::ESV::Get_Res) || pkt.esv == static_cast<uint8_t>(echo::ESV::INF) ||
	    pkt.esv == static_cast<uint8_t>(echo::ESV::Get_SNA)) {
		handle_property_response(raw, pkt);
	}
}

void
BRoute::loop() {
	Frame frame;
	const bool have = driver.read_frame(frame, 100);
	if (have) {
		if (frame.is_error()) {
			ESP_LOGW(TAG, "Error frame %04X", frame.command);
		} else {
			ESP_LOGV(TAG, "frame %04X (%u bytes)", frame.command, frame.data_len);
		}
	}

	switch (state) {
		case state_t::init:
			// 初回突入時のみ: モジュールを確実に未起動状態にするため RESETN パルス +
			// ソフトウェアリセット(0x00D9)を行い、起動完了通知(0x6019)を待つ。
			// (ESP32 再起動後もモジュールが前回状態を保持している場合があり、
			//  初期設定は全体未起動状態でしか実行できないため)
			if (!request_sent) {
				do_hardware_reset();
				driver.reset_rx();
				driver.send_request(cmd::HW_RESET);
				request_sent = true;
				state_started = esphome::millis();
				state_timeout = BOOT_WAIT;
				break;
			}
			if (have && frame.command == notif::BOOT_COMPLETE) {
				ESP_LOGI(TAG, "Module booted");
				set_state(state_t::set_mode);
			}
			break;
		case state_t::set_mode:
			if (!request_sent) {
				expected_resp = cmd::INITIAL_SETTING | 0x2000;
				if (send_initial_setting(channel)) {
					request_sent = true;
					set_state(state_t::set_mode, RESP_TIMEOUT);
				}
			} else if (have) {
				handle_simple_response(frame, state_t::set_auth);
			}
			break;
		case state_t::set_auth:
			if (!request_sent) {
				expected_resp = cmd::BROUTE_SET_AUTH | 0x2000;
				if (send_broute_auth()) {
					request_sent = true;
					set_state(state_t::set_auth, RESP_TIMEOUT);
				}
			} else if (have) {
				handle_simple_response(frame, need_scan_ ? state_t::scan : state_t::broute_start);
			}
			break;
		case state_t::scan: {
			if (!request_sent) {
				if (send_active_scan()) {
					request_sent = true;
					set_state(state_t::scan, SCAN_TIMEOUT);
				}
				break;
			}
			if (!have) {
				break;
			}
			if (frame.command == notif::SCAN_RESULT) {
				// スキャン結果(0x4051): 結果(1) [, チャネル(1), スキャン数(1), [MAC(8),PANID(2),RSSI(1)]*n]
				if (frame.data_len >= 1 && frame.data[0] == 0x00 && frame.data_len >= 2) {
					channel = frame.data[1];
					channel_found = true;
					ESP_LOGI(TAG, "Scan: meter found on channel %u", channel);
				}
			} else if (frame.command == (cmd::ACTIVE_SCAN | 0x2000)) {
				if (channel_found) {
					set_state(state_t::set_channel);
				} else {
					ESP_LOGW(TAG, "Scan done but no meter found, retry");
					request_sent = false;  // re-send scan
				}
			}
			break;
		}
		case state_t::set_channel:
			if (!request_sent) {
				expected_resp = cmd::INITIAL_SETTING | 0x2000;
				if (send_initial_setting(channel)) {
					request_sent = true;
					set_state(state_t::set_channel, RESP_TIMEOUT);
				}
			} else if (have) {
				handle_simple_response(frame, state_t::broute_start);
			}
			break;
		case state_t::broute_start:
			if (!request_sent) {
				expected_resp = cmd::BROUTE_START | 0x2000;
				if (send_broute_start()) {
					request_sent = true;
					set_state(state_t::broute_start, BROUTE_START_TIMEOUT);
				}
			} else if (have && frame.command == expected_resp) {
				if (frame.result() == j11::RESULT_OK) {
					// 応答: 結果(1) + チャネル(1) + PANID(2) + MAC(8) + RSSI(1)
					if (frame.data_len >= 12) {
						channel = frame.data[1];
						std::memcpy(meter_mac, frame.data + 4, 8);
						j11::mac_to_ipv6(meter_mac, meter_ipv6);
						ESP_LOGI(TAG, "B-route started: channel=%u", channel);
					}
					set_state(state_t::open_udp);
				} else {
					ESP_LOGE(TAG, "BROUTE_START result=%02X", frame.result());
					// チャネル不正の可能性 → 再スキャン
					restart_connection(true);
				}
			}
			break;
		case state_t::open_udp:
			if (!request_sent) {
				expected_resp = cmd::UDP_PORT_OPEN | 0x2000;
				if (send_udp_open()) {
					request_sent = true;
					set_state(state_t::open_udp, RESP_TIMEOUT);
				}
			} else if (have) {
				handle_simple_response(frame, state_t::pana_start);
			}
			break;
		case state_t::pana_start:
			if (!request_sent) {
				expected_resp = cmd::BROUTE_PANA_START | 0x2000;
				if (send_pana_start()) {
					request_sent = true;
					set_state(state_t::pana_start, RESP_TIMEOUT);
				}
			} else if (have) {
				handle_simple_response(frame, state_t::pana_wait);
			}
			break;
		case state_t::pana_wait:
			if (have && frame.command == notif::PANA_RESULT) {
				if (frame.data_len >= 1 && frame.data[0] == j11::PANA_SUCCESS) {
					ESP_LOGI(TAG, "PANA authentication succeeded");
					set_state(state_t::running, 0);
					reset_timers();
				} else {
					ESP_LOGE(TAG, "PANA authentication failed (%02X)", frame.data_len >= 1 ? frame.data[0] : 0);
					restart_connection(true);
				}
			}
			break;
		case state_t::running:
			if (have) {
				switch (frame.command) {
					case cmd::SEND_DATA | 0x2000:
						if (frame.result() != j11::RESULT_OK) {
							ESP_LOGW(TAG, "SEND_DATA result=%02X", frame.result());
						}
						break;
					case notif::DATA_RX:
						handle_data_rx(frame);
						break;
					case notif::CONN_STATE:
						if (frame.data_len >= 1) {
							uint8_t cs = frame.data[0];
							ESP_LOGW(TAG, "Connection state change: %02X", cs);
							if (cs == j11::CONN_PANA_DISC || cs == j11::CONN_MAC_DISC) {
								restart_connection(false);
							}
						}
						break;
					case notif::PKT_RX_FAIL:
						ESP_LOGD(TAG, "Packet receive failure");
						break;
					default:
						break;
				}
			}
			if (is_measurement_requesting()) {
				if (rescan_timeout && esphome::millis() - rescan_timer > rescan_timeout) {
					ESP_LOGE(TAG, "計測データを %lu 秒間受信していません。再スキャンします", (esphome::millis() - rescan_timer) / 1000);
					restart_connection(true);
					break;
				}
				if (rejoin_timeout && esphome::millis() - rejoin_timer > rejoin_timeout) {
					ESP_LOGI(TAG, "計測データを %lu 秒間受信していません。再接続します", (esphome::millis() - rejoin_timer) / 1000);
					restart_connection(false);
					break;
				}
				if (reboot_timeout && esphome::millis() - reboot_timer > reboot_timeout) {
					ESP_LOGE(TAG, "計測データを %lu 秒間受信していません。再起動します", (esphome::millis() - reboot_timer) / 1000);
					set_state(state_t::restarting, 0);
					break;
				}
			}
			break;
		case state_t::restarting:
			if (esphome::millis() - state_started >= RESTART_DELAY) {
				mark_failed();
				App.safe_reboot();
			}
			return;
		default:
			break;
	}

	// State timeout (per state)
	if (state == state_t::init) {
		if (state_timeout && esphome::millis() - state_started > state_timeout) {
			set_state(state_t::set_mode);
		}
	} else if (state != state_t::restarting && state != state_t::running) {
		if (state_timeout && esphome::millis() - state_started > state_timeout) {
			ESP_LOGW(TAG, "%s: state timeout, reconnect", state_name(state));
			restart_connection(false);
		}
	}
}

}  // namespace esphome::b_route
