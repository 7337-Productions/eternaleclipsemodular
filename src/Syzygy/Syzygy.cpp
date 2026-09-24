#include <atomic>
#include "plugin.hpp"
#include "EclipseWidgets.hpp"
#include "Syzygy/dsp/OrbitField.hpp"

// Syzygy (OMN-92): orbital note generator. A string winds through a square
// field and sixteen beads ride it; each bead has its own speed (a power of
// phi) and radius, so the string winds from the golden-angle sunflower it
// resets to into spirals, polygons and stars. A bead sounds while it sits
// inside the trigger arc. Beads are masses: MASS loosens their tether to the
// string, the walls bounce them, and you can grab and throw them. Notes rotate
// through eight CV/GATE pairs like a Monome Ansible in MIDI poly mode.
// Reinterprets Antonio Blanca's Lemur template ABreakpoint 2 as a companion to
// Cosmic Clock: every parameter except SCALE, ROOT and EPOCH is a bare
// -5..+5 V input. EPOCH divides the lap (1..21 base laps or clock pulses) and
// the inputs are read once per lap and glided to, so even the fast planets
// move Syzygy slowly.

using syzygy::N;
using syzygy::PAIRS;

// Abyssal Harmonics red: the beads and the line between them, sigils forming
static const NVGcolor SIGIL_RED = nvgRGB(0xff, 0x00, 0x00);

struct Syzygy : Module {
	enum ParamId {
		SCALE_PARAM,
		ROOT_PARAM,
		EPOCH_PARAM,
		MODE_PARAM,
		RUN_PARAM,
		RESET_PARAM,
		RANDOM_PARAM,
		ENUMS(ENABLE_PARAMS, N),
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		RESET_INPUT,
		RANDOM_INPUT,
		VOCT_INPUT,
		RATE_INPUT,
		XRATE_INPUT,
		YRATE_INPUT,
		SPREAD_INPUT,
		SHAPE_INPUT,
		MASS_INPUT,
		WARP_INPUT,
		THRESH_INPUT,
		WIDTH_INPUT,
		RANGE_INPUT,
		PSHAPE_INPUT,
		POFFSET_INPUT,
		SCALE_INPUT,
		ROOT_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(CV_OUTPUTS, PAIRS),
		ENUMS(GATE_OUTPUTS, PAIRS),
		OUTPUTS_LEN
	};
	enum LightId {
		RUN_LIGHT,
		ENUMS(ENABLE_LIGHTS, N),
		LIGHTS_LEN
	};

	static constexpr int BLOCK = 16;        // bead update every 16 samples
	static constexpr int STRING_EVERY = 4;  // computed string refresh every 4 blocks

	// Inputs that are read once per epoch and glided to
	enum Sampled {
		S_RATE, S_XRATE, S_YRATE, S_SPREAD, S_SHAPE, S_MASS, S_WARP, S_THRESH, S_WIDTH,
		S_RANGE, S_PSHAPE, S_POFFSET, S_SCALE, S_ROOT, NUM_SAMPLED
	};
	static constexpr int SAMPLED_INPUT[NUM_SAMPLED] = {
		RATE_INPUT, XRATE_INPUT, YRATE_INPUT, SPREAD_INPUT, SHAPE_INPUT, MASS_INPUT, WARP_INPUT,
		THRESH_INPUT, WIDTH_INPUT, RANGE_INPUT, PSHAPE_INPUT, POFFSET_INPUT, SCALE_INPUT, ROOT_INPUT
	};
	// Unpatched defaults, in volts: the sunflower, a lap a minute, X:Y = 1:phi
	static constexpr float SAMPLED_DEFAULT[NUM_SAMPLED] = {
		0.f, -0.6942f, 0.f, 0.f, -5.f, -2.5f, 0.f, -1.25f, -2.65f, 0.f, 2.5f, 0.f, -5.f, 0.f
	};
	static constexpr int EPOCH_DIV[8] = {1, 1, 2, 3, 5, 8, 13, 21}; // index 0 = LIVE (x1, no sampling)
	// Ceiling on the lap rate (laps per second) on either axis: a fast clock
	// speeds the beads up to this limit instead of teleporting them
	static constexpr float MAX_LAP_RATE = 2.f;

	syzygy::OrbitField field;
	syzygy::Params prm;
	dsp::ClockDivider divider;
	dsp::SchmittTrigger clockTrig, resetTrig, randTrig;
	dsp::BooleanTrigger resetBtn, randBtn;

	bool running = true;
	bool wasRunning = true;
	bool pendingReset = true;
	bool pendingArm = false;
	bool pendingRandom = false;
	int clockCounter = 0;
	int clockPeriod = 0;
	bool clockSeen = false;
	int clockPulses = 0;
	int lastPulses = 0;
	int blockCount = 0;

	// The lap, in laps: the unit the input sampling is gated on. Advanced by
	// the lap rate alone (never the X/Y rates); clocked, resynced to the pulse
	// count so a boundary lands exactly on pulse EPOCH.
	double lapPh = 0.0;

