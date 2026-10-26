# Builds the device probe against the ReXGlue SDK and MoltenVK for iOS.
# Configure with -DREXGLUE_SDK_DIR=<patched SDK checkout> and the iOS toolchain flags
# (see .github/workflows/ios-app.yml). No game code or data is involved.
cmake_minimum_required(VERSION 3.25)
project(mcla_ios_probe LANGUAGES C CXX OBJC OBJCXX)

if(NOT REXGLUE_SDK_DIR)
    message(FATAL_ERROR "Set REXGLUE_SDK_DIR to the patched ReXGlue SDK checkout")
endif()

add_subdirectory(${REXGLUE_SDK_DIR} rexglue EXCLUDE_FROM_ALL)

add_library(mcla_probe STATIC mcla_probe.cpp)
target_link_libraries(mcla_probe PRIVATE rex::core MoltenVK::MoltenVK Vulkan::Headers)

# A static library does not make CMake build its dependencies; an executable does.
add_executable(mcla_probe_linkcheck linkcheck.cpp)
target_link_libraries(mcla_probe_linkcheck PRIVATE mcla_probe)

# Xcode links one archive: the probe plus every static library it depends on.
# The linker only pulls the objects that are actually referenced.
add_custom_target(mcla_probe_all ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${CMAKE_BINARY_DIR}/libmcla_probe_all.a
        "-DSEARCH_DIRS=${CMAKE_BINARY_DIR}|${REXGLUE_SDK_DIR}/out"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/merge_archives.cmake
    DEPENDS mcla_probe mcla_probe_linkcheck
    VERBATIM)
