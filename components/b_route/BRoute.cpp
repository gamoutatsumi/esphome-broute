#include "BRoute.h"
#include <esphome/core/application.h>
#include <cmath>
#include <cstring>
#include "echonet_lite.h"
#include "util.h"

namespace esphome::b_route {

using namespace libbp35::cmd;
using libbp35::BP35;
using libbp35::event_params_t;
using libbp35::event_t;
using libbp35::rxudp_t;
namespace echo = echonet_lite;
namespace meter = echonet_lite::props::lowv_smart_meter;

namespace {

std::array<std::byte, 255> buffer{};
constexpr std::array PROPS_MOMENTARY_POWER{meter::MOMENTARY_POWER};
constexpr std::array PROPS_ENERGY_PARAMS{meter::ENERGY_COEFF, meter::ENERGY_UNIT};
constexpr std::array PROPS_INTEGRAL_ENERGY{meter::INTEGRAL_ENERGY_FWD};

constexpr uint32_t SEND_RETRY_INTERVAL = 2'000;
constexpr uint32_t REQUEST_RETRY_INTERVAL = 5'000;
constexpr uint32_t RESTART_DELAY = 5'000;

constexpr const char* power_task = "power";
constexpr const char* energy_task = "energy";
constexpr const char* params_task = "params";

constexpr std::string_view SCAN_KEY_ADDR = "Addr:";
constexpr std::string_view SCAN_KEY_PANID = "Pan ID:";
constexpr std::string_view SCAN_KEY_CHANNEL = "Channel:";

}  // namespace

BRoute::BRoute() {}

void
BRoute::request_energy_parameters() {
	auto rc = request_property(PROPS_ENERGY_PARAMS);
	if (rc) {
		ESP_LOGD(TAG, "Energy params requested");
	}
	App.scheduler.set_timeout(this, params_task, rc ? REQUEST_RETRY_INTERVAL : SEND_RETRY_INTERVAL,
	                          [this] { request_energy_parameters(); });
}

void
BRoute::request_momentary_power() {
	auto rc = request_property(PROPS_MOMENTARY_POWER);
	if (rc) {
		ESP_LOGD(TAG, "POWER requested");
	}
	App.scheduler.set_timeout(this, power_task, rc ? REQUEST_RETRY_INTERVAL : SEND_RETRY_INTERVAL,
	                          [this] { request_momentary_power(); });
}

void
BRoute::request_integral_energy() {
	auto rc = energy_params_received() && request_property(PROPS_INTEGRAL_ENERGY);
	if (rc) {
		ESP_LOGD(TAG, "ENERGY requested");
	}
	App.scheduler.set_timeout(this, energy_task, rc ? REQUEST_RETRY_INTERVAL : SEND_RETRY_INTERVAL,
	                          [this] { request_integral_energy(); });
}

bool
BRoute::test_nw_info() const {
	ESP_LOGD(TAG, "scan data: mac=%s, panid=%s, channel=%s", mac.c_str(), panid.c_str(), channel.c_str());
	if (mac.empty() && panid.empty() && channel.empty()) {
		return false;
	}
	if (mac.length() != 16 || panid.length() != 4 || channel.length() != 2) {
		ESP_LOGE(TAG, "Unexpected scan data: mac=%s, panid=%s, channel=%s", mac.c_str(), panid.c_str(), channel.c_str());
		return false;
	}
	return true;
}

void
BRoute::setup() {
	if (parent_ == nullptr) {
		ESP_LOGE(TAG, "No serial specified");
		mark_failed();
		return;
	}
	if (rb_id == nullptr || rb_password == nullptr) {
		ESP_LOGE(TAG, "Route B ID/Password not set");
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
	reset_timers();
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

const char*
BRoute::state_name(state_t state) {
	switch (state) {
		case state_t::init:
			return "init";
		case state_t::joining:
			return "joining";
		case state_t::running:
			return "running";
		case state_t::scanning:
			return "scanning";
		case state_t::setting_values:
			return "settings";
		case state_t::wait_ver:
			return "ver";
		case state_t::addr_conv:
			return "addr_conv";
		case state_t::restarting:
			return "restarting";
		default:
			return "unknown";
	}
}

void
BRoute::set_state(state_t state, uint32_t timeout) {
	if (this->state == state_t::restarting) {
		return;
	}
	this->state = state;
	state_timeout = timeout;
	state_started = esphome::millis();
}

void
BRoute::start_join() {
	bp.send_sk("SKJOIN", arg::str(v6_address));
	set_state(state_t::joining, 10'000);
}

void
BRoute::start_scan() {
	mac.clear();
	panid.clear();
	channel.clear();
	bp.send_sk("SKSCAN 2 FFFFFFFF 6");
	set_state(state_t::scanning, 20'000);
}

void
BRoute::handle_rxudp(std::string_view hexstr) {
	ESP_LOGV(TAG, "RXUDP: %s", hexstr.data());
	auto start = esphome::millis();
	rxudp_t rxudp;
	if (!BP35::parse_rxudp(hexstr, rxudp)) {
		ESP_LOGW(TAG, "%s: Failed to parse rxudp, skipped", hexstr.data());
		return;
	}
	if (rxudp.lport != echo::UDP_PORT) {
		ESP_LOGD(TAG, "%u: Destination port is not for EchonetLite", rxudp.lport);
		return;
	}
	ESP_LOGV(TAG, "udp data len = %u, datastr = %s", rxudp.data_len, hexstr.data() + rxudp.data_pos);
	size_t len;
	if (!util::hex2bin(&hexstr[rxudp.data_pos], buffer, len) || len != rxudp.data_len) {
		ESP_LOGW(TAG, "%s: Failed to decode udp data", &hexstr[rxudp.data_pos]);
		return;
	}
	echo::Packet pkt;
	if (!echo::Codec::decode_packet(buffer.data(), len, pkt)) {
		ESP_LOGW(TAG, "%s: Failed to decode echonet packet", &hexstr[rxudp.data_pos]);
		return;
	}
	if (pkt.ehd1 != echo::EHD1 || pkt.ehd2 != echo::EHD2_Format1) {
		return;
	}
	// handle low power smart meter
	if (pkt.seoj.X1 != 0x02 || pkt.seoj.X2 != 0x88) {
		return;
	}
	ESP_LOGV(TAG, "Echonet ehd=%02x,%02x deoj=%02x%02x%02x, esv=%02x, npc=%u, epc[0]=%02x", pkt.ehd1, pkt.ehd2, pkt.deoj.X1,
	         pkt.deoj.X2, pkt.deoj.X3, pkt.esv, pkt.opc, pkt.opc == 0 ? -1 : pkt.properties[0].epc);
	if (pkt.esv == static_cast<uint8_t>(echo::ESV::Get_Res) || pkt.esv == static_cast<uint8_t>(echo::ESV::INF) ||
	    pkt.esv == static_cast<uint8_t>(echo::ESV::Get_SNA)) {
		handle_property_response(buffer.data(), pkt);
	}
}

libbp35::event_t
BRoute::get_event(event_params_t& params) {
	auto ev = bp.get_event(100, params);
	if (ev != event_t::none) {
		if (ev == event_t::event) {
			ESP_LOGV(TAG, "ev = %s(%s)", libbp35::event_str(ev), libbp35::event_num_str(params.event.num));
		} else {
			ESP_LOGV(TAG, "ev = %s, line=%s", libbp35::event_str(ev), params.line.c_str());
		}
	}
	return ev;
}

void
BRoute::loop() {
	event_params_t params{};
	auto ev = get_event(params);
	switch (state) {
		case state_t::restarting:
			if (esphome::millis() - state_started >= RESTART_DELAY) {
				mark_failed();
				App.safe_reboot();
			}
			return;  // DO NOT DO ANYTHING
		case state_t::init:
			bp.send_sk("SKVER");
			set_state(state_t::wait_ver, 1'000);
			break;
		case state_t::wait_ver:
			if (ev == event_t::ver) {
				ESP_LOGD(TAG, "VER=%s", params.remain.data());
			} else if (ev == event_t::ok) {
				// disable echo back
				bp.send_sk("SKSREG", arg::reg(0xfe), arg::mode(0));
				set_state(state_t::setting_values, 1'000);
				setting_value = initial_value_t::echo;
			}
			break;
		case state_t::setting_values:
			if (ev == event_t::ok) {
				switch (setting_value) {
					case initial_value_t::echo:
						// test binary mode
						bp.send_prod("ROPT");
						setting_value = initial_value_t::ropt;
						break;
					case initial_value_t::ropt:
						ESP_LOGD(TAG, "ropt=%s", params.remain.data());
						if (params.remain != "01") {
							// set to ascii mode
							bp.send_prod("WOPT", arg::num8(1));
							setting_value = initial_value_t::wopt;
							break;
						} else {
							[[fallthrough]];
						}
					case initial_value_t::wopt:
						bp.send_sk("SKSETPWD", arg::num8(std::strlen(rb_password)), arg::str(rb_password));
						set_state(state_t::setting_values, 1'000);
						setting_value = initial_value_t::pwd;
						break;
					case initial_value_t::pwd:
						bp.send_sk("SKSETRBID", arg::str(rb_id));
						setting_value = initial_value_t::rbid;
						break;
					case initial_value_t::rbid:
						start_scan();
						break;
					case initial_value_t::channel:
						bp.send_sk("SKSREG", arg::reg(0x03), arg::str(panid));
						setting_value = initial_value_t::panid;
						break;
					case initial_value_t::panid:
						start_join();
						break;
					default:
						ESP_LOGE(TAG, "%d: Unexpected setting value type", static_cast<int>(setting_value));
						break;
				}
			}
			break;
		case state_t::addr_conv:
			if (ev == event_t::unknown) {
				if (params.line.rfind("SKLL", 0) == 0) {
					break;
				}
				if (params.line.length() == 39) {
					v6_address = params.line;
					bp.send_sk("SKSREG", arg::reg(0x02), arg::str(channel));
					setting_value = initial_value_t::channel;
					set_state(state_t::setting_values, 1'000);
				}
			}
			break;
		case state_t::scanning:
			if (ev == event_t::ok) {
				ESP_LOGI(TAG, "Scanning...");
			} else if (ev == event_t::event && params.event.num == 0x22) {
				if (test_nw_info()) {
					ESP_LOGI(TAG, "Scan done");
					rescan_timer = esphome::millis();
					bp.send_sk("SKLL64", arg::str(mac));
					set_state(state_t::addr_conv, 1'000);
				} else {
					ESP_LOGW(TAG, "Scan done but channel not received, scan again");
					start_scan();
				}
				break;
			} else if (ev == event_t::unknown && params.line[0] == ' ') {
				auto line = util::trim_sv(params.line);
				if (line.rfind(SCAN_KEY_ADDR, 0) == 0) {
					mac = line.substr(SCAN_KEY_ADDR.length());
				} else if (line.rfind(SCAN_KEY_PANID, 0) == 0) {
					panid = line.substr(SCAN_KEY_PANID.length());
				} else if (line.rfind(SCAN_KEY_CHANNEL, 0) == 0) {
					channel = line.substr(SCAN_KEY_CHANNEL.length());
				}
			}
			break;
		case state_t::joining:
			if (ev == event_t::ok) {
				ESP_LOGI(TAG, "Joining...");
			} else if (ev == event_t::event) {
				if (params.event.num == 0x25) {
					ESP_LOGI(TAG, "Joined");
					set_state(state_t::running, 0);
					rejoin_timer = esphome::millis();
				} else if (params.event.num == 0x24) {
					ESP_LOGW(TAG, "Failed to join, try scan and join");
					start_scan();
				} else {
					if (params.event.num != 0x21) {
						ESP_LOGD(TAG, "%02x: Ignore event", params.event.num);
					}
				}
			}
			break;
		case state_t::running: {
			switch (ev) {
				case event_t::event:
					switch (params.event.num) {
						case 0x32:
							ESP_LOGW(TAG, "Transmit time limit activated");
							break;
						case 0x33:
							ESP_LOGW(TAG, "Transmit time limit cleared");
							break;
						case 0x29:
							ESP_LOGI(TAG, "Session expired, waiting re-join");
							set_state(state_t::joining, 10'000);
							break;
						default:
							ESP_LOGV(TAG, "%02x: Unhandled event", params.event.num);
							break;
					}
					break;
				case event_t::rxudp:
					handle_rxudp(params.remain);
					break;
				case event_t::none:
					break;
				default:
					ESP_LOGV(TAG, "%d: Unhandled input", static_cast<int>(ev));
					break;
			}
			if (rescan_timeout && is_measurement_requesting()) {
				if (auto elapsed = esphome::millis() - rescan_timer; elapsed > rescan_timeout) {
					ESP_LOGE(TAG, "計測データを %lu 秒間受信していません。再スキャンします", elapsed / 1000);
					start_scan();
					break;
				}
			}
			if (rejoin_timeout && is_measurement_requesting()) {
				if (auto elapsed = esphome::millis() - rejoin_timer; elapsed > rejoin_timeout) {
					ESP_LOGI(TAG, "計測データを %lu 秒間受信していません。再接続します", elapsed / 1000);
					start_join();
					break;
				}
			}
		} break;
		default:
			ESP_LOGD(TAG, "%d: Unhandled state", static_cast<int>(state));
			break;
	}
	if (reboot_timeout && is_measurement_requesting()) {
		if (auto elapsed = esphome::millis() - reboot_timer; elapsed > reboot_timeout) {
			ESP_LOGE(TAG, "計測データを %lu 秒間受信していません。再起動します", elapsed / 1000);
			set_state(state_t::restarting, 0);
			return;
		}
	}
	if (state_timeout && esphome::millis() - state_started > state_timeout) {
		ESP_LOGW(TAG, "%s: State timeout, re-run from init", state_name(state));
		set_state(state_t::init, 0);
	}
}

}  // namespace esphome::b_route
