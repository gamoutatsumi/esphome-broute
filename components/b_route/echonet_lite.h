#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace echonet_lite {

constexpr int MAX_PROPERTIES = 3;
static constexpr uint8_t EHD1 = 0x10;
constexpr uint16_t UDP_PORT = 3610;

static constexpr uint8_t EHD2_Format1 = 0x81;
static constexpr uint8_t EHD2_Format2 = 0x82;

struct __attribute__((__packed__)) EOJ {
	uint8_t X1;
	uint8_t X2;
	uint8_t X3;
};

struct __attribute__((__packed__)) Property {
	uint8_t epc;
	uint8_t pdc;
	int offset;
};

struct __attribute__((__packed__)) Packet {
	uint8_t ehd1;
	uint8_t ehd2;
	uint16_t tid;
	EOJ seoj;
	EOJ deoj;
	uint8_t esv;
	uint8_t opc;
	Property properties[MAX_PROPERTIES];
};

struct __attribute__((__packed__)) IntegralPowerWithDateTime {
	uint16_t year;
	uint8_t mon;
	uint8_t day;
	uint8_t hour;
	uint8_t min;
	uint8_t sec;
	uint32_t value;
};

enum class ESV : uint8_t {
	Get_SNA = 0x52,
	Get = 0x62,
	Get_Res = 0x72,
	INF = 0x73,
};

class Codec {
 private:
	static constexpr uint16_t TID = 0;

 public:
	static void write_eoj(const EOJ& eoj, std::byte*& dest, size_t max_size, size_t& written) {
		if (max_size >= written + 3) {
			*dest++ = std::byte{eoj.X1};
			*dest++ = std::byte{eoj.X2};
			*dest++ = std::byte{eoj.X3};
		}
		written += 3;
	}
	static bool decode_packet(const std::byte* data, size_t data_len, Packet& out);

	template <typename PropertyCodes, size_t N>
	static size_t encode_property_get(std::array<std::byte, N>& out,
	                                  const EOJ& seoj,
	                                  const EOJ& deoj,
	                                  const PropertyCodes& property_codes) {
		size_t written = 0;
		std::byte* dest = std::begin(out);
		if (N >= 2) {
			*dest++ = std::byte{EHD1};
			*dest++ = std::byte{EHD2_Format1};
		}
		written += 2;
		// TID
		if (N >= written + 2) {
			*dest++ = std::byte{TID >> 8};
			*dest++ = std::byte{TID & 0xff};
		}
		written += 2;
		// SEOJ, DEOJ
		write_eoj(seoj, dest, N, written);
		write_eoj(deoj, dest, N, written);
		// ESV
		if (N >= written + 1) {
			*dest++ = std::byte{static_cast<uint8_t>(ESV::Get)};
		}
		written += 1;
		int cnt = 0;
		std::byte* opc_p = dest++;
		written += 1;
		for (uint8_t prop : property_codes) {
			cnt++;
			if (N >= written + 2) {
				*dest++ = std::byte{prop};
				*dest++ = std::byte{0};
			}
			written += 2;
		}
		if (cnt <= 255) {
			*opc_p = static_cast<std::byte>(cnt);
		}
		return written;
	}
	static int32_t get_signed_long(const std::byte* buffer) { return static_cast<int32_t>(get_unsigned_long(buffer)); }
	static uint32_t get_unsigned_long(const std::byte* buffer) {
		return (std::to_integer<uint8_t>(buffer[0]) << 24) + (std::to_integer<uint8_t>(buffer[1]) << 16) +
		       (std::to_integer<uint8_t>(buffer[2]) << 8) + std::to_integer<uint8_t>(buffer[3]);
	}
	static uint16_t get_unsigned_short(const std::byte* buffer) {
		return (std::to_integer<uint8_t>(buffer[0]) << 8) + std::to_integer<uint8_t>(buffer[1]);
	}
};

namespace props::lowv_smart_meter {

constexpr uint8_t ENERGY_COEFF = 0xD3;
constexpr uint8_t ENERGY_UNIT = 0xE1;
constexpr uint8_t INTEGRAL_ENERGY_FWD = 0xE0;
constexpr uint8_t MOMENTARY_POWER = 0xE7;
constexpr uint8_t SCHEDULED_INTEGRAL_ENERGY_FWD = 0xEA;

}  // namespace props::lowv_smart_meter

}  // namespace echonet_lite
