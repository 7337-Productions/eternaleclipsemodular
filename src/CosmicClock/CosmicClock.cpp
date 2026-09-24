#include <atomic>
#include <chrono>
#include "plugin.hpp"
#include "EclipseWidgets.hpp"
#include "SephirothicModulator/Orbits.hpp" // shared Keplerian ephemeris

// Cosmic Clock (OMN-68): astrological waveform engine. Nine planetary LFO
// voices, each an archetypal shape colored by the zodiac sign it currently
// transits (element = waveshaping, modality = time behavior). Aspects between
// planets act as the mixer: MAIN is the weighted sum of all voices, weights
// driven by the aspect intensities drawn on the chart-of-the-moment wheel.
// No gates -- end-of-cycle triggers are the only pulses.

static const int NUM_BODIES = 9;

enum Body {
	B_SUN, B_MOON, B_MERCURY, B_VENUS, B_MARS,
	B_JUPITER, B_SATURN, B_URANUS, B_NEPTUNE
};

static const char* BODY_NAMES[NUM_BODIES] = {
	"SUN", "MOON", "MERCURY", "VENUS", "MARS",
	"JUPITER", "SATURN", "URANUS", "NEPTUNE"
};

// Archetypal voice rates [Hz]: a two-decade spread so MAIN is a fast shimmer
// over a slow ground swell.
static const float BASE_RATE[NUM_BODIES] = {
	8.f, 1.f, 5.f, 2.5f, 3.5f, 0.5f, 0.15f, 1.2f, 0.06f
};

static const int NUM_ASPECTS = 5;

enum Aspect { CONJUNCTION, SEXTILE, SQUARE, TRINE, OPPOSITION };

static const double ASPECT_ANGLE[NUM_ASPECTS] = {0.0, 60.0, 90.0, 120.0, 180.0};
static const double ASPECT_ORB[NUM_ASPECTS] = {8.0, 4.0, 6.0, 6.0, 8.0};
static const char* ASPECT_NAMES[NUM_ASPECTS] = {"CNJ", "SXT", "SQR", "TRI", "OPP"};

// Chord/readout colors per aspect, muted to sit in the panel palette
static const NVGcolor ASPECT_COLOR[NUM_ASPECTS] = {
	{{{1.00f, 0.93f, 0.72f, 1.f}}}, // conjunction: cream
	{{{0.37f, 0.68f, 0.62f, 1.f}}}, // sextile: teal
	{{{0.85f, 0.36f, 0.29f, 1.f}}}, // square: ember
	{{{0.61f, 0.55f, 0.85f, 1.f}}}, // trine: violet
	{{{1.00f, 0.77f, 0.39f, 1.f}}}, // opposition: copper-bright
};

// Low-precision lunar longitude (~1.5 deg): mean longitude plus the largest
// equation-of-center term. Sky only carries the synodic phase.
static double moonLongitude(double unixTime) {
	double d = (unixTime - 946728000.0) / 86400.0;
	double Lp = 218.316 + 13.176396 * d;
	double Mp = 134.963 + 13.064993 * d;
	return std::remainder((Lp + 6.289 * std::sin(Mp * orbits::DEG)) * orbits::DEG, 2.0 * M_PI);
}

static int signOf(double lon) {
	double deg = lon / orbits::DEG;
	deg = std::fmod(deg, 360.0);
	if (deg < 0.0)
		deg += 360.0;
	return clamp((int)(deg / 30.0), 0, 11);
}

// Deterministic per-cycle randomness for Uranus (PCG-style integer hash)
static float hashBipolar(uint32_t n) {
	n = n * 747796405u + 2891336453u;
	n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
	n = (n >> 22u) ^ n;
	return (float)n / 2147483647.5f - 1.f;
}

static float tri(float phase) {
	return phase < 0.5f ? 4.f * phase - 1.f : 3.f - 4.f * phase;
}

struct ActiveAspect {
	int a, b, type;
	float intensity;
};

struct CosmicClock : Module {
	enum ParamId {
		RATE_PARAM,
		ORB_PARAM,
		WARP_PARAM,
		MODE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(WAVE_OUTPUTS, NUM_BODIES),
		ENUMS(EOC_OUTPUTS, NUM_BODIES),
		MAIN_OUTPUT,
		MAINEOC_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	orbits::Sky sky;
	double simUnix = 0.0;

	// Astrology state (refreshed in the divided tick)
	double lon[NUM_BODIES] = {};
	int signIdx[NUM_BODIES] = {};
	int oldSign[NUM_BODIES] = {};
	float signFade[NUM_BODIES] = {}; // 0 -> 1 crossfade after an ingress
	float weight[NUM_BODIES] = {};   // smoothed aspect mix weight
	float moonAmp = 1.f;
	float rateScale = 1.f;
	int mainEocBody = B_NEPTUNE;

	// Voice state
	float phase[NUM_BODIES] = {};
	uint32_t cycle[NUM_BODIES] = {};
	float slew[NUM_BODIES] = {};
	dsp::PulseGenerator eocPulse[NUM_BODIES];
	dsp::PulseGenerator mainEocPulse;