	// Epoch sampler
	float smpPrev[NUM_SAMPLED] = {};
	float smpTarget[NUM_SAMPLED] = {};
	float smpCur[NUM_SAMPLED] = {};
	bool smpInit = false;
	bool smpConnected[NUM_SAMPLED] = {};
	double tSec = 0.0;
	double epochStart = 0.0;
	double epochDur = 0.0;
	int64_t lastLap = -1;

	// UI -> audio
	std::atomic<float> ampX{1.f};
	std::atomic<float> ampY{1.f};
	std::atomic<bool> clearRandom{false};
	std::atomic<int> grabPoint{-1};
	std::atomic<float> grabX{0.f};
	std::atomic<float> grabY{0.f};
	std::atomic<int> flingPoint{-1};
	std::atomic<float> flingVX{0.f};
	std::atomic<float> flingVY{0.f};

	// audio -> UI
	std::atomic<float> dispX[N];
	std::atomic<float> dispY[N];
	std::atomic<float> dispTX[N];
	std::atomic<float> dispTY[N];
	float stringBuf[2][syzygy::STRING_N * 2] = {};
	std::atomic<int> stringFront{0};
	std::atomic<uint16_t> soundingMask{0};
	std::atomic<float> dispRate{1.f / 64.f};
	std::atomic<bool> dispClocked{false};
	std::atomic<float> dispArcStart{-syzygy::PI_F / 4.f};
	std::atomic<float> dispArcWidth{syzygy::PI_F / 2.f};
	std::atomic<int> dispTask{0};
	std::atomic<float> dispXRate{1.f / syzygy::PHI};
	std::atomic<float> dispYRate{1.f};
	std::atomic<float> dispMass{0.25f};
	std::atomic<float> dispEpoch{0.f}; // progress 0..1 through the current epoch
	std::atomic<int> dispScale{0};
	std::atomic<int> dispRoot{0};

	Syzygy() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		std::vector<std::string> scaleNames;
		for (int s = 0; s < syzygy::NUM_SCALES; s++)
			scaleNames.push_back(syzygy::kScales[s].name);
		configSwitch(SCALE_PARAM, 0.f, syzygy::NUM_SCALES - 1, 0.f, "Scale", scaleNames);
		std::vector<std::string> rootNames;
		for (int r = 0; r < 12; r++)
			rootNames.push_back(syzygy::kRootNames[r]);
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root", rootNames);
		configSwitch(EPOCH_PARAM, 0.f, 7.f, 2.f, "Epoch: lap divider (base laps, or clock pulses per lap); inputs are read once per lap",
			{"Live", "1", "2", "3", "5", "8", "13", "21"});
		configSwitch(MODE_PARAM, 0.f, 1.f, 0.f, "Line mode", {"Chain: beads joined in order", "String: the curve between the beads"});
		configSwitch(RUN_PARAM, 0.f, 1.f, 1.f, "Run", {"Stopped", "Running"});
		configButton(RESET_PARAM, "Reset to the sunflower");
		configButton(RANDOM_PARAM, "Random vector");
		for (int i = 0; i < N; i++)
			configSwitch(ENABLE_PARAMS + i, 0.f, 1.f, 1.f, string::f("Bead %d", i + 1), {"Off", "On"});

		configInput(CLOCK_INPUT, "Clock: a lap is EPOCH pulses");
		configInput(RESET_INPUT, "Reset trigger");
		configInput(RANDOM_INPUT, "Random vector trigger");
		configInput(VOCT_INPUT, "V/oct transpose (live)");
		configInput(RATE_INPUT, "Rate: 1 V/oct around a 64 s base lap; x2^round(V) on the clocked lap");
		configInput(XRATE_INPUT, "X rate: 1 V/oct multiplier (default 1/phi)");
		configInput(YRATE_INPUT, "Y rate: 1 V/oct multiplier (default x1)");
		configInput(SPREAD_INPUT, "Speed spread: -5 V none .. +5 V phi^-2..phi^2 (default 0 V = 1/phi..phi)");
		configInput(SHAPE_INPUT, "Task: ORBIT, LISSAJOUS, ROSE, STAR, SWEEP across -5..+5 V (default ORBIT)");
		configInput(MASS_INPUT, "Mass: -5 V rigid string .. +5 V free flight off the walls (default -2.5 V)");
		configInput(WARP_INPUT, "Radius law: -5 V tight centre, 0 V sunflower, +5 V ring");
		configInput(THRESH_INPUT, "Trigger arc start: -5..+5 V = -180..+180 degrees (default -45)");
		configInput(WIDTH_INPUT, "Trigger arc width: -5 V a sliver .. +5 V the whole circle (default a quarter)");
		configInput(RANGE_INPUT, "Pitch range: -5 V unison .. +5 V eight octaves (default 0 V = four)");
		configInput(PSHAPE_INPUT, "Pitch distribution shape (default +2.5 V)");
		configInput(POFFSET_INPUT, "Pitch distribution offset (default 0 V)");
		configInput(SCALE_INPUT, "Scale offset: -5..+5 V steps through the fourteen scales");
		configInput(ROOT_INPUT, "Root offset: 1 V/oct, semitones added to ROOT");
		for (int q = 0; q < PAIRS; q++) {
			configOutput(CV_OUTPUTS + q, string::f("Pair %d V/oct", q + 1));
			configOutput(GATE_OUTPUTS + q, string::f("Pair %d gate", q + 1));
		}

