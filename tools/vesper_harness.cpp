// Standalone sanity harness for the Vesper Swarm choir engine.
// Build:  g++ -std=c++17 -O2 -D_USE_MATH_DEFINES -Isrc tools/vesper_harness.cpp -o vesper_harness
// Sweeps pitch / swarm / vowel / breath / swarm law, asserts the output stays
// finite and the pre-clip peak sane, and prints level tables so the formant
// gains, breath level and makeup can be calibrated by eye.
#include "VesperSwarm/dsp/Choir.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <initializer_list>

static const float RATE = 48000.f;

struct Stats {
	float rms = 0.f, peak = 0.f;
};

// Run the engine for `seconds` after `warm` seconds of settling.
static Stats run(vesper::Choir& choir, const vesper::ChoirParams& p, float warm, float seconds, bool* ok) {
	Stats s;
	double acc = 0.0;
	int n = 0;
	int total = (int)((warm + seconds) * RATE);
	int skip = (int)(warm * RATE);
	choir.rawPeak = 0.f;
	for (int i = 0; i < total; i++) {
		if (i % vesper::BLOCK == 0)
			choir.setBlock(p, RATE);
		float l, r;
		choir.process(l, r);
		if (!std::isfinite(l) || !std::isfinite(r)) {
			*ok = false;
			return s;
		}
		if (i >= skip) {
			acc += 0.5 * (l * l + r * r);
			n++;
		}
	}
	s.rms = (float)std::sqrt(acc / std::fmax(n, 1));
	s.peak = choir.rawPeak;
	return s;
}

static float db(float x) {
	return 20.f * std::log10(std::fmax(x, 1e-9f));
}

