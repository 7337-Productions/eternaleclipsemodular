#include <atomic>
#include "plugin.hpp"
#include "EclipseWidgets.hpp"
#include "ConclaveOfHollowRadiance/dsp/Circle.hpp"

// Conclave of Hollow Radiance (OMN-102): a four-channel stereo VCA mixer
// that is a normal mixer at rest and a feedback instrument when pushed.
// Veils' chain runs forward (an unpatched OUT passes the channel down into
// the next; channel 4 falls into the master); each channel's bipolar FEED
// sends its chained output backward around the ring through a tunable comb
// into the previous channel, channel 1 into channel 4. One stereo send with
// a return that enters the master (POST) or channel 1 (LOOP), MATTER to
// darken every comb, MASTER, and a lookahead limiter before the outputs.
// The engine lives in dsp/Circle.hpp; this file is the Rack module and its
// panel, the Circle and Triangle of Solomon, with the lights that show the
// ring at work: the chain glowing clockwise along the band, feedback
// glowing counter-clockwise in red, and the Triangle's circle burning with
// the limiter's gain reduction.

using conclave::CHANNELS;

struct ConclaveOfHollowRadiance : Module {
	enum ParamId {
		ENUMS(LEVEL_PARAMS, CHANNELS),
		ENUMS(SEND_PARAMS, CHANNELS),
		ENUMS(FEED_PARAMS, CHANNELS),
		ENUMS(TUNE_PARAMS, CHANNELS),
		RETURN_PARAM,
		LOOP_PARAM,
		MATTER_PARAM,
		MASTER_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		ENUMS(INL_INPUTS, CHANNELS),
		ENUMS(INR_INPUTS, CHANNELS),
		ENUMS(CV_INPUTS, CHANNELS),
		ENUMS(SEND_CV_INPUTS, CHANNELS),
		ENUMS(FEED_CV_INPUTS, CHANNELS),
		ENUMS(TUNE_CV_INPUTS, CHANNELS),
		RETL_INPUT,
		RETR_INPUT,
		MATTER_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(OUTL_OUTPUTS, CHANNELS),
		ENUMS(OUTR_OUTPUTS, CHANNELS),
		SENDL_OUTPUT,
		SENDR_OUTPUT,
		MASTERL_OUTPUT,
		MASTERR_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(FEED_LIGHTS, CHANNELS),
		LIGHTS_LEN
	};

	static constexpr float FEED_MAX = 1.5f;   // loop gain at the ends of the FEED knob
	static constexpr float TUNE_MIN_HZ = 20.f, TUNE_MAX_HZ = 2000.f;

	conclave::Circle circle;
	dsp::ClockDivider divider;
	bool expResponse[CHANNELS] = {false, false, false, false};
	bool ceiling10 = false;
	int clipHold = 0; // samples left on the hard-clip flag, for the display

	// audio -> UI, refreshed once per block
	std::atomic<float> dispLevel[CHANNELS]; // chained output level leaving each channel, 0..1
	std::atomic<float> dispFeed[CHANNELS];  // feedback arriving at each channel, 0..1
	std::atomic<float> dispGR{0.f};         // limiter gain reduction, 0..1
	std::atomic<bool> dispClip{false};      // the hard clip engaged in the last 0.3 s

