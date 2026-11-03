# glad (generated GL loader)

Generated with glad2 **2.0.8** from the GL specification bundled with that release
(`--reproducible`, no network fetch): desktop GL **4.3 core** profile, no extensions, C generator
with the built-in loader. From the repository root:

```sh
python3 -m venv /tmp/glad-venv && /tmp/glad-venv/bin/pip install glad2==2.0.8
rm -rf third_party/glad/include third_party/glad/src
/tmp/glad-venv/bin/glad --reproducible --api gl:core=4.3 --extensions '' \
    --out-path third_party/glad c --loader
```

Why a vendored loader: natively sokol_gfx runs with `SOKOL_GLCORE` + `SOKOL_EXTERNAL_GL_LOADER`, so
building needs no system GL headers (`GL/gl.h` is not installed on every developer machine, and on
Windows/macOS the system headers stop at GL 1.1 or need deprecation silencing). `kfm_platform`
calls `gladLoadGL(SDL_GL_GetProcAddress)` right after creating the context. The Emscripten build
uses `SOKOL_GLES3` with Emscripten's own `<GLES3/gl3.h>` and does not compile this loader.

Why 4.3 and not 4.1: sokol_gfx (floooh/sokol efeb776) decides at *compile* time from the
`GL_VERSION_4_x` macros whether to build its texture-view, texture-storage and compute code, but
sets `sg_features.gl_texture_views` at *run* time from the driver version (>= 4.3). Compiled
against a 4.1 header and run on a 4.5 driver (Mesa, NVIDIA, AMD, Intel), views are never created
and texture 0 is bound for every sampled texture: Dear ImGui renders as solid black boxes. Against
a 4.3 header both sides agree. A 4.2 header does not compile (sokol then references 4.3
multisample-storage functions). sokol's own Win32 loader also assumes 4.3. On macOS sokol ignores
the version macros and uses only 4.1 features, so the same header works with Apple's 4.1 core
context. `Window::init` therefore requests and requires 4.3 core everywhere except macOS (4.1).

Licence: the generated code is `(WTFPL OR CC0-1.0) AND Apache-2.0` (header of
`include/glad/gl.h`); `include/KHR/khrplatform.h` carries the Khronos MIT-style licence.
Do not edit these files by hand and do not run clang-format on them; regenerate instead.