int main() {
	bool ok = true;
	int checked = 0;
	float worstPeak = 0.f;
	char worstWhere[128] = "";

	// ---- Sweep: finite + bounded ----
	for (int law = 0; law < 2; law++)
		for (float pitch = -3.f; pitch <= 3.01f; pitch += 1.f)
			for (float swarm : {0.f, 0.5f, 1.f})
				for (float vowel = 0.f; vowel <= 4.01f; vowel += 0.5f)
					for (float breath : {0.f, 1.f}) {
						vesper::Choir choir;
						vesper::ChoirParams p;
						p.law = law;
						p.pitchVolts = pitch;
						p.swarm = swarm;
						p.vowel = vowel;
						p.breath = breath;
						p.attack = 0.005f;
						Stats s = run(choir, p, 0.1f, 0.2f, &ok);
						if (!ok) {
							std::printf("FAIL: non-finite at law=%d pitch=%g swarm=%g vowel=%g breath=%g\n",
							            law, pitch, swarm, vowel, breath);
							return 1;
						}
						if (s.peak > worstPeak) {
							worstPeak = s.peak;
							std::snprintf(worstWhere, sizeof worstWhere, "law=%d pitch=%g swarm=%g vowel=%g breath=%g",
							              law, pitch, swarm, vowel, breath);
						}
						checked++;
					}
	std::printf("sweep: %d combos finite, worst pre-clip peak %.3f at %s\n", checked, worstPeak, worstWhere);

	// ---- Level by vowel at C3 and C5 (all voices, swarm 0.15, breath 0) ----
	std::printf("\nvowel balance (RMS dBFS / pre-clip peak):\n");
	for (float pitch : {-1.f, 1.f}) {
		std::printf("  pitch %+.0f V:", pitch);
		for (int v = 0; v < vesper::NUM_VOWELS; v++) {
			vesper::Choir choir;
			vesper::ChoirParams p;
			p.pitchVolts = pitch;
			p.swarm = 0.15f;
			p.vowel = (float)v;
			p.attack = 0.005f;
			Stats s = run(choir, p, 0.2f, 0.3f, &ok);
			std::printf("  %s %5.1f/%4.2f", vesper::kSoprano[v].name, db(s.rms), s.peak);
		}
		std::printf("\n");
	}

	// ---- Level by pitch, vowel A ----
	std::printf("\npitch balance, vowel A (RMS dBFS / pre-clip peak):\n ");
	for (float pitch = -3.f; pitch <= 3.01f; pitch += 1.f) {
		vesper::Choir choir;
		vesper::ChoirParams p;
		p.pitchVolts = pitch;
		p.swarm = 0.15f;
		p.attack = 0.005f;
		Stats s = run(choir, p, 0.2f, 0.3f, &ok);
		std::printf("  %+.0fV %5.1f/%4.2f", pitch, db(s.rms), s.peak);
	}
	std::printf("\n");

	// ---- Breath: voiced vs voiced+breath ----
	{
		vesper::Choir a, b;
		vesper::ChoirParams p;
		p.swarm = 0.15f;
		p.attack = 0.005f;
		Stats voiced = run(a, p, 0.2f, 0.3f, &ok);
		p.breath = 1.f;
		Stats both = run(b, p, 0.2f, 0.3f, &ok);
		std::printf("\nbreath: voiced %.1f dB, voiced+breath(1.0) %.1f dB\n", db(voiced.rms), db(both.rms));
	}

	// ---- ADSR: gate on 0.5 s, off; expect sustain reached and release to zero ----
	{
		vesper::Choir choir;
		vesper::ChoirParams p;
		p.attack = 0.1f;
		p.decay = 0.1f;
		p.sustain = 0.5f;
		p.release = 0.2f;
		float atGateEnd = 0.f, afterRelease = 0.f;
		int n = (int)(1.0f * RATE);
		for (int i = 0; i < n; i++) {
			p.gate = i < (int)(0.5f * RATE);
			if (i % vesper::BLOCK == 0)
				choir.setBlock(p, RATE);
			float l, r;
			choir.process(l, r);
			if (i == (int)(0.5f * RATE) - 1)
				atGateEnd = choir.env.level;
			if (i == (int)(0.9f * RATE))
				afterRelease = choir.env.level;
		}
		std::printf("adsr: level at gate end %.3f (want ~0.5), 0.4 s into release %.4f (want ~0)\n",
		            atGateEnd, afterRelease);
		if (std::fabs(atGateEnd - 0.5f) > 0.02f || afterRelease > 0.01f) {
			std::printf("FAIL: adsr\n");
			return 1;
		}
	}

	// ---- Envelope on the filter: level stays up, cutoff sweeps, stays finite ----
	{
		vesper::Choir choir;
		vesper::ChoirParams p;
		p.envToFilter = true;
		p.cutoff = 80.f;
		p.res = 0.8f;
		p.attack = 0.05f;
		p.decay = 0.1f;
		p.sustain = 0.2f;
		p.release = 0.1f;
		float early = 0.f, late = 0.f;
		int n = (int)(1.0f * RATE);
		double accE = 0.0, accL = 0.0;
		for (int i = 0; i < n; i++) {
			p.gate = i < (int)(0.5f * RATE);
			if (i % vesper::BLOCK == 0)
				choir.setBlock(p, RATE);
			float l, r;
			choir.process(l, r);
			if (!std::isfinite(l) || !std::isfinite(r)) {
				std::printf("FAIL: non-finite with envelope on the filter\n");
				return 1;
			}
			if (i >= (int)(0.04f * RATE) && i < (int)(0.08f * RATE))
				accE += l * l;
			if (i >= (int)(0.9f * RATE))
				accL += l * l;
		}
		early = (float)std::sqrt(accE / (0.04f * RATE));
		late = (float)std::sqrt(accL / (0.1f * RATE));
		std::printf("env->vcf: open %.1f dB, closed (0.4 s after release) %.1f dB, still sounding\n", db(early), db(late));
		if (late < 1e-4f || early < late) {
			std::printf("FAIL: envelope on the filter\n");
			return 1;
		}
	}

	// ---- Voice toggling: no voices = silence, one voice = sound ----
	{
		vesper::Choir choir;
		vesper::ChoirParams p;
		p.attack = 0.005f;
		p.voiceMask = 0;
		Stats none = run(choir, p, 0.1f, 0.1f, &ok);
		p.voiceMask = 1;
		Stats one = run(choir, p, 0.1f, 0.1f, &ok);
		std::printf("mask: none %.4f rms, one voice %.1f dB\n", none.rms, db(one.rms));
		if (none.rms > 1e-6f) {
			std::printf("FAIL: silent mask leaks\n");
			return 1;
		}
	}

	// Typical peaks sit near 1.0; only sub-audio swarms (C1 at full swarm puts
	// a singer at 16 Hz) lean on the soft clipper, and may.
	if (worstPeak > 2.f) {
		std::printf("\nFAIL: pre-clip peak %.2f (the clipper is a safety, not the sound)\n", worstPeak);
		return 1;
	}
	std::printf("\nvesper harness: OK\n");
	return 0;
}
