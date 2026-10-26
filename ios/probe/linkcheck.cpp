// Never run. Linking it makes CMake build every library the probe needs and fails the
// CMake step (rather than Xcode) if a symbol is missing.
#include "mcla_probe.h"

int main() {
  mcla_probe_run("/tmp", [](const char*) {});
  mcla_gfx_start(nullptr, 0, 0, [](const char*) {});
  mcla_gfx_tick();
  mcla_gfx_resize(0, 0);
  if (mcla_game_available()) {
    mcla_game_start(nullptr, 0, 0, "/tmp", "/tmp", 1, [](const char*) {});
  }
  mcla_game_tick();
  mcla_game_resize(0, 0);
  return 0;
}