	struct MatterQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float hz = conclave::Circle::matterToCutoff(getValue());
			return hz >= 1000.f ? string::f("%.1f kHz", hz / 1000.f) : string::f("%.0f Hz", hz);
		}
	};

	ConclaveOfHollowRadiance() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int n = 0; n < CHANNELS; n++) {
			std::string ch = string::f("Channel %d", n + 1);
			configParam(LEVEL_PARAMS + n, 0.f, 1.f, 1.f, ch + " level", "%", 0.f, 100.f);
			configParam(SEND_PARAMS + n, 0.f, 1.f, 0.f, ch + " send", "%", 0.f, 100.f);
			configParam(FEED_PARAMS + n, -1.f, 1.f, 0.f,
				ch + " feed: loop gain back into channel " + std::to_string(n == 0 ? CHANNELS : n) + " (noon off, left inverted)",
				"x", 0.f, FEED_MAX);
			configParam(TUNE_PARAMS + n, 0.f, 1.f, 0.5f, ch + " tune: comb pitch", " Hz", TUNE_MAX_HZ / TUNE_MIN_HZ, TUNE_MIN_HZ);
			configInput(INL_INPUTS + n, ch + " left");
			configInput(INR_INPUTS + n, ch + " right (normalled to left)");
			configInput(CV_INPUTS + n, ch + " level CV (0-10 V)");
			configInput(SEND_CV_INPUTS + n, ch + " send CV (10 V = full range)");
			configInput(FEED_CV_INPUTS + n, ch + " feed CV (-5..+5 V = full range)");
			configInput(TUNE_CV_INPUTS + n, ch + " tune CV (1 V/oct)");
			configOutput(OUTL_OUTPUTS + n, ch + " left (unpatched: passes down the chain)");
			configOutput(OUTR_OUTPUTS + n, ch + " right (unpatched: passes down the chain)");
			configLight(FEED_LIGHTS + n, ch + " loop above unity");
		}
		configParam(RETURN_PARAM, 0.f, 1.f, 1.f, "Return level", "%", 0.f, 100.f);
		configSwitch(LOOP_PARAM, 0.f, 1.f, 0.f, "Return routing", {"Post: into the master", "Loop: into channel 1, inside the circle"});
		configParam<MatterQuantity>(MATTER_PARAM, 0.f, 1.f, 0.5f, "Matter: damping of every comb, dark to bright");
		configParam(MASTER_PARAM, 0.f, 1.f, 1.f, "Master level", "%", 0.f, 100.f);
		configInput(RETL_INPUT, "Return left");
		configInput(RETR_INPUT, "Return right (normalled to left)");
		configInput(MATTER_INPUT, "Matter CV (10 V = full range)");
		configOutput(SENDL_OUTPUT, "Send left");
		configOutput(SENDR_OUTPUT, "Send right");
		configOutput(MASTERL_OUTPUT, "Master left (limited)");
		configOutput(MASTERR_OUTPUT, "Master right (limited)");
		configBypass(INL_INPUTS + 0, MASTERL_OUTPUT);
		configBypass(INR_INPUTS + 0, MASTERR_OUTPUT);
		divider.setDivision(16);
		for (int n = 0; n < CHANNELS; n++) {
			dispLevel[n].store(0.f);
			dispFeed[n].store(0.f);
		}
	}

	// Knob and CV state the engine recomputes coefficients from (once per block)
	conclave::CircleParams readParams() {
		conclave::CircleParams p = circle.p;
		const float octaves = std::log2(TUNE_MAX_HZ / TUNE_MIN_HZ); // the TUNE knob's span, for 1 V/oct
		for (int n = 0; n < CHANNELS; n++) {
			p.send[n] = clamp(params[SEND_PARAMS + n].getValue() + inputs[SEND_CV_INPUTS + n].getVoltage() / 10.f, 0.f, 1.f);
			p.feed[n] = clamp(params[FEED_PARAMS + n].getValue() + inputs[FEED_CV_INPUTS + n].getVoltage() / 5.f, -1.f, 1.f) * FEED_MAX;
			float tune = clamp(params[TUNE_PARAMS + n].getValue() + inputs[TUNE_CV_INPUTS + n].getVoltage() / octaves, 0.f, 1.f);
			p.tuneHz[n] = TUNE_MIN_HZ * std::pow(TUNE_MAX_HZ / TUNE_MIN_HZ, tune);
			p.outPatched[n] = outputs[OUTL_OUTPUTS + n].isConnected() || outputs[OUTR_OUTPUTS + n].isConnected();
		}
		p.matter = clamp(params[MATTER_PARAM].getValue() + inputs[MATTER_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		p.ret = params[RETURN_PARAM].getValue();
		p.loopReturn = params[LOOP_PARAM].getValue() > 0.5f;
		p.master = params[MASTER_PARAM].getValue();
		return p;
	}

	void onSampleRateChange() override {
		// set() then reset(): the delays start at their targets instead of
		// gliding in from the default pitch (see Circle::reset)
		circle.set(readParams());
		circle.reset(APP->engine->getSampleRate());
		circle.limiter.ceiling = ceiling10 ? 10.f : 5.f;
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_t* expJ = json_array();
		for (int n = 0; n < CHANNELS; n++)
			json_array_append_new(expJ, json_boolean(expResponse[n]));
		json_object_set_new(rootJ, "expResponse", expJ);
		json_object_set_new(rootJ, "ceiling10", json_boolean(ceiling10));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* expJ = json_object_get(rootJ, "expResponse");
		if (expJ)
			for (int n = 0; n < CHANNELS; n++) {
				json_t* vJ = json_array_get(expJ, n);
				if (vJ)
					expResponse[n] = json_boolean_value(vJ);
			}
		json_t* cJ = json_object_get(rootJ, "ceiling10");
		if (cJ)
			ceiling10 = json_boolean_value(cJ);
		circle.limiter.ceiling = ceiling10 ? 10.f : 5.f;
	}

	void process(const ProcessArgs& args) override {
		if (divider.process()) {
			circle.set(readParams());
			circle.limiter.ceiling = ceiling10 ? 10.f : 5.f;
			// The loop closed by channel N's FEED runs through channel N-1's
			// VCA; it will sing on its own once that loop gain reaches unity
			for (int n = 0; n < CHANNELS; n++) {
				int prev = (n + CHANNELS - 1) % CHANNELS;
				float gain = std::fabs(circle.p.feed[n]) * circle.p.level[prev];
				float b = gain >= 1.f ? 1.f : (std::fabs(circle.p.feed[n]) > 0.03f ? 0.25f : 0.f);
				lights[FEED_LIGHTS + n].setBrightness(b);
				dispLevel[n].store(clamp(circle.peak[n] / 5.f, 0.f, 1.f), std::memory_order_relaxed);
				dispFeed[n].store(clamp(circle.fbPeak[n] / 5.f, 0.f, 1.f), std::memory_order_relaxed);
			}
			dispGR.store(circle.limiter.gainReduction, std::memory_order_relaxed);
			dispClip.store(clipHold > 0, std::memory_order_relaxed);
		}

		// Levels follow the CV every sample
		conclave::CircleIn in;
		for (int n = 0; n < CHANNELS; n++) {
			float g = params[LEVEL_PARAMS + n].getValue();
			if (inputs[CV_INPUTS + n].isConnected())
				g *= clamp(inputs[CV_INPUTS + n].getVoltage() / 10.f, 0.f, 1.f);
			if (expResponse[n])
				g = g * g * g * g;
			circle.p.level[n] = g;
			in.in[n].l = inputs[INL_INPUTS + n].getVoltageSum();
			in.in[n].r = inputs[INR_INPUTS + n].isConnected() ? inputs[INR_INPUTS + n].getVoltageSum() : in.in[n].l;
		}
		in.ret.l = inputs[RETL_INPUT].getVoltageSum();
		in.ret.r = inputs[RETR_INPUT].isConnected() ? inputs[RETR_INPUT].getVoltageSum() : in.ret.l;

		conclave::CircleOut out;
		circle.process(in, out);
		if (circle.limiter.clipped)
			clipHold = (int)(0.3f * args.sampleRate);
		else if (clipHold > 0)
			clipHold--;

		for (int n = 0; n < CHANNELS; n++) {
			outputs[OUTL_OUTPUTS + n].setVoltage(out.out[n].l);
			outputs[OUTR_OUTPUTS + n].setVoltage(out.out[n].r);
		}
		outputs[SENDL_OUTPUT].setVoltage(out.send.l);
		outputs[SENDR_OUTPUT].setVoltage(out.send.r);
		outputs[MASTERL_OUTPUT].setVoltage(out.master.l);
		outputs[MASTERR_OUTPUT].setVoltage(out.master.r);
	}
};

// Panel geometry shared by the display and the widget (mm). Mirrors
// tools/gen_conclave_panel.py.
namespace geo {
static constexpr float W = 233.68f; // 46 HP
static constexpr float CX = 99.09f, CY = 70.45f;     // the Circle
static constexpr float R_OUT = 53.f, R_IN = 45.f;    // the two rings
static constexpr float R_TEXT = 49.f;                // the band's centre line (seals, lights)
static constexpr float R_TRIO = 39.5f;               // SEND / FEED / TUNE
static constexpr float R_LEVEL = 22.f;               // LEVEL, on the chain ring
static constexpr float TRI_CX = 156.09f + 45.f / 3.f; // the Triangle's centroid (171.09)
static constexpr float R_TRI_INNER = 13.f;           // the Triangle's circle
static constexpr float QUARTER[CHANNELS] = {-90.f, 0.f, 90.f, 180.f}; // channel 1 north, then clockwise

inline Vec polar(float r, float deg) {
	float a = deg * (float)M_PI / 180.f;
	return Vec(CX + r * std::cos(a), CY + r * std::sin(a));
}
} // namespace geo

// The lights, drawn in layer 1 so they burn at full brightness in a dimmed
// room. The chain glows clockwise along the inside of the band, brighter
// with the level leaving each channel, and a soft highlight travels round
// each quarter at a pace set by that level, lighting the names it passes.
// Feedback glows counter-clockwise just inside the inner ring, in Abyssal
// red, brighter with what each channel receives. The Triangle's circle
// fills with the limiter's gain reduction: copper while limiting, red when
// the hard clip has engaged. In the module browser it shows a quiet
// evening's worth of activity.
struct ConclaveDisplay : TransparentWidget {
	ConclaveOfHollowRadiance* module = NULL;
	float phase[CHANNELS] = {0.f, 22.f, 45.f, 67.f}; // degrees into each quarter
	float grDisp = 0.f;

	static NVGcolor copper(float a) { return nvgRGBAf(1.f, 0.569f, 0.161f, a); }
	static NVGcolor red(float a) { return nvgRGBAf(1.f, 0.f, 0.f, a); }
	static float rad(float deg) { return deg * (float)M_PI / 180.f; }

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		float level[CHANNELS], feed[CHANNELS], gr;
		bool clip;
		if (module) {
			for (int n = 0; n < CHANNELS; n++) {
				level[n] = module->dispLevel[n].load(std::memory_order_relaxed);
				feed[n] = module->dispFeed[n].load(std::memory_order_relaxed);
			}
			gr = module->dispGR.load(std::memory_order_relaxed);
			clip = module->dispClip.load(std::memory_order_relaxed);
		}
		else {
			const float L[CHANNELS] = {0.55f, 0.4f, 0.7f, 0.3f};
			const float F[CHANNELS] = {0.35f, 0.f, 0.5f, 0.2f};
			for (int n = 0; n < CHANNELS; n++) {
				level[n] = L[n];
				feed[n] = F[n];
			}
			gr = 0.3f;
			clip = false;
		}
		float dt = APP->window->getLastFrameDuration();
		dt = clamp(dt, 0.f, 0.1f);
		// Attack at once, release over half a second
		grDisp = std::fmax(gr, grDisp - 2.f * dt);

		NVGcontext* vg = args.vg;
		nvgSave(vg);
		nvgLineCap(vg, NVG_ROUND);
		Vec c = mm2px(Vec(geo::CX, geo::CY));

		for (int n = 0; n < CHANNELS; n++) {
			float q0 = geo::QUARTER[n], q1 = q0 + 90.f;
			// The chain, clockwise from this channel's seal to the next
			if (level[n] > 0.004f) {
				nvgStrokeWidth(vg, mm2px(1.4f));
				nvgStrokeColor(vg, copper(0.1f + 0.55f * level[n]));
				nvgBeginPath(vg);
				nvgArc(vg, c.x, c.y, mm2px(geo::R_IN + 1.2f), rad(q0 + 5.f), rad(q1 - 5.f), NVG_CW);
				nvgStroke(vg);

				// The travelling light, lighting the names as it goes
				phase[n] += dt * (25.f + 150.f * level[n]);
				while (phase[n] >= 90.f)
					phase[n] -= 90.f;
				float a = q0 + phase[n];
				float glow = 0.12f + 0.3f * level[n];
				nvgStrokeWidth(vg, mm2px(geo::R_OUT - geo::R_IN));
				nvgStrokeColor(vg, copper(0.35f * glow));
				nvgBeginPath(vg);
				nvgArc(vg, c.x, c.y, mm2px(geo::R_TEXT), rad(a - 13.f), rad(a + 13.f), NVG_CW);
				nvgStroke(vg);
				nvgStrokeColor(vg, copper(glow));
				nvgBeginPath(vg);
				nvgArc(vg, c.x, c.y, mm2px(geo::R_TEXT), rad(a - 5.f), rad(a + 5.f), NVG_CW);
				nvgStroke(vg);
			}
			// Feedback, counter-clockwise from the next channel's seal back to this one
			if (feed[n] > 0.004f) {
				nvgStrokeWidth(vg, mm2px(0.9f));
				nvgStrokeColor(vg, red(0.15f + 0.8f * feed[n]));
				nvgBeginPath(vg);
				nvgArc(vg, c.x, c.y, mm2px(geo::R_IN - 0.9f), rad(q1 - 5.f), rad(q0 + 5.f), NVG_CCW);
				nvgStroke(vg);
			}
		}

		// The Triangle's circle: the spirit manifests
		if (grDisp > 0.003f) {
			Vec t = mm2px(Vec(geo::TRI_CX, geo::CY));
			float rIn = mm2px(7.62f + 0.7f), rOut = mm2px(geo::R_TRI_INNER - 0.5f);
			NVGcolor col = clip ? red(0.85f * grDisp) : copper(0.8f * grDisp);
			nvgStrokeWidth(vg, rOut - rIn);
			nvgStrokeColor(vg, col);
			nvgBeginPath(vg);
			nvgCircle(vg, t.x, t.y, 0.5f * (rIn + rOut));
			nvgStroke(vg);
			// and a halo beyond the circle
			float rHalo = mm2px(geo::R_TRI_INNER + 5.f);
			NVGcolor inner = clip ? red(0.35f * grDisp) : copper(0.3f * grDisp);
			NVGcolor outer = clip ? red(0.f) : copper(0.f);
			nvgFillPaint(vg, nvgRadialGradient(vg, t.x, t.y, rOut, rHalo, inner, outer));
			nvgBeginPath(vg);
			nvgCircle(vg, t.x, t.y, rHalo);
			nvgCircle(vg, t.x, t.y, rOut);
			nvgPathWinding(vg, NVG_HOLE);
			nvgFill(vg);
		}
		nvgRestore(vg);
	}
};

