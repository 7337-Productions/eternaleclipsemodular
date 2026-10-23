// Standalone harness for the Conclave of Hollow Radiance mixing engine.
// Build:  g++ -std=c++17 -O2 -D_USE_MATH_DEFINES -Isrc tools/conclave_harness.cpp -o conclave_harness
// The four checks from the OMN-102 spec, plus the limiter on its own:
//   1. every FEED at noon: the master is the plain weighted sum (delayed by
//      the limiter's lookahead), the chain breaks where an OUT is patched,
//      and the send bus is the post-fader sum;
//   2. silent input, one FEED past unity: the ring sings at the TUNE pitch
//      (and at half of it with the FEED inverted);
//   3. everything at its extremes for 60 s, including the master fed back
//      into the return: nothing above the ceiling, nothing non-finite, no
//      DC, and the hard clip idle;
//   4. CPU per sample.
#include "ConclaveOfHollowRadiance/dsp/Circle.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <vector>
#include <memory>
#include <algorithm>
#include <initializer_list>
#include <limits>

using namespace conclave;

static const float RATE = 48000.f;
static bool ok = true;

static void fail(const char* what) {
	std::printf("FAIL: %s\n", what);
	ok = false;
}

// Deterministic noise in -1..1
struct Rng {
	uint32_t s = 0x9E3779B9u;
	float next() {
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		return (float)(s & 0xFFFFFF) / 8388608.f - 1.f;
	}
};

// Fundamental frequency of a run of samples: the first strong peak of the
// normalised autocorrelation (lags from 2.5 kHz down to 8 Hz), refined by a
// parabola. Zero-crossing counting is fooled by the highpass undershoot
// after each pulse of a pulse train; this is not.
static float measureHz(const std::vector<float>& x, float rate) {
	int N = (int)x.size();
	int lmin = (int)(rate / 2500.f), lmax = std::min((int)(rate / 8.f), N / 2);
	std::vector<double> r(lmax + 1, 0.0);
	double r0 = 0.0;
	for (int i = 0; i < N; i++)
		r0 += (double)x[i] * x[i];
	if (r0 <= 0.0) return 0.f;
	double best = 0.0;
	for (int L = lmin; L <= lmax; L++) {
		double acc = 0.0;
		for (int i = L; i < N; i++)
			acc += (double)x[i] * x[i - L];
		r[L] = acc / r0 * (double)N / (N - L);
		best = std::fmax(best, r[L]);
	}
	for (int L = lmin + 1; L < lmax; L++) {
		if (r[L] > 0.8 * best && r[L] >= r[L - 1] && r[L] >= r[L + 1]) {
			double d = r[L - 1] - 2.0 * r[L] + r[L + 1];
			double frac = d != 0.0 ? 0.5 * (r[L - 1] - r[L + 1]) / d : 0.0;
			return (float)(rate / (L + frac));
		}
	}
	return 0.f;
}

