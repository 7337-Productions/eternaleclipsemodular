#pragma once
#include <cmath>
#include <cstdint>
#include "Util.hpp"
#include "Formants.hpp"

namespace vesper {

static const float PI_F = 3.14159265358979f;
static const float TWO_PI_F = 6.28318530717959f;

// TPT state-variable bandpass (Zavalishin / Cytomic form), unity gain at the
// centre frequency. One per formant.
struct BandPass {
	float ic1 = 0.f, ic2 = 0.f;
	float a1 = 0.f, a2 = 0.f, a3 = 0.f, k = 1.f;

	void set(float fc, float bw, float sampleRate) {
		fc = clampf(fc, 20.f, sampleRate * 0.45f);
		bw = clampf(bw, 10.f, fc); // Q never below 1
		float g = std::tan(PI_F * fc / sampleRate);
		k = bw / fc;
		a1 = 1.f / (1.f + g * (g + k));
		a2 = g * a1;
		a3 = g * a2;
	}

	float process(float v0) {
		float v3 = v0 - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		return k * v1;
	}

	void reset() {
		ic1 = ic2 = 0.f;
	}
};

// What makes one singer differ from the next. Fixed per voice index and
// deterministic, so a patch sounds the same every time it loads.
struct Singer {
	float tract;      // vocal tract length factor on every formant
	float detune;     // cents, static
	float vibRate;    // Hz
	float vibDepth;   // cents
	float driftRate;  // Hz, slow wobble on the vibrato rate
	float phase0;     // oscillator / LFO start phase
	float pan;        // -1 left .. +1 right
	uint32_t noiseSeed;
};

inline float hash01(uint32_t& h) {
	h = h * 1664525u + 1013904223u;
	return (float)((h >> 8) & 0xFFFF) / 65535.f;
}

inline Singer makeSinger(int index, int count) {
	uint32_t h = 0x9E3779B9u * (uint32_t)(index + 1) + 0x7F4A7C15u;
	Singer s;
	s.tract = 1.f + (hash01(h) - 0.5f) * 0.08f;
	s.detune = (hash01(h) - 0.5f) * 8.f;
	s.vibRate = 4.8f + hash01(h) * 1.6f;
	s.vibDepth = 15.f + hash01(h) * 15.f;
	s.driftRate = 0.08f + hash01(h) * 0.2f;
	s.phase0 = hash01(h);
	s.noiseSeed = h | 1u;
	// Fanned across the stereo field by rank, outer voices widest
	float rank = ((float)index - 0.5f * (float)(count - 1)) / (0.5f * (float)(count - 1));
	s.pan = rank * 0.7f;
	return s;
}

// PolyBLEP residual for a sawtooth discontinuity at phase 0.
inline float polyBlep(float t, float dt) {
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.f;
	}
	if (t > 1.f - dt) {
		t = (t - 1.f) / dt;
		return t * t + t + t + 1.f;
	}
	return 0.f;
}

// One choir voice: a band-limited glottal pulse train (the differentiated
// polyBLEP sawtooth, scaled so every harmonic has the same amplitude, which
// is what the formant table's levels assume) plus breath noise, through five
// parallel formant bandpasses, with its own vibrato. Output in +/-1 units.
// Rack-free.
struct ChoirVoice {
	Singer singer{};
	float phase = 0.f, inc = 0.f;
	float prevSaw = 0.f;
	float pulseGain = 0.f;
	uint32_t rng = 1u;
	BandPass bp[NUM_FORMANTS];
	float gain[NUM_FORMANTS] = {};
	float breath = 0.f;
	float vibPhase = 0.f, driftPhase = 0.f;
	float onset = 0.f; // vibrato develops after the note starts
	bool lastGate = false;

	static constexpr float SRC_LEVEL = 0.3f;    // amplitude of every source harmonic
	static constexpr float BREATH_LEVEL = 3.f;  // breath 1.0 ~ a little under the voiced part
	static constexpr float BW_FLOOR = 0.5f;     // bandwidth never narrower than this fraction of f0
	static constexpr float TUNED_GAIN = 0.5f;   // the raised first formant sits right on the fundamental

	void init(int index, int count) {
		singer = makeSinger(index, count);
		phase = singer.phase0;
		vibPhase = singer.phase0;
		driftPhase = 1.f - singer.phase0;
		rng = singer.noiseSeed;
	}

	void reset() {
		prevSaw = 0.f;
		onset = 0.f;
		for (int k = 0; k < NUM_FORMANTS; k++)
			bp[k].reset();
	}

	// Once per block. baseFreq is this voice's swarm pitch in Hz.
	void setBlock(float baseFreq, const FormantSet& fs, float breathAmt, bool gate, float blockDt, float sampleRate) {
		if (gate && !lastGate)
			onset = 0.f;
		lastGate = gate;
		if (gate)
			onset = std::fmin(1.f, onset + blockDt / 0.4f);

		driftPhase += singer.driftRate * blockDt;
		if (driftPhase >= 1.f)
			driftPhase -= 1.f;
		float rate = singer.vibRate * (1.f + 0.08f * std::sin(TWO_PI_F * driftPhase));
		vibPhase += rate * blockDt;
		if (vibPhase >= 1.f)
			vibPhase -= 1.f;
		float cents = singer.detune + onset * singer.vibDepth * std::sin(TWO_PI_F * vibPhase);
		float f = clampf(baseFreq * std::exp2(cents / 1200.f), 8.f, sampleRate * 0.45f);
		inc = f / sampleRate;
		// A unit-area impulse train at rate inc has harmonics of amplitude
		// 2*inc; scale them flat
		pulseGain = SRC_LEVEL / (2.f * inc);
		// Below ~C3 the table bandwidths take in more than one harmonic each
		// and the level creeps up; ease it back
		pulseGain *= clampf(std::pow(f / 130.f, 0.4f), 0.6f, 1.f);

		// Formant bandwidths widen with pitch, as a soprano's do: with few
		// harmonics per formant the level would otherwise swing wildly on
		// whether one happens to sit on a resonance.
		float bwFloor = BW_FLOOR * f;
		for (int k = 0; k < NUM_FORMANTS; k++) {
			float fc = fs.freq[k] * singer.tract;
			float bw = std::fmax(fs.bw[k] * singer.tract, bwFloor);
			float g = fs.gain[k];
			// Soprano formant tuning: the first formant rides up with the
			// fundamental rather than leaving the note hollow
			if (k == 0 && fc < f) {
				fc = f;
				g *= TUNED_GAIN;
			}
			bp[k].set(fc, bw, sampleRate);
			gain[k] = g;
		}

		breath = breathAmt;
	}

	float process() {
		phase += inc;
		if (phase >= 1.f)
			phase -= 1.f;
		float saw = 2.f * phase - 1.f - polyBlep(phase, inc);
		// Differentiate: the ramp cancels to zero, the wrap leaves a
		// band-limited pulse of area one per period
		float src = ((prevSaw - saw) * 0.5f + inc) * pulseGain;
		prevSaw = saw;

		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		float noise = (float)(int32_t)rng * (1.f / 2147483648.f);
		src += breath * BREATH_LEVEL * noise;

		float out = 0.f;
		for (int k = 0; k < NUM_FORMANTS; k++)
			out += gain[k] * bp[k].process(src);
		return out;
	}
};

} // namespace vesper
