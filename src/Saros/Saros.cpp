#include "plugin.hpp"
#include "EclipseWidgets.hpp"

// Saros: envelope and modulation generator named for the ~18-year eclipse cycle.
// A bank of 16 shapes morphed by SHAPE, skewed in time by WARP, rippled by
// FLUX, stretched from 5 ms to 30 minutes by TIME. The screen shows the
// current curve and a playhead.
struct Saros : Module {
	enum ParamId {
		TIME_PARAM,
		SHAPE_PARAM,
		WARP_PARAM,
		FLUX_PARAM,
		LOOP_PARAM,
		TRIG_PARAM,
		POLY_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TIME_INPUT,
		SHAPE_INPUT,
		WARP_INPUT,
		FLUX_INPUT,
		TRIG_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_OUTPUT,
		EOC_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	static constexpr int NUM_SHAPES = 16;
	static constexpr int TABLE_N = 256;

	// One envelope per channel. POLY off: channel 0 only, exactly the mono module.
	static constexpr int MAX_CH = 16;
	float phase[MAX_CH];
	bool running[MAX_CH];
	dsp::SchmittTrigger trigTrigger[MAX_CH];
	dsp::PulseGenerator eocPulse[MAX_CH];
	int lastChannels = 1;
	bool bipolar = false;
	// Hand-drawn shape: drawing on the screen fills this and switches it on
	float drawTable[TABLE_N];
	bool drawMode = false;
	dsp::BooleanTrigger buttonTrigger;

	// Effective values, read by the panel display
	float dispShape = 0.f;
	float dispWarp = 0.f;
	float dispFlux = 0.f;
	float dispPhase = 1.f;
	float dispDuration = 3.f;
	bool dispLoop = false;