// ---- 1. a straightforward mixer when the feeds are at noon ----
static void checkPlainMixer() {
	auto cPtr = std::make_unique<Circle>();
	Circle& c = *cPtr;
	c.reset(RATE);
	CircleParams p;
	float levels[CHANNELS] = {0.9f, 0.5f, 0.25f, 0.7f};
	float sends[CHANNELS] = {0.3f, 0.f, 1.f, 0.6f};
	for (int n = 0; n < CHANNELS; n++) {
		p.level[n] = levels[n];
		p.send[n] = sends[n];
	}
	p.ret = 0.8f;
	p.master = 0.9f;
	c.set(p);
	c.reset(RATE); // delays start at their targets

	Rng rng;
	int look = c.limiter.look;
	int total = (int)(10 * RATE);
	std::vector<float> expL(total), expR(total);
	float worst = 0.f, worstSend = 0.f;
	for (int i = 0; i < total; i++) {
		CircleIn in;
		float sumL = 0.f, sumR = 0.f, sendL = 0.f, sendR = 0.f;
		for (int n = 0; n < CHANNELS; n++) {
			in.in[n].l = 0.9f * rng.next();
			in.in[n].r = 0.9f * rng.next();
			sumL += in.in[n].l * levels[n];
			sumR += in.in[n].r * levels[n];
			sendL += in.in[n].l * levels[n] * sends[n];
			sendR += in.in[n].r * levels[n] * sends[n];
		}
		in.ret.l = 0.9f * rng.next();
		in.ret.r = 0.9f * rng.next();
		expL[i] = (sumL + in.ret.l * p.ret) * p.master;
		expR[i] = (sumR + in.ret.r * p.ret) * p.master;
		CircleOut out;
		c.process(in, out);
		worstSend = std::fmax(worstSend, std::fmax(std::fabs(out.send.l - sendL), std::fabs(out.send.r - sendR)));
		if (i >= look) {
			worst = std::fmax(worst, std::fabs(out.master.l - expL[i - look]));
			worst = std::fmax(worst, std::fabs(out.master.r - expR[i - look]));
		}
	}
	std::printf("plain mixer: worst master error %.2e V (lookahead %d samples), worst send error %.2e V\n", worst, look, worstSend);
	if (worst > 1e-6f) fail("plain mixer is not the weighted sum");
	if (worstSend > 1e-6f) fail("send bus is not the post-fader sum");
	if (c.limiter.gainReduction != 0.f) fail("limiter acted on a signal under the ceiling");

	// The chain breaks where an OUT is patched: OUT 2 takes channels 1+2, the master gets 3+4
	c.reset(RATE);
	p.outPatched[1] = true;
	p.ret = 0.f;
	c.set(p);
	c.reset(RATE); // delays start at their targets
	worst = 0.f;
	float worstOut2 = 0.f;
	for (int i = 0; i < total; i++) {
		CircleIn in;
		float m = 0.f, o2 = 0.f;
		for (int n = 0; n < CHANNELS; n++) {
			in.in[n].l = in.in[n].r = 0.9f * rng.next();
			if (n >= 2) m += in.in[n].l * levels[n];
			if (n <= 1) o2 += in.in[n].l * levels[n];
		}
		expL[i] = m * p.master;
		CircleOut out;
		c.process(in, out);
		worstOut2 = std::fmax(worstOut2, std::fabs(out.out[1].l - o2));
		if (i >= look)
			worst = std::fmax(worst, std::fabs(out.master.l - expL[i - look]));
	}
	std::printf("chain break at OUT 2: master error %.2e V, OUT 2 error %.2e V\n", worst, worstOut2);
	if (worst > 1e-6f || worstOut2 > 1e-6f) fail("patched OUT does not break the chain as specified");
}

// ---- 2. the ring sings at TUNE ----
static float ringPitch(float tuneHz, float feed, float matter, float* level, float burstVolts = 0.f) {
	auto cPtr = std::make_unique<Circle>();
	Circle& c = *cPtr;
	c.reset(RATE);
	CircleParams p;
	for (int n = 0; n < CHANNELS; n++) p.level[n] = 0.f;
	p.level[0] = 1.f;          // channel 1 open
	p.feed[1] = feed;          // channel 2's FEED back into channel 1: the 1 > 2 > 1 loop
	p.tuneHz[1] = tuneHz;
	p.matter = matter;
	c.limiter.ceiling = 10.f;  // out of the way; we want the loop's own level
	c.set(p);
	c.reset(RATE); // delays start at their targets
	// Near unity a ring grows slowly (the 0.1 mV self-seed needs ~220 round
	// trips at FEED 1.05), so give it 300 periods and then 6 s more
	int total = (int)((300.f / tuneHz + 6.f) * RATE);
	int tail = (int)(0.5f * RATE);
	int burst = (int)(0.05f * RATE);
	std::vector<float> x;
	x.reserve(tail);
	double acc = 0.0;
	Rng rng;
	for (int i = 0; i < total; i++) {
		CircleIn in;
		if (i < burst) in.in[0].l = in.in[0].r = burstVolts * rng.next(); // optional 50 ms noise burst
		CircleOut out;
		c.process(in, out);
		if (!std::isfinite(out.out[0].l)) { fail("non-finite in the ring"); return 0.f; }
		if (i >= total - tail) {
			x.push_back(out.out[0].l);
			acc += out.out[0].l * out.out[0].l;
		}
	}
	*level = (float)std::sqrt(acc / tail);
	return measureHz(x, RATE);
}

