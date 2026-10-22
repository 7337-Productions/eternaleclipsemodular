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
// The engine lives in dsp/Circle.hpp; this file is the Rack module and a
// provisional panel (the Circle and Triangle of Solomon come in phase 4).

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
		RETL_INPUT,
		RETR_INPUT,
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
		configOutput(SENDL_OUTPUT, "Send left");
		configOutput(SENDR_OUTPUT, "Send right");
		configOutput(MASTERL_OUTPUT, "Master left (limited)");
		configOutput(MASTERR_OUTPUT, "Master right (limited)");
		configBypass(INL_INPUTS + 0, MASTERL_OUTPUT);
		configBypass(INR_INPUTS + 0, MASTERR_OUTPUT);
		divider.setDivision(16);
	}

	// Knob state the engine recomputes coefficients from (once per block)
	conclave::CircleParams readParams() {
		conclave::CircleParams p = circle.p;
		for (int n = 0; n < CHANNELS; n++) {
			p.send[n] = params[SEND_PARAMS + n].getValue();
			p.feed[n] = params[FEED_PARAMS + n].getValue() * FEED_MAX;
			p.tuneHz[n] = TUNE_MIN_HZ * std::pow(TUNE_MAX_HZ / TUNE_MIN_HZ, params[TUNE_PARAMS + n].getValue());
			p.outPatched[n] = outputs[OUTL_OUTPUTS + n].isConnected() || outputs[OUTR_OUTPUTS + n].isConnected();
		}
		p.matter = params[MATTER_PARAM].getValue();
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
			}
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

// The panel: the Circle and Triangle of Solomon, the Triangle to the east.
// The four channels stand at the quarters of the Circle (1 north, then
// clockwise, the way the chain runs): LEVEL on the chain ring, SEND / FEED /
// TUNE on the arc inside the band, the FEED light in the quarter seal on the
// band. MASTER sits in the Triangle's inner circle, RETURN and its routing
// above the Triangle, MATTER below. Inputs bay on the far left, outs and the
// loop on the far right. Geometry constants mirror tools/gen_conclave_panel.py.
struct ConclaveOfHollowRadianceWidget : ModuleWidget {
	static constexpr float W = 243.84f; // 48 HP
	static constexpr float XC = W / 2.f;
	static constexpr float CX = 98.f, CY = 70.45f;      // the Circle
	static constexpr float R_TEXT = 49.f;               // the band's centre line (seals, lights)
	static constexpr float R_TRIO = 39.5f;              // SEND / FEED / TUNE
	static constexpr float R_LEVEL = 22.f;              // LEVEL, on the chain ring
	static constexpr float R_NUMERAL = 11.f;            // channel numerals, inside the LEVEL knobs
	static constexpr float R_TRIO_LABEL = 33.2f;        // the east and west clusters' turned labels
	static constexpr float TRI_CX = 155.f + 45.f / 3.f; // the Triangle's centroid (170)
	static constexpr float BAY_L_COL[3] = {15.f, 25.5f, 36.f};
	static constexpr float BAY_R_COL[2] = {215.9f, 231.9f};
	static constexpr float BAY_ROW0 = 32.f, BAY_STEP = 14.3f; // both bays share the row grid

	static Vec polar(float r, float deg) {
		float a = deg * (float)M_PI / 180.f;
		return Vec(CX + r * std::cos(a), CY + r * std::sin(a));
	}

	void addJack(float x, float y, int id, bool output) {
		if (output)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, id));
		else
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, id));
	}

	ConclaveOfHollowRadianceWidget(ConclaveOfHollowRadiance* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ConclaveOfHollowRadiance.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		eclipse::addHeader(this, XC, "C O N C L A V E   O F   H O L L O W   R A D I A N C E");

		// The Circle: a channel at each quarter, north first, clockwise
		const float QUARTER[CHANNELS] = {-90.f, 0.f, 90.f, 180.f};
		for (int n = 0; n < CHANNELS; n++) {
			float q = QUARTER[n];
			Vec level = polar(R_LEVEL, q);
			addParam(createParamCentered<RoundBigBlackKnob>(mm2px(level), module, ConclaveOfHollowRadiance::LEVEL_PARAMS + n));
			eclipse::addLabel(this, polar(R_NUMERAL, q), string::f("%d", n + 1), eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);

			Vec send = polar(R_TRIO, q - 22.f), feed = polar(R_TRIO, q), tune = polar(R_TRIO, q + 22.f);
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(send), module, ConclaveOfHollowRadiance::SEND_PARAMS + n));
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(feed), module, ConclaveOfHollowRadiance::FEED_PARAMS + n));
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(tune), module, ConclaveOfHollowRadiance::TUNE_PARAMS + n));
			// North and south: labels read upright, 6.6 mm inward of their
			// knobs. East and west: the labels turn to run along the band,
			// their tops toward the heart of the Circle.
			bool side = (n == 1 || n == 3);
			float rLabel = side ? R_TRIO_LABEL : R_TRIO - 6.6f;
			float angle = side ? (n == 1 ? -90.f : 90.f) : 0.f;
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
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(TRI_CX, 101.f)), module, ConclaveOfHollowRadiance::MATTER_PARAM));
		eclipse::addLabel(this, Vec(TRI_CX, 101.f + 7.2f), "MATTER", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);

		// Input bay: IN L, IN R, CV per channel
		eclipse::addLabel(this, Vec(BAY_L_COL[0], 24.f), "IN L", eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		eclipse::addLabel(this, Vec(BAY_L_COL[1], 24.f), "IN R", eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		eclipse::addLabel(this, Vec(BAY_L_COL[2], 24.f), "CV", eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
		// Input rows share the output bay's grid, so IN 1 faces OUT 1 across the panel
		for (int n = 0; n < CHANNELS; n++) {
			float y = BAY_ROW0 + n * BAY_STEP;
			addJack(BAY_L_COL[0], y, ConclaveOfHollowRadiance::INL_INPUTS + n, false);
			addJack(BAY_L_COL[1], y, ConclaveOfHollowRadiance::INR_INPUTS + n, false);
			addJack(BAY_L_COL[2], y, ConclaveOfHollowRadiance::CV_INPUTS + n, false);
			eclipse::addLabel(this, Vec(7.5f, y), string::f("%d", n + 1), eclipse::LABEL_SIZE, eclipse::LABEL_COLOR);
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