		divider.setDivision(BLOCK);
		for (int i = 0; i < N; i++) {
			dispX[i].store(0.f);
			dispY[i].store(0.f);
			dispTX[i].store(0.f);
			dispTY[i].store(0.f);
		}
	}

	// Bare jack: unpatched = default voltage, patched = clamped to +-5 V
	float cv(int inputId, float def) {
		if (!inputs[inputId].isConnected())
			return def;
		float v = inputs[inputId].getVoltage();
		if (!std::isfinite(v))
			return def;
		return clamp(v, -5.f, 5.f);
	}

	static float uni(float v) {
		return (v + 5.f) / 10.f;
	}

	void onReset() override {
		field.setSeed(0);
		ampX.store(1.f);
		ampY.store(1.f);
		pendingReset = true;
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "ampX", json_real(ampX.load()));
		json_object_set_new(rootJ, "ampY", json_real(ampY.load()));
		json_object_set_new(rootJ, "seed", json_integer(field.seed));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* axJ = json_object_get(rootJ, "ampX");
		if (axJ)
			ampX.store(clamp((float)json_real_value(axJ), 0.05f, 1.f));
		json_t* ayJ = json_object_get(rootJ, "ampY");
		if (ayJ)
			ampY.store(clamp((float)json_real_value(ayJ), 0.05f, 1.f));
		json_t* seedJ = json_object_get(rootJ, "seed");
		if (seedJ)
			field.setSeed((uint32_t)json_integer_value(seedJ));
		pendingReset = true;
	}

	// RANDOM VECTOR: a new seed for the ripples along the string and a fresh
	// enable pattern; knobs are never touched.
	void randomVector() {
		field.setSeed(field.seed + 1);
		uint32_t h = field.seed * 2246822519u;
		for (int i = 0; i < N; i++) {
			h = h * 1664525u + 1013904223u;
			params[ENABLE_PARAMS + i].setValue(((h >> 8) & 0xFFFF) > 0x8000 ? 1.f : 0.f);
		}
	}

	// Read the sampled inputs: live, or once per epoch with a glide across
	// the following epoch
	void sampleInputs(bool live) {
		float raw[NUM_SAMPLED];
		for (int k = 0; k < NUM_SAMPLED; k++)
			raw[k] = cv(SAMPLED_INPUT[k], SAMPLED_DEFAULT[k]);
		if (live || !smpInit) {
			for (int k = 0; k < NUM_SAMPLED; k++) {
				smpPrev[k] = smpTarget[k] = smpCur[k] = raw[k];
				smpConnected[k] = inputs[SAMPLED_INPUT[k]].isConnected();
			}
			smpInit = true;
			epochStart = tSec;
			epochDur = 0.0;
			lastLap = -1;
			dispEpoch.store(live ? 0.f : 1.f, std::memory_order_relaxed);
			return;
		}
		for (int k = 0; k < NUM_SAMPLED; k++) {
			bool c = inputs[SAMPLED_INPUT[k]].isConnected();
			if (c != smpConnected[k]) {
				smpConnected[k] = c;
				smpPrev[k] = smpTarget[k] = smpCur[k] = raw[k];
			}
		}
		bool boundary = false;
		int64_t lap = (int64_t)std::floor(lapPh);
		if (lastLap < 0 || lap < lastLap)
			lastLap = lap; // first lap, or a clock resync stepped back
		else if (lap > lastLap) {
			lastLap = lap;
			boundary = true;
		}
		if (boundary) {
			for (int k = 0; k < NUM_SAMPLED; k++) {
				smpPrev[k] = smpCur[k];
				smpTarget[k] = raw[k];
			}
			epochDur = tSec - epochStart;
			epochStart = tSec;
		}
		float f = epochDur > 0.0 ? clamp((float)((tSec - epochStart) / epochDur), 0.f, 1.f) : 1.f;
		for (int k = 0; k < NUM_SAMPLED; k++)
			smpCur[k] = smpPrev[k] + (smpTarget[k] - smpPrev[k]) * f;
		dispEpoch.store(f, std::memory_order_relaxed);
	}

	void block(const ProcessArgs& args) {
		float dt = BLOCK * args.sampleTime;
		tSec += dt;
		blockCount++;

		running = params[RUN_PARAM].getValue() > 0.5f;
		if (running != wasRunning) {
			field.allOff();
			if (running)
				pendingArm = true;
			wasRunning = running;
		}

		bool clocked = inputs[CLOCK_INPUT].isConnected();
		int epochIdx = clamp((int)params[EPOCH_PARAM].getValue(), 0, 7);
		int div = EPOCH_DIV[epochIdx];

		// Advance the lap (at last block's rate) before the inputs are sampled
		if (clocked && clockPulses != lastPulses) {
			lastPulses = clockPulses;
			lapPh = (double)clockPulses * std::exp2(std::round(smpCur[S_RATE])) / div;
		}
		else {
			if (!clocked)
				lastPulses = 0;
			lapPh += (double)prm.rate * dt;
		}
		sampleInputs(epochIdx == 0);

		// A lap is EPOCH base laps (64 s each at RATE 0 V), or EPOCH clock pulses
		float rateV = smpCur[S_RATE];
		if (clocked)
			prm.rate = clockSeen ? args.sampleRate / clockPeriod / div * std::exp2(std::round(rateV)) : 0.f;
		else
			prm.rate = (1.f / 64.f) / div * std::exp2(rateV);
		prm.xRate = std::exp2(smpCur[S_XRATE]);
		prm.yRate = std::exp2(smpCur[S_YRATE]);
		float axisMax = std::fmax(1.f, std::fmax(prm.xRate, prm.yRate));
		prm.rate = std::fmin(prm.rate, MAX_LAP_RATE / axisMax);
		prm.spread = uni(smpCur[S_SPREAD]);
		prm.shape = uni(smpCur[S_SHAPE]);
		prm.mass = uni(smpCur[S_MASS]);
		prm.warp = smpCur[S_WARP] / 5.f;
		prm.arcStart = smpCur[S_THRESH] / 5.f * syzygy::PI_F;
		prm.arcWidth = (0.02f + 0.98f * uni(smpCur[S_WIDTH])) * syzygy::TWO_PI;
		prm.rangeSemis = 96.f * uni(smpCur[S_RANGE]);
		prm.pitchShape = uni(smpCur[S_PSHAPE]);
		prm.pitchOffset = uni(smpCur[S_POFFSET]);
		prm.ampX = ampX.load(std::memory_order_relaxed);
		prm.ampY = ampY.load(std::memory_order_relaxed);

		int sc = (int)params[SCALE_PARAM].getValue()
			+ (int)std::lround(uni(smpCur[S_SCALE]) * (syzygy::NUM_SCALES - 1));
		sc = ((sc % syzygy::NUM_SCALES) + syzygy::NUM_SCALES) % syzygy::NUM_SCALES;
		int rt = (int)params[ROOT_PARAM].getValue() + (int)std::lround(smpCur[S_ROOT] * 12.f);
		rt = ((rt % 12) + 12) % 12;
		prm.scaleMask = syzygy::scaleMask(sc);
		prm.root = rt;

		if (pendingRandom) {
			pendingRandom = false;
			randomVector();
		}
		if (clearRandom.exchange(false))
			field.setSeed(0);

		uint16_t en = 0;
		for (int i = 0; i < N; i++)
			if (params[ENABLE_PARAMS + i].getValue() > 0.5f)
				en |= (uint16_t)(1u << i);
		prm.enableMask = en;

		if (pendingReset) {
			pendingReset = false;
			pendingArm = false;
			field.reset(prm);
			lapPh = 0.0;
			clockPulses = lastPulses = 0;
			lastLap = -1;
		}
		else if (pendingArm) {
			pendingArm = false;
			field.computeTargets(prm);
			field.arm(prm);
		}

		// Mouse: pin the grabbed bead, apply a throw
		int g = grabPoint.load(std::memory_order_relaxed);
		if (g >= 0)
			field.hold(g, grabX.load(std::memory_order_relaxed), grabY.load(std::memory_order_relaxed));
		else
			field.held = -1;
		int fp = flingPoint.exchange(-1);
		if (fp >= 0)
			field.fling(fp, flingVX.load(std::memory_order_relaxed), flingVY.load(std::memory_order_relaxed));

		if (running)
			field.update(prm, dt);
		else {
			field.computeTargets(prm);
			field.integrate(prm, dt);
		}

		if (params[MODE_PARAM].getValue() > 0.5f && blockCount % STRING_EVERY == 0) {
			field.computeString(prm);
			int back = 1 - stringFront.load(std::memory_order_relaxed);
			for (int j = 0; j < syzygy::STRING_N; j++) {
				stringBuf[back][2 * j] = field.stringX[j];
				stringBuf[back][2 * j + 1] = field.stringY[j];
			}
			stringFront.store(back, std::memory_order_release);
		}

		uint16_t sm = 0;
		for (int i = 0; i < N; i++) {
			const syzygy::Point& pt = field.pts[i];
			dispX[i].store(pt.x, std::memory_order_relaxed);
			dispY[i].store(pt.y, std::memory_order_relaxed);
			dispTX[i].store(pt.tx, std::memory_order_relaxed);
			dispTY[i].store(pt.ty, std::memory_order_relaxed);
			bool held = pt.pair >= 0;
			if (held)
				sm |= (uint16_t)(1u << i);
			bool enabled = (en >> i) & 1u;
			lights[ENABLE_LIGHTS + i].setBrightness(held ? 1.f : (enabled ? 0.25f : 0.f));
		}
		soundingMask.store(sm, std::memory_order_relaxed);
		lights[RUN_LIGHT].setBrightness(running ? 1.f : 0.f);

		dispRate.store(prm.rate, std::memory_order_relaxed);
		dispClocked.store(clocked, std::memory_order_relaxed);
		dispArcStart.store(prm.arcStart, std::memory_order_relaxed);
		dispArcWidth.store(prm.arcWidth, std::memory_order_relaxed);
		dispTask.store(field.taskIndex(prm), std::memory_order_relaxed);
		dispXRate.store(prm.xRate, std::memory_order_relaxed);
		dispYRate.store(prm.yRate, std::memory_order_relaxed);
		dispMass.store(prm.mass, std::memory_order_relaxed);
		dispScale.store(sc, std::memory_order_relaxed);
		dispRoot.store(rt, std::memory_order_relaxed);
	}

	void process(const ProcessArgs& args) override {
		if (inputs[CLOCK_INPUT].isConnected()) {
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (clockCounter > 0) {
					clockPeriod = clockCounter;
					clockSeen = true;
				}
				clockCounter = 0;
				clockPulses++;
			}
			if (clockCounter < (1 << 24))
				clockCounter++;
		}
		else {
			clockSeen = false;
			clockCounter = 0;
			clockPulses = 0;
		}
		bool rst = resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		if (resetBtn.process(params[RESET_PARAM].getValue() > 0.5f))
			rst = true;
		if (rst)
			pendingReset = true;
		bool rnd = randTrig.process(inputs[RANDOM_INPUT].getVoltage(), 0.1f, 1.f);
		if (randBtn.process(params[RANDOM_PARAM].getValue() > 0.5f))
			rnd = true;
		if (rnd)
			pendingRandom = true;

		if (divider.process())
			block(args);

		float voct = inputs[VOCT_INPUT].getVoltage();
		if (!std::isfinite(voct))
			voct = 0.f;
		for (int q = 0; q < PAIRS; q++) {
			outputs[CV_OUTPUTS + q].setVoltage(field.pairs[q].pitch + voct);
			outputs[GATE_OUTPUTS + q].setVoltage(running && field.gateHigh(q) ? 10.f : 0.f);
		}
	}
};