static void checkRing() {
	std::printf("ring self-oscillation (TUNE -> measured Hz / rms V):\n");
	for (float feed : {1.05f, 1.2f, 1.5f})
		for (float matter : {0.f, 0.5f, 1.f}) {
			std::printf("  FEED %.2f MATTER %.1f:", feed, matter);
			for (float tune : {20.f, 55.f, 110.f, 440.f, 1500.f, 2000.f}) {
				float level;
				float hz = ringPitch(tune, feed, matter, &level);
				float err = std::fabs(hz / tune - 1.f);
				std::printf("  %4.0f: %7.2f/%.2f", tune, hz, level);
				if (err > 0.01f) fail("ring pitch off by more than 1%");
				if (level < 0.5f) fail("ring does not sustain");
			}
			std::printf("\n");
		}
	float level;
	std::printf("  inverted FEED, sings an octave down (TUNE/MATTER: Hz / rms V):");
	for (float tune : {55.f, 440.f, 1500.f})
		for (float matter : {0.f, 1.f}) {
			float hz = ringPitch(tune, -1.2f, matter, &level);
			std::printf("  %4.0f/%.0f: %7.2f/%.2f", tune, matter, hz, level);
			if (std::fabs(hz / (0.5f * tune) - 1.f) > 0.01f) fail("inverted ring is not at half pitch");
			if (level < 0.5f) fail("inverted ring does not sustain");
		}
	std::printf("\n");
	float hz = ringPitch(440.f, 0.9f, 0.5f, &level, 5.f);
	std::printf("  FEED under unity at 440 after a 5 V burst: %.3e V rms; want silence\n", level);
	if (level > 1e-3f) fail("ring under unity does not die away");
	ringPitch(440.f, 0.f, 0.5f, &level);
	if (level != 0.f) fail("ring with FEED at noon is not exactly silent");
	// For the record: a loud burst before silence can leave a two-bump shape
	// in the loop, which saturates into the octave. Reported, not judged.
	std::printf("  after a 5 V burst, ratio to TUNE (FEED 1.1, MATTER 0.75):");
	for (float tune : {110.f, 440.f, 1100.f}) {
		hz = ringPitch(tune, 1.1f, 0.75f, &level, 5.f);
		std::printf("  %4.0f: %.3f", tune, hz / tune);
	}
	std::printf("\n");
}

