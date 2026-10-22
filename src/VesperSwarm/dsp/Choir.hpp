#pragma once
#include <cmath>
#include <cstdint>
#include "Util.hpp"
#include "Formants.hpp"
#include "ChoirVoice.hpp"
#include "Adsr.hpp"
#include "CoronalAnnihilator/dsp/OtaFilter.hpp"

namespace vesper {

static const int VOICES = 8;
static const int BLOCK = 16;

enum SwarmLaw {
	LAW_EQUAL = 0,    // equal semitone steps fanned around the pitch
	LAW_HARMONIC = 1  // whole-number multiples of the pitch (Swarmatron)
};

struct ChoirParams {
	float pitchVolts = 0.f; // V/oct relative to C4, glide already applied
	float swarm = 0.f;      // 0..1
	int law = LAW_EQUAL;
	float vowel = 0.f;      // 0..4
	float breath = 0.f;     // 0..1
	uint8_t voiceMask = 0xFF;
	bool gate = true;
	float cutoff = 20000.f; // Hz
	float res = 0.f;        // 0..1
	float attack = 0.15f, decay = 0.1f, sustain = 1.f, release = 0.4f; // s
	bool envToFilter = false; // ADSR opens the filter instead of the amp
};

// Eight voices -> stereo mix -> 4-pole lowpass -> ADSR. Output in +/-1 units.
// Rack-free so tools/vesper_harness.cpp can drive it.
struct Choir {
	ChoirVoice voices[VOICES];
	float panL[VOICES], panR[VOICES];
	float voicePeak[VOICES] = {}; // for the display
	Adsr env;
	corona::OtaCoeffs coeffs;
	corona::OtaFilter fltL, fltR;
	uint8_t mask = 0xFF;
	bool gate = true;
	bool envToFilter = false;
	float mixGain = 0.f;
	float rawPeak = 0.f; // pre-clip peak since the last read

	// Mix level: eight incoherent voices normalised by sqrt(count), then
	// brought up to a healthy modular level before the safety clipper.
	static constexpr float MAKEUP = 1.6f;
	// Envelope -> filter: full envelope lifts the cutoff this many octaves
	static constexpr float ENV_OCTAVES = 7.f;

	Choir() {
		for (int i = 0; i < VOICES; i++) {
			voices[i].init(i, VOICES);
			float a = (voices[i].singer.pan + 1.f) * 0.25f * PI_F;
			panL[i] = std::cos(a);
			panR[i] = std::sin(a);
		}
	}

	void reset() {
		for (int i = 0; i < VOICES; i++) {
			voices[i].reset();
			voicePeak[i] = 0.f;
		}
		env.reset();
		fltL.reset();
		fltR.reset();
	}

	static float voiceFreq(float f0, int i, float swarm, int law) {
		if (law == LAW_HARMONIC)
			return f0 * (1.f + (float)i * swarm);
		float step = 3.43f * swarm * swarm; // semitones per rank; +/-12 at full swarm
		return f0 * std::exp2(((float)i - 0.5f * (float)(VOICES - 1)) * step / 12.f);
	}

	void setBlock(const ChoirParams& p, float sampleRate) {
		float blockDt = (float)BLOCK / sampleRate;
		float f0 = 261.6256f * std::exp2(clampf(p.pitchVolts, -8.f, 8.f));
		FormantSet fs = morphVowel(p.vowel);
		float swarm = clampf(p.swarm, 0.f, 1.f);
		mask = p.voiceMask;
		gate = p.gate;
		int active = 0;
		for (int i = 0; i < VOICES; i++) {
			if (!((mask >> i) & 1u))
				continue;
			active++;
			voices[i].setBlock(voiceFreq(f0, i, swarm, p.law), fs, clampf(p.breath, 0.f, 1.f), gate, blockDt, sampleRate);
		}
		mixGain = active > 0 ? MAKEUP / std::sqrt((float)active) : 0.f;
		env.setBlock(p.attack, p.decay, p.sustain, p.release, 1.f / sampleRate);
		envToFilter = p.envToFilter;
		float cutoff = p.cutoff;
		if (envToFilter)
			cutoff = clampf(cutoff * std::exp2(ENV_OCTAVES * env.level), 20.f, sampleRate * 0.45f);
		coeffs.update(cutoff, p.res, sampleRate);
	}

	void process(float& l, float& r) {
		float sl = 0.f, sr = 0.f;
		for (int i = 0; i < VOICES; i++) {
			if (!((mask >> i) & 1u)) {
				voicePeak[i] *= 0.999f;
				continue;
			}
			float v = voices[i].process();
			float a = std::fabs(v);
			voicePeak[i] = a > voicePeak[i] ? a : voicePeak[i] * 0.999f;
			sl += v * panL[i];
			sr += v * panR[i];
		}
		sl *= mixGain;
		sr *= mixGain;
		sl = fltL.process(sl, coeffs, true);
		sr = fltR.process(sr, coeffs, true);
		float e = env.process(gate);
		if (!envToFilter) {
			sl *= e;
			sr *= e;
		}
		float pk = std::fmax(std::fabs(sl), std::fabs(sr));
		if (pk > rawPeak)
			rawPeak = pk;
		l = softClip(sl);
		r = softClip(sr);
	}
};

} // namespace vesper
