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