// ---- 3. the extremes, with the master fed back into the return ----
static void checkStress() {
	struct Case { float feedSign[CHANNELS]; float matter; bool loop; float ceiling; const char* name; };
	Case cases[] = {
		{{1, 1, 1, 1}, 1.f, true, 5.f, "all +1.5, bright, LOOP, master into return"},
		{{1, -1, 1, -1}, 1.f, true, 5.f, "alternating, bright, LOOP"},
		{{-1, -1, -1, -1}, 0.f, false, 5.f, "all -1.5, dark, POST, master into return"},
		{{1, 1, 1, 1}, 0.5f, true, 10.f, "all +1.5, noon, LOOP, 10 V ceiling"},
	};
	float tunes[CHANNELS] = {20.f, 97.f, 440.f, 2000.f};
	for (const Case& k : cases) {
		auto cPtr = std::make_unique<Circle>();
		Circle& c = *cPtr;
		c.reset(RATE);
		CircleParams p;
		for (int n = 0; n < CHANNELS; n++) {
			p.level[n] = 1.f;
			p.send[n] = 1.f;
			p.feed[n] = 1.5f * k.feedSign[n];
			p.tuneHz[n] = tunes[n];
		}
		p.matter = k.matter;
		p.loopReturn = k.loop;
		p.ret = 1.f;
		p.master = 1.f;
		c.limiter.ceiling = k.ceiling;
		c.set(p);
		c.reset(RATE); // delays start at their targets
		Rng rng;
		int total = (int)(60 * RATE);
		int tail = (int)(30 * RATE);
		float over = 0.f;
		long clips = 0;
		double dcL = 0.0, dcR = 0.0, grAcc = 0.0;
		Frame prev;
		bool finite = true;
		for (int i = 0; i < total; i++) {
			CircleIn in;
			for (int n = 0; n < CHANNELS; n++) {
				in.in[n].l = 10.f * rng.next();
				in.in[n].r = 10.f * rng.next();
			}
			in.ret = prev; // the outside world's feedback: the master straight back in
			// Wobble TUNE 1 and MATTER every 4800 samples so the glide gets exercised
			if (i % 4800 == 0) {
				p.tuneHz[0] = 20.f + 1980.f * (0.5f + 0.5f * rng.next());
				c.set(p);
			}
			CircleOut out;
			c.process(in, out);
			prev = out.master;
			if (!std::isfinite(out.master.l) || !std::isfinite(out.master.r) || !std::isfinite(out.send.l)) {
				finite = false;
				break;
			}
			over = std::fmax(over, std::fmax(std::fabs(out.master.l), std::fabs(out.master.r)) - k.ceiling);
			if (c.limiter.clipped) clips++;
			if (i >= total - tail) {
				dcL += out.master.l;
				dcR += out.master.r;
				grAcc += c.limiter.gainReduction;
			}
		}
		dcL /= tail; dcR /= tail; grAcc /= tail;
		std::printf("stress [%s]: finite %s, over ceiling %.1e V, hard clips %ld (pre-clip overshoot %.1e V), DC %+.1f/%+.1f mV, mean GR %.1f dB\n",
			k.name, finite ? "yes" : "NO", over, clips, c.limiter.preClipOver, dcL * 1e3, dcR * 1e3,
			-20.0 * std::log10(std::fmax(1.0 - grAcc, 1e-6)));
		if (!finite) fail("non-finite under stress");
		if (over > 0.f) fail("sample above the ceiling");
		if (std::fabs(dcL) > 0.01 || std::fabs(dcR) > 0.01) fail("DC above 10 mV");
		// The clip exists for float rounding; anything past a few ulps is the limiter failing
		if (c.limiter.preClipOver > 1e-4f) fail("hard clip is doing the limiter's work");
	}
}

// ---- the limiter alone ----
static void checkLimiter() {
	Limiter lim;
	lim.reset(RATE);
	lim.ceiling = 5.f;
	// Transparent under the ceiling: output is the input, delayed by the lookahead
	std::vector<float> hist(lim.look + 1);
	float worst = 0.f;
	for (int i = 0; i < (int)RATE; i++) {
		float x = 3.f * std::sin(2.f * (float)M_PI * 100.f * i / RATE);
		hist[i % hist.size()] = x;
		float l = x, r = x;
		lim.process(l, r);
		if (i >= lim.look)
			worst = std::fmax(worst, std::fabs(l - hist[(i - lim.look) % hist.size()]));
	}
	std::printf("limiter: transparent under the ceiling to %.1e V\n", worst);
	if (worst > 1e-6f) fail("limiter is not transparent under the ceiling");
	// 20 V sine burst: never above 5 V, and the hard clip idle
	lim.reset(RATE);
	float peak = 0.f;
	long clips = 0;
	for (int i = 0; i < (int)(2 * RATE); i++) {
		float amp = (i > (int)(0.5f * RATE) && i < (int)(1.5f * RATE)) ? 20.f : 2.f;
		float x = amp * std::sin(2.f * (float)M_PI * 100.f * i / RATE);
		float l = x, r = -x;
		lim.process(l, r);
		peak = std::fmax(peak, std::fmax(std::fabs(l), std::fabs(r)));
		if (lim.clipped) clips++;
	}
	std::printf("limiter: 20 V burst peaks at %.4f V, hard clips %ld, pre-clip overshoot %.1e V\n", peak, clips, lim.preClipOver);
	if (peak > 5.f) fail("limiter let a peak through");
	if (lim.preClipOver > 1e-4f) fail("limiter leaned on the hard clip");
}

