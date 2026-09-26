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

  // The file-transfer headers. Nothing in this driver implements the block
  // transfer yet -- these exist so `raw` can ask V1.0.11 whether it does. The
  // whole value is that they reach the wire verbatim, in order, so that is
  // what gets asserted rather than merely that build_frame still returns a
  // frame.
  const std::vector<Header> xfer = {{"FileName", "probe.png"},
                                    {"FileSize", "512"},
                                    {"FileBlockId", "0"},
                                    {"ContentRange", "0-511"}};
  auto xf = build_frame("POST", "transport", "{}", "1", 9, xfer);
  const std::string xframe(xf.begin(), xf.end());
  bool r7 = true;
  for (const auto& [name, value] : xfer) {
    if (xframe.find(name + "=" + value + "\r\n") == std::string::npos) r7 = false;
  }
  printf("carries the four file headers:   %s\n", r7 ? "yes" : "NO"); fail += !r7;

  // Order matters to a parser that reads headers positionally, and a map
  // would have silently sorted these. Checking the sequence catches that.
  const size_t p_name = xframe.find("FileName=");
  const size_t p_size = xframe.find("FileSize=");
  const size_t p_blk  = xframe.find("FileBlockId=");
  const size_t p_rng  = xframe.find("ContentRange=");
  bool r8 = p_name < p_size && p_size < p_blk && p_blk < p_rng;
  printf("keeps the order they were given: %s\n", r8 ? "yes" : "NO"); fail += !r8;

  // Extra headers belong in the header block, not the body. Off-by-one in
  // the separator would put them after the blank line, where the device
  // reads them as payload and the frame still parses.
  const size_t sep = xframe.find("\r\n\r\n");
  bool r9 = sep != std::string::npos && p_rng < sep;
  printf("puts them before the body:       %s\n", r9 ? "yes" : "NO"); fail += !r9;

  // A frame that asked for none must be byte-identical to one built before
  // the parameter existed -- otherwise every existing command changed shape.
  auto plain = build_frame("POST", "waterBlockScreenId", content, "1", 7);
  bool r10 = plain == built;
  printf("no headers changes nothing:      %s\n", r10 ? "yes" : "NO"); fail += !r10;

  // The method is a free string, not an enum. The protocol's vocabulary is
  // GET/POST/STATE/DELETE and this driver only ever sends two of them -- but
  // `raw` must be able to put the other two on the wire, because that is how
  // anyone finds out whether an endpoint accepts them. Asserting it here so a
  // future "tidy this into an enum" cannot quietly remove the capability.
  auto del = build_frame("DELETE", "mediaDelete", "{}", "1", 3);
  const std::string dframe(del.begin(), del.end());
  bool r11 = dframe.find("DELETE mediaDelete 1\r\n") != std::string::npos;
  printf("passes DELETE through verbatim:   %s\n", r11 ? "yes" : "NO"); fail += !r11;

  printf("%s\n", fail ? "FAILURES" : "all checks passed");
  return fail != 0;
}
