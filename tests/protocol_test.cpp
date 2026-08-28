// Protocol-layer checks. No hardware required: the frames below are real
// captures from a Panorama 360 ARGB (firmware V1.0.11).
//
//   cmake .. -DREED_BUILD_TESTS=ON && make && ./reed-protocol-test
#include "reed/protocol.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
using namespace reed;
static std::vector<uint8_t> hex(const char* h) {
  std::vector<uint8_t> o;
  for (; h[0] && h[1]; h += 2)
    o.push_back((uint8_t)std::stoi(std::string(h, 2), nullptr, 16));
  return o;
}
static const char* GOOD =
  "5A003E31203230300D0A41636B4E756D6265723D300D0A436F6E74656E744C656E677468"
  "3D300D0A436F6E74656E74547970653D6A736F6E0D0A0D0A975A";
int main() {
  int fail = 0;
  auto ok = parse_response(hex(GOOD));
  printf("valid frame accepted:            %s\n", ok ? "yes" : "NO"); fail += !ok;
  printf("  status parsed as 200:          %s\n",
         (ok && ok->status == "200") ? "yes" : "NO"); fail += !(ok && ok->status=="200");

  auto bad_crc = hex(GOOD); bad_crc[bad_crc.size()-2] ^= 0xFF;
  bool r1 = !parse_response(bad_crc).has_value();
  printf("corrupted CRC rejected:          %s\n", r1 ? "yes" : "NO"); fail += !r1;

  auto bad_len = hex(GOOD); bad_len[2] = 0x99;
  bool r2 = !parse_response(bad_len).has_value();
  printf("wrong declared length rejected:  %s\n", r2 ? "yes" : "NO"); fail += !r2;

  auto truncated = hex(GOOD); truncated.pop_back();
  bool r3 = !parse_response(truncated).has_value();
  printf("unterminated frame rejected:     %s\n", r3 ? "yes" : "NO"); fail += !r3;

  // two frames in one buffer: must parse the first, not both as one
  auto two = hex(GOOD); auto g2 = hex(GOOD);
  two.insert(two.end(), g2.begin(), g2.end());
  auto t = parse_response(two);
  printf("two frames -> first parsed:      %s\n", t ? "yes" : "NO"); fail += !t;

  // Round-trip over a payload containing the frame marker itself. 'Z' is
  // 0x5A, so a media file called Zelda.mp4 exercises the escaper -- without
  // an escapable byte in the content, a broken escape() is invisible here and
  // the frame simply truncates at the embedded marker.
  //
  // Comparing the body, not merely that it parsed: has_value() alone passes
  // on a frame that lost everything after the marker.
  const std::string content = "{\"media\":[\"Zelda.mp4\"]}";
  auto built = build_frame("POST", "waterBlockScreenId", content, "1", 7);
  auto parsed = parse_response(built);
  bool r4 = parsed && parsed->body == content;
  printf("round-trips a payload with 0x5A: %s\n", r4 ? "yes" : "NO"); fail += !r4;

  // The request header vocabulary. The host numbers its own frames with
  // SeqNumber; AckNumber is the device's field, and sending it on a request
  // is what this once did. The device parses either, so the mistake is silent
  // -- it shows up only as SeqNumber=-1 in the device's own logging.
  const std::string frame(built.begin(), built.end());
  bool r5 = frame.find("SeqNumber=7") != std::string::npos;
  printf("request carries SeqNumber:       %s\n", r5 ? "yes" : "NO"); fail += !r5;
  bool r6 = frame.find("AckNumber=") == std::string::npos;
  printf("request carries no AckNumber:    %s\n", r6 ? "yes" : "NO"); fail += !r6;

  printf("%s\n", fail ? "FAILURES" : "all checks passed");
  return fail != 0;
}