	// Display state, read by the wheel/readout widgets
	ActiveAspect active[64];
	int numActive = 0;
	float lonDisplay[NUM_BODIES] = {};
	float dispWeight[NUM_BODIES] = {};
	int sunSign = 0;
	// Strongest aspect for the readout, packed into one word so the UI thread
	// never sees a half-written (body, body, type) triple: bit 31 = valid,
	// bits 16-23 body A, bits 8-15 body B, bits 0-7 aspect type.
	std::atomic<uint32_t> strongPacked{0};
	std::atomic<float> strongDelta{0.f};

	dsp::ClockDivider skyDivider;

	static double realNow() {
		using namespace std::chrono;
		return duration<double>(system_clock::now().time_since_epoch()).count();
	}

	CosmicClock() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, -1.301f, 1.301f, 0.f, "Rate scale", "x", 10.f);
		configParam(ORB_PARAM, 0.25f, 2.f, 1.f, "Orb width", "x");
		configParam(WARP_PARAM, 0.f, 8.f, 5.f, "Time warp", "x", 10.f);
		configSwitch(MODE_PARAM, 0.f, 1.f, 1.f, "Time source", {"Real (true sky)", "Warp"});

		static const char* WAVE_DESC[NUM_BODIES] = {
			"Sun - intense triangle",
			"Moon - sine, amplitude follows the lunar phase",
			"Mercury - stepped triangle",
			"Venus - five-lobed curve",
			"Mars - hard ramp",
			"Jupiter - broad swelling sine",
			"Saturn - slow rounded plateaus",
			"Uranus - erratic random slews",
			"Neptune - two beating sines",
		};
		for (int p = 0; p < NUM_BODIES; p++) {
			configOutput(WAVE_OUTPUTS + p,
				string::f("%s, colored by its zodiac sign (+-5V)", WAVE_DESC[p]));
			configOutput(EOC_OUTPUTS + p, string::f("%s end-of-cycle trigger", BODY_NAMES[p]));
		}
		configOutput(MAIN_OUTPUT, "Main - aspect-weighted sum of all nine voices (+-5V)");
		configOutput(MAINEOC_OUTPUT, "Main end of cycle - slowest aspected planet");