// The panel: the Circle and Triangle of Solomon, the Triangle to the east.
// The four channels stand at the quarters of the Circle (1 north, then
// clockwise, the way the chain runs): LEVEL on the chain ring, SEND / FEED /
// TUNE on the arc inside the band, the FEED light in the quarter seal on the
// band. MASTER sits in the Triangle's inner circle, RETURN and its routing
// above the Triangle, MATTER below. Inputs bay on the far left, outs and the
// loop on the far right. Geometry constants mirror tools/gen_conclave_panel.py.
struct ConclaveOfHollowRadianceWidget : ModuleWidget {
	static constexpr float W = geo::W, CX = geo::CX, CY = geo::CY;
	static constexpr float R_TEXT = geo::R_TEXT, R_TRIO = geo::R_TRIO, R_LEVEL = geo::R_LEVEL;
	static constexpr float TRI_CX = geo::TRI_CX;
	static constexpr float XC = W / 2.f;
	static Vec polar(float r, float deg) { return geo::polar(r, deg); }
	static constexpr float R_NUMERAL = 11.f;            // channel numerals, inside the LEVEL knobs
	static constexpr float R_TRIO_LABEL = 33.2f;        // the east and west clusters' turned labels
	static constexpr float BAY_L_COL[3] = {13.f, 23.5f, 34.f};
	static constexpr float BAY_L_ROW_A = 31.f, BAY_L_ROW_B = 42.f, BAY_L_GROUP = 24.5f; // a channel is two rows
	static constexpr float BAY_R_COL[2] = {213.18f, 223.68f};
	static constexpr float BAY_ROW0 = 32.f, BAY_STEP = 14.3f;
	static constexpr float MATTER_X = 166.1f, MATTER_CV_X = 176.1f, MATTER_Y = 101.f;

