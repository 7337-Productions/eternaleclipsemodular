#pragma once
#include <cmath>
#include <algorithm>
#include "Limiter.hpp"

// Conclave of Hollow Radiance (OMN-102): the mixing engine.
//
// Four stereo VCA channels in a ring. Forward, Veils' chain: channel N's
// output is its VCA plus whatever channel N-1 passed down, and an unpatched
// OUT passes it on; channel 4 falls into the master. Backward, the feedback:
// each channel's FEED taps that chained output, runs it through the
// channel's comb (a TUNE-long delay, darkened by MATTER, saturated and
// DC-blocked) and adds it into the *previous* channel's input, channel 1
// into channel 4, so the circle only closes when every FEED is engaged. A
// stereo send bus, a return that can enter the master (POST) or channel 1
// (LOOP, putting the effects inside the circle), a MASTER level and the
// lookahead limiter complete the path. Everything in volts, Rack-free, so
// tools/conclave_harness.cpp can drive it.

namespace conclave {

static const int CHANNELS = 4;
static const float PI_F = 3.14159265358979f;

inline float clampf(float x, float lo, float hi) {
	return x < lo ? lo : (x > hi ? hi : x);
}

// Soft clipping using tanh approximation, in +/-1 units (the Liminal Vast one)
inline float softClip(float x) {
	if (x > 3.f) return 1.f;
	if (x < -3.f) return -1.f;
	float x2 = x * x;
	return x * (27.f + x2) / (27.f + 9.f * x2);
}

struct Frame {
	float l = 0.f, r = 0.f;
};
inline Frame operator+(Frame a, Frame b) { return {a.l + b.l, a.r + b.r}; }
inline Frame operator*(Frame a, float g) { return {a.l * g, a.r * g}; }

struct CircleParams {
	float level[CHANNELS] = {1.f, 1.f, 1.f, 1.f}; // effective VCA gain, CV and response applied, 0..1
	float send[CHANNELS] = {0.f, 0.f, 0.f, 0.f};  // post-fader send, 0..1
	float feed[CHANNELS] = {0.f, 0.f, 0.f, 0.f};  // -1.5..1.5; 0 is off, negative inverts
	float tuneHz[CHANNELS] = {440.f, 440.f, 440.f, 440.f}; // comb pitch
	float matter = 0.5f;          // 0 dark .. 1 bright: damping of every comb
	float ret = 1.f;              // return level, 0..1
	bool loopReturn = false;      // true: return enters channel 1; false: the master
	bool outPatched[CHANNELS] = {false, false, false, false}; // a patched OUT breaks the chain
	float master = 1.f;           // 0..1
};

struct CircleIn {
	Frame in[CHANNELS];
	Frame ret;
};

struct CircleOut {
	Frame out[CHANNELS];
	Frame send;
	Frame master;
};

// One channel's feedback comb. What goes in is the channel's chained
// output; what comes out, TUNE later, is added into the previous channel.
// Between: a damping lowpass (MATTER, floored at 4x the pitch), the soft
// clip, and a highpass at a sixteenth of the pitch. The two filters keep
// the pitched mode the strongest thing in the loop: a saturating delay loop
// with gain over unity grows whichever mode has the most gain, and without
// the highpass the loop parks near saturation and motorboats at a few tens
// of hertz instead of singing; without the lowpass floor a dark MATTER
// starves a high TUNE the same way. The highpass also removes DC. With
// FEED inverted the loop sings an octave down (odd modes of half the
// pitch), and both filters follow that pitch instead.
//
// A digital loop has no noise floor: an unpatched input is exactly zero,
// and a ring with FEED past unity would stay silent forever. So while the
// loop is quiet and FEED is past unity the comb seeds itself with a sine of
// a tenth of a millivolt at the pitch it will sing. A sine, not a tick: a
// tick seeds every harmonic equally, and in the two hundred round trips a
// near-unity loop spends growing before it saturates, a bright loop's
// octave can win that race (the highpass favours it by a hair). A sine has
// no octave in it, so a ring started from silence sings TUNE itself every
// time. The seed stops as soon as the loop is alive. (A loud burst followed
// by silence is different: the loop then remembers the shape of what it
// was fed, and two evenly spaced bumps saturate into the octave.)
struct Comb {
	static constexpr int LEN = 65536; // 50 ms at 768 kHz is 38400
	static constexpr int MASK = LEN - 1;
	static constexpr float SAT_VOLTS = 5.f;   // the loop saturates around here
	static constexpr float SEED_VOLTS = 1e-4f; // the self-seeding tick
	static constexpr float QUIET_VOLTS = 1e-3f; // below this the loop counts as silent

