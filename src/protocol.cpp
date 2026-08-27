#include "reed/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>

namespace reed {

uint8_t calculate_crc(const std::vector<uint8_t>& data) {
  uint32_t sum = 0;
  for (uint8_t b : data) {
    sum += b;
  }
  return static_cast<uint8_t>(sum & 0xFF);
}

std::vector<uint8_t> escape_data(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> result;
  result.reserve(data.size() * 2);

  for (uint8_t b : data) {
    if (b == 0x5A) {
      result.push_back(0x5B);
      result.push_back(0x01);
    } else if (b == 0x5B) {
      result.push_back(0x5B);
      result.push_back(0x02);
    } else {
      result.push_back(b);
    }
  }

  return result;
}

std::vector<uint8_t> unescape_data(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> result;
  result.reserve(data.size());

  for (size_t i = 0; i < data.size(); ++i) {
    if (data[i] == 0x5B && i + 1 < data.size()) {
      if (data[i + 1] == 0x01) {
        result.push_back(0x5A);
        ++i;
      } else if (data[i + 1] == 0x02) {
        result.push_back(0x5B);
        ++i;
      } else {
        result.push_back(data[i]);
      }
    } else {
      result.push_back(data[i]);
    }
  }

  return result;
}

std::vector<uint8_t> build_frame(const std::string& request_state,
                                 const std::string& cmd_type,
                                 const std::string& content,
                                 const std::string& version, int ack_number) {
  std::ostringstream body;

  // First line: REQUEST_STATE CMD_TYPE VERSION
  body << request_state << " " << cmd_type << " " << version << "\r\n";

  // Headers. The host numbers its own frames with SeqNumber and stamps Date;
  // AckNumber is the device's field, echoed back in its reply. We used to send
  // AckNumber here, i.e. format requests like responses -- the device parsed
  // them anyway but logged `SeqNumber=-1` for every one.
  body << "ContentType=json\r\n";
  body << "ContentLength=" << content.size() << "\r\n";
  body << "SeqNumber=" << ack_number << "\r\n";
  body << "Date="
       << std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()
       << "\r\n";

  // Double CRLF separator + content
  body << "\r\n" << content;

  std::string message_body = body.str();

  // Total length = message length + 5 (overhead)
  uint16_t total_length = static_cast<uint16_t>(message_body.size() + 5);

  // Build data: length (2 bytes BE) + message
  std::vector<uint8_t> data_with_length;
  data_with_length.push_back(static_cast<uint8_t>((total_length >> 8) & 0xFF));
  data_with_length.push_back(static_cast<uint8_t>(total_length & 0xFF));

  for (char c : message_body) {
    data_with_length.push_back(static_cast<uint8_t>(c));
  }

  // Add CRC
  uint8_t crc = calculate_crc(data_with_length);
  data_with_length.push_back(crc);

  // Escape special bytes
  std::vector<uint8_t> escaped = escape_data(data_with_length);

  // Add frame markers
  std::vector<uint8_t> frame;
  frame.reserve(escaped.size() + 2);
  frame.push_back(FRAME_MARKER);
  frame.insert(frame.end(), escaped.begin(), escaped.end());
  frame.push_back(FRAME_MARKER);

  return frame;
}

std::optional<Response> parse_response(const std::vector<uint8_t>& data) {
  if (data.size() < 4) {
    return std::nullopt;
  }

  // Take the FIRST complete frame, not everything up to the last byte. A read
  // can return two frames in one buffer, and slicing to the end then parses
  // them as one. Escaping guarantees 0x5A never occurs inside a payload, so the
  // next marker after the opening one is this frame's end.
  auto begin = std::find(data.begin(), data.end(), FRAME_MARKER);
  if (begin == data.end()) {
    return std::nullopt;
  }
  auto end = std::find(begin + 1, data.end(), FRAME_MARKER);
  if (end == data.end()) {
    return std::nullopt;  // incomplete frame
  }

  std::vector<uint8_t> payload(begin + 1, end);
  payload = unescape_data(payload);

  if (payload.size() < 4) {  // 2 length + >=1 message + 1 CRC
    return std::nullopt;
  }

  // Validate the declared length and the checksum. Neither was checked before,
  // so a corrupted or interleaved frame parsed as if it were valid -- which is
  // exactly what two daemons sharing the tty produced.
  const std::string message(payload.begin() + 2, payload.end() - 1);
  const uint16_t declared_length =
      static_cast<uint16_t>((payload[0] << 8) | payload[1]);
  const size_t expected_length = message.size() + 5;

  if (declared_length != expected_length) {
    std::cerr << "protocol: frame length mismatch (declared " << declared_length
              << ", got " << expected_length << ") -- discarding frame\n";
    return std::nullopt;
  }

  std::vector<uint8_t> crc_input(payload.begin(), payload.end() - 1);
  const uint8_t expected_crc = calculate_crc(crc_input);
  if (payload.back() != expected_crc) {
    std::cerr << "protocol: CRC mismatch (frame 0x" << std::hex
              << static_cast<int>(payload.back()) << ", computed 0x"
              << static_cast<int>(expected_crc) << std::dec
              << ") -- discarding frame\n";
    return std::nullopt;
  }

  Response response;
  response.raw = message;

  // Split headers and body
  size_t separator = message.find("\r\n\r\n");
  if (separator != std::string::npos) {
    std::string header_part = message.substr(0, separator);
    response.body = message.substr(separator + 4);

    // Try to parse body as JSON
    if (!response.body.empty()) {
      picojson::value v;
      std::string err = picojson::parse(v, response.body);
      if (err.empty()) {
        response.json = v;
      }
    }

    // Parse first line
    size_t first_line_end = header_part.find("\r\n");
    std::string first_line = (first_line_end != std::string::npos)
                                 ? header_part.substr(0, first_line_end)
                                 : header_part;

    // Extract version and status from first line
    std::istringstream iss(first_line);
    iss >> response.version >> response.status;

    // AckNumber echoes the SeqNumber of the request being answered.
    const size_t ack_at = header_part.find("AckNumber=");
    if (ack_at != std::string::npos) {
      try {
        response.ack = std::stoi(header_part.substr(ack_at + 10));
      } catch (const std::exception&) {
        // Malformed: leave unset rather than guessing at a correlation.
      }
    }
  }

  return response;
}

}  // namespace reed