		skyDivider.setDivision(32);
		simUnix = realNow();
		reseed();
	}

	// Re-derive sign state from the current instant so time jumps cause no
	// spurious crossfades or triggers.
	void reseed() {
		sky.compute(simUnix);
		updateLongitudes();
		for (int p = 0; p < NUM_BODIES; p++) {
			signIdx[p] = oldSign[p] = signOf(lon[p]);
			signFade[p] = 1.f;
			weight[p] = 0.4f;
		}
		sunSign = signIdx[B_SUN];
	}

	void updateLongitudes() {
		using namespace orbits;
		lon[B_SUN] = sky.sunLon;
		lon[B_MOON] = moonLongitude(simUnix);
		// EMB is the geocentric origin -- its geoLon is undefined; map bodies
		// explicitly and never loop over the raw Planet enum.
		lon[B_MERCURY] = sky.geoLon[MERCURY];
		lon[B_VENUS] = sky.geoLon[VENUS];
		lon[B_MARS] = sky.geoLon[MARS];
		lon[B_JUPITER] = sky.geoLon[JUPITER];
		lon[B_SATURN] = sky.geoLon[SATURN];
		lon[B_URANUS] = sky.geoLon[URANUS];
		lon[B_NEPTUNE] = sky.geoLon[NEPTUNE];
	}

	// Base archetypal shapes, [-1, 1]
	float baseShape(int p, float ph) {
		switch (p) {
			case B_SUN:
				return tri(ph);
			case B_MOON:
				return moonAmp * std::sin(2.f * M_PI * ph);
			case B_MERCURY:
				return tri(std::floor(ph * 8.f) / 8.f);
			case B_VENUS:
				return std::sin(2.f * M_PI * ph) * (0.7f + 0.3f * std::cos(10.f * M_PI * ph));
			case B_MARS:
				return ph < 0.85f ? -1.f + 2.f * std::pow(ph / 0.85f, 1.3f)
				                  : 1.f - 2.f * (ph - 0.85f) / 0.15f;
			case B_JUPITER:
				return (std::sin(2.f * M_PI * ph) + 0.33f * std::sin(4.f * M_PI * ph)) / 1.15f;
			case B_SATURN:
				return std::tanh(3.f * std::sin(2.f * M_PI * ph));
			case B_URANUS: {
				// Four random target levels per cycle, reached with fast slews
				int seg = std::min((int)(ph * 4.f), 3);
				float t = ph * 4.f - seg;
				uint32_t idx = cycle[p] * 4u + seg;
				float cur = hashBipolar(idx);
				float nxt = hashBipolar(idx + 1u);
				return cur + (nxt - cur) * std::fmin(1.f, t * 3.f);
			}
			case B_NEPTUNE: {
				// Two detuned sines beating against each other
				double T = (double)cycle[p] + ph;
				return 0.5f * (float)(std::sin(2.0 * M_PI * T) + std::sin(2.0 * M_PI * 1.007 * T));
			}
		}
		return 0.f;
	}

	// Mutable-sign shape variants: {base, sine, triangle, folded base}
	float variantShape(int p, int v, float ph) {
		switch (v & 3) {
			case 0: return baseShape(p, ph);
			case 1: return std::sin(2.f * M_PI * ph);
			case 2: return tri(ph);
			default: return std::sin(1.5f * M_PI * baseShape(p, ph));
		}
	}

	// One voice colored by one sign: element shapes the wave, modality shapes
	// its time behavior. Water's slew is applied later (it is stateful).
	float colorVoice(int p, int sign, float ph) {
		int elem = sign % 4;    // fire, earth, air, water
		int mode = sign % 3;    // cardinal, fixed, mutable

		float y;
		if (mode == 2) {
			// Mutable: morph through the variant bank, one blend per cycle
			float a = variantShape(p, cycle[p], ph);
			float b = variantShape(p, cycle[p] + 1u, ph);
			y = a + (b - a) * ph;
		}
		else {
			y = baseShape(p, ph);
		}

		if (elem == 0)
			y = (y < 0.f ? -1.f : 1.f) * std::pow(std::fabs(y), 0.6f); // fire: sharpen
		else if (elem == 1)
			y = std::round(y * 2.5f) / 2.5f; // earth: quantize to 6 levels
		else if (elem == 2)
			y = 0.85f * y + 0.25f * std::sin(6.f * M_PI * ph); // air: shimmer

		if (mode == 0 && ph < 0.25f)
			y *= 1.3f - 1.2f * ph; // cardinal: accent at cycle start

		return y;
	}

	void process(const ProcessArgs& args) override {
		if (skyDivider.process()) {
			double dt = args.sampleTime * skyDivider.getDivision();
			if (params[MODE_PARAM].getValue() > 0.5f) {
				simUnix += dt * std::pow(10.0, (double)params[WARP_PARAM].getValue());
				// Keep within the +-500 century range the element set tolerates
				simUnix = std::fmin(std::fmax(simUnix, 946727935.816 - 1.578e12), 946727935.816 + 1.578e12);
			}
			else {
				simUnix = realNow();
			}
			sky.compute(simUnix);
			updateLongitudes();
			moonAmp = 0.5f * (1.f - (float)std::cos(2.0 * M_PI * sky.moonPhase));
			rateScale = std::pow(10.f, params[RATE_PARAM].getValue());

			// Ingresses: restart the coloring crossfade
			for (int p = 0; p < NUM_BODIES; p++) {
				int s = signOf(lon[p]);
				if (s != signIdx[p]) {
					oldSign[p] = signIdx[p];
					signIdx[p] = s;
					signFade[p] = 0.f;
				}
				signFade[p] = std::fmin(1.f, signFade[p] + (float)dt / 0.05f);
			}
			sunSign = signIdx[B_SUN];

			// Aspect scan: 36 pairs against 5 aspect angles
			float orbScale = params[ORB_PARAM].getValue();
			float target[NUM_BODIES];
			for (int p = 0; p < NUM_BODIES; p++)
				target[p] = 0.4f;
			numActive = 0;
			float strongestI = 0.f;
			int strongA = -1, strongB = -1, strongType = -1;
			float strongDeltaNew = 0.f;
			for (int a = 0; a < NUM_BODIES; a++) {
				for (int b = a + 1; b < NUM_BODIES; b++) {
					double sepDeg = std::fabs(std::remainder(lon[a] - lon[b], 2.0 * M_PI)) / orbits::DEG;
					for (int k = 0; k < NUM_ASPECTS; k++) {
						double delta = std::fabs(sepDeg - ASPECT_ANGLE[k]);
						double orb = ASPECT_ORB[k] * orbScale;
						if (delta >= orb)
							continue;
						float in = 0.5f * (1.f + (float)std::cos(M_PI * delta / orb));
						target[a] += in;
						target[b] += in;
						if (numActive < 64)
							active[numActive++] = {a, b, k, in};
						if (in > strongestI) {
							strongestI = in;
							strongA = a;
							strongB = b;
							strongType = k;
							strongDeltaNew = (float)delta;
						}
					}
				}
			}

			strongDelta.store(strongDeltaNew, std::memory_order_relaxed);
			strongPacked.store(strongType < 0 ? 0u
				: (1u << 31) | ((uint32_t)strongA << 16) | ((uint32_t)strongB << 8) | (uint32_t)strongType,
				std::memory_order_relaxed);

			// Smooth mix weights (~100 ms) so chord blooms don't zipper MAIN
			float alpha = 1.f - std::exp(-(float)dt / 0.1f);
			int slowest = B_NEPTUNE;
			for (int p = 0; p < NUM_BODIES; p++) {
				weight[p] += (target[p] - weight[p]) * alpha;
				if (target[p] > 0.45f && BASE_RATE[p] < BASE_RATE[slowest])
					slowest = p;
			}
			mainEocBody = slowest;

			for (int p = 0; p < NUM_BODIES; p++) {
				lonDisplay[p] = (float)lon[p];
				dispWeight[p] = weight[p];
			}
		}

		// Voice engine, per sample
		float sum = 0.f;
		float totalW = 0.f;
		for (int p = 0; p < NUM_BODIES; p++) {
			phase[p] += BASE_RATE[p] * rateScale * args.sampleTime;
			if (phase[p] >= 1.f) {
				phase[p] -= 1.f;
				cycle[p]++;
				eocPulse[p].trigger(1e-3f);
				if (p == mainEocBody)
					mainEocPulse.trigger(1e-3f);
			}

			float y = colorVoice(p, signIdx[p], phase[p]);
			if (signFade[p] < 1.f) {
				float yOld = colorVoice(p, oldSign[p], phase[p]);
				y = yOld + (y - yOld) * signFade[p];
			}

			// Water slews heavily, earth lightly; cutoffs track the voice rate
			// so the character is rate-independent
			int elem = signIdx[p] % 4;
			if (elem == 3 || elem == 1) {
				float fc = BASE_RATE[p] * rateScale * (elem == 3 ? 6.f : 30.f);
				float k = 1.f - std::exp(-2.f * M_PI * fc * args.sampleTime);
				slew[p] += (y - slew[p]) * std::fmin(1.f, k);
				y = slew[p];
			}
			else {
				slew[p] = y;
			}

			float v = 5.f * y;
			outputs[WAVE_OUTPUTS + p].setVoltage(v);
			outputs[EOC_OUTPUTS + p].setVoltage(eocPulse[p].process(args.sampleTime) ? 10.f : 0.f);
			sum += weight[p] * v;
			totalW += weight[p];
		}

		// MAIN: normalized weighted sum with a gentle tanh drive
		float main = 5.f * std::tanh(1.5f * sum / (totalW * 5.f));
		outputs[MAIN_OUTPUT].setVoltage(main);
		outputs[MAINEOC_OUTPUT].setVoltage(mainEocPulse.process(args.sampleTime) ? 10.f : 0.f);
	}

	void onReset() override {
		simUnix = realNow();
		reseed();
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "simUnix", json_real(simUnix));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* simJ = json_object_get(rootJ, "simUnix");
		if (simJ)
			simUnix = json_real_value(simJ);
		reseed();
	}
};