	float bufL[LEN], bufR[LEN];
	int wpos = 0;
	float delay = 100.f, targetDelay = 100.f, glide = 0.001f; // samples
	float dampL = 0.f, dampR = 0.f, dampCoef = 1.f;
	float dcxL = 0.f, dcyL = 0.f, dcxR = 0.f, dcyR = 0.f, dcR = 0.999f; // highpass state and pole
	bool seedOn = false;   // FEED is past unity
	float seedPhase = 0.f, seedInc = 0.f;
	float loopPeak = 0.f, peakDecay = 0.9995f;

	void reset() {
		std::fill(bufL, bufL + LEN, 0.f);
		std::fill(bufR, bufR + LEN, 0.f);
		wpos = 0;
		delay = targetDelay;
		dampL = dampR = 0.f;
		dcxL = dcyL = dcxR = dcyR = 0.f;
		seedPhase = 0.f;
		loopPeak = 0.f;
	}

	Frame read() const {
		int i = (int)delay;
		float f = delay - i;
		int a = (wpos - i) & MASK;     // written `i` samples ago
		int b = (a - 1) & MASK;        // one older
		return {bufL[a] + f * (bufL[b] - bufL[a]), bufR[a] + f * (bufR[b] - bufR[a])};
	}

	void write(Frame x) {
		loopPeak = std::fmax(std::fmax(std::fabs(x.l), std::fabs(x.r)), loopPeak * peakDecay);
		if (seedOn && loopPeak < QUIET_VOLTS) {
			// The seed sings the pitch the loop is tuned to *now*, not where
			// TUNE is heading, so it stays coherent while the delay glides
			seedPhase += seedInc * (targetDelay / delay);
			if (seedPhase > 2.f * 3.14159265358979f)
				seedPhase -= 2.f * 3.14159265358979f;
			float s = SEED_VOLTS * std::sin(seedPhase);
			x.l += s;
			x.r += s;
		}
		// Clip first, then damp: the lowpass takes the clipper's harmonics
		// off before they go round again, which keeps a hard-driven loop
		// from aliasing itself into period doubling.
		float sl = softClip(x.l / SAT_VOLTS) * SAT_VOLTS;
		float sr = softClip(x.r / SAT_VOLTS) * SAT_VOLTS;
		dampL += dampCoef * (sl - dampL);
		dampR += dampCoef * (sr - dampR);
		float yl = dampL - dcxL + dcR * dcyL;
		float yr = dampR - dcxR + dcR * dcyR;
		dcxL = dampL; dcyL = yl;
		dcxR = dampR; dcyR = yr;
		bufL[wpos] = yl;
		bufR[wpos] = yr;
		wpos = (wpos + 1) & MASK;
		delay += (targetDelay - delay) * glide;
	}
};

struct Circle {
	static constexpr float MIN_HZ = 20.f, MAX_HZ = 2000.f;
	static constexpr float LP_FLOOR = 4.f;    // MATTER's lowpass never falls below this times the pitch
	static constexpr float HP_RATIO = 1.f / 16.f; // the loop highpass sits at this times the pitch
	static constexpr float GLIDE_S = 0.02f;   // TUNE changes slide over this
	static constexpr float PEAK_HOLD_S = 0.1f;

	float sampleRate = 48000.f;
	CircleParams p;
	Comb comb[CHANNELS];
	Limiter limiter;
	float peak[CHANNELS] = {0.f, 0.f, 0.f, 0.f};   // channel output peaks, for the display
	float fbPeak[CHANNELS] = {0.f, 0.f, 0.f, 0.f}; // feedback arriving at each channel, for the display
	float peakDecay = 0.999f;

	Circle() {
		reset(48000.f);
	}

	// Clears the delays and starts them at the lengths the current
	// parameters ask for: call set() first, then reset(), when the patch
	// loads, so a quiet ring does not glide in from the default pitch.
	void reset(float sr) {
		sampleRate = sr;
		for (int n = 0; n < CHANNELS; n++) {
			comb[n].glide = 1.f - std::exp(-1.f / (GLIDE_S * sr));
			comb[n].peakDecay = std::exp(-1.f / (PEAK_HOLD_S * sr));
		}
		set(p);
		for (int n = 0; n < CHANNELS; n++) {
			comb[n].reset();
			peak[n] = 0.f;
			fbPeak[n] = 0.f;
		}
		peakDecay = std::exp(-1.f / (PEAK_HOLD_S * sr));
		limiter.reset(sr);
	}

