cmake_minimum_required(VERSION 3.22)
project(daemon LANGUAGES CXX)

# This sub-build is invoked by the top-level CMakeLists.txt with the NDK
# toolchain pre-configured. It produces the chrooted daemon binary 'main'.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# -----------------------------------------------------------------------------
# Third-party deps
# -----------------------------------------------------------------------------
# cpp-httplib: header-only HTTP/1.1 server. MIT license.
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.5
    GIT_SHALLOW    TRUE
)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# nlohmann::json: JSON parser/serializer. MIT license.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)


# -----------------------------------------------------------------------------
# Daemon executable
# -----------------------------------------------------------------------------
add_executable(main
    main.cpp
    server.cpp
    apple/auth.cpp
    apple/decrypt.cpp
    apple/loader.cpp
    apple/runtime.cpp
    apple/tokens.cpp
)

target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(main PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
)

target_compile_options(main PRIVATE
    -Wall -Wextra -Wpedantic -Werror=return-type
    -fexceptions
)

# RTTI is intentionally on. nlohmann::json compiles fine with or
# without it but cpp-httplib's exception path uses typeid lookups,
# and we share libc++_shared.so with Apple's libs which were also
# built with RTTI. Disabling here would split the runtime.

# Our vendored `linker64` is from an older Android build that requires
# DT_HASH and does not understand GNU-hash-only executables. NDK r23b's
# default is `--hash-style=gnu`, which produces only DT_GNU_HASH and trips
# the linker with "empty/missing DT_HASH in <argv[0]>". Force `both` so the
# daemon ELF carries both hash sections - works with old and new linkers.
target_link_options(main PRIVATE
    -Wl,--hash-style=both
)

# -----------------------------------------------------------------------------
# Apple native libraries (Phase 1)
# -----------------------------------------------------------------------------
# Apple's libstoreservicescore / libmediaplatform / libandroidappmusic
# are NOT in the daemon's DT_NEEDED chain. The Loader (apple/loader.cpp)
# resolves them at runtime via dlopen/dlsym, which means the daemon can
# start in stub mode even when the Apple libs (and their AOSP closure)
# are absent.
#
# We DO link against libc++_shared.so here so that the daemon's own
# C++ runtime SONAME matches what Apple's libs expect. At runtime the
# dynamic linker resolves "libc++_shared.so" against /system/lib64/
# (set via -rpath below) so the same .so file backs both the daemon
# and Apple's libs once they get dlopen'd.
if(NOT DEFINED LIBS_DIR)
    message(FATAL_ERROR
        "LIBS_DIR is not set. The top-level CMakeLists.txt should pass "
        "-DLIBS_DIR=<repo>/rootfs/system/lib64.")
endif()

if(NOT EXISTS "${LIBS_DIR}/libc++_shared.so")
    message(FATAL_ERROR
        "${LIBS_DIR}/libc++_shared.so is missing. Run tools/extract-libs.sh "
        "first to populate rootfs/system/lib64/ from a verified APK bundle.")
endif()

target_link_libraries(main PRIVATE
    "${LIBS_DIR}/libc++_shared.so"
    log
    dl
)

target_link_options(main PRIVATE
    -Wl,-rpath,/system/lib64
)

set_target_properties(main PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
