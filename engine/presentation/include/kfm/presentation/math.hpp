// Float math for presentation (architecture §7): vectors, column-major 4×4
// matrices and the projection helpers the camera and renderer need. No GLM.
//
// Floating point is allowed here (and in platform/apps), never in the sim;
// fixed-point values are converted at the boundary (world_basis.hpp).
//
// Matrices are column-major: element (row r, column c) is m[c * 4 + r], so a
// Mat4's data() uploads to a GL uniform without transposition, and points are
// column vectors transformed as M * p. Projections follow the right-handed GL
// convention (view space looks down -z); ClipDepth selects whether NDC depth
// spans [-1, 1] (OpenGL, WebGL2) or [0, 1] (D3D11, Metal, WebGPU).
#pragma once

#include "kfm/core/fixed.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace kfm::presentation {

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * kPi;

// --- scalars ------------------------------------------------------------------

[[nodiscard]] constexpr float lerp(float a, float b, float t) {
  return a + ((b - a) * t);
}
[[nodiscard]] constexpr float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  return v > 1.0f ? 1.0f : v;
}
// Quadratic ease-out over [0, 1]: fast, then settling.
[[nodiscard]] constexpr float easeOut(float t) {
  return 1.0f - ((1.0f - t) * (1.0f - t));
}
[[nodiscard]] constexpr float degreesToRadians(float deg) {
  return deg * (kPi / 180.0f);
}

// Sim Angle (65536 BAM per turn, counter-clockwise from +x) to radians in [0, 2pi).
[[nodiscard]] constexpr float angleToRadians(Angle a) {
  return static_cast<float>(a.bam) * (kTwoPi / 65536.0f);
}

// Wraps an angle in radians to [-pi, pi).
[[nodiscard]] inline float wrapRadians(float a) {
  const float wrapped = std::fmod(a + kPi, kTwoPi);
  return (wrapped < 0.0f ? wrapped + kTwoPi : wrapped) - kPi;
}

// Interpolates two angles along the shortest arc; the result is wrapped.
[[nodiscard]] inline float lerpAngle(float a, float b, float t) {
  return wrapRadians(a + (wrapRadians(b - a) * t));
}

// Turns `current` toward `target` by at most `maxStep` radians (>= 0) along
// the shortest arc; the result is wrapped.
[[nodiscard]] inline float approachAngle(float current, float target, float maxStep) {
  const float delta = wrapRadians(target - current);
  if (std::fabs(delta) <= maxStep) return wrapRadians(target);
  return wrapRadians(current + (delta > 0.0f ? maxStep : -maxStep));
}

// --- vectors ------------------------------------------------------------------

struct Vec2f {
  float x{0.0f};
  float y{0.0f};

  friend constexpr Vec2f operator+(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
  friend constexpr Vec2f operator-(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }
  friend constexpr Vec2f operator*(Vec2f a, float s) { return {a.x * s, a.y * s}; }
  constexpr bool operator==(const Vec2f&) const = default;
};

[[nodiscard]] inline float length(Vec2f a) {
  return std::sqrt((a.x * a.x) + (a.y * a.y));
}
[[nodiscard]] constexpr Vec2f lerp(Vec2f a, Vec2f b, float t) {
  return {lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
}

struct Vec3 {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};

  friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
  friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
  friend constexpr Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
  friend constexpr Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
  friend constexpr Vec3 operator*(float s, Vec3 a) { return a * s; }
  constexpr bool operator==(const Vec3&) const = default;
};

[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) {
  return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) {
  return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}
[[nodiscard]] inline float length(Vec3 a) {
  return std::sqrt(dot(a, a));
}
// Unit vector along a; the zero vector stays zero.
[[nodiscard]] inline Vec3 normalize(Vec3 a) {
  const float len = length(a);
  return len > 0.0f ? a * (1.0f / len) : Vec3{};
}
[[nodiscard]] constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) {
  return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}

struct Vec4 {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
  float w{0.0f};
  constexpr bool operator==(const Vec4&) const = default;
};

// --- matrices -----------------------------------------------------------------

// Column-major: m[col * 4 + row].
struct Mat4 {
  std::array<float, 16> m{};

  [[nodiscard]] static constexpr Mat4 identity() {
    Mat4 r;
    r.m[0] = 1.0f;
    r.m[5] = 1.0f;
    r.m[10] = 1.0f;
    r.m[15] = 1.0f;
    return r;
  }
  [[nodiscard]] constexpr float& at(int row, int col) { return m[index(row, col)]; }
  [[nodiscard]] constexpr float at(int row, int col) const { return m[index(row, col)]; }
  [[nodiscard]] static constexpr std::size_t index(int row, int col) {
    return (static_cast<std::size_t>(col) * 4) + static_cast<std::size_t>(row);
  }
  // 16 floats, column-major, ready for a GL-style uniform upload.
  [[nodiscard]] constexpr const float* data() const { return m.data(); }
  constexpr bool operator==(const Mat4&) const = default;
};

