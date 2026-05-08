cmake_minimum_required(VERSION 3.16)
project(rawaccel-linux VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Build options ─────────────────────────────────────────────────────────────
option(BUILD_GUI    "Build GTK4 GUI"    ON)
option(BUILD_DAEMON "Build daemon"      ON)
option(BUILD_CLI    "Build CLI tool"    ON)
option(RAWACCEL_PORTABLE "Do not use -march=native" OFF)
if(DEFINED ENV{RAWACCEL_PORTABLE} AND "$ENV{RAWACCEL_PORTABLE}" STREQUAL "1")
    set(RAWACCEL_PORTABLE ON)
endif()

# ── Dependencies ─────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(EVDEV REQUIRED libevdev)

if(BUILD_GUI)
    pkg_check_modules(GTK4 REQUIRED gtk4)
endif()

# ── Include dirs ──────────────────────────────────────────────────────────────
include_directories(include)

# ── Common optimization flags ─────────────────────────────────────────────────
add_compile_options(-O3 -Wall -Wextra -Wno-unused-parameter)
if(NOT RAWACCEL_PORTABLE)
    add_compile_options(-march=native)
endif()

# ── Shared config library ─────────────────────────────────────────────────────
add_library(rawaccel-config STATIC src/config.cpp)
target_include_directories(rawaccel-config PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(rawaccel-config PRIVATE -O2)

# ── Daemon ────────────────────────────────────────────────────────────────────
if(BUILD_DAEMON)
    add_executable(rawaccel-daemon
        daemon/main.cpp
        daemon/daemon.cpp
    )
    target_include_directories(rawaccel-daemon PRIVATE
        ${EVDEV_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/daemon
    )
    target_link_libraries(rawaccel-daemon
        rawaccel-config
        ${EVDEV_LIBRARIES}
        pthread
    )
    target_compile_options(rawaccel-daemon PRIVATE ${EVDEV_CFLAGS_OTHER})

    install(TARGETS rawaccel-daemon DESTINATION /usr/local/bin)
endif()

# ── CLI ───────────────────────────────────────────────────────────────────────
if(BUILD_CLI)
    add_executable(rawaccel-cli
        cli/main.cpp
    )
    target_include_directories(rawaccel-cli PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(rawaccel-cli
        rawaccel-config
    )

    install(TARGETS rawaccel-cli DESTINATION /usr/local/bin)
endif()

# ── GUI ───────────────────────────────────────────────────────────────────────
if(BUILD_GUI)
    add_executable(rawaccel-gui
        gui/main.cpp
    )
    target_include_directories(rawaccel-gui PRIVATE
        ${GTK4_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(rawaccel-gui
        rawaccel-config
        ${GTK4_LIBRARIES}
    )
    target_compile_options(rawaccel-gui PRIVATE ${GTK4_CFLAGS_OTHER})

    install(TARGETS rawaccel-gui DESTINATION /usr/local/bin)

    # Desktop file
    install(FILES scripts/rawaccel.desktop
            DESTINATION /usr/share/applications)
endif()

# ── Systemd service ───────────────────────────────────────────────────────────
install(FILES scripts/rawaccel.service
        DESTINATION /usr/lib/systemd/system)

# ── Config dir ────────────────────────────────────────────────────────────────
install(CODE "
    file(MAKE_DIRECTORY \"/etc/rawaccel\")
    if(NOT EXISTS \"/etc/rawaccel/settings.json\")
        file(INSTALL DESTINATION \"/etc/rawaccel\" TYPE FILE RENAME \"settings.json\" FILES \"${CMAKE_SOURCE_DIR}/config/default.json\")
    else()
        message(STATUS \"Keeping existing /etc/rawaccel/settings.json\")
    endif()
")

# ── Tests ─────────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_executable(rawaccel-tests
        tests/test_accel.cpp
    )
    target_include_directories(rawaccel-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(rawaccel-tests
        rawaccel-config
    )
    target_compile_options(rawaccel-tests PRIVATE -O2)
    add_test(NAME rawaccel-unit-tests COMMAND rawaccel-tests)
endif()

message(STATUS "")
message(STATUS "RawAccel Linux build configuration:")
message(STATUS "  Build daemon : ${BUILD_DAEMON}")
message(STATUS "  Build CLI    : ${BUILD_CLI}")
message(STATUS "  Build GUI    : ${BUILD_GUI}")
message(STATUS "  Build tests  : ${BUILD_TESTS}")
message(STATUS "  Portable     : ${RAWACCEL_PORTABLE}")
message(STATUS "")
