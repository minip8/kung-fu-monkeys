#include "kfm/presentation/meshes.hpp"

#include "kfm/presentation/math.hpp"
#include "kfm/presentation/tuning.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace kfm::presentation {
namespace {

void pushVertex(MeshData& m, Vec3 p, Vec3 n, float u, float v) {
  const Vec3 nn = normalize(n);
  m.vertices.push_back(MeshVertex{p.x, p.y, p.z, nn.x, nn.y, nn.z, u, v});
}

void pushTriangle(MeshData& m, uint32_t a, uint32_t b, uint32_t c) {
  m.indices.push_back(static_cast<uint16_t>(a));
  m.indices.push_back(static_cast<uint16_t>(b));
  m.indices.push_back(static_cast<uint16_t>(c));
}

[[nodiscard]] float toFloat(uint32_t v) {
  return static_cast<float>(v);
}

}  // namespace

MeshData makeCapsule(float radius, float height, uint32_t segments, uint32_t hemisphereRings) {
  segments = std::max<uint32_t>(segments, 3);
  hemisphereRings = std::max<uint32_t>(hemisphereRings, 1);
  radius = std::max(radius, 1e-3f);
  height = std::max(height, (2.0f * radius) + 1e-3f);

  MeshData m;
  const uint32_t stride = segments + 1;  // the seam column is duplicated for uv
  const uint32_t rings = 2 * hemisphereRings;

  // Top pole, then rings from the top downward, then the bottom pole. Ring
  // `r` for r < hemisphereRings belongs to the upper hemisphere (centre
  // height - radius) and ends on the upper equator; the rest belong to the
  // lower hemisphere (centre radius) and start on the lower equator, so the
  // pair of equators bounds the cylinder band.
  pushVertex(m, Vec3{0.0f, height, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 0.5f, 1.0f);
  for (uint32_t r = 0; r < rings; ++r) {
    const bool upper = r < hemisphereRings;
    const float k = upper ? toFloat(r + 1) : toFloat(r - hemisphereRings);
    const float phi = upper ? 0.5f * kPi * (1.0f - (k / toFloat(hemisphereRings)))
                            : -0.5f * kPi * (k / toFloat(hemisphereRings));
    const float centreY = upper ? height - radius : radius;
    const float ringRadius = radius * std::cos(phi);
    const float y = centreY + (radius * std::sin(phi));
    for (uint32_t j = 0; j <= segments; ++j) {
      const float theta = kTwoPi * toFloat(j) / toFloat(segments);
      const float c = std::cos(theta);
      const float s = std::sin(theta);
      const Vec3 n{std::cos(phi) * c, std::sin(phi), -std::cos(phi) * s};
      pushVertex(m, Vec3{ringRadius * c, y, -ringRadius * s}, n, toFloat(j) / toFloat(segments),
                 y / height);
    }
  }
  const auto bottomPole = static_cast<uint32_t>(m.vertices.size());
  pushVertex(m, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 0.5f, 0.0f);

  const auto ringVertex = [&](uint32_t r, uint32_t j) { return 1 + (r * stride) + j; };
  for (uint32_t j = 0; j < segments; ++j)
    pushTriangle(m, 0, ringVertex(0, j), ringVertex(0, j + 1));
  for (uint32_t r = 0; r + 1 < rings; ++r) {
    for (uint32_t j = 0; j < segments; ++j) {
      const uint32_t a = ringVertex(r, j);
      const uint32_t b = ringVertex(r, j + 1);
      const uint32_t c = ringVertex(r + 1, j);
      const uint32_t d = ringVertex(r + 1, j + 1);
      pushTriangle(m, a, c, d);
      pushTriangle(m, a, d, b);
    }
  }
  for (uint32_t j = 0; j < segments; ++j)
    pushTriangle(m, ringVertex(rings - 1, j), bottomPole, ringVertex(rings - 1, j + 1));
  return m;
}

MeshData makeMonkeyCapsule(const sim::PlayerDef& player) {
  const float r = player.radius.toFloat();
  return makeCapsule(r, r * kMonkeyHeightOverRadius);
}

MeshData makeBanana(float length, float thickness, uint32_t segmentsAlong,
                    uint32_t segmentsAround) {
  segmentsAlong = std::max<uint32_t>(segmentsAlong, 2);
  segmentsAround = std::max<uint32_t>(segmentsAround, 3);
  length = std::max(length, 1e-3f);
  thickness = std::max(thickness, 1e-4f);

  // Quarter arc of radius A in the xz plane, psi in [-pi/4, pi/4], bulging
  // toward +x at psi = 0; the chord between the tips is `length`.
  const float arcRadius = length / std::numbers::sqrt2_v<float>;
  const float tipX = arcRadius * std::cos(0.25f * kPi);
  const float centreX = -0.5f * (tipX + arcRadius);  // centre the bounding box on x
  const float arcLength = arcRadius * 0.5f * kPi;
  constexpr float kTaperExponent = 0.7f;

  const auto psiAt = [](float t) { return (-0.25f * kPi) + (0.5f * kPi * t); };
  const auto spine = [&](float t) {
    const float psi = psiAt(t);
    return Vec3{centreX + (arcRadius * std::cos(psi)), 0.0f, arcRadius * std::sin(psi)};
  };
  const auto tangent = [&](float t) {
    const float psi = psiAt(t);
    return Vec3{-std::sin(psi), 0.0f, std::cos(psi)};
  };

  MeshData m;
  const uint32_t ringCount = segmentsAlong - 1;  // rings strictly between the tips
  const uint32_t stride = segmentsAround + 1;

  pushVertex(m, spine(0.0f), -tangent(0.0f), 0.0f, 0.5f);  // tip at t = 0
  for (uint32_t k = 1; k <= ringCount; ++k) {
    const float t = toFloat(k) / toFloat(segmentsAlong);
    const float psi = psiAt(t);
    const Vec3 centre = spine(t);
    const Vec3 tan = tangent(t);
    const Vec3 radial{std::cos(psi), 0.0f, std::sin(psi)};
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const float s = std::sin(kPi * t);
    const float rho = thickness * std::pow(s, kTaperExponent);
    // d(rho)/d(arc length): tilts the normal toward the nearer tip.
    const float dRho = thickness * kTaperExponent * std::pow(s, kTaperExponent - 1.0f) *
                       std::cos(kPi * t) * kPi / arcLength;
    for (uint32_t j = 0; j <= segmentsAround; ++j) {
      const float alpha = kTwoPi * toFloat(j) / toFloat(segmentsAround);
      const Vec3 dir = (radial * std::cos(alpha)) + (up * std::sin(alpha));
      pushVertex(m, centre + (dir * rho), dir - (tan * dRho), t,
                 toFloat(j) / toFloat(segmentsAround));
    }
  }
  const auto endTip = static_cast<uint32_t>(m.vertices.size());
  pushVertex(m, spine(1.0f), tangent(1.0f), 1.0f, 0.5f);

  // Rings advance along +t; around each ring alpha goes from the outer side
  // upward. Seen from outside, (k, j) -> (k, j+1) -> (k+1, j) is
  // counter-clockwise.
  const auto ringVertex = [&](uint32_t k, uint32_t j) { return 1 + (k * stride) + j; };
  for (uint32_t j = 0; j < segmentsAround; ++j)
    pushTriangle(m, ringVertex(0, j), 0, ringVertex(0, j + 1));
  for (uint32_t k = 0; k + 1 < ringCount; ++k) {
    for (uint32_t j = 0; j < segmentsAround; ++j) {
      const uint32_t a = ringVertex(k, j);
      const uint32_t b = ringVertex(k + 1, j);
      const uint32_t c = ringVertex(k, j + 1);
      const uint32_t d = ringVertex(k + 1, j + 1);
      pushTriangle(m, a, c, b);
      pushTriangle(m, b, c, d);
    }
  }
  for (uint32_t j = 0; j < segmentsAround; ++j)
    pushTriangle(m, ringVertex(ringCount - 1, j), ringVertex(ringCount - 1, j + 1), endTip);

  // Centre the bounding box on x exactly (the tube's thickness shifts it).
  float lo = m.vertices.front().px;
  float hi = lo;
  for (const MeshVertex& v : m.vertices) {
    lo = std::min(lo, v.px);
    hi = std::max(hi, v.px);
  }
  const float shift = -0.5f * (lo + hi);
  for (MeshVertex& v : m.vertices)
    v.px += shift;
  return m;
}

MeshData makeBananaMesh(const sim::BoomerangDef& boomerang) {
  const float r = boomerang.radius.toFloat();
  return makeBanana(2.0f * r, 0.28f * r);
}

MeshData makeDisc(float radius, uint32_t segments) {
  segments = std::max<uint32_t>(segments, 3);
  radius = std::max(radius, 1e-4f);
  MeshData m;
  const Vec3 up{0.0f, 1.0f, 0.0f};
  pushVertex(m, Vec3{}, up, 0.5f, 0.5f);
  for (uint32_t j = 0; j < segments; ++j) {
    const float theta = kTwoPi * toFloat(j) / toFloat(segments);
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    pushVertex(m, Vec3{radius * c, 0.0f, -radius * s}, up, 0.5f + (0.5f * c), 0.5f + (0.5f * s));
  }
  for (uint32_t j = 0; j < segments; ++j)
    pushTriangle(m, 0, 1 + j, 1 + ((j + 1) % segments));
  return m;
}

MeshData makeFloorQuad(float halfX, float halfZ) {
  MeshData m;
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const std::array<Vec3, 4> corners = {Vec3{-halfX, 0.0f, -halfZ}, Vec3{halfX, 0.0f, -halfZ},
                                       Vec3{halfX, 0.0f, halfZ}, Vec3{-halfX, 0.0f, halfZ}};
  for (const Vec3& p : corners)
    pushVertex(m, p, up, p.x, -p.z);
  pushTriangle(m, 0, 3, 2);
  pushTriangle(m, 0, 2, 1);
  return m;
}

MeshData makeRoundedBlock(float halfX, float halfZ, float height, float radius, float bevel,
                          uint32_t segments) {
  segments = std::max<uint32_t>(segments, 1);
  halfX = std::max(halfX, 1e-2f);
  halfZ = std::max(halfZ, 1e-2f);
  height = std::max(height, 1e-2f);
  radius = std::clamp(radius, 1e-2f * std::min(halfX, halfZ), 0.95f * std::min(halfX, halfZ));
  bevel = std::clamp(bevel, 0.0f, std::min(0.75f * radius, 0.5f * height));

  // The footprint: a rounded rectangle, counter-clockwise seen from above
  // (angle t from +x toward -z, as rotateY turns), segments + 1 points per
  // corner, each with its outward normal.
  struct ProfilePoint {
    float x;
    float z;
    float nx;
    float nz;
  };
  std::vector<ProfilePoint> profile;
  const std::array<Vec2f, 4> centres = {
      Vec2f{halfX - radius, -(halfZ - radius)}, Vec2f{-(halfX - radius), -(halfZ - radius)},
      Vec2f{-(halfX - radius), halfZ - radius}, Vec2f{halfX - radius, halfZ - radius}};
  for (uint32_t q = 0; q < 4; ++q) {
    for (uint32_t k = 0; k <= segments; ++k) {
      const float t = 0.5f * kPi * (toFloat(q) + (toFloat(k) / toFloat(segments)));
      const float c = std::cos(t);
      const float sn = -std::sin(t);
      profile.push_back(
          ProfilePoint{centres[q].x + (radius * c), centres[q].y + (radius * sn), c, sn});
    }
  }
  const auto ringSize = static_cast<uint32_t>(profile.size());

  // Rings from the ground up: the foot, the start of the bevel, then the
  // bevel's quarter circle (inset and raised, its normal turning up).
  MeshData m;
  const auto pushRing = [&](float inset, float y, float tilt) {
    const float cs = std::cos(tilt);
    const float sn = std::sin(tilt);
    for (const ProfilePoint& pp : profile) {
      const Vec3 at{pp.x - (pp.nx * inset), y, pp.z - (pp.nz * inset)};
      pushVertex(m, at, Vec3{pp.nx * cs, sn, pp.nz * cs}, at.x, -at.z);
    }
  };
  pushRing(0.0f, 0.0f, 0.0f);
  pushRing(0.0f, height - bevel, 0.0f);
  const uint32_t bevelRings = bevel > 0.0f ? segments : 0;
  for (uint32_t k = 1; k <= bevelRings; ++k) {
    const float phi = 0.5f * kPi * toFloat(k) / toFloat(bevelRings);
    pushRing(bevel * (1.0f - std::cos(phi)), height - bevel + (bevel * std::sin(phi)), phi);
  }
  const uint32_t rings = 2 + bevelRings;
  for (uint32_t r = 0; r + 1 < rings; ++r) {
    for (uint32_t i = 0; i < ringSize; ++i) {
      const uint32_t j = (i + 1) % ringSize;
      const uint32_t a = (r * ringSize) + i;
      const uint32_t b = (r * ringSize) + j;
      const uint32_t c = ((r + 1) * ringSize) + j;
      const uint32_t d = ((r + 1) * ringSize) + i;
      pushTriangle(m, a, b, c);
      pushTriangle(m, a, c, d);
    }
  }
  // The top: a fan from the centre over the last ring (convex), facing up.
  const uint32_t top = (rings - 1) * ringSize;
  const auto centre = static_cast<uint32_t>(m.vertices.size());
  pushVertex(m, Vec3{0.0f, height, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 0.0f, 0.0f);
  for (uint32_t i = 0; i < ringSize; ++i) {
    const MeshVertex& v = m.vertices[top + i];
    m.vertices[top + i] = MeshVertex{v.px, v.py, v.pz, 0.0f, 1.0f, 0.0f, v.u, v.v};
  }
  for (uint32_t i = 0; i < ringSize; ++i)
    pushTriangle(m, centre, top + i, top + ((i + 1) % ringSize));
  return m;
}

uint32_t appendMesh(MeshData& out, const MeshData& part, Vec3 offset) {
  const auto base = static_cast<uint32_t>(out.vertices.size());
  const auto first = static_cast<uint32_t>(out.indices.size());
  for (MeshVertex v : part.vertices) {
    v.px += offset.x;
    v.py += offset.y;
    v.pz += offset.z;
    v.u += offset.x;
    v.v -= offset.z;
    out.vertices.push_back(v);
  }
  for (const uint16_t i : part.indices)
    out.indices.push_back(static_cast<uint16_t>(base + i));
  return first;
}

MeshData makeUnitCube() {
  MeshData m;
  const std::array<Vec3, 3> axes = {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                                    Vec3{0.0f, 0.0f, 1.0f}};
  for (uint32_t i = 0; i < 3; ++i) {
    for (const float sign : {1.0f, -1.0f}) {
      // cross(u, v) == n, so the corner order below is counter-clockwise
      // seen from outside.
      const Vec3 n = axes[i] * sign;
      const Vec3 e1 = axes[(i + 1) % 3];
      const Vec3 e2 = axes[(i + 2) % 3];
      const Vec3 u = sign > 0.0f ? e1 : e2;
      const Vec3 v = sign > 0.0f ? e2 : e1;
      const Vec3 c = n * 0.5f;
      const auto base = static_cast<uint32_t>(m.vertices.size());
      pushVertex(m, c - (u * 0.5f) - (v * 0.5f), n, 0.0f, 0.0f);
      pushVertex(m, c + (u * 0.5f) - (v * 0.5f), n, 1.0f, 0.0f);
      pushVertex(m, c + (u * 0.5f) + (v * 0.5f), n, 1.0f, 1.0f);
      pushVertex(m, c - (u * 0.5f) + (v * 0.5f), n, 0.0f, 1.0f);
      pushTriangle(m, base, base + 1, base + 2);
      pushTriangle(m, base, base + 2, base + 3);
    }
  }
  return m;
}

}  // namespace kfm::presentation