[[nodiscard]] constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k)
        sum += a.at(row, k) * b.at(k, c);
      r.at(row, c) = sum;
    }
  }
  return r;
}

[[nodiscard]] constexpr Vec4 operator*(const Mat4& a, Vec4 v) {
  const auto row = [&](int r) {
    return (a.at(r, 0) * v.x) + (a.at(r, 1) * v.y) + (a.at(r, 2) * v.z) + (a.at(r, 3) * v.w);
  };
  return {row(0), row(1), row(2), row(3)};
}

// M * (p, 1) without the perspective divide (affine transforms).
[[nodiscard]] constexpr Vec3 transformPoint(const Mat4& a, Vec3 p) {
  const Vec4 r = a * Vec4{p.x, p.y, p.z, 1.0f};
  return {r.x, r.y, r.z};
}
// M * (p, 1) followed by the divide by w (projections). w must be non-zero.
[[nodiscard]] constexpr Vec3 projectPoint(const Mat4& a, Vec3 p) {
  const Vec4 r = a * Vec4{p.x, p.y, p.z, 1.0f};
  return {r.x / r.w, r.y / r.w, r.z / r.w};
}
// M * (d, 0): ignores translation.
[[nodiscard]] constexpr Vec3 transformDirection(const Mat4& a, Vec3 d) {
  const Vec4 r = a * Vec4{d.x, d.y, d.z, 0.0f};
  return {r.x, r.y, r.z};
}

[[nodiscard]] constexpr Mat4 transpose(const Mat4& a) {
  Mat4 r;
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row)
      r.at(row, c) = a.at(c, row);
  }
  return r;
}

[[nodiscard]] constexpr Mat4 translate(Vec3 t) {
  Mat4 r = Mat4::identity();
  r.at(0, 3) = t.x;
  r.at(1, 3) = t.y;
  r.at(2, 3) = t.z;
  return r;
}

[[nodiscard]] constexpr Mat4 scale(Vec3 s) {
  Mat4 r = Mat4::identity();
  r.at(0, 0) = s.x;
  r.at(1, 1) = s.y;
  r.at(2, 2) = s.z;
  return r;
}

// Right-handed rotation about +y by `radians`: +x turns toward -z. In the
// world basis (world_basis.hpp) this is exactly a sim Angle: rotateY(theta)
// maps a model's +x (its forward) onto the sim direction at angle theta.
[[nodiscard]] inline Mat4 rotateY(float radians) {
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  Mat4 r = Mat4::identity();
  r.at(0, 0) = c;
  r.at(0, 2) = s;
  r.at(2, 0) = -s;
  r.at(2, 2) = c;
  return r;
}

// Right-handed rotations about +x (+y turns toward +z) and +z (+x turns
// toward +y), for tilting placeholder shapes (power-up pickups).
[[nodiscard]] inline Mat4 rotateX(float radians) {
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  Mat4 r = Mat4::identity();
  r.at(1, 1) = c;
  r.at(1, 2) = -s;
  r.at(2, 1) = s;
  r.at(2, 2) = c;
  return r;
}
[[nodiscard]] inline Mat4 rotateZ(float radians) {
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  Mat4 r = Mat4::identity();
  r.at(0, 0) = c;
  r.at(0, 1) = -s;
  r.at(1, 0) = s;
  r.at(1, 1) = c;
  return r;
}

// General 4×4 inverse (cofactor expansion). Returns the identity for a
// singular matrix.
[[nodiscard]] Mat4 inverse(const Mat4& a);

enum class ClipDepth : uint8_t {
  NegativeOneToOne = 0,  // OpenGL / WebGL2
  ZeroToOne,             // D3D11, Metal, WebGPU
};

// Right-handed perspective: view space looks down -z; fovY in radians,
// aspect = width / height, 0 < zNear < zFar. zNear maps to NDC depth -1 (or 0)
// and zFar to +1.
[[nodiscard]] Mat4 perspective(float fovY, float aspect, float zNear, float zFar,
                               ClipDepth depth = ClipDepth::NegativeOneToOne);

// Right-handed orthographic projection of the view-space box
// [left, right] × [bottom, top] × [-zFar, -zNear].
[[nodiscard]] Mat4 orthographic(float left, float right, float bottom, float top, float zNear,
                                float zFar, ClipDepth depth = ClipDepth::NegativeOneToOne);

// Right-handed view matrix: the camera at `eye` looks at `target`, with `up`
// roughly up on screen. Requires eye != target and up not parallel to the view
// direction.
[[nodiscard]] Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up);

}  // namespace kfm::presentation
