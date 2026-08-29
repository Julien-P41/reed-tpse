cmake_minimum_required(VERSION 3.16)
project(reed-tpse VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(reed STATIC
    src/protocol.cpp
    src/device.cpp
    src/adb.cpp
    src/media.cpp
    src/config.cpp
    src/mapping.cpp
    src/status_cache.cpp
    src/sysinfo.cpp
)

target_include_directories(reed PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_options(reed PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-g -O0>
)

add_executable(reed-tpse
    cli/main.cpp
    cli/cli_common.cpp
    cli/cmd_device.cpp
    cli/cmd_media.cpp
    cli/cmd_hud.cpp
    cli/cmd_fan.cpp
    cli/cmd_daemon.cpp
)
target_link_libraries(reed-tpse PRIVATE reed)

target_compile_options(reed-tpse PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-g -O0>
)

# udev rule. Not installed under CMAKE_INSTALL_PREFIX, because udev does not
# read /usr/local/lib/udev/rules.d -- installing there would look successful and
# do nothing. The destination is therefore explicit.
option(REED_INSTALL_UDEV "Install the udev rule" OFF)
set(REED_UDEV_DIR "/etc/udev/rules.d" CACHE PATH "Where to install the udev rule")
if(REED_INSTALL_UDEV)
    install(FILES udev/71-reed-tpse.rules DESTINATION ${REED_UDEV_DIR})
    message(STATUS "udev rule will be installed to ${REED_UDEV_DIR}")
endif()

# systemd sleep hook. Same reasoning as the udev rule: systemd only reads
# /usr/lib/systemd/system-sleep, so installing under CMAKE_INSTALL_PREFIX would
# look successful and never run. Off by default -- it stops the daemon on the
# way into suspend, which is not something to enable behind a user's back.
option(REED_INSTALL_SLEEP_HOOK "Install the suspend/resume hook" OFF)
set(REED_SLEEP_HOOK_DIR "/usr/lib/systemd/system-sleep"
    CACHE PATH "Where to install the sleep hook")
if(REED_INSTALL_SLEEP_HOOK)
    install(PROGRAMS systemd/reed-tpse-sleep DESTINATION ${REED_SLEEP_HOOK_DIR}
            RENAME reed-tpse)
    message(STATUS "sleep hook will be installed to ${REED_SLEEP_HOOK_DIR}")
endif()

# Hardware-free checks over the protocol layer. Off by default so the normal
# build stays a single binary.
option(REED_BUILD_TESTS "Build the protocol unit tests" OFF)
if(REED_BUILD_TESTS)
    add_executable(reed-protocol-test tests/protocol_test.cpp)
    target_link_libraries(reed-protocol-test PRIVATE reed)
    add_executable(reed-config-test tests/config_test.cpp)
    target_link_libraries(reed-config-test PRIVATE reed)
    add_executable(reed-payload-test tests/payload_test.cpp)
    target_link_libraries(reed-payload-test PRIVATE reed)
    enable_testing()
    add_test(NAME protocol COMMAND reed-protocol-test)
    add_test(NAME config COMMAND reed-config-test)
    add_test(NAME payload COMMAND reed-payload-test)
endif()

install(TARGETS reed-tpse RUNTIME DESTINATION bin)
install(TARGETS reed
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/reed DESTINATION include)

# The daemon holds /dev/ttyACM0 exclusively, so exactly one scope may be
# installed. Default is "none": `make install` never silently enables a daemon.
set(REED_SYSTEMD_SCOPE "none" CACHE STRING
    "Which systemd unit to install: system, user, or none")
set_property(CACHE REED_SYSTEMD_SCOPE PROPERTY STRINGS system user none)

if(REED_SYSTEMD_SCOPE STREQUAL "user")
    # Templated like the system unit. Hardcoding /usr/local/bin meant a build
    # with a different CMAKE_INSTALL_PREFIX installed a unit pointing at a
    # binary that was never there.
    configure_file(systemd/user/reed-tpse.service.in
                   ${CMAKE_CURRENT_BINARY_DIR}/reed-tpse.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/reed-tpse.service
            DESTINATION lib/systemd/user)
elseif(REED_SYSTEMD_SCOPE STREQUAL "system")
    if(NOT REED_SERVICE_USER)
        # `sudo make install` would otherwise bake in "root".
        if(DEFINED ENV{SUDO_USER})
            set(REED_SERVICE_USER "$ENV{SUDO_USER}")
        else()
            set(REED_SERVICE_USER "$ENV{USER}")
        endif()
    endif()
    if(NOT REED_SERVICE_USER)
        message(FATAL_ERROR
            "REED_SYSTEMD_SCOPE=system needs an account to run as. "
            "Pass -DREED_SERVICE_USER=<name>.")
    endif()
    message(STATUS "System-scope unit will run as user: ${REED_SERVICE_USER}")
    configure_file(systemd/system/reed-tpse.service.in
                   ${CMAKE_CURRENT_BINARY_DIR}/reed-tpse.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/reed-tpse.service
            DESTINATION lib/systemd/system)
elseif(NOT REED_SYSTEMD_SCOPE STREQUAL "none")
    message(FATAL_ERROR
        "REED_SYSTEMD_SCOPE must be one of: system, user, none "
        "(got '${REED_SYSTEMD_SCOPE}')")
endif()