	Saros() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TIME_PARAM, 0.f, 1.f, 0.5f, "Time", " s", 360000.f, 0.005f);
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.f, "Shape");
		configParam(WARP_PARAM, -1.f, 1.f, 0.f, "Warp");
		configParam(FLUX_PARAM, 0.f, 1.f, 0.f, "Flux", "%", 0.f, 100.f);
		configSwitch(LOOP_PARAM, 0.f, 1.f, 0.f, "Loop", {"Off", "On"});
		configButton(TRIG_PARAM, "Trigger");
		configSwitch(POLY_PARAM, 0.f, 1.f, 0.f, "Voices", {"Mono", "Poly"});
		configInput(TIME_INPUT, "Time CV (10V = full range)");
		configInput(SHAPE_INPUT, "Shape CV (10V = full range)");
		configInput(WARP_INPUT, "Warp CV (5V = full range)");
		configInput(FLUX_INPUT, "Flux CV (10V = full range)");
		configInput(TRIG_INPUT, "Trigger (sets the voice count in POLY)");
		configOutput(OUT_OUTPUT, "Envelope");
		configOutput(EOC_OUTPUT, "End of cycle trigger");
		initDrawTable();
		resetVoices(0);
	}

	// Park voices c.. : idle at the end of the curve
	void resetVoices(int from) {
		for (int c = from; c < MAX_CH; c++) {
			phase[c] = 1.f;
			running[c] = false;
			eocPulse[c].reset();
		}
	}

	void initDrawTable() {
		for (int i = 0; i < TABLE_N; i++) {
			float t = (float)i / (TABLE_N - 1);
			drawTable[i] = 1.f - std::fabs(2.f * t - 1.f);
		}
	}

	static float bell(float p, float c, float w) {
		float d = (p - c) / w;
		return std::exp(-0.5f * d * d);
	}

	// One shape from the bank; p in [0,1], returns [0,1]
	static float shapeValue(int i, float p) {
		switch (i) {
			// Decays
			case 0: return (std::exp(-9.f * p) - 1.2341e-4f) / (1.f - 1.2341e-4f); // pluck
			case 1: return 1.f - p; // linear decay
			case 2: { // sharp AD
				float a = 0.08f;
				return p < a ? p / a : 1.f - (p - a) / (1.f - a);
			}
			case 3: return 1.f - std::fabs(2.f * p - 1.f); // triangle
			// Bells and swells
			case 4: return std::sin(M_PI * p); // half sine
			case 5: return bell(p, 0.5f, 0.15f); // gaussian bell
			case 6: return p; // linear rise
			case 7: return p * p * p; // slow swell
			// Complex
			case 8: return std::max(bell(p, 0.28f, 0.1f), 0.75f * bell(p, 0.7f, 0.1f)); // double peak
			case 9: return (1.f - p) * (0.6f + 0.4f * std::cos(2.f * M_PI * 4.f * p)); // rippled decay
			case 10: return std::pow(1.f - p, 1.5f) * std::fabs(std::cos(M_PI * 3.5f * p)); // bounce
			case 11: return std::min((float)(int)(p * 5.f), 4.f) / 4.f; // stairs up
			case 12: return 1.f - std::min((float)(int)(p * 5.f), 4.f) / 4.f; // stairs down
			case 13: return (std::fmod(p * 6.f, 1.f) < 0.5f ? 1.f : 0.f) * (1.f - p); // pulse train decay
			case 14: return 0.5f + 0.5f * std::sin(2.f * M_PI * 3.f * p) * std::exp(-2.f * p); // damped wave
			default: return clamp(0.5f + 0.35f * std::sin(2.f * M_PI * 1.7f * p + 1.f)
				+ 0.25f * std::sin(2.f * M_PI * 3.1f * p + 2.f)
				+ 0.15f * std::sin(2.f * M_PI * 5.3f * p + 4.f), 0.f, 1.f); // drift
		}
	}

	// Full curve: morphed shape bank (or the drawn table), time-warped, flux ripple
	static float envValue(const float* table, float shape, float warp, float flux, float p) {
		p = clamp(p, 0.f, 1.f);
		float pw = std::pow(p, std::pow(2.f, 2.5f * warp));
		float v;
		if (table) {
			float x = pw * (TABLE_N - 1);
			int i = std::min((int)x, TABLE_N - 2);
			v = crossfade(table[i], table[i + 1], x - i);
		}
		else {
			float s = shape * (NUM_SHAPES - 1);
			int i0 = clamp((int)s, 0, NUM_SHAPES - 1);
			int i1 = std::min(i0 + 1, NUM_SHAPES - 1);
			v = crossfade(shapeValue(i0, pw), shapeValue(i1, pw), s - i0);
		}
		// Ripple keeps the onset intact (0 at p=0), carves 8 notches across the curve
		float ripple = 0.5f - 0.5f * std::cos(2.f * M_PI * 8.f * pw);
		v *= 1.f - flux * ripple;
		return clamp(v, 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		// POLY: one voice per TRIG channel. Mono CV is copied to every voice.
		bool poly = params[POLY_PARAM].getValue() > 0.5f;
		int channels = poly ? std::max(1, inputs[TRIG_INPUT].getChannels()) : 1;
		if (channels < lastChannels)
			resetVoices(channels);
		lastChannels = channels;

		bool loop = params[LOOP_PARAM].getValue() > 0.5f;
		bool button = buttonTrigger.process(params[TRIG_PARAM].getValue() > 0.f);
		const float* table = drawMode ? drawTable : NULL;

		for (int c = 0; c < channels; c++) {
			float time = clamp(params[TIME_PARAM].getValue() + inputs[TIME_INPUT].getPolyVoltage(c) / 10.f, 0.f, 1.f);
			float shape = clamp(params[SHAPE_PARAM].getValue() + inputs[SHAPE_INPUT].getPolyVoltage(c) / 10.f, 0.f, 1.f);
			float warp = clamp(params[WARP_PARAM].getValue() + inputs[WARP_INPUT].getPolyVoltage(c) / 5.f, -1.f, 1.f);
			float flux = clamp(params[FLUX_PARAM].getValue() + inputs[FLUX_INPUT].getPolyVoltage(c) / 10.f, 0.f, 1.f);

			bool trig = trigTrigger[c].process(inputs[TRIG_INPUT].getPolyVoltage(c), 0.1f, 1.f);
			trig |= button;
			if (trig) {
				phase[c] = 0.f;
				running[c] = true;
			}
			else if (loop && !running[c]) {
				if (phase[c] >= 1.f)
					phase[c] = 0.f;
				running[c] = true;
			}

			float duration = 0.005f * std::pow(360000.f, time);
			if (running[c]) {
				phase[c] += args.sampleTime / duration;
				if (phase[c] >= 1.f) {
					eocPulse[c].trigger(1e-3f);
					if (loop) {
						phase[c] -= std::floor(phase[c]);
					}
					else {
						phase[c] = 1.f;
						running[c] = false;
					}
				}
			}

			float v = envValue(table, shape, warp, flux, phase[c]);
			outputs[OUT_OUTPUT].setVoltage(bipolar ? v * 10.f - 5.f : v * 10.f, c);
			outputs[EOC_OUTPUT].setVoltage(eocPulse[c].process(args.sampleTime) ? 10.f : 0.f, c);

			if (c == 0) {
				dispShape = shape;
				dispWarp = warp;
				dispFlux = flux;
				dispPhase = phase[0];
				dispDuration = duration;
				dispLoop = loop;
			}
		}
		outputs[OUT_OUTPUT].setChannels(channels);
		outputs[EOC_OUTPUT].setChannels(channels);
	}

	void onReset() override {
		resetVoices(0);
		drawMode = false;
		initDrawTable();
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "bipolar", json_boolean(bipolar));
		json_object_set_new(rootJ, "drawMode", json_boolean(drawMode));
		json_t* tableJ = json_array();
		for (int i = 0; i < TABLE_N; i++)
			json_array_append_new(tableJ, json_real(drawTable[i]));
		json_object_set_new(rootJ, "drawTable", tableJ);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* bipolarJ = json_object_get(rootJ, "bipolar");
		if (bipolarJ)
			bipolar = json_boolean_value(bipolarJ);
		json_t* drawModeJ = json_object_get(rootJ, "drawMode");
		if (drawModeJ)
			drawMode = json_boolean_value(drawModeJ);
		json_t* tableJ = json_object_get(rootJ, "drawTable");
		if (tableJ) {
			for (int i = 0; i < TABLE_N; i++) {
				json_t* vJ = json_array_get(tableJ, i);
				if (vJ)
					drawTable[i] = clamp((float)json_real_value(vJ), 0.f, 1.f);
			}
		}
	}
};

