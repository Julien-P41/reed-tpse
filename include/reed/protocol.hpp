#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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

// One `Name=Value` request header beyond the four we always send.
//
// The protocol's header vocabulary is twelve names, not the four a normal
// command needs. `FileName`, `FileSize`, `FileBlockId` and `ContentRange`
// belong to the block file transfer, which nothing here implements yet -- so
// they are carried as data rather than given typed setters, and `raw` can put
// any of them on the wire for probing. See docs/vendor-protocol.md.
using Header = std::pair<std::string, std::string>;

// Build a complete protocol frame.
//
// `extra_headers` are appended after the standard four, in the order given.
// Nothing validates the names: the point is to be able to send a header this
// code does not understand.
std::vector<uint8_t> build_frame(const std::string& request_state,
                                 const std::string& cmd_type,
                                 const std::string& content = "",
                                 const std::string& version = "1",
                                 int ack_number = 0,
                                 const std::vector<Header>& extra_headers = {});

// Parse a response frame
std::optional<Response> parse_response(const std::vector<uint8_t>& data);

}  // namespace reed
