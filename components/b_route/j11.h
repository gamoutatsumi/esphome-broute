#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace j11 {

// Unique codes (3.2.4)
constexpr uint32_t UNIQUE_REQUEST = 0xD0EA83FCu;
constexpr uint32_t UNIQUE_RESPONSE = 0xD0F9EE5Du;

// Header is fixed 12 bytes: unique(4) + command(2) + message_len(2) + header_cs(2) + data_cs(2)
// message_len = 4 + data_len   (covers header_cs + data_cs + data)
constexpr size_t HEADER_SIZE = 12;
constexpr size_t MAX_FRAME_SIZE = 1361;
constexpr uint16_t MSGLEN_MIN = 4;  // header_cs(2) + data_cs(2), no data

// Command result (表34)
constexpr uint8_t RESULT_OK = 0x01;

// Request command codes (表33)
namespace req {
constexpr uint16_t UDP_PORT_OPEN = 0x0005;
constexpr uint16_t SEND_DATA = 0x0008;
constexpr uint16_t GET_MAC_ADDRESS = 0x000E;
constexpr uint16_t ACTIVE_SCAN = 0x0051;
constexpr uint16_t BROUTE_START = 0x0053;
constexpr uint16_t BROUTE_SET_AUTH = 0x0054;
constexpr uint16_t BROUTE_PANA_START = 0x0056;
constexpr uint16_t BROUTE_PANA_END = 0x0057;
constexpr uint16_t BROUTE_STOP = 0x0058;
constexpr uint16_t INITIAL_SETTING = 0x005F;
constexpr uint16_t HW_RESET = 0x00D9;
}  // namespace req

// Notification command codes (表33)
namespace notif {
constexpr uint16_t SCAN_RESULT = 0x4051;
constexpr uint16_t DATA_RX = 0x6018;
constexpr uint16_t BOOT_COMPLETE = 0x6019;
constexpr uint16_t CONN_STATE = 0x601A;
constexpr uint16_t PANA_RESULT = 0x6028;
constexpr uint16_t PKT_RX_FAIL = 0x6038;
}  // namespace notif

// Error response command codes (2.10.7 / 2.10.8)
constexpr uint16_t ERR_INVALID_CMD = 0xFFFF;
constexpr uint16_t ERR_HEADER_CS = 0x2FFF;

// Initial setting values (表20)
constexpr uint8_t MODE_DUAL = 0x05;
constexpr uint8_t CHANNEL_UNSPEC = 0xFF;
constexpr uint8_t TX_POWER_20MW = 0x00;
constexpr uint8_t HAN_SLEEP_DISABLE = 0x00;

// Connection state (3.2.5.3)
constexpr uint8_t CONN_MAC_DONE = 0x01;
constexpr uint8_t CONN_PANA_DONE = 0x02;
constexpr uint8_t CONN_MAC_DISC = 0x03;
constexpr uint8_t CONN_PANA_DISC = 0x04;

// PANA result (3.2.5.4)
constexpr uint8_t PANA_SUCCESS = 0x01;
constexpr uint8_t PANA_FAILED = 0x02;
constexpr uint8_t PANA_NO_RESPONSE = 0x03;

class SerialIO {
 public:
	virtual ~SerialIO() = default;
	virtual size_t write(const uint8_t* data, size_t len) = 0;
	virtual int read() = 0;  // returns byte 0..255, or -1 when no data
};

// A decoded frame. `data` points into the driver's internal buffer and is valid
// only until the next read_frame() call.
struct Frame {
	uint16_t command;
	const uint8_t* data;
	size_t data_len;

	bool is_error() const { return command == ERR_INVALID_CMD || command == ERR_HEADER_CS; }
	// First data byte of a response is the result code (表34). Returns RESULT_OK when no data.
	uint8_t result() const { return data_len > 0 ? data[0] : RESULT_OK; }
};

class Driver {
 public:
	explicit Driver(SerialIO& io) : io_(io) {}

	void reset_rx() { rxlen_ = 0; }

	// Build and send a request frame. `data` may be nullptr when len == 0.
	bool send_request(uint16_t cmd, const uint8_t* data = nullptr, size_t len = 0);

	// Try to decode one complete frame within timeout_ms. Bytes already in the
	// internal buffer are consumed first; the serial is read until a complete,
	// checksum-valid frame is available or the timeout elapses.
	bool read_frame(Frame& out, uint32_t timeout_ms);

 private:
	SerialIO& io_;
	std::array<uint8_t, MAX_FRAME_SIZE> rxbuf_{};
	size_t rxlen_ = 0;
	size_t pending_ = 0;  // bytes of the last returned frame still in rxbuf_
};

// Build a link-local IPv6 address from a 64bit MAC (EUI-64).
// fe80:: + MAC with the Universal/Local bit (bit 1 of the first byte) inverted.
void mac_to_ipv6(const uint8_t mac[8], uint8_t ipv6[16]);

}  // namespace j11