constexpr int Syzygy::SAMPLED_INPUT[Syzygy::NUM_SAMPLED];
constexpr float Syzygy::SAMPLED_DEFAULT[Syzygy::NUM_SAMPLED];
constexpr int Syzygy::EPOCH_DIV[8];

// The field: the beads joined in order (the Breakpoint's line), the trigger
// arc, readouts. Grab a bead and throw it; a click without a drag toggles its
// enable.
struct FieldDisplay : TransparentWidget {
	Syzygy* module = NULL;
	int grabbed = -1;
	Vec pressPos, curPos;
	float velX = 0.f, velY = 0.f; // field units per second
	double lastTime = 0.0;
	bool moved = false;

	float inset() { return mm2px(1.5f); }

	Vec toPx(float x, float y) {
		float in = inset();
		float w = box.size.x - 2.f * in;
		float h = box.size.y - 2.f * in;
		return Vec(in + (x + 1.f) * 0.5f * w, in + (1.f - y) * 0.5f * h);
	}

	Vec toField(Vec px) {
		float in = inset();
		float w = box.size.x - 2.f * in;
		float h = box.size.y - 2.f * in;
		return Vec((px.x - in) / w * 2.f - 1.f, 1.f - (px.y - in) / h * 2.f);
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS || e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		float best = mm2px(3.f);
		int hit = -1;
		for (int i = 0; i < N; i++) {
			Vec p = toPx(module->dispX[i].load(), module->dispY[i].load());
			float d = p.minus(e.pos).norm();
			if (d < best) {
				best = d;
				hit = i;
			}
		}
		if (hit < 0)
			return;
		e.consume(this);
		grabbed = hit;
		pressPos = curPos = e.pos;
		moved = false;
		velX = velY = 0.f;
		lastTime = system::getTime();
		Vec f = toField(e.pos);
		module->grabX.store(f.x);
		module->grabY.store(f.y);
		module->grabPoint.store(hit);
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (!module || grabbed < 0 || e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		Vec d = e.mouseDelta.div(getAbsoluteZoom());
		curPos = curPos.plus(d);
		if (curPos.minus(pressPos).norm() > mm2px(0.7f))
			moved = true;
		double now = system::getTime();
		float dt = std::fmax((float)(now - lastTime), 0.004f);
		lastTime = now;
		float in = inset();
		float w = box.size.x - 2.f * in;
		float h = box.size.y - 2.f * in;
		float ivx = d.x / w * 2.f / dt;
		float ivy = -d.y / h * 2.f / dt;
		velX += (ivx - velX) * 0.5f;
		velY += (ivy - velY) * 0.5f;
		Vec f = toField(curPos);
		module->grabX.store(clamp(f.x, -1.f, 1.f));
		module->grabY.store(clamp(f.y, -1.f, 1.f));
	}

	void onDragEnd(const DragEndEvent& e) override {
		if (!module || grabbed < 0)
			return;
		int i = grabbed;
		grabbed = -1;
		if (!moved) {
			module->grabPoint.store(-1);
			float cur = module->params[Syzygy::ENABLE_PARAMS + i].getValue();
			APP->engine->setParamValue(module, Syzygy::ENABLE_PARAMS + i, cur > 0.5f ? 0.f : 1.f);
			return;
		}
		module->flingVX.store(velX);
		module->flingVY.store(velY);
		module->flingPoint.store(i);
		module->grabPoint.store(-1);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		float w = box.size.x;
		float h = box.size.y;
		nvgSave(args.vg);
		nvgScissor(args.vg, 0, 0, w, h);

		float px[N], py[N];
		static float str[syzygy::STRING_N * 2];
		bool useString = false;
		uint16_t enabled = 0, sounding = 0;
		float arcStart = -syzygy::PI_F / 4.f, arcWidth = syzygy::PI_F / 2.f;
		if (module) {
			for (int i = 0; i < N; i++) {
				px[i] = module->dispX[i].load(std::memory_order_relaxed);
				py[i] = module->dispY[i].load(std::memory_order_relaxed);
				if (module->params[Syzygy::ENABLE_PARAMS + i].getValue() > 0.5f)
					enabled |= (uint16_t)(1u << i);
			}
			useString = module->params[Syzygy::MODE_PARAM].getValue() > 0.5f;
			if (useString) {
				int front = module->stringFront.load(std::memory_order_acquire);
				for (int j = 0; j < syzygy::STRING_N * 2; j++)
					str[j] = module->stringBuf[front][j];
			}
			sounding = module->soundingMask.load(std::memory_order_relaxed);
			arcStart = module->dispArcStart.load(std::memory_order_relaxed);
			arcWidth = module->dispArcWidth.load(std::memory_order_relaxed);
		}
		else {
			// Browser preview: the sunflower a little way into its first lap
			syzygy::OrbitField f;
			syzygy::Params p;
			f.phX = 0.35;
			f.phY = 0.35;
			f.computeTargets(p);
			for (int i = 0; i < N; i++) {
				px[i] = f.pts[i].tx;
				py[i] = f.pts[i].ty;
				enabled |= (uint16_t)(1u << i);
				if (syzygy::OrbitField::insideArc(px[i], py[i], p))
					sounding |= (uint16_t)(1u << i);
			}
			enabled &= ~(uint16_t)((1u << 5) | (1u << 9));
		}

		// A faint crosshair, nothing more: the line and the beads carry the picture
		Vec c = toPx(0.f, 0.f);
		float rx = 0.5f * w - inset();
		float ry = 0.5f * h - inset();
		nvgStrokeColor(args.vg, nvgRGB(0x2a, 0x19, 0x0c));
		nvgStrokeWidth(args.vg, mm2px(0.2f));
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, c.x, inset());
		nvgLineTo(args.vg, c.x, h - inset());
		nvgMoveTo(args.vg, inset(), c.y);
		nvgLineTo(args.vg, w - inset(), c.y);
		nvgStroke(args.vg);

		// Trigger arc: a sector from the centre. Field angles are CCW with y
		// up; on screen (y down) that is nanovg's CCW direction with negated angles.
		float R = std::sqrt(rx * rx + ry * ry) + mm2px(2.f);
		nvgSave(args.vg);
		nvgScissor(args.vg, inset(), inset(), w - 2.f * inset(), h - 2.f * inset());
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, c.x, c.y);
		nvgArc(args.vg, c.x, c.y, R, -arcStart, -(arcStart + arcWidth), NVG_CCW);
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, nvgRGBA(0xcd, 0x76, 0x2b, 26));
		nvgFill(args.vg);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, c.x, c.y);
		nvgLineTo(args.vg, c.x + R * std::cos(arcStart), c.y - R * std::sin(arcStart));
		nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xee, 0xb8, 230));
		nvgStrokeWidth(args.vg, mm2px(0.3f));
		nvgStroke(args.vg);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, c.x, c.y);
		nvgLineTo(args.vg, c.x + R * std::cos(arcStart + arcWidth), c.y - R * std::sin(arcStart + arcWidth));
		nvgStrokeColor(args.vg, nvgRGBA(0xcd, 0x76, 0x2b, 128));
		nvgStrokeWidth(args.vg, mm2px(0.25f));
		nvgStroke(args.vg);
		nvgRestore(args.vg);

		// The line: beads joined in order, as the Breakpoint object drew them,
		// or the computed string the orbit law traces between them
		nvgBeginPath(args.vg);
		if (useString) {
			for (int j = 0; j < syzygy::STRING_N; j++) {
				Vec s = toPx(str[2 * j], str[2 * j + 1]);
				if (j == 0)
					nvgMoveTo(args.vg, s.x, s.y);
				else
					nvgLineTo(args.vg, s.x, s.y);
			}
		}
		else {
			for (int i = 0; i < N; i++) {
				Vec s = toPx(px[i], py[i]);
				if (i == 0)
					nvgMoveTo(args.vg, s.x, s.y);
				else
					nvgLineTo(args.vg, s.x, s.y);
			}
		}
		nvgStrokeColor(args.vg, nvgRGBA(0xff, 0x00, 0x00, 175));
		nvgStrokeWidth(args.vg, mm2px(0.3f));
		nvgLineJoin(args.vg, NVG_ROUND);
		nvgStroke(args.vg);

		// Beads
		for (int i = 0; i < N; i++) {
			Vec p = toPx(px[i], py[i]);
			bool en = (enabled >> i) & 1u;
			bool on = (sounding >> i) & 1u;
			if (!en) {
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, p.x, p.y, mm2px(1.3f));
				nvgStrokeColor(args.vg, nvgRGBA(0xff, 0x00, 0x00, 150));
				nvgStrokeWidth(args.vg, mm2px(0.3f));
				nvgStroke(args.vg);
				continue;
			}
			if (on) {
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, p.x, p.y, mm2px(2.8f));
				nvgFillColor(args.vg, nvgRGBA(0xff, 0x00, 0x00, 120));
				nvgFill(args.vg);
			}
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, p.x, p.y, mm2px(1.3f));
			nvgFillColor(args.vg, on ? eclipse::LABEL_COLOR : SIGIL_RED);
			nvgFill(args.vg);
		}

		// Readouts
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, eclipse::FINE_SIZE);
			float tx0 = mm2px(3.f);
			float ty0 = mm2px(3.2f);
			std::string scaleText, rateText, detail;
			if (module) {
				scaleText = string::f("%s · %s", syzygy::kRootNames[module->dispRoot.load()],
					syzygy::kScales[module->dispScale.load()].name);
				float rate = module->dispRate.load(std::memory_order_relaxed);
				int epIdx = clamp((int)module->params[Syzygy::EPOCH_PARAM].getValue(), 0, 7);
				std::string epText = epIdx == 0 ? "LIVE" : string::f("EPOCH %d", Syzygy::EPOCH_DIV[epIdx]);
				std::string lapText = "LAP ...";
				if (rate > 0.f) {
					float secs = 1.f / rate;
					lapText = secs < 60.f ? string::f("LAP %.1f s", secs)
						: string::f("LAP %dm%02ds", (int)(secs / 60.f), (int)std::fmod(secs, 60.f));
				}
				rateText = std::string(module->dispClocked.load() ? "CLK · " : "") + lapText + " · " + epText;
				detail = string::f("%s · X %.2f · Y %.2f · MASS %.2f",
					syzygy::kTaskNames[clamp(module->dispTask.load(), 0, syzygy::NUM_TASKS - 1)],
					module->dispXRate.load(), module->dispYRate.load(), module->dispMass.load());
			}
			else {
				scaleText = "C · MAJOR";
				rateText = "LAP 2m08s · EPOCH 2";
				detail = "ORBIT · X 0.62 · Y 1.00 · MASS 0.25";
			}
			nvgFillColor(args.vg, eclipse::LABEL_COLOR);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgText(args.vg, tx0, ty0, scaleText.c_str(), NULL);
			nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgText(args.vg, w - tx0, ty0, rateText.c_str(), NULL);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgText(args.vg, tx0, h - ty0, detail.c_str(), NULL);

			// Epoch progress: a thin bar under the top-right readout
			if (module) {
				float f = module->dispEpoch.load(std::memory_order_relaxed);
				float bw = mm2px(18.f);
				nvgBeginPath(args.vg);
				nvgRect(args.vg, w - tx0 - bw, ty0 + mm2px(2.2f), bw * f, mm2px(0.4f));
				nvgFillColor(args.vg, nvgRGBA(0xcd, 0x76, 0x2b, 160));
				nvgFill(args.vg);
			}
		}

		nvgResetScissor(args.vg);
		nvgRestore(args.vg);
	}
};