// --- Panel geometry (mm), shared with res/CosmicClock.svg ---

static const Vec WHEEL_CENTER = Vec(60.f, 71.f);
static const float R_SIGN = 49.25f;  // sign glyph ring
static const float R_TICK = 44.5f;   // planet position ticks (ring inner edge)
static const float R_PLANET = 38.f;  // planet glyph ring
static const float R_CHORD = 31.5f;  // aspect chord anchors

// Chart orientation: 0 deg Aries at 9 o'clock, longitude counterclockwise
static Vec wheelPos(float lonRad, float r) {
	return Vec(WHEEL_CENTER.x - r * std::cos(lonRad), WHEEL_CENTER.y + r * std::sin(lonRad));
}

// --- Hand-drawn glyphs (no font carries them). Each draws stroke paths in a
// unit box [-0.5, 0.5], y down, scaled to `sizePx`. ---

static void strokeGlyph(NVGcontext* vg, float sizePx) {
	nvgStrokeWidth(vg, mm2px(0.32f) / sizePx);
	nvgLineCap(vg, NVG_ROUND);
	nvgLineJoin(vg, NVG_ROUND);
	nvgStroke(vg);
}

static void drawPlanetGlyph(NVGcontext* vg, int body, Vec pos, float sizePx, NVGcolor color) {
	nvgSave(vg);
	nvgTranslate(vg, pos.x, pos.y);
	nvgScale(vg, sizePx, sizePx);
	nvgStrokeColor(vg, color);
	nvgFillColor(vg, color);

	switch (body) {
		case B_SUN:
			nvgBeginPath(vg);
			nvgCircle(vg, 0.f, 0.f, 0.4f);
			strokeGlyph(vg, sizePx);
			nvgBeginPath(vg);
			nvgCircle(vg, 0.f, 0.f, 0.09f);
			nvgFill(vg);
			break;
		case B_MOON:
			nvgBeginPath(vg);
			nvgMoveTo(vg, 0.f, -0.42f);
			nvgBezierTo(vg, -0.55f, -0.42f, -0.55f, 0.42f, 0.f, 0.42f);
			nvgBezierTo(vg, -0.25f, 0.42f, -0.25f, -0.42f, 0.f, -0.42f);
			strokeGlyph(vg, sizePx);
			break;
		case B_MERCURY:
			nvgBeginPath(vg);
			nvgCircle(vg, 0.f, 0.02f, 0.22f);
			nvgMoveTo(vg, 0.f, 0.24f);
			nvgLineTo(vg, 0.f, 0.5f);
			nvgMoveTo(vg, -0.13f, 0.37f);
			nvgLineTo(vg, 0.13f, 0.37f);
			nvgMoveTo(vg, -0.2f, -0.48f);
			nvgBezierTo(vg, -0.2f, -0.18f, 0.2f, -0.18f, 0.2f, -0.48f);
			strokeGlyph(vg, sizePx);
			break;
		case B_VENUS:
			nvgBeginPath(vg);
			nvgCircle(vg, 0.f, -0.12f, 0.26f);
			nvgMoveTo(vg, 0.f, 0.14f);
			nvgLineTo(vg, 0.f, 0.5f);
			nvgMoveTo(vg, -0.15f, 0.32f);
			nvgLineTo(vg, 0.15f, 0.32f);
			strokeGlyph(vg, sizePx);
			break;
		case B_MARS:
			nvgBeginPath(vg);
			nvgCircle(vg, -0.08f, 0.1f, 0.26f);
			nvgMoveTo(vg, 0.11f, -0.09f);
			nvgLineTo(vg, 0.36f, -0.34f);
			nvgMoveTo(vg, 0.16f, -0.34f);
			nvgLineTo(vg, 0.36f, -0.34f);
			nvgLineTo(vg, 0.36f, -0.14f);
			strokeGlyph(vg, sizePx);
			break;
		case B_JUPITER:
			nvgBeginPath(vg);
			nvgMoveTo(vg, -0.35f, -0.18f);
			nvgBezierTo(vg, -0.35f, -0.48f, 0.05f, -0.48f, 0.05f, -0.18f);
			nvgLineTo(vg, -0.38f, 0.2f);
			nvgLineTo(vg, 0.38f, 0.2f);
			nvgMoveTo(vg, 0.18f, -0.02f);
			nvgLineTo(vg, 0.18f, 0.5f);
			strokeGlyph(vg, sizePx);
			break;
		case B_SATURN:
			nvgBeginPath(vg);
			nvgMoveTo(vg, -0.15f, -0.45f);
			nvgLineTo(vg, -0.15f, 0.12f);
			nvgMoveTo(vg, -0.32f, -0.3f);
			nvgLineTo(vg, 0.05f, -0.3f);
			nvgMoveTo(vg, -0.15f, 0.12f);
			nvgBezierTo(vg, 0.2f, -0.15f, 0.35f, 0.25f, 0.f, 0.45f);
			strokeGlyph(vg, sizePx);
			break;
		case B_URANUS:
			nvgBeginPath(vg);
			nvgCircle(vg, 0.f, 0.3f, 0.13f);
			nvgMoveTo(vg, -0.2f, -0.5f);
			nvgLineTo(vg, -0.2f, 0.f);
			nvgMoveTo(vg, 0.2f, -0.5f);
			nvgLineTo(vg, 0.2f, 0.f);
			nvgMoveTo(vg, -0.2f, -0.25f);
			nvgLineTo(vg, 0.2f, -0.25f);
			nvgMoveTo(vg, 0.f, -0.25f);
			nvgLineTo(vg, 0.f, 0.17f);
			strokeGlyph(vg, sizePx);
			break;
		case B_NEPTUNE:
			nvgBeginPath(vg);
			nvgMoveTo(vg, -0.28f, -0.42f);
			nvgBezierTo(vg, -0.28f, 0.08f, 0.28f, 0.08f, 0.28f, -0.42f);
			nvgMoveTo(vg, 0.f, -0.45f);
			nvgLineTo(vg, 0.f, 0.5f);
			nvgMoveTo(vg, -0.15f, 0.32f);
			nvgLineTo(vg, 0.15f, 0.32f);
			strokeGlyph(vg, sizePx);
			break;
	}
	nvgRestore(vg);
}

