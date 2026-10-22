#pragma once
#include <cmath>

// Small numeric helpers for Vesper Swarm. Local copy by convention (each
// module carries its own util header so modules stay self-contained).
namespace vesper {

inline float lerp(float a, float b, float t) {
	return a + t * (b - a);
}

// Soft clipping using tanh approximation
inline float softClip(float x) {
	if (x > 3.f) return 1.f;
	if (x < -3.f) return -1.f;
	float x2 = x * x;
	return x * (27.f + x2) / (27.f + 9.f * x2);
}

// Clamp value to range. NaN-safe: a NaN input lands on lo instead of
// passing through, so every CV entry point is a NaN firewall.
inline float clampf(float x, float lo, float hi) {
	return x >= lo ? (x <= hi ? x : hi) : lo;
}

// Audio-path firewall: NaN/Inf becomes silence.
inline float sanitize(float x) {
	return std::isfinite(x) ? x : 0.f;
}

inline float dbToGain(float db) {
	return std::pow(10.f, db * 0.05f);
}

} // namespace vesper