// The screen: current curve, playhead, time readout. Dragging with the left
// mouse button draws a custom shape into the module's table.
struct SarosDisplay : TransparentWidget {
	Saros* module = NULL;
	Vec dragPos;

	float marginX() { return mm2px(1.5f); }
	float marginY() { return mm2px(2.f); }

	// Write a line segment between two screen positions into the draw table
	void plotLine(Vec a, Vec b) {
		float mx = marginX(), my = marginY();
		float cw = box.size.x - 2.f * mx;
		float ch = box.size.y - 2.f * my;
		float t0 = clamp((a.x - mx) / cw, 0.f, 1.f);
		float v0 = clamp(1.f - (a.y - my) / ch, 0.f, 1.f);
		float t1 = clamp((b.x - mx) / cw, 0.f, 1.f);
		float v1 = clamp(1.f - (b.y - my) / ch, 0.f, 1.f);
		if (t0 > t1) {
			std::swap(t0, t1);
			std::swap(v0, v1);
		}
		int i0 = (int)std::round(t0 * (Saros::TABLE_N - 1));
		int i1 = (int)std::round(t1 * (Saros::TABLE_N - 1));
		for (int i = i0; i <= i1; i++) {
			float f = (i1 > i0) ? (float)(i - i0) / (i1 - i0) : 0.f;
			module->drawTable[i] = crossfade(v0, v1, f);
		}
		module->drawMode = true;
	}

	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			e.consume(this);
			dragPos = e.pos;
			plotLine(e.pos, e.pos);
		}
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		Vec newPos = dragPos.plus(e.mouseDelta.div(getAbsoluteZoom()));
		plotLine(dragPos, newPos);
		dragPos = newPos;
	}

	static std::string formatTime(float s) {
		if (s < 1.f)
			return string::f("%.0f ms", s * 1000.f);
		if (s < 60.f)
			return string::f("%.2f s", s);
		int m = (int)(s / 60.f);
		return string::f("%dm %02ds", m, (int)(s - 60.f * m));
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		// Browser preview: double peak with some flux, playhead mid-flight
		float shape = module ? module->dispShape : 8.f / 15.f;
		float warp = module ? module->dispWarp : 0.f;
		float flux = module ? module->dispFlux : 0.25f;
		float phase = module ? module->dispPhase : 0.4f;
		float duration = module ? module->dispDuration : 3.f;
		bool loop = module ? module->dispLoop : false;
		const float* table = (module && module->drawMode) ? module->drawTable : NULL;

		float w = box.size.x;
		float h = box.size.y;
		nvgScissor(args.vg, 0, 0, w, h);

		// Screen background
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, w, h);
		nvgFillColor(args.vg, nvgRGB(0x07, 0x03, 0x02));
		nvgFill(args.vg);

		float mx = marginX();
		float my = marginY();
		float cw = w - 2.f * mx;
		float ch = h - 2.f * my;

		// Grid: quarter verticals, mid horizontal
		nvgStrokeColor(args.vg, nvgRGBA(0x38, 0x21, 0x10, 0xb0));
		nvgStrokeWidth(args.vg, mm2px(0.15f));
		for (int i = 0; i <= 4; i++) {
			float x = mx + cw * i / 4.f;
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, x, my);
			nvgLineTo(args.vg, x, my + ch);
			nvgStroke(args.vg);
		}
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, mx, my + ch / 2.f);
		nvgLineTo(args.vg, mx + cw, my + ch / 2.f);
		nvgStroke(args.vg);

		// Curve
		static constexpr int N = 128;
		nvgBeginPath(args.vg);
		for (int i = 0; i <= N; i++) {
			float p = (float)i / N;
			float v = Saros::envValue(table, shape, warp, flux, p);
			float x = mx + p * cw;
			float y = my + (1.f - v) * ch;
			if (i == 0)
				nvgMoveTo(args.vg, x, y);
			else
				nvgLineTo(args.vg, x, y);
		}
		nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xc4, 0x64, 0xe6));
		nvgStrokeWidth(args.vg, mm2px(0.3f));
		nvgStroke(args.vg);
		// Fill under the curve
		nvgLineTo(args.vg, mx + cw, my + ch);
		nvgLineTo(args.vg, mx, my + ch);
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, nvgRGBA(0xff, 0xc4, 0x64, 0x28));
		nvgFill(args.vg);

		// Playhead
		float px = mx + clamp(phase, 0.f, 1.f) * cw;
		float pv = Saros::envValue(table, shape, warp, flux, phase);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, px, my);
		nvgLineTo(args.vg, px, my + ch);
		nvgStrokeColor(args.vg, nvgRGBA(0xff, 0x00, 0x00, 0xa0));
		nvgStrokeWidth(args.vg, mm2px(0.2f));
		nvgStroke(args.vg);
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, px, my + (1.f - pv) * ch, mm2px(0.7f));
		nvgFillColor(args.vg, nvgRGB(0xff, 0x00, 0x00));
		nvgFill(args.vg);

		// Readouts
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, mm2px(2.6f));
			nvgFillColor(args.vg, eclipse::LABEL_COLOR);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
			nvgText(args.vg, mx + mm2px(0.5f), h - mm2px(0.6f), formatTime(duration).c_str(), NULL);
			if (loop) {
				nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
				nvgText(args.vg, w - mx - mm2px(0.5f), h - mm2px(0.6f), "LOOP", NULL);
			}
			if (table) {
				nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
				nvgText(args.vg, w - mx - mm2px(0.5f), my + mm2px(0.3f), "DRAW", NULL);
			}
		}
		nvgResetScissor(args.vg);
	}
};

