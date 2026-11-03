// Placeholder: replaced by the M2 renderer work unit.
#include "kfm/presentation/renderer.hpp"

namespace kfm::presentation {

bool Renderer::init(const sim::ContentDB& /*content*/) {
  initialised_ = true;
  return true;
}

void Renderer::shutdown() {
  initialised_ = false;
}

void Renderer::draw(const RenderState& /*state*/, const Camera& /*camera*/, int /*width*/,
                    int /*height*/) {
  stats_ = {};
}

}  // namespace kfm::presentation
