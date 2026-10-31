/*
Tiles - tiling scene transitions for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Just enough HLSL, in C++, to compile the math the effects keep in their
 * *_math.effect includes, so the tests run the shaders' own code instead of a
 * copy of it.
 *
 * The effects' uniforms are not declared in those files - they belong to the
 * effect that includes them - so a test declares each one as a plain global
 * before including the math, and sets it the way the plugin would.
 *
 * HLSL literals are single precision. Built with GCC the tests pass
 * -fsingle-precision-constant so that C++ agrees; elsewhere an unsuffixed
 * literal promotes the expression it is in to double, which is closer to exact
 * than the GPU rather than further from it.
 *
 * Only what the effects use is here. A swizzle is a named member, so one the
 * math starts using has to be added below.
 */

#pragma once

#include <cmath>

namespace hlsl {

template<int N> struct vec;

/* A read-only view of some of a vector's components, as another vector. */
template<int N, int... I> struct swizzle {
	float d[N];

	operator vec<sizeof...(I)>() const { return vec<sizeof...(I)>(d[I]...); }

	swizzle &operator*=(float s)
	{
		((d[I] *= s), ...);
		return *this;
	}
};

template<> struct vec<2> {
	union {
		float v[2];
		struct {
			float x, y;
		};
		struct {
			float r, g;
		};
	};

	vec() : x(0.0f), y(0.0f) {}
	vec(float a, float b) : x(a), y(b) {}
	vec(const vec &o) : x(o.x), y(o.y) {}
	vec &operator=(const vec &o)
	{
		x = o.x;
		y = o.y;
		return *this;
	}
};

template<> struct vec<3> {
	union {
		float v[3];
		struct {
			float x, y, z;
		};
		struct {
			float r, g, b;
		};
		swizzle<3, 0, 1> xy;
	};

	vec() : x(0.0f), y(0.0f), z(0.0f) {}
	vec(float a, float b, float c) : x(a), y(b), z(c) {}
	vec(const vec<2> &a, float c) : x(a.x), y(a.y), z(c) {}
	vec(const vec &o) : x(o.x), y(o.y), z(o.z) {}
	vec &operator=(const vec &o)
	{
		x = o.x;
		y = o.y;
		z = o.z;
		return *this;
	}
};

template<> struct vec<4> {
	union {
		float v[4];
		struct {
			float x, y, z, w;
		};
		struct {
			float r, g, b, a;
		};
		swizzle<4, 0, 1> xy;
		swizzle<4, 0, 1, 2> xyz;
		swizzle<4, 0, 1, 2> rgb;
	};

	vec() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
	vec(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
	vec(const vec<3> &a, float d) : x(a.x), y(a.y), z(a.z), w(d) {}
	vec(const vec &o) : x(o.x), y(o.y), z(o.z), w(o.w) {}
	vec &operator=(const vec &o)
	{
		x = o.x;
		y = o.y;
		z = o.z;
		w = o.w;
		return *this;
	}
};

typedef vec<2> float2;
typedef vec<3> float3;
typedef vec<4> float4;

/* ------------------------------------------------------------------ */
/* operators, componentwise and with a scalar on either side          */
/* ------------------------------------------------------------------ */

#define HLSL_BINARY(OP)                                                     \
	template <int N> inline vec<N> operator OP(vec<N> a, const vec<N> &b) \
	{                                                                   \
		for (int i = 0; i < N; i++)                                 \
			a.v[i] = a.v[i] OP b.v[i];                          \
		return a;                                                   \
	}                                                                   \
	template <int N> inline vec<N> operator OP(vec<N> a, float s)       \
	{                                                                   \
		for (int i = 0; i < N; i++)                                 \
			a.v[i] = a.v[i] OP s;                               \
		return a;                                                   \
	}                                                                   \
	template <int N> inline vec<N> operator OP(float s, vec<N> a)       \
	{                                                                   \
		for (int i = 0; i < N; i++)                                 \
			a.v[i] = s OP a.v[i];                               \
		return a;                                                   \
	}                                                                   \
	template <int N> inline vec<N> &operator OP##=(vec<N> &a, const vec<N> &b) \
	{                                                                   \
		a = a OP b;                                                 \
		return a;                                                   \
	}                                                                   \
	template <int N> inline vec<N> &operator OP##=(vec<N> &a, float s)  \
	{                                                                   \
		a = a OP s;                                                 \
		return a;                                                   \
	}

HLSL_BINARY(+)
HLSL_BINARY(-)
HLSL_BINARY(*)
HLSL_BINARY(/)

#undef HLSL_BINARY

/* An unsuffixed literal is a double in C++, which would not deduce against the
 * float overloads above. */
#define HLSL_DOUBLE(OP)                                                          \
	template <int N> inline vec<N> operator OP(const vec<N> &a, double s)    \
	{                                                                        \
		return a OP(float) s;                                            \
	}                                                                        \
	template <int N> inline vec<N> operator OP(double s, const vec<N> &a)    \
	{                                                                        \
		return (float)s OP a;                                            \
	}                                                                        \
	template <int N> inline vec<N> &operator OP##=(vec<N> &a, double s)      \
	{                                                                        \
		return a OP## = (float)s;                                        \
	}

HLSL_DOUBLE(+)
HLSL_DOUBLE(-)
HLSL_DOUBLE(*)
HLSL_DOUBLE(/)

#undef HLSL_DOUBLE

template<int N> inline vec<N> operator-(vec<N> a)
{
	for (int i = 0; i < N; i++)
		a.v[i] = -a.v[i];
	return a;
}

/* ------------------------------------------------------------------ */
/* intrinsics                                                         */
/* ------------------------------------------------------------------ */

inline float frac(float x)
{
	return x - floorf(x);
}

inline float saturate(float x)
{
	return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

inline float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

inline float clamp(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

inline float max(float a, float b)
{
	return a > b ? a : b;
}

inline float min(float a, float b)
{
	return a < b ? a : b;
}

inline float smoothstep(float lo, float hi, float x)
{
	float t = saturate((x - lo) / (hi - lo));

	return t * t * (3.0f - 2.0f * t);
}

inline float step(float edge, float x)
{
	return x >= edge ? 1.0f : 0.0f;
}

inline float sign(float x)
{
	return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f);
}

/* The standard library already has float overloads of these, the same names
 * HLSL uses, and so does the global namespace through <math.h>; defining them
 * again here would make every call ambiguous. HLSL's fmod keeps the sign of x,
 * like C's. */
using std::abs;
using std::atan2;
using std::ceil;
using std::cos;
using std::exp;
using std::floor;
using std::fmod;
using std::log;
using std::pow;
using std::sin;
using std::sqrt;

#define HLSL_UNARY(NAME)                              \
	template <int N> inline vec<N> NAME(vec<N> a) \
	{                                             \
		for (int i = 0; i < N; i++)           \
			a.v[i] = NAME(a.v[i]);        \
		return a;                             \
	}

HLSL_UNARY(frac)
HLSL_UNARY(saturate)
HLSL_UNARY(abs)
HLSL_UNARY(floor)
HLSL_UNARY(ceil)
HLSL_UNARY(sqrt)
HLSL_UNARY(sin)
HLSL_UNARY(cos)
HLSL_UNARY(exp)
HLSL_UNARY(log)
HLSL_UNARY(sign)

#undef HLSL_UNARY

#define HLSL_BINARY_FN(NAME)                                               \
	template <int N> inline vec<N> NAME(vec<N> a, const vec<N> &b)     \
	{                                                                  \
		for (int i = 0; i < N; i++)                                \
			a.v[i] = NAME(a.v[i], b.v[i]);                     \
		return a;                                                  \
	}                                                                  \
	template <int N> inline vec<N> NAME(vec<N> a, float s)             \
	{                                                                  \
		for (int i = 0; i < N; i++)                                \
			a.v[i] = NAME(a.v[i], s);                          \
		return a;                                                  \
	}

HLSL_BINARY_FN(max)
HLSL_BINARY_FN(min)
HLSL_BINARY_FN(pow)
HLSL_BINARY_FN(step)
HLSL_BINARY_FN(fmod)

#undef HLSL_BINARY_FN

template<int N> inline vec<N> clamp(vec<N> x, float lo, float hi)
{
	for (int i = 0; i < N; i++)
		x.v[i] = clamp(x.v[i], lo, hi);
	return x;
}

template<int N> inline vec<N> lerp(const vec<N> &a, const vec<N> &b, float t)
{
	return a + (b - a) * t;
}

template<int N> inline vec<N> lerp(const vec<N> &a, const vec<N> &b, const vec<N> &t)
{
	return a + (b - a) * t;
}

template<int N> inline float dot(const vec<N> &a, const vec<N> &b)
{
	float s = 0.0f;

	for (int i = 0; i < N; i++)
		s += a.v[i] * b.v[i];
	return s;
}

template<int N> inline float length(const vec<N> &a)
{
	return sqrtf(dot(a, a));
}

template<int N> inline vec<N> normalize(const vec<N> &a)
{
	return a / length(a);
}

inline float3 cross(const float3 &a, const float3 &b)
{
	return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

/* A swizzle is not a vec, so it does not deduce against the templates above;
 * these give it somewhere to go. */
inline float3 lerp(const float3 &a, const float3 &b, double t)
{
	return lerp(a, b, (float)t);
}

} // namespace hlsl
