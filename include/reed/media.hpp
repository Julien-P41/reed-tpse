#pragma once

#include <string>

namespace reed {

enum class MediaType { Unknown, Video, Gif, Image };

class Media {
 public:
  static constexpr const char* TMP_DIR = "/tmp/reed-tpse/";

  static MediaType detect_type(const std::string& path);
  static std::string get_extension(const std::string& path);
  static std::string get_basename(const std::string& path);
  static std::string get_filename(const std::string& path);
  static std::string get_converted_name(const std::string& original);

  // The name the DEVICE holds for this input.
  //
  // Every command that names media on the device goes through here, so the
  // rule lives in one place and can be tested. Two things happen: a .gif is
  // asked for as the .mp4 that `upload` converted it into, and any path is
  // reduced to its basename because /sdcard/pcMedia only ever stores bare
  // names -- `upload` pushes with get_filename().
  //
  // Those were separate branches at the call sites, and only the gif one
  // reduced a path, so `display /path/x.gif` worked while
  // `display /path/x.mp4` reported "Not on device" for a file that was there.
  static std::string device_name(const std::string& input);
  static bool convert_gif_to_mp4(const std::string& input,
                                 const std::string& output);
  static bool is_ffmpeg_available();
};

}  // namespace reed