	void addJack(float x, float y, int id, bool output) {
		if (output)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, id));
		else
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, id));
	}

	ConclaveOfHollowRadianceWidget(ConclaveOfHollowRadiance* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ConclaveOfHollowRadiance.svg")));

		ConclaveDisplay* display = new ConclaveDisplay;
		display->module = module;
		display->box.pos = Vec(0.f, 0.f);
		display->box.size = box.size;
		addChild(display);

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		eclipse::addHeader(this, XC, "C O N C L A V E   O F   H O L L O W   R A D I A N C E");

		// The Circle: a channel at each quarter, north first, clockwise
		for (int n = 0; n < CHANNELS; n++) {
			float q = geo::QUARTER[n];
			Vec level = polar(R_LEVEL, q);
			addParam(createParamCentered<RoundBigBlackKnob>(mm2px(level), module, ConclaveOfHollowRadiance::LEVEL_PARAMS + n));
			eclipse::addLabel(this, polar(R_NUMERAL, q), string::f("%d", n + 1), eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);

			Vec send = polar(R_TRIO, q - 22.f), feed = polar(R_TRIO, q), tune = polar(R_TRIO, q + 22.f);
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(send), module, ConclaveOfHollowRadiance::SEND_PARAMS + n));
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(feed), module, ConclaveOfHollowRadiance::FEED_PARAMS + n));
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(tune), module, ConclaveOfHollowRadiance::TUNE_PARAMS + n));
			// North and south: labels read upright, 6.6 mm inward of their
			// knobs. East and west: the labels turn to run along the band,
			// their baselines toward the LEVEL knobs.
			bool side = (n == 1 || n == 3);
			float rLabel = side ? R_TRIO_LABEL : R_TRIO - 6.6f;
			float angle = side ? (n == 1 ? 90.f : -90.f) : 0.f;
			eclipse::addLabel(this, polar(rLabel, q - 22.f), "SEND", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR, angle);
			eclipse::addLabel(this, polar(rLabel, q), "FEED", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR, angle);
			eclipse::addLabel(this, polar(rLabel, q + 22.f), "TUNE", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR, angle);

			addChild(createLightCentered<SmallLight<RedLight>>(mm2px(polar(R_TEXT, q)), module, ConclaveOfHollowRadiance::FEED_LIGHTS + n));
		}

		// The Triangle: MASTER within its circle, RETURN above, MATTER below
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(TRI_CX, CY)), module, ConclaveOfHollowRadiance::MASTER_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(TRI_CX, 29.f)), module, ConclaveOfHollowRadiance::RETURN_PARAM));
		eclipse::addLabel(this, Vec(TRI_CX, 29.f + 7.2f), "RETURN", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addParam(createParamCentered<eclipse::CKSSHorizontal>(mm2px(Vec(TRI_CX, 43.f)), module, ConclaveOfHollowRadiance::LOOP_PARAM));
		eclipse::addLabel(this, Vec(TRI_CX - 9.5f, 43.f), "POST", eclipse::FINE_SIZE, eclipse::LABEL_COLOR);
		eclipse::addLabel(this, Vec(TRI_CX + 9.5f, 43.f), "LOOP", eclipse::FINE_SIZE, eclipse::LABEL_COLOR);
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(MATTER_X, MATTER_Y)), module, ConclaveOfHollowRadiance::MATTER_PARAM));
		eclipse::addLabel(this, Vec(MATTER_X, MATTER_Y + 7.2f), "MATTER", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addJack(MATTER_CV_X, MATTER_Y, ConclaveOfHollowRadiance::MATTER_INPUT, false);
		eclipse::addLabel(this, Vec(MATTER_CV_X, MATTER_Y + 6.6f), "CV", eclipse::FINE_SIZE, eclipse::LABEL_COLOR);

		// Input bay: each channel is two rows, IN L / IN R / level CV above
		// and SEND / FEED / TUNE CV below; the two header lines read in that order
		eclipse::addLabel(this, Vec(BAY_L_COL[0], 20.8f), "IN L", eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		eclipse::addLabel(this, Vec(BAY_L_COL[1], 20.8f), "IN R", eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		eclipse::addLabel(this, Vec(BAY_L_COL[2], 20.8f), "CV", eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		eclipse::addLabel(this, Vec(BAY_L_COL[0], 24.2f), "SEND", eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		eclipse::addLabel(this, Vec(BAY_L_COL[1], 24.2f), "FEED", eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		eclipse::addLabel(this, Vec(BAY_L_COL[2], 24.2f), "TUNE", eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		for (int n = 0; n < CHANNELS; n++) {
			float ya = BAY_L_ROW_A + n * BAY_L_GROUP, yb = BAY_L_ROW_B + n * BAY_L_GROUP;
			addJack(BAY_L_COL[0], ya, ConclaveOfHollowRadiance::INL_INPUTS + n, false);
			addJack(BAY_L_COL[1], ya, ConclaveOfHollowRadiance::INR_INPUTS + n, false);
			addJack(BAY_L_COL[2], ya, ConclaveOfHollowRadiance::CV_INPUTS + n, false);
			addJack(BAY_L_COL[0], yb, ConclaveOfHollowRadiance::SEND_CV_INPUTS + n, false);
			addJack(BAY_L_COL[1], yb, ConclaveOfHollowRadiance::FEED_CV_INPUTS + n, false);
			addJack(BAY_L_COL[2], yb, ConclaveOfHollowRadiance::TUNE_CV_INPUTS + n, false);
			eclipse::addLabel(this, Vec(6.5f, 0.5f * (ya + yb)), string::f("%d", n + 1), eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		}

		// Output bay: the four direct outs, the send, the return, the master
		const char* ROW_LABEL[7] = {"OUT 1", "OUT 2", "OUT 3", "OUT 4", "SEND", "RETURN", "MASTER"};
		const bool ROW_OUTPUT[7] = {true, true, true, true, true, false, true};
		const int ROW_L[7] = {ConclaveOfHollowRadiance::OUTL_OUTPUTS + 0, ConclaveOfHollowRadiance::OUTL_OUTPUTS + 1,
			ConclaveOfHollowRadiance::OUTL_OUTPUTS + 2, ConclaveOfHollowRadiance::OUTL_OUTPUTS + 3,
			ConclaveOfHollowRadiance::SENDL_OUTPUT, ConclaveOfHollowRadiance::RETL_INPUT, ConclaveOfHollowRadiance::MASTERL_OUTPUT};
		const int ROW_R[7] = {ConclaveOfHollowRadiance::OUTR_OUTPUTS + 0, ConclaveOfHollowRadiance::OUTR_OUTPUTS + 1,
			ConclaveOfHollowRadiance::OUTR_OUTPUTS + 2, ConclaveOfHollowRadiance::OUTR_OUTPUTS + 3,
			ConclaveOfHollowRadiance::SENDR_OUTPUT, ConclaveOfHollowRadiance::RETR_INPUT, ConclaveOfHollowRadiance::MASTERR_OUTPUT};
		float mid = 0.5f * (BAY_R_COL[0] + BAY_R_COL[1]);
		eclipse::addLabel(this, Vec(BAY_R_COL[0], 21.8f), "L", eclipse::FINE_SIZE, eclipse::LABEL_COLOR);
		eclipse::addLabel(this, Vec(BAY_R_COL[1], 21.8f), "R", eclipse::FINE_SIZE, eclipse::LABEL_COLOR);
		for (int r = 0; r < 7; r++) {
			float y = BAY_ROW0 + r * BAY_STEP;
			addJack(BAY_R_COL[0], y, ROW_L[r], ROW_OUTPUT[r]);
			addJack(BAY_R_COL[1], y, ROW_R[r], ROW_OUTPUT[r]);
			eclipse::addLabel(this, Vec(mid, y - 6.4f), ROW_LABEL[r], eclipse::LABEL_SIZE,
				ROW_OUTPUT[r] ? eclipse::ACCENT_COLOR : eclipse::LABEL_COLOR);
		}
	}

	void appendContextMenu(Menu* menu) override {
		ConclaveOfHollowRadiance* module = getModule<ConclaveOfHollowRadiance>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Level response"));
		for (int n = 0; n < CHANNELS; n++)
			menu->addChild(createBoolPtrMenuItem(string::f("Channel %d exponential", n + 1), "", &module->expResponse[n]));
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Limiter ceiling 10 V (default 5 V)", "", &module->ceiling10));
	}
};

Model* modelConclaveOfHollowRadiance = createModel<ConclaveOfHollowRadiance, ConclaveOfHollowRadianceWidget>("ConclaveOfHollowRadiance");
