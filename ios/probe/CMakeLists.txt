# Builds the iOS native library against the ReXGlue SDK and MoltenVK: the device probe, the
# graphics test and the game host. Configure with -DREXGLUE_SDK_DIR=<patched SDK checkout> and
# the iOS toolchain flags (see .github/workflows/ios-app.yml).
#
# Public build: no game code or data is involved; the game host is compiled without the game.
# Private build only: -DMCLA_GAME_DIR=<folder holding generated/default> adds the recompiled
# game and the host sources from src/ that it calls into. Never pass it in a public workflow.
cmake_minimum_required(VERSION 3.25)
project(mcla_ios_probe LANGUAGES C CXX OBJC OBJCXX)

if(NOT REXGLUE_SDK_DIR)
    message(FATAL_ERROR "Set REXGLUE_SDK_DIR to the patched ReXGlue SDK checkout")
endif()
set(MCLA_GAME_DIR "" CACHE PATH "Folder holding generated/default (private build only)")
set(MCLA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src" CACHE PATH "MCLA host sources")

add_subdirectory(${REXGLUE_SDK_DIR} rexglue EXCLUDE_FROM_ALL)

# rexruntime (static on iOS, SDK patch 07) already contains the SDK's object libraries
# (rexcore, rexui, ...); linking those directly as well would duplicate their global state.
add_library(mcla_probe STATIC mcla_probe.cpp mcla_gfx.cpp mcla_game.cpp ios_gamepad.mm)
target_link_libraries(mcla_probe PRIVATE rex::runtime MoltenVK::MoltenVK Vulkan::Headers
    "-framework GameController" "-framework Foundation")
set_source_files_properties(ios_gamepad.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-ferror-limit=0")

# A static library does not make CMake build its dependencies; an executable does. The Xenos
# GPU emulator is linked in (iOS cannot load it as a plugin), so the public build compiles it
# too even though nothing there calls it yet.
add_executable(mcla_probe_linkcheck linkcheck.cpp)
target_link_libraries(mcla_probe_linkcheck PRIVATE mcla_probe rex::gpu-xenos)

# The game half of mcla_game.cpp, compiled without linking: it declares the game's symbols but
# needs none of its code, so the public build can check it compiles before a private build.
add_library(mcla_game_compilecheck OBJECT EXCLUDE_FROM_ALL mcla_game.cpp)
target_link_libraries(mcla_game_compilecheck PRIVATE rex::runtime MoltenVK::MoltenVK Vulkan::Headers)
target_compile_definitions(mcla_game_compilecheck PRIVATE MCLA_HAS_GAME=1)

if(MCLA_GAME_DIR)
    set(MCLA_GENERATED_DIR "${MCLA_GAME_DIR}/generated/default")
    if(NOT EXISTS "${MCLA_GENERATED_DIR}/sources.cmake")
        message(FATAL_ERROR "No generated/default/sources.cmake under MCLA_GAME_DIR=${MCLA_GAME_DIR}")
    endif()
    include("${MCLA_GENERATED_DIR}/sources.cmake")
    list(LENGTH GENERATED_SOURCES n)
    message(STATUS "Private build: ${n} generated sources from ${MCLA_GENERATED_DIR}")

    # Host sources include "generated/default/mcla_init.h" relative to the repository root.
    set(MCLA_GAME_INCLUDES ${MCLA_GAME_DIR} ${MCLA_SRC_DIR} ${MCLA_GENERATED_DIR})
    # As in the PS5 build: no fused multiply-add the PPC code did not ask for (guest floating
    # point should match the desktop build), no strict-aliasing assumptions, no debug info.
    set(MCLA_GAME_OPTIONS -ffp-contract=off -fno-strict-aliasing -fno-char8_t -g0 -w)

    add_library(mcla_recomp STATIC ${GENERATED_SOURCES})
    target_include_directories(mcla_recomp PRIVATE ${MCLA_GAME_INCLUDES})
    target_link_libraries(mcla_recomp PRIVATE rex::runtime)
    target_compile_options(mcla_recomp PRIVATE ${MCLA_GAME_OPTIONS})
    target_precompile_headers(mcla_recomp PRIVATE "${MCLA_GENERATED_DIR}/mcla_pch.h")

    # What the PS5 build links from src/: the desktop window host (main.cpp, mcla_app.cpp) and
    # its development tools are left out.
    set(MCLA_HOST_SOURCES)
    foreach(name audio_fallback frame_timing render_perf perf_options warp_fix button_prompts
                 settings_menu)
        list(APPEND MCLA_HOST_SOURCES "${MCLA_SRC_DIR}/${name}.cpp")
    endforeach()
    add_library(mcla_host STATIC ${MCLA_HOST_SOURCES})
    target_include_directories(mcla_host PRIVATE ${MCLA_GAME_INCLUDES})
    target_link_libraries(mcla_host PRIVATE rex::runtime)
    target_compile_options(mcla_host PRIVATE ${MCLA_GAME_OPTIONS})
    # The host sources include the SDK's UI headers (rex_app.h -> imgui). Static rexruntime
    # carries rexui's objects but not its usage requirements, so take the include paths and
    # definitions from those libraries without linking them a second time.
    foreach(dep imgui::imgui SDL3::SDL3 renderdoc)
        if(TARGET ${dep})
            get_target_property(real ${dep} ALIASED_TARGET)
            if(NOT real)
                set(real ${dep})
            endif()
            target_include_directories(mcla_host PRIVATE
                $<TARGET_PROPERTY:${real},INTERFACE_INCLUDE_DIRECTORIES>)
            target_compile_definitions(mcla_host PRIVATE
                $<TARGET_PROPERTY:${real},INTERFACE_COMPILE_DEFINITIONS>)
        endif()
    endforeach()

    target_compile_definitions(mcla_probe PRIVATE MCLA_HAS_GAME=1)
    target_link_libraries(mcla_probe_linkcheck PRIVATE mcla_recomp mcla_host)
endif()

# Xcode links one archive: the probe plus every static library it depends on.
# The linker only pulls the objects that are actually referenced.
add_custom_target(mcla_probe_all ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${CMAKE_BINARY_DIR}/libmcla_probe_all.a
        "-DSEARCH_DIRS=${CMAKE_BINARY_DIR}|${REXGLUE_SDK_DIR}/out"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/merge_archives.cmake
    DEPENDS mcla_probe mcla_probe_linkcheck
    VERBATIM)
