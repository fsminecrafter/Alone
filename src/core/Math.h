#pragma once
#include <algorithm>
#include <cmath>

namespace alone {

constexpr float kPi = 3.14159265358979f;

struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    float length() const { return std::sqrt(x * x + y * y); }
    float lengthSq() const { return x * x + y * y; }
    Vec2 normalized() const { float l = length(); return l > 1e-6f ? Vec2{x / l, y / l} : Vec2{}; }
};

inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline float distance(Vec2 a, Vec2 b) { return (a - b).length(); }
inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
// 0 below lo, 1 above hi, linear in between.
inline float ramp(float v, float lo, float hi) { return clamp01((v - lo) / (hi - lo)); }
inline float smoothstep(float lo, float hi, float v) { float t = ramp(v, lo, hi); return t * t * (3 - 2 * t); }

// Distance from p to segment ab; also returns the parameter t along ab.
inline float distToSegment(Vec2 p, Vec2 a, Vec2 b, float* tOut = nullptr) {
    Vec2 ab = b - a;
    float len2 = ab.lengthSq();
    float t = len2 > 0 ? std::clamp(dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    if (tOut) *tOut = t;
    return distance(p, a + ab * t);
}

} // namespace alone
