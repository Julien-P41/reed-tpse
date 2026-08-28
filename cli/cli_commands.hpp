#pragma once

// The command implementations, as main() sees them.
//
// Separate from cli_common.hpp on purpose: this is a flat list of entry
// points with no shared state, and main() needs nothing else from the command
// modules.

#include <string>
#include <vector>

int cmd_info(const std::string& port, bool verbose);
int cmd_status(const std::string& port, bool json_output, int watch,
               bool verbose);
int cmd_raw(const std::string& port, const std::string& method,
            const std::string& endpoint, const std::string& body,
            bool verbose);
int cmd_rotate(const std::string& port, const std::string& arg, bool force,
               bool verbose);
int cmd_screen(const std::string& port, const std::string& arg, bool verbose);
int cmd_sleep_display(const std::string& port, const std::string& arg,
                      bool verbose);
int cmd_power(const std::string& port, const std::string& arg, bool verbose);

int cmd_filter(const std::string& port, const std::string& name, int opacity,
               bool opacity_given, bool verbose);
int cmd_preset(const std::string& port, const std::vector<std::string>& args,
               bool verbose);
int cmd_upload(const std::string& file, bool verbose);
int cmd_display(const std::string& port, const std::vector<std::string>& files,
                const std::string& ratio, int brightness, bool brightness_given,
                const std::string& play_mode, bool split, bool verbose);
int cmd_brightness(const std::string& port, int value, bool verbose);
int cmd_list();
int cmd_delete(const std::vector<std::string>& files);
int cmd_lock_display(const std::string& port,
                     const std::vector<std::string>& args, int brightness,
                     bool brightness_given, bool verbose);

int cmd_hud(const std::string& port, const std::vector<std::string>& args,
            bool verbose);
int cmd_fan(const std::string& port, bool reset, const std::string& tier_arg,
            int duty_arg, bool smart, const std::string& profile_path,
            bool force, bool verbose);

int cmd_daemon_start(const std::string& port, bool foreground,
                     bool system_scope, bool verbose);
int cmd_daemon_stop(bool system_scope);
int cmd_daemon_status(bool system_scope);
