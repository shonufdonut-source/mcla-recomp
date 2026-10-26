// Never run. Linking it makes CMake build every library the probe needs and fails the
// CMake step (rather than Xcode) if a symbol is missing.
#include "mcla_probe.h"

int main() {
  mcla_probe_run("/tmp", [](const char*) {});
  return 0;
}
