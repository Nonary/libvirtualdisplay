#include "virtual_display/driver/device_identity.h"

#include <array>
#include <cstdint>
#include <vector>

namespace virtual_display::driver {
  namespace {
    constexpr char kHexDigits[] = "0123456789abcdef";

    void append_hex(std::string &out, const std::uint64_t value, const int digits) {
      for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        out.push_back(kHexDigits[(value >> shift) & 0xfu]);
      }
    }

    std::uint32_t rotl(const std::uint32_t value, const int bits) {
      return (value << bits) | (value >> (32 - bits));
    }

    // Minimal, dependency-free SHA-1 (FIPS 180-1). Used only to derive Windows service
    // SIDs; not security-sensitive itself, but must match Windows' algorithm exactly.
    std::array<std::uint8_t, 20> sha1(const std::vector<std::uint8_t> &message) {
      std::uint32_t h0 = 0x67452301u;
      std::uint32_t h1 = 0xEFCDAB89u;
      std::uint32_t h2 = 0x98BADCFEu;
      std::uint32_t h3 = 0x10325476u;
      std::uint32_t h4 = 0xC3D2E1F0u;

      std::vector<std::uint8_t> padded = message;
      const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8u;
      padded.push_back(0x80u);
      while (padded.size() % 64u != 56u) {
        padded.push_back(0x00u);
      }
      for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffu));
      }

      for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64u) {
        std::array<std::uint32_t, 80> w {};
        for (std::size_t i = 0; i < 16; ++i) {
          w[i] = (static_cast<std::uint32_t>(padded[chunk + i * 4 + 0]) << 24) |
                 (static_cast<std::uint32_t>(padded[chunk + i * 4 + 1]) << 16) |
                 (static_cast<std::uint32_t>(padded[chunk + i * 4 + 2]) << 8) |
                 static_cast<std::uint32_t>(padded[chunk + i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
          w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (std::size_t i = 0; i < 80; ++i) {
          std::uint32_t f = 0, k = 0;
          if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
          } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
          } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
          } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
          }
          const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
          e = d;
          d = c;
          c = rotl(b, 30);
          b = a;
          a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
      }

      std::array<std::uint8_t, 20> digest {};
      const std::uint32_t state[5] {h0, h1, h2, h3, h4};
      for (std::size_t i = 0; i < 5; ++i) {
        digest[i * 4 + 0] = static_cast<std::uint8_t>((state[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i] & 0xffu);
      }
      return digest;
    }
  }  // namespace

  std::string format_inf_guid(const Guid &guid) {
    std::string out;
    out.reserve(38);
    out.push_back('{');
    append_hex(out, guid.data1, 8);
    out.push_back('-');
    append_hex(out, guid.data2, 4);
    out.push_back('-');
    append_hex(out, guid.data3, 4);
    out.push_back('-');
    append_hex(out, guid.data4[0], 2);
    append_hex(out, guid.data4[1], 2);
    out.push_back('-');
    for (std::size_t i = 2; i < guid.data4.size(); ++i) {
      append_hex(out, guid.data4[i], 2);
    }
    out.push_back('}');
    return out;
  }

  std::string derive_service_sid(const std::string_view service_name) {
    // Windows derives service SIDs from the UPPER-cased service name encoded as UTF-16LE.
    std::vector<std::uint8_t> utf16le;
    utf16le.reserve(service_name.size() * 2u);
    for (const char ch : service_name) {
      auto upper = static_cast<unsigned char>(ch);
      if (upper >= 'a' && upper <= 'z') {
        upper = static_cast<unsigned char>(upper - ('a' - 'A'));
      }
      utf16le.push_back(upper);
      utf16le.push_back(0x00u);
    }

    const auto digest = sha1(utf16le);
    std::string sid = "S-1-5-80";
    for (std::size_t word = 0; word < 5; ++word) {
      // Each 32-bit authority is read little-endian from the digest.
      const std::uint32_t value =
        static_cast<std::uint32_t>(digest[word * 4 + 0]) |
        (static_cast<std::uint32_t>(digest[word * 4 + 1]) << 8) |
        (static_cast<std::uint32_t>(digest[word * 4 + 2]) << 16) |
        (static_cast<std::uint32_t>(digest[word * 4 + 3]) << 24);
      sid.push_back('-');
      sid += std::to_string(value);
    }
    return sid;
  }

  std::string control_interface_security_descriptor() {
    // D:P            -> DACL, protected (no inherited ACEs)
    // (A;;GA;;;SY)   -> allow GENERIC_ALL to LocalSystem
    // (A;;GA;;;<sid>)-> allow GENERIC_ALL to the broker service
    return "D:P(A;;GA;;;SY)(A;;GA;;;" + derive_service_sid(kBrokerServiceName) + ")";
  }

  std::string protocol_version_string() {
    return std::to_string(kProtocolVersionMajor) + "." +
           std::to_string(kProtocolVersionMinor) + "." +
           std::to_string(kProtocolVersionPatch);
  }
}  // namespace virtual_display::driver
