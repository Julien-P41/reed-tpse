// Filename handling. No hardware, no device.
//
//   cmake .. -DREED_BUILD_TESTS=ON && make && ./reed-media-test
//
// These decide what every command means by a filename, and had no coverage at
// all -- which is how `display /path/to/x.mp4` came to report "Not on device"
// for a file that was on the device. `upload` pushes with get_filename(), so
// /sdcard/pcMedia only ever holds basenames; the gif branch reduced a path
// (get_converted_name takes the stem) and the plain branch did not.
#include "reed/media.hpp"

#include <cstdio>
#include <string>

static int failures = 0;

static void check(const std::string& what, bool ok) {
  std::printf("  %-56s %s\n", what.c_str(), ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

static void eq(const std::string& what, const std::string& got,
               const std::string& want) {
  const bool ok = got == want;
  std::printf("  %-56s %s\n", what.c_str(), ok ? "ok" : "FAIL");
  if (!ok) {
    std::printf("      got  %s\n      want %s\n", got.c_str(), want.c_str());
    ++failures;
  }
}

using reed::Media;
using reed::MediaType;

int main() {
  // --- type detection ---
  check("mp4 is video", Media::detect_type("a.mp4") == MediaType::Video);
  check("mkv is video", Media::detect_type("a.mkv") == MediaType::Video);
  check("gif is its own type, not video",
        Media::detect_type("a.gif") == MediaType::Gif);
  check("png is image", Media::detect_type("a.png") == MediaType::Image);
  check("webp is image", Media::detect_type("a.webp") == MediaType::Image);
  check("unknown extension is Unknown",
        Media::detect_type("a.tar") == MediaType::Unknown);
  check("no extension is Unknown",
        Media::detect_type("plainname") == MediaType::Unknown);

  // Case matters here: the device's own listing is whatever the user uploaded,
  // and a user typing .GIF must still get the gif path.
  check("extension match is case-insensitive",
        Media::detect_type("A.GIF") == MediaType::Gif);
  check("...for video too",
        Media::detect_type("A.MP4") == MediaType::Video);

  // --- what a path becomes ---
  //
  // Every one of these must come out as a bare name, because that is all the
  // device ever stores.
  eq("bare name is unchanged", Media::get_filename("a.mp4"), "a.mp4");
  eq("absolute path reduces to the name",
     Media::get_filename("/home/u/clips/a.mp4"), "a.mp4");
  eq("relative path reduces to the name",
     Media::get_filename("./clips/a.mp4"), "a.mp4");
  eq("a name with dots survives",
     Media::get_filename("/p/2025-11-16_16-21-41-513.mp4"),
     "2025-11-16_16-21-41-513.mp4");

  // --- gif conversion naming ---
  //
  // upload converts a gif and pushes the .mp4, so display must ask for the
  // converted name. It must also be a bare name.
  eq("gif becomes mp4", Media::get_converted_name("a.gif"), "a.mp4");
  eq("gif path becomes a bare mp4 name",
     Media::get_converted_name("/home/u/a.gif"), "a.mp4");
  eq("only the last extension is replaced",
     Media::get_converted_name("/p/a.b.gif"), "a.b.mp4");
  eq("uppercase gif converts too", Media::get_converted_name("A.GIF"), "A.mp4");

  // --- the invariant the bug violated ---
  //
  // Whatever the input, display and lock-display must end up asking the device
  // for a name with no directory in it. This is the check that would have
  // caught it: before the fix, the non-gif branch returned the path verbatim.
  {
    const char* inputs[] = {"a.mp4", "/home/u/a.mp4", "./a.mp4", "a.gif",
                            "/home/u/a.gif", "../x/a.png", "A.GIF"};
    bool all_bare = true;
    for (const char* in : inputs) {
      const std::string out = Media::device_name(in);
      if (out.find('/') != std::string::npos) {
        std::printf("      leaked a path: %s -> %s\n", in, out.c_str());
        all_bare = false;
      }
    }
    check("no input yields a name containing a directory", all_bare);
  }

  // --- helpers used by the above ---
  eq("extension is lowercased", Media::get_extension("A.MP4"), ".mp4");
  eq("extension of a bare name is empty", Media::get_extension("plain"), "");
  eq("basename drops directory and extension",
     Media::get_basename("/home/u/a.mp4"), "a");

  // device_name is what the commands actually call, so these bind to real
  // behaviour rather than restating it.
  eq("device_name: bare mp4 unchanged", Media::device_name("a.mp4"), "a.mp4");
  eq("device_name: mp4 path reduced", Media::device_name("/h/u/a.mp4"), "a.mp4");
  eq("device_name: gif path becomes bare mp4",
     Media::device_name("/h/u/a.gif"), "a.mp4");
  eq("device_name: png path reduced", Media::device_name("../x/a.png"), "a.png");

  std::printf("%s\n", failures ? "FAILURES" : "all checks passed");
  return failures != 0;
}