static void drawSignGlyph(NVGcontext* vg, int sign, Vec pos, float sizePx, NVGcolor color) {
	nvgSave(vg);
	nvgTranslate(vg, pos.x, pos.y);
	nvgScale(vg, sizePx, sizePx);
	nvgStrokeColor(vg, color);
	nvgBeginPath(vg);

	switch (sign) {
		case 0: // Aries: ram horns on a stem
			nvgMoveTo(vg, 0.f, 0.45f);
			nvgBezierTo(vg, 0.f, -0.2f, -0.1f, -0.5f, -0.32f, -0.33f);
			nvgMoveTo(vg, 0.f, 0.45f);
			nvgBezierTo(vg, 0.f, -0.2f, 0.1f, -0.5f, 0.32f, -0.33f);
			break;
		case 1: // Taurus: circle with horns
			nvgCircle(vg, 0.f, 0.16f, 0.28f);
			nvgMoveTo(vg, -0.3f, -0.45f);
			nvgBezierTo(vg, -0.12f, -0.1f, 0.12f, -0.1f, 0.3f, -0.45f);
			break;
		case 2: // Gemini: twin pillars
			nvgMoveTo(vg, -0.15f, -0.3f);
			nvgLineTo(vg, -0.15f, 0.3f);
			nvgMoveTo(vg, 0.15f, -0.3f);
			nvgLineTo(vg, 0.15f, 0.3f);
			nvgMoveTo(vg, -0.32f, -0.4f);
			nvgBezierTo(vg, -0.1f, -0.26f, 0.1f, -0.26f, 0.32f, -0.4f);
			nvgMoveTo(vg, -0.32f, 0.4f);
			nvgBezierTo(vg, -0.1f, 0.26f, 0.1f, 0.26f, 0.32f, 0.4f);
			break;
		case 3: // Cancer: the 69 curls
			nvgCircle(vg, -0.2f, -0.16f, 0.12f);
			nvgMoveTo(vg, -0.08f, -0.2f);
			nvgBezierTo(vg, 0.12f, -0.36f, 0.3f, -0.3f, 0.36f, -0.12f);
			nvgCircle(vg, 0.2f, 0.16f, 0.12f);
			nvgMoveTo(vg, 0.08f, 0.2f);
			nvgBezierTo(vg, -0.12f, 0.36f, -0.3f, 0.3f, -0.36f, 0.12f);
			break;
		case 4: // Leo: mane loop with a tail flick
			nvgCircle(vg, -0.22f, 0.08f, 0.12f);
			nvgMoveTo(vg, -0.12f, 0.f);
			nvgBezierTo(vg, -0.05f, -0.5f, 0.3f, -0.45f, 0.28f, -0.1f);
			nvgBezierTo(vg, 0.26f, 0.15f, 0.08f, 0.22f, 0.18f, 0.42f);
			break;
		case 5: // Virgo: the maiden's M with looped tail
			nvgMoveTo(vg, -0.4f, 0.32f);
			nvgLineTo(vg, -0.4f, -0.18f);
			nvgBezierTo(vg, -0.4f, -0.4f, -0.14f, -0.4f, -0.14f, -0.18f);
			nvgLineTo(vg, -0.14f, 0.32f);
			nvgMoveTo(vg, -0.14f, -0.18f);
			nvgBezierTo(vg, -0.14f, -0.4f, 0.12f, -0.4f, 0.12f, -0.18f);
			nvgLineTo(vg, 0.12f, 0.2f);
			nvgBezierTo(vg, 0.16f, 0.45f, 0.36f, 0.4f, 0.34f, 0.1f);
			break;
		case 6: // Libra: the scales
			nvgMoveTo(vg, -0.38f, 0.32f);
			nvgLineTo(vg, 0.38f, 0.32f);
			nvgMoveTo(vg, -0.38f, 0.05f);
			nvgLineTo(vg, -0.13f, 0.05f);
			nvgBezierTo(vg, -0.13f, -0.35f, 0.13f, -0.35f, 0.13f, 0.05f);
			nvgLineTo(vg, 0.38f, 0.05f);
			break;
		case 7: // Scorpio: M with a stinger
			nvgMoveTo(vg, -0.4f, 0.32f);
			nvgLineTo(vg, -0.4f, -0.18f);
			nvgBezierTo(vg, -0.4f, -0.4f, -0.16f, -0.4f, -0.16f, -0.18f);
			nvgLineTo(vg, -0.16f, 0.32f);
			nvgMoveTo(vg, -0.16f, -0.18f);
			nvgBezierTo(vg, -0.16f, -0.4f, 0.08f, -0.4f, 0.08f, -0.18f);
			nvgLineTo(vg, 0.08f, 0.14f);
			nvgBezierTo(vg, 0.08f, 0.32f, 0.24f, 0.34f, 0.34f, 0.22f);
			nvgMoveTo(vg, 0.26f, 0.14f);
			nvgLineTo(vg, 0.36f, 0.2f);
			nvgLineTo(vg, 0.3f, 0.32f);
			break;
		case 8: // Sagittarius: the arrow
			nvgMoveTo(vg, -0.32f, 0.32f);
			nvgLineTo(vg, 0.34f, -0.34f);
			nvgMoveTo(vg, 0.08f, -0.36f);
			nvgLineTo(vg, 0.34f, -0.34f);
			nvgLineTo(vg, 0.36f, -0.08f);
			nvgMoveTo(vg, -0.24f, -0.04f);
			nvgLineTo(vg, 0.04f, 0.24f);
			break;
		case 9: // Capricorn: V with a looped tail
			nvgMoveTo(vg, -0.42f, -0.32f);
			nvgLineTo(vg, -0.22f, 0.1f);
			nvgLineTo(vg, -0.04f, -0.32f);
			nvgMoveTo(vg, -0.04f, -0.32f);
			nvgLineTo(vg, -0.04f, 0.14f);
			nvgBezierTo(vg, -0.04f, 0.42f, 0.3f, 0.42f, 0.3f, 0.16f);
			nvgBezierTo(vg, 0.3f, -0.04f, 0.08f, 0.f, 0.05f, 0.14f);
			break;
		case 10: // Aquarius: the waves
			nvgMoveTo(vg, -0.38f, -0.04f);
			nvgLineTo(vg, -0.19f, -0.24f);
			nvgLineTo(vg, 0.f, -0.04f);
			nvgLineTo(vg, 0.19f, -0.24f);
			nvgLineTo(vg, 0.38f, -0.04f);
			nvgMoveTo(vg, -0.38f, 0.26f);
			nvgLineTo(vg, -0.19f, 0.06f);
			nvgLineTo(vg, 0.f, 0.26f);
			nvgLineTo(vg, 0.19f, 0.06f);
			nvgLineTo(vg, 0.38f, 0.26f);
			break;
		case 11: // Pisces: two fishes bound together
			nvgMoveTo(vg, -0.16f, -0.42f);
			nvgBezierTo(vg, -0.4f, -0.2f, -0.4f, 0.2f, -0.16f, 0.42f);
			nvgMoveTo(vg, 0.16f, -0.42f);
			nvgBezierTo(vg, 0.4f, -0.2f, 0.4f, 0.2f, 0.16f, 0.42f);
			nvgMoveTo(vg, -0.34f, 0.f);
			nvgLineTo(vg, 0.34f, 0.f);
			break;
	}
	strokeGlyph(vg, sizePx);
	nvgRestore(vg);
}