// ---- 4. CPU ----
static void checkCpu() {
	auto cPtr = std::make_unique<Circle>();
	Circle& c = *cPtr;
	c.reset(RATE);
	CircleParams p;
	for (int n = 0; n < CHANNELS; n++) {
		p.feed[n] = 0.8f;
		p.send[n] = 0.5f;
	}
	c.set(p);
	c.reset(RATE); // delays start at their targets
	Rng rng;
	int total = (int)(10 * RATE);
	float sink = 0.f;
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < total; i++) {
		CircleIn in;
		for (int n = 0; n < CHANNELS; n++)
			in.in[n].l = in.in[n].r = rng.next();
		CircleOut out;
		c.process(in, out);
		sink += out.master.l;
	}
	auto t1 = std::chrono::steady_clock::now();
	double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
	double perSample = us / total;
	std::printf("cpu: %.3f us/sample, %.2f%% of one core at 48 kHz (sink %g)\n", perSample, perSample * RATE / 1e4, sink);
}

// ---- 3b. poison: NaN and Inf from upstream are dropped, nothing sticks ----
// One bad sample on an input, on a closed channel, and on one side of the
// return; afterwards the ring must be clean and still pass a signal.
static void checkPoison() {
	struct Case { const char* name; float feed; bool loop; };
	const Case cases[] = {
		{"feeds at noon", 0.f, false},
		{"feeds past unity, LOOP", 1.2f, true},
	};
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	for (const Case& k : cases) {
		auto cPtr = std::make_unique<Circle>();
		Circle& c = *cPtr;
		c.reset(RATE);
		CircleParams p;
		for (int n = 0; n < CHANNELS; n++) {
			p.level[n] = n == 1 ? 0.f : 1.f; // channel 2 closed: Inf * 0
			p.feed[n] = k.feed;
		}
		p.loopReturn = k.loop;
		p.ret = 1.f;
		p.master = 1.f;
		c.set(p);
		c.reset(RATE);
		int total = (int)(2 * RATE);
		bool finite = true;
		double rms = 0.0;
		int counted = 0;
		for (int i = 0; i < total; i++) {
			CircleIn in;
			in.in[0].l = in.in[0].r = 1.f;
			if (i == 100) in.in[2].l = nan;
			if (i == 200) in.in[1].l = inf;
			if (i == 300) in.ret.r = nan;
			if (i == 400) in.in[3].l = -inf;
			CircleOut out;
			c.process(in, out);
			for (int n = 0; n < CHANNELS; n++)
				if (!std::isfinite(out.out[n].l) || !std::isfinite(out.out[n].r)) finite = false;
			if (!std::isfinite(out.master.l) || !std::isfinite(out.master.r)) finite = false;
			if (!std::isfinite(out.send.l) || !std::isfinite(out.send.r)) finite = false;
			if (i >= total - (int)RATE) {
				rms += (double)out.master.l * out.master.l;
				counted++;
			}
		}
		rms = std::sqrt(rms / counted);
		std::printf("poison [%s]: finite %s, master rms after %.3f V (want > 0.5)\n", k.name, finite ? "yes" : "NO", rms);
		if (!finite) fail("NaN or Inf reached an output");
		if (rms < 0.5) fail("the ring did not recover from a bad sample");
	}
}

int main() {
	checkLimiter();
	checkPlainMixer();
	checkRing();
	checkPoison();
	checkStress();
	checkCpu();
	if (!ok) {
		std::printf("\nconclave harness: FAILED\n");
		return 1;
	}
	std::printf("\nconclave harness: OK\n");
	return 0;
}