struct SarosWidget : ModuleWidget {
	static constexpr float XC = 30.48f, X1 = 13.5f, X2 = 47.46f;
	static constexpr float XL = 24.82f, XP = 36.14f; // LOOP and POLY, evenly between X1 and X2

	SarosWidget(Saros* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Saros.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		eclipse::addHeader(this, XC, "S A R O S");

		SarosDisplay* display = new SarosDisplay;
		display->module = module;
		display->box.pos = mm2px(Vec(3.5f, 18.5f));
		display->box.size = mm2px(Vec(53.96f, 40.5f));
		addChild(display);

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(X1, 69.f)), module, Saros::TIME_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(XC, 69.f)), module, Saros::SHAPE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(X2, 69.f)), module, Saros::WARP_PARAM));
		eclipse::addLabel(this, Vec(X1, 76.2f), "TIME", eclipse::LABEL_SIZE);
		eclipse::addLabel(this, Vec(XC, 76.2f), "SHAPE", eclipse::LABEL_SIZE);
		eclipse::addLabel(this, Vec(X2, 76.2f), "WARP", eclipse::LABEL_SIZE);

		// Second row is four wide: the two switches share the middle
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(X1, 84.f)), module, Saros::FLUX_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(XL, 84.f)), module, Saros::LOOP_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(XP, 84.f)), module, Saros::POLY_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(X2, 84.f)), module, Saros::TRIG_PARAM));
		eclipse::addLabel(this, Vec(X1, 91.2f), "FLUX", eclipse::LABEL_SIZE);
		eclipse::addLabel(this, Vec(XL, 91.2f), "LOOP", eclipse::LABEL_SIZE);
		eclipse::addLabel(this, Vec(XP, 91.2f), "POLY", eclipse::LABEL_SIZE);
		eclipse::addLabel(this, Vec(X2, 91.2f), "PUSH", eclipse::LABEL_SIZE);

		eclipse::addLabel(this, Vec(X1, 99.7f), "TIME", eclipse::LABEL_SIZE);
		eclipse::addLabel(this, Vec(XC, 99.7f), "SHAPE", eclipse::LABEL_SIZE);
		eclipse::addLabel(this, Vec(X2, 99.7f), "WARP", eclipse::LABEL_SIZE);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(X1, 105.5f)), module, Saros::TIME_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(XC, 105.5f)), module, Saros::SHAPE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(X2, 105.5f)), module, Saros::WARP_INPUT));

		eclipse::addLabel(this, Vec(9.5f, 112.5f), "TRIG", eclipse::LABEL_SIZE);
		eclipse::addLabel(this, Vec(23.5f, 112.5f), "FLUX", eclipse::LABEL_SIZE);
		eclipse::addLabel(this, Vec(37.46f, 112.5f), "EOC", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		eclipse::addLabel(this, Vec(51.46f, 112.5f), "OUT", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(9.5f, 118.3f)), module, Saros::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(23.5f, 118.3f)), module, Saros::FLUX_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(37.46f, 118.3f)), module, Saros::EOC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(51.46f, 118.3f)), module, Saros::OUT_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Saros* module = getModule<Saros>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Bipolar output (±5V)", "", &module->bipolar));
		menu->addChild(createBoolPtrMenuItem("Drawn shape (draw on screen)", "", &module->drawMode));
		menu->addChild(createMenuItem("Clear drawn shape", "", [module]() {
			module->initDrawTable();
		}));
	}
};

Model* modelSaros = createModel<Saros, SarosWidget>("Saros");