// --- The live chart-of-the-moment wheel ---

struct CosmicWheel : TransparentWidget {
	CosmicClock* module = NULL;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;

		// Module-browser preview: a hardcoded sample sky
		static const float PREVIEW_LON[NUM_BODIES] = {
			100.f, 220.f, 130.f, 250.f, 10.f, 40.f, 285.f, 55.f, 355.f
		};
		static const ActiveAspect PREVIEW_ACTIVE[2] = {
			{B_SUN, B_MOON, TRINE, 0.8f},
			{B_MERCURY, B_JUPITER, SQUARE, 0.6f},
		};

		float lon[NUM_BODIES];
		const ActiveAspect* active;
		int numActive;
		float weightAlpha[NUM_BODIES];
		int sunSign;
		if (module) {
			for (int p = 0; p < NUM_BODIES; p++) {
				lon[p] = module->lonDisplay[p];
				weightAlpha[p] = 0.45f + 0.55f * clamp((module->dispWeight[p] - 0.4f) / 1.6f, 0.f, 1.f);
			}
			active = module->active;
			numActive = module->numActive;
			sunSign = module->sunSign;
		}
		else {
			for (int p = 0; p < NUM_BODIES; p++) {
				lon[p] = PREVIEW_LON[p] * (float)orbits::DEG;
				weightAlpha[p] = 0.7f;
			}
			active = PREVIEW_ACTIVE;
			numActive = 2;
			sunSign = 3;
		}

