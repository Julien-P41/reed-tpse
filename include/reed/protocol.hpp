#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "picojson.h"

namespace reed {

constexpr uint8_t FRAME_MARKER = 0x5A;
constexpr uint8_t ESCAPE_MARKER = 0x5B;

struct Response {
  std::string raw;
  std::string body;
  std::optional<picojson::value> json;
  std::string version;
  std::string status;
  // The device's AckNumber, if present.
  //
  // ⚠ Not usable for correlating a reply with a request. It looks like an echo
  // of the SeqNumber sent -- it tracks it in captured vendor traffic -- but it
  // is the device's own counter: SeqNumber=1 out, AckNumber=2 back on a fresh
  // connection. Exposed for diagnostics, not for matching.
  std::optional<int> ack;
};

// Calculate CRC (sum of all bytes & 0xFF)
uint8_t calculate_crc(const std::vector<uint8_t>& data);

// Escape special bytes in data
std::vector<uint8_t> escape_data(const std::vector<uint8_t>& data);

// Unescape special bytes in data
std::vector<uint8_t> unescape_data(const std::vector<uint8_t>& data);

// Build a complete protocol frame
std::vector<uint8_t> build_frame(const std::string& request_state,
                                 const std::string& cmd_type,
                                 const std::string& content = "",
                                 const std::string& version = "1",
                                 int ack_number = 0);

// Parse a response frame
std::optional<Response> parse_response(const std::vector<uint8_t>& data);

}  // namespace reed
