// The one implementation translation unit for sokol_gfx, sokol_log and
// sokol_imgui (target kfm::sokol, cmake/KfmDependencies.cmake). The backend
// define (SOKOL_GLCORE natively, SOKOL_GLES3 under Emscripten) and
// SOKOL_IMGUI_NO_SOKOL_APP come from the target's compile definitions so that
// every includer agrees with this file. Natively GL is declared by the
// vendored glad loader (SOKOL_EXTERNAL_GL_LOADER); kfm_platform calls
// gladLoadGL after creating the context and before sg_setup.
#if defined(SOKOL_EXTERNAL_GL_LOADER)
#include <glad/gl.h>
#endif

#include <imgui.h>

#define SOKOL_IMPL
#include <sokol_gfx.h>
#include <sokol_log.h>
#include <util/sokol_imgui.h>