		// Aspect chords, under everything else
		for (int i = 0; i < numActive; i++) {
			const ActiveAspect& a = active[i];
			Vec p1 = mm2px(wheelPos(lon[a.a], R_CHORD));
			Vec p2 = mm2px(wheelPos(lon[a.b], R_CHORD));
			NVGcolor c = ASPECT_COLOR[a.type];
			c.a = 0.15f + 0.75f * a.intensity;
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, p1.x, p1.y);
			nvgLineTo(args.vg, p2.x, p2.y);
			nvgStrokeColor(args.vg, c);
			nvgStrokeWidth(args.vg, mm2px(0.4f * (1.f + a.intensity)));
			nvgLineCap(args.vg, NVG_ROUND);
			nvgStroke(args.vg);
		}

		// Sign glyphs at each sign's mid-angle; the Sun's sign in amber
		for (int s = 0; s < 12; s++) {
			float mid = (s * 30.f + 15.f) * (float)orbits::DEG;
			NVGcolor c = (s == sunSign) ? eclipse::ACCENT_COLOR : eclipse::DIM_COLOR;
			drawSignGlyph(args.vg, s, mm2px(wheelPos(mid, R_SIGN)), mm2px(4.5f), c);
		}

		// Planet position ticks and glyphs; brightness tracks the mix weight
		for (int p = 0; p < NUM_BODIES; p++) {
			NVGcolor c = (p == B_SUN) ? eclipse::ACCENT_COLOR : eclipse::LABEL_COLOR;
			c.a = weightAlpha[p];
			Vec t1 = mm2px(wheelPos(lon[p], R_TICK));
			Vec t2 = mm2px(wheelPos(lon[p], R_TICK - 2.2f));
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, t1.x, t1.y);
			nvgLineTo(args.vg, t2.x, t2.y);
			nvgStrokeColor(args.vg, c);
			nvgStrokeWidth(args.vg, mm2px(0.35f));
			nvgStroke(args.vg);
			drawPlanetGlyph(args.vg, p, mm2px(wheelPos(lon[p], R_PLANET)), mm2px(3.8f), c);
		}
	}
};

// Two-line readout: simulated date/time and the strongest in-orb aspect
struct ChartReadout : TransparentWidget {
	CosmicClock* module = NULL;

