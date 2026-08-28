#pragma once

// Saved state -> device payload.
//
// This mapping is the highest-risk code in the project, and the reason it has
// a header of its own. Every visible bug of the "the panel is wrong but the
// logs look right" kind has come from here rather than from the serialisers:
// a HUD block copied before it was populated, so a split zone shipped blank; a
// filter carried on one path and dropped on another. It lived as five
// hand-written copies at the call sites and drifted between them.
//
// Kept out of the CLI so tests can reach it: the payload builders are covered
// against captured vendor traffic, and this is what feeds them.

#include <string>

#include "reed/config.hpp"
#include "reed/device.hpp"

namespace reed {

// The `settings` block, from an overlay config plus the media filter. Screen
// configs, presets and the overlay command all carry the same block.
DisplaySettings settings_from(const HudConfig& hud, const std::string& filter,
                              int filter_opacity);

// A complete screen payload from saved state, including the two-zone form when
// the saved screen mode is Screen Splitting.
ScreenConfig screen_config_from(const DisplayState& state);

}  // namespace reed
