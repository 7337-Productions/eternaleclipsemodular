#pragma once
#include <cmath>
#include <cstdint>
#include "Scales.hpp"

// Syzygy orbit field. Sixteen beads, each with its own speed (a power of phi)
// and radius, drift around a square field; a line joins them in order, the
// way the Breakpoint object of Antonio Blanca's ABreakpoint 2 (2009/2011)
// drew its points. Reset lays them on a golden-angle sunflower, and as the
// phase accumulators grow that line winds into spirals, polygons and stars.
// Beads are masses: their orbit is only an attractor, MASS loosens the tether,
// the walls bounce them, and they can be grabbed and thrown. A bead sounds
// while it sits inside the trigger arc, a sector of the field. Rack-free so a
// plain g++ harness can drive it.
namespace syzygy {

static constexpr int N = 16;
static constexpr int PAIRS = 8;
static constexpr int STRING_SEG = 16;                     // computed-string segments per bead gap
static constexpr int STRING_N = STRING_SEG * (N - 1) + 1; // bead i sits at sample STRING_SEG * i
static constexpr float TWO_PI = 6.28318530717958647692f;
static constexpr float PI_F = 3.14159265358979323846f;
static constexpr float PHI = 1.61803398874989484820f;
static constexpr float GOLDEN_ANGLE = TWO_PI * (1.f - 1.f / PHI); // 137.5 degrees

static constexpr int NUM_TASKS = 5;
static const char* kTaskNames[NUM_TASKS] = {"ORBIT", "LISSAJOUS", "ROSE", "STAR", "SWEEP"};

// Physics constants
static constexpr float BOUNCE = 0.85f;      // wall restitution
static constexpr float ZETA = 0.3f;         // spring damping ratio: beads overshoot their orbit
static constexpr float TAU_MIN = 0.05f;     // orbit-following time constant at MASS = 0+ (s)
static constexpr float TAU_MAX = 8.f;       // ... and at MASS = 1: barely tethered
static constexpr float TRIGGER_MIN_R = 0.08f; // beads this close to the centre have no angle

inline float cubed(float v) {
	return v * v * v;
}

inline float clampf(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// The original phaseTransform: a two-parameter cubic that spreads a value
// across the beads (used for the pitch distribution). ph = 0.5 collapses every
// bead to the same value; away from the centre the spread grows and wraps.
inline float phaseTransform(int i, float ph, float amount) {
	float shapeX = -0.5f * cubed(2.f * amount - 1.f);
	float shapeY = 0.5f * cubed(2.f * ph - 1.f);
	float ramp = (float)i / N;
	float v = std::fabs(2.f * ((N - 1) * ramp * shapeY - 0.5f * (N - 1) * shapeY + shapeX) + 0.5f);
	return v - std::floor(v);
}

struct Params {
	float rate = 1.f / 64.f;      // laps per second
	float xRate = 1.f / PHI;      // X phase multiplier
	float yRate = 1.f;            // Y phase multiplier
	float shape = 0.f;            // 0..1 across the tasks (0 ORBIT .. 1 SWEEP)
	float spread = 0.5f;          // speed spread: exponent range +-2*spread in powers of phi
	float warp = 0.f;             // radius law: -1 tight centre .. 0 sunflower .. +1 ring
	float mass = 0.25f;           // 0 rigid orbit .. 1 free flight
	float ampX = 1.f;
	float ampY = 1.f;
	float arcStart = -PI_F / 4.f; // trigger arc start angle, radians (CCW, y up)
	float arcWidth = PI_F / 2.f;  // trigger arc width, radians
	float pitchShape = 0.75f;
	float pitchOffset = 0.5f;
	float rangeSemis = 24.f;
	uint16_t enableMask = 0xFFFF;
	uint16_t scaleMask = 0x0AB5; // major
	int root = 0;
};

struct Point {
	float tx = 0.f, ty = 0.f;   // orbit position (the attractor)
	float x = 0.f, y = 0.f;     // physical position, -1..1
	float vx = 0.f, vy = 0.f;   // velocity, field units per second
	bool sounding = false;      // inside the trigger arc
	int pair = -1;              // output pair holding this bead, or -1
};

struct Pair {
	int point = -1;
	bool gate = false;
	float pitch = 0.f; // volts, held after release
	int retrig = 0;    // blocks of forced low gate after a steal
};

struct OrbitField {
	static constexpr int RETRIG_BLOCKS = 3;

	Point pts[N];
	Pair pairs[PAIRS];
	int next = 0;

	// Phase accumulators, in laps (double: hours of drift without rounding)
	double phX = 0.0, phY = 0.0;

	// A bead pinned under the mouse (-1 = none)
	int held = -1;
	float heldX = 0.f, heldY = 0.f;

	// Random vector (seed 0 = clean)
	uint32_t seed = 0;
	float rndPhase[N] = {}; // turns
	float rndSpeed[N] = {}; // phi exponent
	int rndPitch[N] = {};

	// The computed string (STRING mode): the same orbit law evaluated between
	// the beads, with the random vector interpolated so it still passes
	// through every bead
	float stringX[STRING_N] = {}, stringY[STRING_N] = {};

	void setSeed(uint32_t s) {
		seed = s;
		if (s == 0) {
			for (int i = 0; i < N; i++) {
				rndPhase[i] = 0.f;
				rndSpeed[i] = 0.f;
				rndPitch[i] = 0;
			}
			return;
		}
		uint32_t h = s * 2654435761u;
		auto nextf = [&h]() {
			h = h * 1664525u + 1013904223u;
			return (float)((h >> 8) & 0xFFFF) / 65535.f; // 0..1
		};
		for (int i = 0; i < N; i++) {
			rndPhase[i] = (nextf() - 0.5f) * 0.5f;  // +-1/4 turn
			rndSpeed[i] = (nextf() - 0.5f);         // +-1/2 phi exponent
			rndPitch[i] = (int)std::lround(4.f * nextf() - 2.f);
		}
	}

	// Random vector between the beads: linear in u, exact at the beads
	static float lerpBeads(const float* v, float u) {
		float t = clampf(u, 0.f, 1.f) * N;
		int i = (int)t;
		if (i >= N - 1)
			return v[N - 1];
		float f = t - i;
		return v[i] + (v[i + 1] - v[i]) * f;
	}

	// Speed along u = i/N: powers of phi, rising with u, so relative phases never realign
	float speedAt(float u, const Params& p) const {
		return std::pow(PHI, 2.f * p.spread * (2.f * u - 1.f) + lerpBeads(rndSpeed, u));
	}

	float speedOf(int i, const Params& p) const {
		return speedAt((float)i / N, p);
	}

	// Radius along u: WARP bends the law from a tight centre through the
	// Fermat (sunflower) sqrt to an outer ring
	float radiusAt(float u, const Params& p) const {
		float v = (u * N + 1.f) / N; // bead 0 at 1/N, the outer bead at 1
		float e = 0.5f * std::exp2(-2.f * clampf(p.warp, -1.f, 1.f));
		return std::pow(v, e);
	}

	float radiusOf(int i, const Params& p) const {
		return radiusAt((float)i / N, p);
	}

	void anglesAt(float u, const Params& p, float& ax, float& ay) const {
		double base = (double)u * N * (1.0 - 1.0 / PHI) + lerpBeads(rndPhase, u); // golden angle, in turns
		double sp = speedAt(u, p);
		double fx = std::fmod(base + sp * phX, 1.0);
		double fy = std::fmod(base + sp * phY, 1.0);
		if (fx < 0.0)
			fx += 1.0;
		if (fy < 0.0)
			fy += 1.0;
		ax = (float)(fx * TWO_PI);
		ay = (float)(fy * TWO_PI);
	}

	void anglesOf(int i, const Params& p, float& ax, float& ay) const {
		anglesAt((float)i / N, p, ax, ay);
	}

	static void task(int k, float u, float ax, float ay, float r, float& x, float& y) {
		switch (k) {
			case 0: // ORBIT
				x = r * std::cos(ax);
				y = r * std::sin(ax);
				break;
			case 1: // LISSAJOUS: X and Y phases run at their own rates
				x = r * std::cos(ax);
				y = r * std::sin(ay);
				break;
			case 2: { // ROSE with pi petals per turn: never closes
				float rho = std::cos(PI_F * ay);
				x = r * rho * std::cos(ax);
				y = r * rho * std::sin(ax);
				break;
			}
			case 3: { // STAR: five-fold hypotrochoid, the pentagram's cousin
				x = r * (std::cos(ax) + 0.5f * std::cos(4.f * ax)) / 1.5f;
				y = r * (std::sin(ax) - 0.5f * std::sin(4.f * ax)) / 1.5f;
				break;
			}
			default: { // SWEEP: sawtooth across X on fixed rows
				float f = ax / TWO_PI;
				x = r * (2.f * f - 1.f);
				y = 2.f * (u + 0.5f / N) - 1.f;
				break;
			}
		}
	}

	// Orbit position at u: a task, with a short crossfade at the boundaries
	// so a TASK sweep steps from figure to figure
	void orbitAt(float u, const Params& p, float& x, float& y) const {
		float ax, ay;
		anglesAt(u, p, ax, ay);
		float r = radiusAt(u, p);
		float s = clampf(p.shape, 0.f, 1.f) * (NUM_TASKS - 1);
		int k = (int)s;
		if (k > NUM_TASKS - 2)
			k = NUM_TASKS - 2;
		float f = s - k;
		float blend = clampf((f - 0.4f) / 0.2f, 0.f, 1.f);
		float xa = 0.f, ya = 0.f;
		if (blend < 1.f)
			task(k, u, ax, ay, r, xa, ya);
		if (blend > 0.f) {
			float xb, yb;
			task(k + 1, u, ax, ay, r, xb, yb);
			xa += (xb - xa) * blend;
			ya += (yb - ya) * blend;
		}
		x = xa * p.ampX;
		y = ya * p.ampY;
	}

	int taskIndex(const Params& p) const {
		return (int)std::lround(clampf(p.shape, 0.f, 1.f) * (NUM_TASKS - 1));
	}

	void orbitOf(int i, const Params& p, float& x, float& y) const {
		orbitAt((float)i / N, p, x, y);
	}

	void computeTargets(const Params& p) {
		for (int i = 0; i < N; i++)
			orbitOf(i, p, pts[i].tx, pts[i].ty);
	}

	void computeString(const Params& p) {
		for (int j = 0; j < STRING_N; j++)
			orbitAt((float)j / (STRING_SEG * N), p, stringX[j], stringY[j]);
	}

	// Physics step: spring toward the orbit, friction, wall bounce
	void integrate(const Params& p, float dt) {
		float m = clampf(p.mass, 0.f, 1.f);
		if (m < 0.005f) {
			for (int i = 0; i < N; i++) {
				Point& pt = pts[i];
				if (i == held) {
					pt.x = heldX;
					pt.y = heldY;
				}
				else {
					pt.x = pt.tx;
					pt.y = pt.ty;
				}
				pt.vx = pt.vy = 0.f;
			}
			return;
		}
		float tau = TAU_MIN * std::pow(TAU_MAX / TAU_MIN, m);
		float k = 1.f / (tau * tau);
		float c = 2.f * ZETA / tau;
		for (int i = 0; i < N; i++) {
			Point& pt = pts[i];
			if (i == held) {
				pt.x = heldX;
				pt.y = heldY;
				pt.vx = pt.vy = 0.f;
				continue;
			}
			float ax = k * (pt.tx - pt.x) - c * pt.vx;
			float ay = k * (pt.ty - pt.y) - c * pt.vy;
			pt.vx += ax * dt;
			pt.vy += ay * dt;
			pt.x += pt.vx * dt;
			pt.y += pt.vy * dt;
			if (pt.x > 1.f) {
				pt.x = 2.f - pt.x;
				pt.vx = -pt.vx * BOUNCE;
			}
			else if (pt.x < -1.f) {
				pt.x = -2.f - pt.x;
				pt.vx = -pt.vx * BOUNCE;
			}
			if (pt.y > 1.f) {
				pt.y = 2.f - pt.y;
				pt.vy = -pt.vy * BOUNCE;
			}
			else if (pt.y < -1.f) {
				pt.y = -2.f - pt.y;
				pt.vy = -pt.vy * BOUNCE;
			}
			pt.x = clampf(pt.x, -1.f, 1.f);
			pt.y = clampf(pt.y, -1.f, 1.f);
		}
	}

	void hold(int i, float x, float y) {
		held = i;
		heldX = clampf(x, -1.f, 1.f);
		heldY = clampf(y, -1.f, 1.f);
	}

	// Let go of the held bead with a velocity (field units per second)
	void fling(int i, float vx, float vy) {
		if (held == i)
			held = -1;
		if (i < 0 || i >= N)
			return;
		pts[i].vx = clampf(vx, -20.f, 20.f);
		pts[i].vy = clampf(vy, -20.f, 20.f);
	}

	// Inside the trigger arc: a sector from arcStart spanning arcWidth (CCW)
	static bool insideArc(float x, float y, const Params& p) {
		if (x * x + y * y < TRIGGER_MIN_R * TRIGGER_MIN_R)
			return false;
		float a = std::atan2(y, x) - p.arcStart;
		a -= TWO_PI * std::floor(a / TWO_PI);
		return a < p.arcWidth;
	}

	float pitchOf(int i, const Params& p) const {
		int semi = (int)std::lround(phaseTransform(i, p.pitchShape, p.pitchOffset) * p.rangeSemis) + rndPitch[i];
		if (semi < 0)
			semi = 0;
		semi = quantizeUp(semi, p.scaleMask, p.root);
		return semi / 12.f - 1.f; // C3 base; V/OCT transposes
	}

	void noteOn(int i, float pitch) {
		int p = next;
		next = (next + 1) % PAIRS;
		if (pairs[p].point >= 0)
			pts[pairs[p].point].pair = -1; // stolen
		pairs[p].point = i;
		pairs[p].gate = true;
		pairs[p].pitch = pitch;
		pairs[p].retrig = RETRIG_BLOCKS;
		pts[i].pair = p;
	}

	void noteOff(int i) {
		int p = pts[i].pair;
		if (p >= 0 && pairs[p].point == i) {
			pairs[p].gate = false;
			pairs[p].point = -1;
		}
		pts[i].pair = -1;
	}

	void allOff() {
		for (int i = 0; i < N; i++)
			pts[i].pair = -1;
		for (int p = 0; p < PAIRS; p++) {
			pairs[p].point = -1;
			pairs[p].gate = false;
			pairs[p].retrig = 0;
		}
	}

	// Mark beads already inside the arc as sounding without firing them
	void arm(const Params& p) {
		for (int i = 0; i < N; i++)
			pts[i].sounding = insideArc(pts[i].x, pts[i].y, p);
	}

	// Back to the sunflower: phases zero, beads at rest on their orbits
	void reset(const Params& p) {
		phX = phY = 0.0;
		allOff();
		next = 0;
		held = -1;
		computeTargets(p);
		for (int i = 0; i < N; i++) {
			pts[i].x = pts[i].tx;
			pts[i].y = pts[i].ty;
			pts[i].vx = pts[i].vy = 0.f;
		}
		arm(p);
	}

	// One control block: advance the phases, move the beads, trigger
	void update(const Params& p, float dt) {
		phX += (double)p.rate * p.xRate * dt;
		phY += (double)p.rate * p.yRate * dt;
		computeTargets(p);
		integrate(p, dt);

		for (int i = 0; i < N; i++) {
			Point& pt = pts[i];
			bool enabled = (p.enableMask >> i) & 1u;
			bool in = insideArc(pt.x, pt.y, p);
			if (!pt.sounding && in) {
				pt.sounding = true;
				if (enabled)
					noteOn(i, pitchOf(i, p));
			}
			else if (pt.sounding && !in) {
				pt.sounding = false;
				noteOff(i);
			}
			if (!enabled && pt.pair >= 0)
				noteOff(i);
		}
		for (int q = 0; q < PAIRS; q++)
			if (pairs[q].retrig > 0)
				pairs[q].retrig--;
	}

	bool gateHigh(int q) const {
		return pairs[q].gate && pairs[q].retrig == 0;
	}
};

} // namespace syzygy
