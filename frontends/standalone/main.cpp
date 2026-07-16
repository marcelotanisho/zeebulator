#include <cstdio>

#include "core/core.h"

int main() {
  std::printf("Zeebulator %s (standalone dev frontend — nothing to run yet)\n",
              zeebulator::VersionString());
  return 0;
}
