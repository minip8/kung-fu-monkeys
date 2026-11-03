#include "kfm/presentation/camera.hpp"

#include "kfm/presentation/world_basis.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace kfm::presentation {
namespace {

constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
constexpr int kBisectIterations = 48;

struct Extents {
  float minX{0.0f};
  float maxX{0.0f};
  float minY{0.0f};
  float maxY{0.0f};
};

// The points that must stay in frame: the arena box at ground level and at
// framingHeight.
std::array<Vec3, 8> framingPoints(float hx, float hy, float height) {
  std::array<Vec3, 8> pts{};
  std::size_t n = 0;
  for (const float h : {0.0f, height}) {
    for (const float sx : {-hx, hx}) {
      for (const float sy : {-hy, hy})
        pts[n++] = simToWorld(Vec2f{sx, sy}, h);
    }
  }
  return pts;
}

Vec3 eyeAt(float distance, float pitch) {
  // On the world +z side (sim -y), raised: looking toward -z and down.
  return Vec3{0.0f, distance * std::sin(pitch), distance * std::cos(pitch)};
}

Extents ndcExtents(const Mat4& viewProj, const std::array<Vec3, 8>& pts) {
  Extents e{1e30f, -1e30f, 1e30f, -1e30f};
  for (const Vec3& p : pts) {
    const Vec3 ndc = projectPoint(viewProj, p);
    e.minX = std::min(e.minX, ndc.x);
    e.maxX = std::max(e.maxX, ndc.x);
    e.minY = std::min(e.minY, ndc.y);
    e.maxY = std::max(e.maxY, ndc.y);
  }
  return e;
}

struct Fit {
  Mat4 view;
  Mat4 proj;  // unshifted
  Extents extents;
  float zNear;
  float zFar;
};

Fit fitAt(float distance, float radius, float aspect, const CameraParams& params,
          const std::array<Vec3, 8>& pts) {
  const float pitch = degreesToRadians(params.pitchDegrees);
  const float fovY = degreesToRadians(params.fovYDegrees);
  Fit f{};
  f.zNear = std::max(0.05f, (distance - radius) * 0.5f);
  f.zFar = ((distance + radius) * 2.0f) + 10.0f;
  f.view = lookAt(eyeAt(distance, pitch), Vec3{}, kWorldUp);
  f.proj = perspective(fovY, aspect, f.zNear, f.zFar, params.clipDepth);
  f.extents = ndcExtents(f.proj * f.view, pts);
  return f;
}

float halfSpan(const Extents& e) {
  return std::max(e.maxX - e.minX, e.maxY - e.minY) * 0.5f;
}

}  // namespace

Camera makeArenaCamera(float halfExtentX, float halfExtentY, float viewportWidth,
                       float viewportHeight, const CameraParams& params) {
  Camera cam;
  cam.params = params;
  cam.viewportWidth = std::max(viewportWidth, 1.0f);
  cam.viewportHeight = std::max(viewportHeight, 1.0f);
  cam.aspect = cam.viewportWidth / cam.viewportHeight;
  cam.fovY = degreesToRadians(params.fovYDegrees);

  const std::array<Vec3, 8> pts = framingPoints(halfExtentX, halfExtentY, params.framingHeight);
  const float radius = std::sqrt((halfExtentX * halfExtentX) + (halfExtentY * halfExtentY) +
                                 (params.framingHeight * params.framingHeight));
  const float goal = 1.0f - params.margin;

  // Every framing point is in front of the camera for distance > radius, and
  // the projected extent shrinks monotonically with distance: bisect for the
  // distance at which the limiting axis spans exactly `goal`.
  float lo = (radius * 1.001f) + 0.01f;
  float hi = lo * 2.0f;
  for (int i = 0; i < 32 && halfSpan(fitAt(hi, radius, cam.aspect, params, pts).extents) > goal;
       ++i)
    hi *= 2.0f;
  if (halfSpan(fitAt(lo, radius, cam.aspect, params, pts).extents) <= goal) hi = lo;
  for (int i = 0; i < kBisectIterations; ++i) {
    const float mid = 0.5f * (lo + hi);
    if (halfSpan(fitAt(mid, radius, cam.aspect, params, pts).extents) > goal) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  const Fit fit = fitAt(hi, radius, cam.aspect, params, pts);
  cam.distance = hi;
  cam.zNear = fit.zNear;
  cam.zFar = fit.zFar;
  cam.eye = eyeAt(hi, degreesToRadians(params.pitchDegrees));
  cam.target = Vec3{};
  cam.lensShift = Vec2f{-0.5f * (fit.extents.minX + fit.extents.maxX),
                        -0.5f * (fit.extents.minY + fit.extents.maxY)};
  cam.view = fit.view;
  cam.proj = translate(Vec3{cam.lensShift.x, cam.lensShift.y, 0.0f}) * fit.proj;
  cam.viewProj = cam.proj * cam.view;
  cam.invViewProj = inverse(cam.viewProj);
  return cam;
}

Camera makeArenaCamera(const sim::ArenaDef& arena, float viewportWidth, float viewportHeight,
                       const CameraParams& params) {
  return makeArenaCamera(arena.halfExtents.x.toFloat(), arena.halfExtents.y.toFloat(),
                         viewportWidth, viewportHeight, params);
}

ScreenPoint worldToScreen(const Camera& camera, Vec3 world) {
  const Vec4 clip = camera.viewProj * Vec4{world.x, world.y, world.z, 1.0f};
  ScreenPoint sp;
  sp.inFront = clip.w > 0.0f;
  if (!sp.inFront) return sp;
  const float invW = 1.0f / clip.w;
  const float ndcX = clip.x * invW;
  const float ndcY = clip.y * invW;
  sp.x = (ndcX + 1.0f) * 0.5f * camera.viewportWidth;
  sp.y = (1.0f - ndcY) * 0.5f * camera.viewportHeight;
  sp.depth = clip.z * invW;
  return sp;
}

std::optional<Vec3> screenToGround(const Camera& camera, float screenX, float screenY,
                                   float groundHeight) {
  const float ndcX = ((screenX / camera.viewportWidth) * 2.0f) - 1.0f;
  const float ndcY = 1.0f - ((screenY / camera.viewportHeight) * 2.0f);
  const float nearZ = camera.params.clipDepth == ClipDepth::NegativeOneToOne ? -1.0f : 0.0f;
  const auto unproject = [&](float z) {
    const Vec4 p = camera.invViewProj * Vec4{ndcX, ndcY, z, 1.0f};
    return Vec3{p.x / p.w, p.y / p.w, p.z / p.w};
  };
  const Vec3 a = unproject(nearZ);
  const Vec3 b = unproject(1.0f);
  const Vec3 dir = b - a;
  if (std::fabs(dir.y) < 1e-9f) return std::nullopt;
  const float t = (groundHeight - a.y) / dir.y;
  if (t < 0.0f) return std::nullopt;
  return a + (dir * t);
}

}  // namespace kfm::presentation