	// MATTER: 300 Hz at full dark, 4 kHz at noon, 12 kHz at full bright. The
	// top stops at 12 kHz so a hard-driven loop always has some smoothing
	// against the clipper's aliasing, the same at every sample rate.
	static float matterToCutoff(float m) {
		m = clampf(m, 0.f, 1.f);
		if (m < 0.5f)
			return 4000.f * std::pow(300.f / 4000.f, (0.5f - m) * 2.f);
		return 4000.f * std::pow(12000.f / 4000.f, (m - 0.5f) * 2.f);
	}

	// Call when the knobs move (per block is plenty); cheap enough per sample.
	void set(const CircleParams& np) {
		p = np;
		float sr = sampleRate;
		float matterFc = matterToCutoff(p.matter);
		for (int n = 0; n < CHANNELS; n++) {
			Comb& cb = comb[n];
			float f = clampf(p.tuneHz[n], MIN_HZ, MAX_HZ);
			bool inverted = p.feed[n] < 0.f;
			cb.seedOn = std::fabs(p.feed[n]) > 1.f;
			float f0 = inverted ? 0.5f * f : f; // the pitch the loop will sing
			float fc = std::fmin(std::fmax(matterFc, LP_FLOOR * f0), 0.45f * sr);
			float c = 1.f - std::exp(-2.f * PI_F * fc / sr);
			float a = 1.f - c;
			cb.dampCoef = c;
			float R = 1.f - 2.f * PI_F * (HP_RATIO * f0) / sr;
			cb.dcR = R;
			float w = 2.f * PI_F * f0 / sr;
			cb.seedInc = w;
			// The loop must come round with a phase of exactly -2*pi at the
			// pitch it sings (-pi with FEED inverted). The lowpass lags, so
			// the delay is shortened to absorb it (the Karplus-Strong tuning
			// correction). The highpass leads a sine by atan(fc/f) but passes
			// a pulse untouched, and a saturated loop carries pulses, so half
			// its lead is absorbed: TUNE then reads within half a percent
			// whether the loop is barely singing or driven hard.
			float phLp = -std::atan2(a * std::sin(w), 1.f - a * std::cos(w));
			float phHp = std::atan2(std::sin(w), 1.f - std::cos(w)) - std::atan2(R * std::sin(w), 1.f - R * std::cos(w));
			float target = inverted ? PI_F : 2.f * PI_F;
			float d = (target + phLp + 0.5f * phHp) / w;
			cb.targetDelay = clampf(d, 1.f, (float)(Comb::LEN - 3));
		}
	}

	static float finiteOrZero(float v) { return std::isfinite(v) ? v : 0.f; }
	static Frame finiteOrZero(Frame f) { return {finiteOrZero(f.l), finiteOrZero(f.r)}; }

	void process(const CircleIn& raw, CircleOut& out) {
		// A NaN or Inf from upstream must never reach a comb: its filter state
		// would hold it for good, and NaN * 0 is NaN, so even a FEED at noon
		// would carry it round the ring. Drop the sample instead.
		CircleIn in;
		for (int n = 0; n < CHANNELS; n++)
			in.in[n] = finiteOrZero(raw.in[n]);
		in.ret = finiteOrZero(raw.ret);

		// Read every comb first: comb N delivers into channel N-1, channel 1's into channel 4
		Frame fb[CHANNELS];
		for (int n = 0; n < CHANNELS; n++)
			fb[(n + CHANNELS - 1) % CHANNELS] = comb[n].read() * p.feed[n];

		Frame chain, sendBus;
		for (int n = 0; n < CHANNELS; n++) {
			fbPeak[n] = std::fmax(std::fmax(std::fabs(fb[n].l), std::fabs(fb[n].r)), fbPeak[n] * peakDecay);
			Frame x = in.in[n] + fb[n];
			if (n == 0 && p.loopReturn)
				x = x + in.ret * p.ret;
			Frame vca = x * p.level[n];
			Frame o = vca + chain;
			out.out[n] = o;
			sendBus = sendBus + vca * p.send[n];
			chain = p.outPatched[n] ? Frame{} : o;
			comb[n].write(o);
			peak[n] = std::fmax(std::fmax(std::fabs(o.l), std::fabs(o.r)), peak[n] * peakDecay);
		}
		out.send = sendBus;

		Frame m = chain;
		if (!p.loopReturn)
			m = m + in.ret * p.ret;
		m = m * p.master;
		limiter.process(m.l, m.r);
		out.master = m;
	}
};

} // namespace conclave
