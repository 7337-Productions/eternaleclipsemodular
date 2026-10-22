#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace conclave {

// Stereo lookahead peak limiter: the last thing before the Conclave's
// outputs. The signal is delayed by the lookahead. The gain applied to it is
// the sliding minimum of the required gain over a window one sample longer
// than that delay, smoothed by a moving average exactly as long as the
// delay, so the gain has fully reached its floor by the time the peak that
// demanded it comes out of the delay line: nothing above the ceiling gets
// through. A release stage before the minimum lets the gain return slowly,
// and program-dependently (50 ms after brief hits, 300 ms after sustained
// reduction). A hard clip at the ceiling follows as the last resort for
// float rounding; it should essentially never engage.
// Rack-free so tools/conclave_harness.cpp can drive it.
struct Limiter {
	static constexpr int MAX_LOOK = 2048;      // 2 ms at 768 kHz is 1536
	static constexpr int MASK = MAX_LOOK - 1;
	static constexpr float LOOKAHEAD_S = 0.002f;
	static constexpr float RELEASE_FAST_S = 0.05f;
	static constexpr float RELEASE_SLOW_S = 0.3f;
	static constexpr float HISTORY_S = 1.f;

	float ceiling = 5.f;        // volts
	int look = 96;              // delay, in samples
	float gainReduction = 0.f;  // 0..1 on the last sample, for the display
	bool clipped = false;       // the hard clip engaged on the last sample
	float preClipOver = 0.f;    // the most any sample has exceeded the ceiling before the clip, since reset

	float bufL[MAX_LOOK], bufR[MAX_LOOK];
	int wpos = 0;
	// release
	float gRel = 1.f, relFast = 0.f, relSlow = 0.f, hist = 0.f, histCoef = 0.f;
	// sliding minimum: a monotonic deque of (value, sample index)
	float dqVal[MAX_LOOK];
	int64_t dqIdx[MAX_LOOK];
	int64_t dqHead = 0, dqTail = 0, n = 0;
	// moving average
	float avgBuf[MAX_LOOK];
	int apos = 0, resum = 0;
	double avgSum = 0.0;

	Limiter() {
		reset(48000.f);
	}

	void reset(float sr) {
		look = (int)std::ceil(sr * LOOKAHEAD_S);
		look = std::min(std::max(look, 1), MAX_LOOK - 2);
		std::fill(bufL, bufL + MAX_LOOK, 0.f);
		std::fill(bufR, bufR + MAX_LOOK, 0.f);
		wpos = 0;
		gRel = 1.f;
		hist = 0.f;
		relFast = 1.f - std::exp(-1.f / (RELEASE_FAST_S * sr));
		relSlow = 1.f - std::exp(-1.f / (RELEASE_SLOW_S * sr));
		histCoef = 1.f - std::exp(-1.f / (HISTORY_S * sr));
		dqHead = dqTail = n = 0;
		std::fill(avgBuf, avgBuf + MAX_LOOK, 1.f);
		apos = resum = 0;
		avgSum = look;
		gainReduction = 0.f;
		clipped = false;
		preClipOver = 0.f;
	}

	void process(float& l, float& r) {
		float peak = std::fmax(std::fabs(l), std::fabs(r));
		if (!std::isfinite(peak)) {
			// Never let a NaN reach the speakers; the Circle guards upstream
			l = r = peak = 0.f;
		}
		float gReq = peak > ceiling ? ceiling / peak : 1.f;

		// Release: down at once, back up at the program-dependent rate
		if (gReq < gRel) {
			gRel = gReq;
		}
		else {
			float h = std::fmin(hist * 4.f, 1.f);
			float c = relFast + (relSlow - relFast) * h;
			gRel += (1.f - gRel) * c;
		}
		hist += ((1.f - gReq) - hist) * histCoef;

		// Sliding minimum over the last look+1 values of gRel
		while (dqTail != dqHead && dqVal[(dqTail - 1) & MASK] >= gRel)
			dqTail--;
		dqVal[dqTail & MASK] = gRel;
		dqIdx[dqTail & MASK] = n;
		dqTail++;
		while (dqIdx[dqHead & MASK] <= n - (look + 1))
			dqHead++;
		float m = dqVal[dqHead & MASK];
		n++;

		// Moving average over the last `look` minima
		avgSum += (double)m - (double)avgBuf[apos];
		avgBuf[apos] = m;
		if (++apos >= look)
			apos = 0;
		if (++resum >= 65536) {
			// Re-sum now and then so float error cannot accumulate in the sum
			resum = 0;
			avgSum = 0.0;
			for (int i = 0; i < look; i++)
				avgSum += avgBuf[i];
		}
		float g = (float)(avgSum / look);
		if (g > 1.f)
			g = 1.f;

		// Apply to the delayed signal
		int rp = (wpos - look) & MASK;
		float dl = bufL[rp], dr = bufR[rp];
		bufL[wpos] = l;
		bufR[wpos] = r;
		wpos = (wpos + 1) & MASK;
		l = dl * g;
		r = dr * g;
		gainReduction = 1.f - g;

		preClipOver = std::fmax(preClipOver, std::fmax(std::fabs(l), std::fabs(r)) - ceiling);
		clipped = false;
		if (l > ceiling) { l = ceiling; clipped = true; }
		else if (l < -ceiling) { l = -ceiling; clipped = true; }
		if (r > ceiling) { r = ceiling; clipped = true; }
		else if (r < -ceiling) { r = -ceiling; clipped = true; }
	}
};

} // namespace conclave