// Context-menu slider for the orbit amplitude
struct AmpQuantity : Quantity {
	Syzygy* module;
	int axis;
	AmpQuantity(Syzygy* m, int a) : module(m), axis(a) {}
	std::atomic<float>& ref() { return axis ? module->ampY : module->ampX; }
	void setValue(float v) override { ref().store(clamp(v, 0.05f, 1.f)); }
	float getValue() override { return ref().load(); }
	float getMinValue() override { return 0.05f; }
	float getMaxValue() override { return 1.f; }
	float getDefaultValue() override { return 1.f; }
	float getDisplayValue() override { return getValue() * 100.f; }
	void setDisplayValue(float v) override { setValue(v / 100.f); }
	std::string getLabel() override { return axis ? "Amplitude Y" : "Amplitude X"; }
	std::string getUnit() override { return "%"; }
	int getDisplayPrecision() override { return 3; }
};

struct SyzygyWidget : ModuleWidget {
	// 40 HP. Field on the left, framed middle column, framed vertical bay.
	static constexpr float TOP = 16.f, FIELD = 106.5f;
	static constexpr float BAY_IN_A = 155.f, BAY_IN_B = 167.f, BAY_CV = 181.f, BAY_GATE = 193.f;
	static constexpr float ROW0 = 27.5f, ROW_DY = 12.2f;

