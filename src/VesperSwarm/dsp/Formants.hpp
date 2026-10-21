#pragma once
#include <cmath>
#include <cstdio>
#include "Util.hpp"

namespace vesper {

static const int NUM_FORMANTS = 5;
static const int NUM_VOWELS = 5;

struct VowelSpec {
	const char* name;
	float freq[NUM_FORMANTS]; // Hz
	float db[NUM_FORMANTS];   // relative level
	float bw[NUM_FORMANTS];   // Hz
};

// The classic soprano formant table (Csound / CLM lineage): five vowels,
// five formants each. Formant 1 carries the vowel's weight; the upper
// formants give it a throat.
static const VowelSpec kSoprano[NUM_VOWELS] = {
	{"A", {800.f, 1150.f, 2900.f, 3900.f, 4950.f}, {0.f, -6.f, -32.f, -20.f, -50.f}, {80.f, 90.f, 120.f, 130.f, 140.f}},
	{"E", {350.f, 2000.f, 2800.f, 3600.f, 4950.f}, {0.f, -20.f, -15.f, -40.f, -56.f}, {60.f, 100.f, 120.f, 150.f, 200.f}},
	{"I", {270.f, 2140.f, 2950.f, 3900.f, 4950.f}, {0.f, -12.f, -26.f, -26.f, -44.f}, {60.f, 90.f, 100.f, 120.f, 120.f}},
	{"O", {450.f, 800.f, 2830.f, 3800.f, 4950.f}, {0.f, -11.f, -22.f, -22.f, -50.f}, {70.f, 80.f, 100.f, 130.f, 135.f}},
	{"U", {325.f, 700.f, 2700.f, 3800.f, 4950.f}, {0.f, -16.f, -35.f, -40.f, -60.f}, {50.f, 60.f, 170.f, 180.f, 200.f}},
};

struct FormantSet {
	float freq[NUM_FORMANTS];
	float gain[NUM_FORMANTS]; // linear
	float bw[NUM_FORMANTS];
};

// VOWEL position 0..4 sweeps A -> E -> I -> O -> U. Frequencies interpolate
// in the log domain, levels in dB, bandwidths linearly.
inline FormantSet morphVowel(float pos) {
	pos = clampf(pos, 0.f, (float)(NUM_VOWELS - 1));
	int i = (int)pos;
	if (i >= NUM_VOWELS - 1)
		i = NUM_VOWELS - 2;
	float t = pos - (float)i;
	const VowelSpec& a = kSoprano[i];
	const VowelSpec& b = kSoprano[i + 1];
	FormantSet s;
	for (int k = 0; k < NUM_FORMANTS; k++) {
		s.freq[k] = a.freq[k] * std::pow(b.freq[k] / a.freq[k], t);
		s.gain[k] = dbToGain(lerp(a.db[k], b.db[k], t));
		s.bw[k] = lerp(a.bw[k], b.bw[k], t);
	}
	return s;
}

// Readout for the display and the knob tooltip: "A" near a vowel, "A·E"
// between two.
inline void vowelName(float pos, char* out, size_t n) {
	pos = clampf(pos, 0.f, (float)(NUM_VOWELS - 1));
	int near = (int)(pos + 0.5f);
	if (std::fabs(pos - (float)near) < 0.2f) {
		std::snprintf(out, n, "%s", kSoprano[near].name);
		return;
	}
	int lo = (int)pos;
	if (lo >= NUM_VOWELS - 1)
		lo = NUM_VOWELS - 2;
	std::snprintf(out, n, "%s\xC2\xB7%s", kSoprano[lo].name, kSoprano[lo + 1].name);
}

} // namespace vesper
