// Placeholder client entry point; replaced by the M2 platform work unit.
#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/render_state.hpp"

#include <cstdio>

int main() {
  std::printf("kfm_client placeholder %d %d\n", kfm::platform::placeholder(),
              kfm::presentation::placeholder());
  return 0;
}