	void addLabel(Vec mmPos, const std::string& text, float fontSize = eclipse::LABEL_SIZE,
	              NVGcolor color = eclipse::LABEL_COLOR) {
		eclipse::addLabel(this, mmPos, text, fontSize, color);
	}

	void addBayInput(float x, int row, const char* label, int inputId, Syzygy* module) {
		float y = ROW0 + ROW_DY * row;
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, inputId));
		addLabel(Vec(x, y + 6.6f), label, eclipse::FINE_SIZE);
	}

	SyzygyWidget(Syzygy* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Syzygy.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		eclipse::addHeader(this, 101.6f, "S Y Z Y G Y");

		// ===== Field =====
		FieldDisplay* display = new FieldDisplay;
		display->module = module;
		display->box.pos = mm2px(Vec(3.f, TOP));
		display->box.size = mm2px(Vec(FIELD, FIELD));
		addChild(display);

		// ===== Middle column =====
		addLabel(Vec(129.5f, 20.f), "MODE", eclipse::FINE_SIZE);
		addParam(createParamCentered<eclipse::CKSSHorizontal>(mm2px(Vec(129.5f, 25.2f)), module, Syzygy::MODE_PARAM));
		addLabel(Vec(118.3f, 25.2f), "CHAIN", eclipse::FINE_SIZE);
		addLabel(Vec(140.8f, 25.2f), "STRING", eclipse::FINE_SIZE);

		static const float GX[4] = {118.4f, 125.8f, 133.2f, 140.6f};
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++) {
				int i = r * 4 + c;
				addParam(createLightParamCentered<VCVLightBezelLatch<YellowLight>>(
					mm2px(Vec(GX[c], 32.f + 7.6f * r)), module, Syzygy::ENABLE_PARAMS + i, Syzygy::ENABLE_LIGHTS + i));
			}
		addLabel(Vec(129.5f, 61.f), "BEADS", eclipse::FINE_SIZE);

		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<YellowLight>>>(
			mm2px(Vec(119.f, 69.f)), module, Syzygy::RUN_PARAM, Syzygy::RUN_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(129.5f, 69.f)), module, Syzygy::RESET_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(140.f, 69.f)), module, Syzygy::RANDOM_PARAM));
		addLabel(Vec(119.f, 74.7f), "RUN", eclipse::FINE_SIZE);
		addLabel(Vec(129.5f, 74.7f), "RESET", eclipse::FINE_SIZE);
		addLabel(Vec(140.f, 74.7f), "RAND", eclipse::FINE_SIZE);

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(121.f, 85.5f)), module, Syzygy::SCALE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(138.f, 85.5f)), module, Syzygy::ROOT_PARAM));
		addLabel(Vec(121.f, 92.7f), "SCALE");
		addLabel(Vec(138.f, 92.7f), "ROOT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(121.f, 100.f)), module, Syzygy::SCALE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(138.f, 100.f)), module, Syzygy::ROOT_INPUT));
		addLabel(Vec(121.f, 106.4f), "CV", eclipse::FINE_SIZE);
		addLabel(Vec(138.f, 106.4f), "CV", eclipse::FINE_SIZE);

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(129.5f, 113.8f)), module, Syzygy::EPOCH_PARAM));
		addLabel(Vec(129.5f, 120.f), "EPOCH", eclipse::FINE_SIZE);

		// ===== Bay =====
		addLabel(Vec(161.f, 20.5f), "IN");
		addLabel(Vec(BAY_CV, 20.5f), "CV");
		addLabel(Vec(BAY_GATE, 20.5f), "GATE");

		addBayInput(BAY_IN_A, 0, "CLOCK", Syzygy::CLOCK_INPUT, module);
		addBayInput(BAY_IN_A, 1, "RESET", Syzygy::RESET_INPUT, module);
		addBayInput(BAY_IN_A, 2, "RAND", Syzygy::RANDOM_INPUT, module);
		addBayInput(BAY_IN_A, 3, "V/OCT", Syzygy::VOCT_INPUT, module);
		addBayInput(BAY_IN_A, 4, "RATE", Syzygy::RATE_INPUT, module);
		addBayInput(BAY_IN_A, 5, "X RATE", Syzygy::XRATE_INPUT, module);
		addBayInput(BAY_IN_A, 6, "Y RATE", Syzygy::YRATE_INPUT, module);
		addBayInput(BAY_IN_A, 7, "SPREAD", Syzygy::SPREAD_INPUT, module);
		addBayInput(BAY_IN_B, 0, "TASK", Syzygy::SHAPE_INPUT, module);
		addBayInput(BAY_IN_B, 1, "MASS", Syzygy::MASS_INPUT, module);
		addBayInput(BAY_IN_B, 2, "WARP", Syzygy::WARP_INPUT, module);
		addBayInput(BAY_IN_B, 3, "ARC", Syzygy::THRESH_INPUT, module);
		addBayInput(BAY_IN_B, 4, "WIDTH", Syzygy::WIDTH_INPUT, module);
		addBayInput(BAY_IN_B, 5, "RANGE", Syzygy::RANGE_INPUT, module);
		addBayInput(BAY_IN_B, 6, "P.SHAPE", Syzygy::PSHAPE_INPUT, module);
		addBayInput(BAY_IN_B, 7, "P.OFFS", Syzygy::POFFSET_INPUT, module);

		for (int q = 0; q < PAIRS; q++) {
			float y = ROW0 + ROW_DY * q;
			addLabel(Vec(177.3f, y), string::f("%d", q + 1), eclipse::FINE_SIZE, eclipse::ACCENT_COLOR);
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BAY_CV, y)), module, Syzygy::CV_OUTPUTS + q));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BAY_GATE, y)), module, Syzygy::GATE_OUTPUTS + q));
		}
	}

	void appendContextMenu(Menu* menu) override {
		Syzygy* module = getModule<Syzygy>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Field"));
		for (int axis = 0; axis < 2; axis++) {
			ui::Slider* slider = new ui::Slider;
			slider->quantity = new AmpQuantity(module, axis);
			slider->box.size.x = 200.f;
			menu->addChild(slider);
		}
		menu->addChild(createMenuItem("Clear random vector", "", [module]() {
			module->clearRandom.store(true);
		}, module->field.seed == 0));

	}
};

Model* modelSyzygy = createModel<Syzygy, SyzygyWidget>("Syzygy");
