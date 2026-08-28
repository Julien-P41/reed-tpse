#include "reed/mapping.hpp"

namespace reed {

DisplaySettings settings_from(const HudConfig& hud, const std::string& filter,
                              int filter_opacity) {
  DisplaySettings out;
  out.align = hud.align;
  out.color = hud.color;
  out.badges = hud.badges;
  out.filter = filter;
  out.filter_opacity = filter_opacity;
  return out;
}

ScreenConfig screen_config_from(const DisplayState& state) {
  ScreenConfig cfg;
  cfg.media = state.media;
  cfg.ratio = state.ratio;
  cfg.screen_mode = state.screen_mode;
  cfg.play_mode = state.play_mode;

  // The filter belongs to the media, not the overlay: it applies whether or
  // not the HUD is on.
  cfg.settings = settings_from(state.hud, state.filter, state.filter_opacity);
  if (state.hud.enabled) cfg.sysinfo_display = state.hud.metrics;

  if (cfg.screen_mode == "Screen Splitting") {
    cfg.split = true;
    // The right zone mirrors the left unless it has its own configuration.
    cfg.split_settings_right = cfg.settings;
    if (state.hud_right) {
      const HudConfig& r = *state.hud_right;
      cfg.split_settings_right.align = r.align;
      cfg.split_settings_right.color = r.color;
      cfg.split_settings_right.badges = r.badges;
      if (r.enabled) cfg.split_sysinfo_right = r.metrics;
    }
  }
  return cfg;
}

}  // namespace reed
