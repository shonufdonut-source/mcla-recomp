// MCLA iOS device probe: C interface used by the Swift app (also its bridging header).
// The probe exercises the parts of the ReXGlue SDK that iOS is most likely to break
// (guest memory mapping, fault handling, fibers) and reports what MoltenVK offers on
// this GPU. It contains no game code and reads no game data.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mcla_log_fn)(const char* line);

// Runs every check in order, reporting each result through `log` before the next check
// starts, so a crash still leaves the last completed step in the log.
// `temp_dir` is a writable directory (NSTemporaryDirectory) for the file-backed fallback.
void mcla_probe_run(const char* temp_dir, mcla_log_fn log);

// Graphics test, all on the main thread. Brings up the SDK's Vulkan provider (MoltenVK called
// directly), an iOS window around `metal_layer` (a CAMetalLayer) and the SDK presenter, which
// paints a moving coloured bar through the SDK's immediate drawer. Returns 0 on success.
int mcla_gfx_start(const void* metal_layer, unsigned width_px, unsigned height_px,
                   mcla_log_fn log);
// Call once per display refresh: requests the next paint and logs the frame rate now and then.
void mcla_gfx_tick(void);
// Call when the layer's drawable size changes.
void mcla_gfx_resize(unsigned width_px, unsigned height_px);

// Game host (mcla_game.cpp). The recompiled game is only linked into the owner's private build;
// in the public build mcla_game_available() returns 0 and mcla_game_start() only logs that.
int mcla_game_available(void);
// Starts the game on the main thread and stops after `stage`:
//   1  rex::Runtime and Setup() (guest memory, kernel, file systems)
//   2  ... and load default.xex into guest memory
//   3  ... and create the guest heap and the suspended main guest thread (the entry point)
//   4  ... and resume the main guest thread, no graphics (expected to stop at video setup)
//   5  the same as 4 with the Xenos GPU system presenting into `metal_layer`
// `game_dir` holds default.xex; `data_dir` is writable (user data, cache, settings).
// Returns 0 when the stage was reached.
int mcla_game_start(const void* metal_layer, unsigned width_px, unsigned height_px,
                    const char* game_dir, const char* data_dir, int stage, mcla_log_fn log);
// Call once per display refresh: logs "alive" lines once the guest is running.
void mcla_game_tick(void);
void mcla_game_resize(unsigned width_px, unsigned height_px);

#ifdef __cplusplus
}
#endif
