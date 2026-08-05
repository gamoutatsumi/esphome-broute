#pragma once
#include <esphome/components/sensor/sensor.h>
#include <esphome/components/uart/uart.h>
#include <esphome/core/component.h>
#include <array>
#include <cmath>
#include <cstdint>
#include "echonet_lite.h"
#include "j11.h"

namespace esphome {
namespace b_route {

using echonet_lite::EOJ;

class BRoute : public Component, public uart::UARTDevice, public j11::SerialIO {
 public:
	BRoute();

	virtual void loop() override;
	virtual void setup() override;

	void set_power_sensor(sensor::Sensor* sensor) { power_sensor = sensor; }
	void set_energy_sensor(sensor::Sensor* sensor) { energy_sensor = sensor; }
	void set_power_sensor_interval_sec(uint32_t interval) { power_sensor_interval = interval * 1000; }
	void set_energy_sensor_interval_sec(uint32_t interval) { energy_sensor_interval = interval * 1000; }
	void set_rejoin_miss_count(uint8_t count) { rejoin_miss_count = count; }
	void set_rejoin_timeout_sec(uint32_t sec) { rejoin_timeout = sec * 1000; }
	void set_rescan_timeout_sec(uint32_t sec) { rescan_timeout = sec * 1000; }
	void set_restart_timeout_sec(uint32_t sec) { reboot_timeout = sec * 1000; }
	void set_rbid(const char* id, const char* password) {
		rb_id = id;
		rb_password = password;
	}

	// j11::SerialIO
	virtual size_t write(const uint8_t* data, size_t len) override {
		write_array(data, len);
		return len;
	}
	virtual int read() override {
		if (available() < 1) {
			return -1;
		}
		uint8_t b;
		return read_byte(&b) ? b : -1;
	}

 private:
	static constexpr EOJ EOJ_CONTROLLER{0x05, 0xff, 0x01};
	static constexpr EOJ EOJ_LOWV_SMART_METER{0x02, 0x88, 0x01};
	static constexpr uint32_t REQUEST_PROPERTY_INTERVAL = 5'000;
	static constexpr uint16_t UDP_PORT_ECHONET = echonet_lite::UDP_PORT;  // 3610
	static constexpr const char* TAG = "b_route";

	enum class state_t {
		init,
		set_mode,      // 初期設定 (Dual, channel = stored)
		set_auth,      // Bルート PANA 認証情報設定
		scan,          // アクティブスキャン実行
		set_channel,   // 初期設定 (Dual, scanned channel)
		broute_start,  // Bルート 動作開始
		open_udp,      // UDP ポート OPEN (3610)
		pana_start,    // Bルート PANA 開始 (accept)
		pana_wait,     // PANA 認証結果通知 (0x6028) 待ち
		running,
		restarting,
	} state = state_t::init;

	j11::Driver driver{*this};
	sensor::Sensor* power_sensor = nullptr;
	sensor::Sensor* energy_sensor = nullptr;
	const char* rb_password = nullptr;
	const char* rb_id = nullptr;

	int32_t energy_coeff = -1;
	float energy_unit = NAN;
	uint32_t property_requested = 0;
	uint32_t state_timeout = 0;
	uint32_t state_started = 0;
	uint32_t rejoin_timer = 0;
	uint32_t rescan_timer = 0;
	uint32_t reboot_timer = 0;
	uint8_t miss_count = 0;
	uint32_t power_sensor_interval = 30'000;
	uint32_t energy_sensor_interval = 60'000;
	uint32_t rejoin_timeout = 0;
	uint32_t rescan_timeout = 0;
	uint32_t reboot_timeout = 0;
	uint8_t rejoin_miss_count = 0;

	uint8_t channel = j11::CHANNEL_UNSPEC;  // smart meter channel (scanned)
	uint8_t meter_mac[8] = {0};
	uint8_t meter_ipv6[16] = {0};
	bool channel_found = false;
	bool request_sent = false;    // current state already sent its request
	bool need_scan_ = true;       // perform active scan during connection
	bool awaiting_boot_ = false;  // waiting for boot-complete after HW reset
	uint16_t expected_resp = 0;   // response command code awaited by simple states

	std::array<std::byte, 255> out_buffer{};
	std::array<uint8_t, 64> tx_buffer{};

	void set_state(state_t state, uint32_t timeout = 0);
	void restart_connection(bool with_scan);  // HW reset → boot → reconnect (rescan when true)
	static const char* state_name(state_t);

	bool handle_simple_response(const j11::Frame& frame, state_t ok_state);
	bool send_initial_setting(uint8_t channel);
	bool send_broute_auth();
	bool send_active_scan();
	bool send_broute_start();
	bool send_udp_open();
	bool send_pana_start();

	template <size_t N>
	bool request_property_impl(std::array<uint8_t, N> props);

	void handle_data_rx(const j11::Frame& frame);
	void handle_property_response(const std::byte* data, const echonet_lite::Packet& pkt);
	void request_momentary_power();
	void request_integral_energy();
	void request_energy_parameters();
	bool energy_params_received() const { return std::isfinite(energy_unit) && energy_coeff > 0; }
	void reset_timers() { rejoin_timer = rescan_timer = reboot_timer = esphome::millis(); }
	bool is_measurement_requesting() const {
		return (power_sensor && power_sensor_interval > 0 && power_sensor_interval != esphome::SCHEDULER_DONT_RUN) ||
		       (energy_sensor && energy_sensor_interval > 0 && energy_sensor_interval != esphome::SCHEDULER_DONT_RUN);
	}
};

}  // namespace b_route
}  // namespace esphome
