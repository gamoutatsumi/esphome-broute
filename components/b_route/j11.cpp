#include "j11.h"
#include <esphome/core/hal.h>
#include <cstring>

namespace j11 {

namespace {

constexpr uint8_t
be16_hi(uint16_t v) {
	return static_cast<uint8_t>(v >> 8);
}

constexpr uint8_t
be16_lo(uint16_t v) {
	return static_cast<uint8_t>(v & 0xFF);
}

uint16_t
be16(const uint8_t* p) {
	return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

uint16_t
sum_bytes(const uint8_t* p, size_t n) {
	uint16_t s = 0;
	for (size_t i = 0; i < n; i++) {
		s += p[i];
	}
	return s;
}

constexpr std::array<uint8_t, 4> UNIQUE_RESP_BYTES{{0xD0, 0xF9, 0xEE, 0x5D}};

// Returns the index of the first occurrence of the response unique code, or npos.
size_t
find_unique(const uint8_t* p, size_t n) {
	if (n < 4) {
		return std::string::npos;  // any large sentinel; use SIZE_MAX-1 style via npos
	}
	for (size_t i = 0; i + 3 < n; i++) {
		if (p[i] == UNIQUE_RESP_BYTES[0] && p[i + 1] == UNIQUE_RESP_BYTES[1] && p[i + 2] == UNIQUE_RESP_BYTES[2] &&
		    p[i + 3] == UNIQUE_RESP_BYTES[3]) {
			return i;
		}
	}
	return SIZE_MAX;
}

}  // namespace

void
mac_to_ipv6(const uint8_t mac[8], uint8_t ipv6[16]) {
	std::memset(ipv6, 0, 16);
	ipv6[0] = 0xFE;
	ipv6[1] = 0x80;
	ipv6[8] = mac[0] ^ 0x02;  // invert Universal/Local bit
	std::memcpy(&ipv6[9], &mac[1], 7);
}

bool
Driver::send_request(uint16_t cmd, const uint8_t* data, size_t len) {
	std::array<uint8_t, HEADER_SIZE + 256> buf{};
	if (len > buf.size() - HEADER_SIZE) {
		return false;
	}
	const uint16_t msg_len = static_cast<uint16_t>(MSGLEN_MIN + len);
	size_t p = 0;
	// unique code (request)
	buf[p++] = 0xD0;
	buf[p++] = 0xEA;
	buf[p++] = 0x83;
	buf[p++] = 0xFC;
	// command code (BE)
	buf[p++] = be16_hi(cmd);
	buf[p++] = be16_lo(cmd);
	// message length (BE)
	buf[p++] = be16_hi(msg_len);
	buf[p++] = be16_lo(msg_len);
	// header checksum = sum of first 8 bytes
	const uint16_t hdr_cs = sum_bytes(buf.data(), 8);
	buf[p++] = be16_hi(hdr_cs);
	buf[p++] = be16_lo(hdr_cs);
	// data checksum = sum of data
	const uint16_t data_cs = sum_bytes(data, len);
	buf[p++] = be16_hi(data_cs);
	buf[p++] = be16_lo(data_cs);
	// data
	if (len) {
		std::memcpy(&buf[p], data, len);
		p += len;
	}
	return io_.write(buf.data(), p) == p;
}

bool
Driver::read_frame(Frame& out, uint32_t timeout_ms) {
	const uint32_t started = esphome::millis();
	// Discard any previously returned frame.
	if (pending_ > 0) {
		if (pending_ < rxlen_) {
			std::memmove(rxbuf_.data(), rxbuf_.data() + pending_, rxlen_ - pending_);
		}
		rxlen_ -= pending_;
		pending_ = 0;
	}

	for (;;) {
		// Try to decode a frame from what we have.
		size_t pos = find_unique(rxbuf_.data(), rxlen_);
		if (pos == SIZE_MAX) {
			// Keep last 3 bytes (a partial unique code may straddle the boundary).
			if (rxlen_ > 3) {
				std::memmove(rxbuf_.data(), rxbuf_.data() + (rxlen_ - 3), 3);
				rxlen_ = 3;
			}
		} else {
			if (pos > 0) {
				std::memmove(rxbuf_.data(), rxbuf_.data() + pos, rxlen_ - pos);
				rxlen_ -= pos;
			}
			if (rxlen_ >= HEADER_SIZE) {
				const uint16_t cmd = be16(&rxbuf_[4]);
				const uint16_t msg_len = be16(&rxbuf_[6]);
				if (msg_len >= MSGLEN_MIN) {
					const size_t data_len = static_cast<size_t>(msg_len) - MSGLEN_MIN;
					const size_t total = HEADER_SIZE + data_len;
					if (total <= rxbuf_.size() && rxlen_ >= total) {
						const uint16_t hdr_cs = sum_bytes(rxbuf_.data(), 8);
						const uint16_t data_cs = sum_bytes(&rxbuf_[HEADER_SIZE], data_len);
						if (hdr_cs == be16(&rxbuf_[8]) && data_cs == be16(&rxbuf_[10])) {
							out.command = cmd;
							out.data = &rxbuf_[HEADER_SIZE];
							out.data_len = data_len;
							pending_ = total;
							return true;
						}
					} else if (total > rxbuf_.size()) {
						// Bogus length: drop one byte and resync.
						std::memmove(rxbuf_.data(), rxbuf_.data() + 1, rxlen_ - 1);
						rxlen_--;
					}
					// else: need more bytes (frame not complete yet).
				} else {
					// Invalid message length: drop one byte and resync.
					std::memmove(rxbuf_.data(), rxbuf_.data() + 1, rxlen_ - 1);
					rxlen_--;
				}
			}
		}

		// Read more bytes from the serial.
		if (rxlen_ >= rxbuf_.size()) {
			rxlen_ = 0;  // overflow guard, should not happen
		}
		if (esphome::millis() - started >= timeout_ms) {
			return false;
		}
		int c = io_.read();
		if (c < 0) {
			esphome::delay(1);
			continue;
		}
		rxbuf_[rxlen_++] = static_cast<uint8_t>(c);
	}
}

}  // namespace j11