	// Howard Hinnant's civil_from_days: works far outside gmtime's range
	static void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
		z += 719468;
		int64_t era = (z >= 0 ? z : z - 146096) / 146097;
		unsigned doe = (unsigned)(z - era * 146097);
		unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		int64_t yy = (int64_t)yoe + era * 400;
		unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		unsigned mp = (5 * doy + 2) / 153;
		d = doy - (153 * mp + 2) / 5 + 1;
		m = mp < 10 ? mp + 3 : mp - 9;
		y = (int)(yy + (m <= 2));
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		std::string line1 = "2026-07-08";
		std::string line2 = "12:00 UTC";
		std::string line3 = "SUN TRI MOON 2.4\xC2\xB0";
		NVGcolor c3 = ASPECT_COLOR[TRINE];
		if (module) {
			int y;
			unsigned m, d;
			double days = std::floor(module->simUnix / 86400.0);
			civilFromDays((int64_t)days, y, m, d);
			int secs = (int)(module->simUnix - days * 86400.0);
			line1 = string::f("%d-%02u-%02u", y, m, d);
			line2 = string::f("%02d:%02d UTC", secs / 3600, (secs / 60) % 60);
			uint32_t packed = module->strongPacked.load(std::memory_order_relaxed);
			int sA = (packed >> 16) & 0xFF, sB = (packed >> 8) & 0xFF, sT = packed & 0xFF;
			if ((packed & (1u << 31)) && sA < NUM_BODIES && sB < NUM_BODIES && sT < NUM_ASPECTS) {
				line3 = string::f("%s %s %s %.1f\xC2\xB0",
					BODY_NAMES[sA], ASPECT_NAMES[sT], BODY_NAMES[sB],
					module->strongDelta.load(std::memory_order_relaxed));
				c3 = ASPECT_COLOR[sT];
			}
			else {
				line3 = "-";
				c3 = eclipse::DIM_COLOR;
			}
		}
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFontSize(args.vg, eclipse::TITLE_SIZE);
		nvgFillColor(args.vg, eclipse::ACCENT_COLOR);
		nvgText(args.vg, box.size.x / 2.f, box.size.y * 0.19f, line1.c_str(), NULL);
		nvgText(args.vg, box.size.x / 2.f, box.size.y * 0.5f, line2.c_str(), NULL);
		nvgFontSize(args.vg, eclipse::LABEL_SIZE);
		nvgFillColor(args.vg, c3);
		nvgText(args.vg, box.size.x / 2.f, box.size.y * 0.81f, line3.c_str(), NULL);
	}
};

// Static planet glyph used as a jack column label
struct PlanetGlyphLabel : TransparentWidget {
	int body = 0;

	void draw(const DrawArgs& args) override {
		drawPlanetGlyph(args.vg, body, box.size.div(2), mm2px(3.f), eclipse::LABEL_COLOR);
	}
};

struct CosmicClockWidget : ModuleWidget {
	CosmicClockWidget(CosmicClock* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/CosmicClock.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		eclipse::addHeader(this, 101.6f, "C O S M I C   C L O C K");

		CosmicWheel* wheel = new CosmicWheel;
		wheel->module = module;
		wheel->box.pos = Vec(0, 0);
		wheel->box.size = box.size;
		addChild(wheel);

		// Middle column: readout on top, then WARP+TIME, then ORB+RATE
		ChartReadout* readout = new ChartReadout;
		readout->module = module;
		readout->box.size = mm2px(Vec(44, 16));
		readout->box.pos = mm2px(Vec(118, 18));
		addChild(readout);

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(130.5f, 48)), module, CosmicClock::WARP_PARAM));
		eclipse::addLabel(this, Vec(130.5f, 55), "WARP");
		addParam(createParamCentered<CKSS>(mm2px(Vec(149.5f, 48)), module, CosmicClock::MODE_PARAM));
		eclipse::addLabel(this, Vec(149.5f, 55), "TIME");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(130.5f, 68)), module, CosmicClock::ORB_PARAM));
		eclipse::addLabel(this, Vec(130.5f, 75), "ORB");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(149.5f, 68)), module, CosmicClock::RATE_PARAM));
		eclipse::addLabel(this, Vec(149.5f, 75), "RATE");

		// Framed patch bay: one row per planet -- glyph, wave out,
		// EOC trigger -- with the MAIN pair below the separator. The five
		// aspect colors live on the wheel; tooltips carry the correspondences.
		static const float BAY_GLYPH_X = 171.5f;
		static const float BAY_WAVE_X = 183.f;
		static const float BAY_EOC_X = 194.f;
		eclipse::addLabel(this, Vec(BAY_WAVE_X, 22.5f), "WAVE");
		eclipse::addLabel(this, Vec(BAY_EOC_X, 22.5f), "EOC");
		for (int p = 0; p < NUM_BODIES; p++) {
			float y = 29.5f + 9.7f * p;
			PlanetGlyphLabel* glyph = new PlanetGlyphLabel;
			glyph->body = p;
			glyph->box.size = mm2px(Vec(6, 6));
			glyph->box.pos = mm2px(Vec(BAY_GLYPH_X, y)).minus(glyph->box.size.div(2));
			addChild(glyph);
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BAY_WAVE_X, y)), module, CosmicClock::WAVE_OUTPUTS + p));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BAY_EOC_X, y)), module, CosmicClock::EOC_OUTPUTS + p));
		}
		eclipse::addLabel(this, Vec(BAY_GLYPH_X, 118), "MAIN", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BAY_WAVE_X, 118)), module, CosmicClock::MAIN_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BAY_EOC_X, 118)), module, CosmicClock::MAINEOC_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		CosmicClock* module = getModule<CosmicClock>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Set time to now", "", [=]() {
			module->simUnix = CosmicClock::realNow();
			module->reseed();
		}));
	}
};

Model* modelCosmicClock = createModel<CosmicClock, CosmicClockWidget>("CosmicClock");
