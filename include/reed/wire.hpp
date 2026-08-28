#pragma once

// String values the firmware matches on.
//
// These are protocol constants, not labels: the device compares them exactly
// and silently ignores anything it does not recognise, so a typo produces a
// command that returns 200 and does nothing. They were written as literals in
// five files, which is one edit away from exactly that.
//
// Tests deliberately do NOT use these. A golden payload built from the same
// constant as the code it checks would agree with a typo in it; the test
// literals come from captured vendor traffic and are independent by design.

namespace reed::wire {

// waterBlockScreenId / config: screen layout.
inline constexpr const char* kFullScreen = "Full Screen";
inline constexpr const char* kScreenSplitting = "Screen Splitting";

// The `id` that selects custom media rather than a firmware preset. Presets
// use "Pre-set <n>: <Name>"; the number is not read -- the device splits on
// ": " and keeps the name -- but the prefix must be there or the command is
// not dispatched at all.
inline constexpr const char* kCustomization = "Customization";
inline constexpr const char* kPresetPrefix = "Pre-set 1: ";

// Playback order for a multi-entry media list.
inline constexpr const char* kPlaySingle = "Single";
inline constexpr const char* kPlayShuffle = "Shuffle";
inline constexpr const char* kPlayLoop = "Loop";

// fanLCDSet mode.
inline constexpr const char* kFanSmart = "Smart Mode";
inline constexpr const char* kFanFixed = "Fixed Mode";

// temperature.
inline constexpr const char* kCelsius = "Celsius";
inline constexpr const char* kFahrenheit = "Fahrenheit";

}  // namespace reed::wire
