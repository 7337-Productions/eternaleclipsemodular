// Litany Engine (OMN-90): looping sample player with a focused granular
// layer. Hybrid banks: the loops shipped in res/litany/ are the instrument
// out of the box (Gristleism / Buddha Machine ethos), and "Load sample
// folder..." swaps in the user's own bank per instance -- crossed with a
// Morphagene-style tape/microsound voice. Stereo companion to the Coronal
// Annihilator's external input.
#define DR_WAV_IMPLEMENTATION
#include <atomic>
#include <mutex>
#include <thread>
#include <osdialog.h>
#include "plugin.hpp"
#include "EclipseWidgets.hpp"
#include "LitanyEngine/dsp/SampleBank.hpp"
#include "LitanyEngine/dsp/GranularEngine.hpp"

struct LitanyEngine : Module {
	enum ParamId {
		LOOP_PARAM,
		SPEED_PARAM,
		GRAIN_PARAM,
		MORPH_PARAM,
		SCAN_PARAM,
		TEXTURE_PARAM,
		LEVEL_PARAM,
		ADVANCE_PARAM,
		SPEED_ATT_PARAM,
		GRAIN_ATT_PARAM,
		MORPH_ATT_PARAM,
		SCAN_ATT_PARAM,
		TEXTURE_ATT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,
		SPEED_INPUT,
		GRAIN_INPUT,
		MORPH_INPUT,
		SCAN_INPUT,
		TEXTURE_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTL_OUTPUT,
		OUTR_OUTPUT,
		EOC_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ADVANCE_LIGHT,
		LIGHTS_LEN
	};

	// Bank load states (mirrors the Coronal Annihilator's model loader)
	enum LoadState { LOAD_ONBOARD, LOAD_LOADING, LOAD_OK, LOAD_MISSING, LOAD_BAD };

	litany::GranularEngine engine;
	dsp::SchmittTrigger trigTrigger;
	dsp::SchmittTrigger buttonTrigger;
	dsp::PulseGenerator eocPulse;
	std::atomic<float> loopPhase{0.f};
	std::atomic<float> playSpeed{1.f};

	// ----- user bank ownership -----
	// activeBank is owned by the audio thread (NULL = onboard litany). The
	// loader thread parks a fully decoded bank in pendingBank; process()
	// swaps it in after killing every grain (grains hold buffer pointers),
	// and parks the old bank in retiredBank for the widget's step() to free.
	litany::UserBank* activeBank = NULL;
	std::atomic<litany::UserBank*> pendingBank{NULL};
	std::atomic<litany::UserBank*> retiredBank{NULL};
	std::atomic<bool> clearRequested{false};
	std::thread loader;
	std::atomic<int> loadState{LOAD_ONBOARD};
	std::string folderPath; // UI thread
	std::mutex nameMutex;
	std::vector<std::string> loopNames; // for the LOOP knob + display
	std::string bankInfo;               // context-menu summary

	LitanyEngine() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		litany::SampleBank& bank = litany::SampleBank::get();
		bank.retain();
		loopNames = bank.names();
		int count = (int)bank.count();
		configParam<LoopQuantity>(LOOP_PARAM, 0.f, std::max(count - 1, 0), 0.f, "Loop");
		paramQuantities[LOOP_PARAM]->snapEnabled = true;

		configParam(SPEED_PARAM, -2.f, 2.f, 1.f, "Speed", "x");
		configParam(GRAIN_PARAM, 0.f, 1.f, 0.f, "Grain size");
		configParam(MORPH_PARAM, 0.f, 1.f, 0.f, "Morph (grain overlap)");
		configParam(SCAN_PARAM, 0.f, 1.f, 0.f, "Scan (position offset)");
		configParam(TEXTURE_PARAM, 0.f, 1.f, 0.f, "Texture (spray + spread)");
		configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Level", "%", 0.f, 100.f);
		configButton(ADVANCE_PARAM, "Advance loop");
		configParam(SPEED_ATT_PARAM, -1.f, 1.f, 0.f, "Speed CV amount");
		configParam(GRAIN_ATT_PARAM, -1.f, 1.f, 0.f, "Grain CV amount");
		configParam(MORPH_ATT_PARAM, -1.f, 1.f, 0.f, "Morph CV amount");
		configParam(SCAN_ATT_PARAM, -1.f, 1.f, 0.f, "Scan CV amount");
		configParam(TEXTURE_ATT_PARAM, -1.f, 1.f, 0.f, "Texture CV amount");

		configInput(TRIG_INPUT, "Advance loop trigger");
		configInput(SPEED_INPUT, "Speed CV");
		configInput(GRAIN_INPUT, "Grain CV");
		configInput(MORPH_INPUT, "Morph CV");
		configInput(SCAN_INPUT, "Scan CV");
		configInput(TEXTURE_INPUT, "Texture CV");
		configOutput(OUTL_OUTPUT, "Left audio");
		configOutput(OUTR_OUTPUT, "Right audio");
		configOutput(EOC_OUTPUT, "End of loop");
	}

	// Unipolar knob + CV (10 V = full range) through its attenuverter
	float cvParam(int paramId, int inputId, int attId, float lo = 0.f, float hi = 1.f) {
		float v = params[paramId].getValue()
			+ inputs[inputId].getVoltage() / 10.f * params[attId].getValue() * (hi - lo);
		if (!std::isfinite(v))
			v = lo;
		return clamp(v, lo, hi);
	}

	int loopCount() {
		return activeBank ? (int)activeBank->count() : (int)litany::SampleBank::get().count();
	}

	// The LOOP knob names the current bank's loops (UI thread)
	struct LoopQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			LitanyEngine* m = dynamic_cast<LitanyEngine*>(module);
			if (!m)
				return "";
			int i = (int)std::round(getValue());
			std::lock_guard<std::mutex> lock(m->nameMutex);
			if (i < 0 || i >= (int)m->loopNames.size())
				return "(no loops)";
			return m->loopNames[i];
		}
	};

	std::string loopName(int i) {
		std::lock_guard<std::mutex> lock(nameMutex);
		if (i < 0 || i >= (int)loopNames.size())
			return "";
		return loopNames[i];
	}

	std::string getBankInfo() {
		std::lock_guard<std::mutex> lock(nameMutex);
		return bankInfo;
	}

	void setNames(const std::vector<std::string>& names, const std::string& info) {
		std::lock_guard<std::mutex> lock(nameMutex);
		loopNames = names;
		bankInfo = info;
	}

	// ----- user bank loading (UI / patch-load thread) -----
	void requestLoad(const std::string& dir) {
		if (loader.joinable())
			loader.join();
		folderPath = dir;
		if (dir.empty()) {
			delete pendingBank.exchange(NULL);
			clearRequested = true;
			loadState = LOAD_ONBOARD;
			setNames(litany::SampleBank::get().names(), "");
			return;
		}
		loadState = LOAD_LOADING;
		loader = std::thread([this, dir]() {
			std::string error;
			litany::UserBank* b = litany::UserBank::load(dir, error);
			if (!b) {
				loadState = (error == "missing") ? LOAD_MISSING : LOAD_BAD;
				return;
			}
			std::string info = string::f("%d loop%s Â· %.0f MB",
				(int)b->count(), b->count() == 1 ? "" : "s", b->totalBytes / 1048576.0);
			if (b->skipped > 0)
				info += string::f(" Â· %d file%s skipped by guardrails", b->skipped, b->skipped == 1 ? "" : "s");
			setNames(b->names(), info);
			delete pendingBank.exchange(b);
			loadState = LOAD_OK;
		});
	}

	int currentLoop() {
		return (int)std::round(params[LOOP_PARAM].getValue());
	}

	void advanceLoop() {
		int count = loopCount();
		if (count > 0)
			params[LOOP_PARAM].setValue((currentLoop() + 1) % count);
	}

	~LitanyEngine() override {
		if (loader.joinable())
			loader.join();
		delete pendingBank.exchange(NULL);
		delete retiredBank.exchange(NULL);
		delete activeBank;
		litany::SampleBank::get().release();
	}

	void retire(litany::UserBank* b) {
		if (!b)
			return;
		litany::UserBank* expected = NULL;
		if (!retiredBank.compare_exchange_strong(expected, b))
			delete b; // slot occupied (no widget to drain it): free inline
	}

	void applyBankCount() {
		int count = loopCount();
		paramQuantities[LOOP_PARAM]->maxValue = std::max(count - 1, 0);
		if (currentLoop() >= count)
			params[LOOP_PARAM].setValue(0.f);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "folderPath", json_string(folderPath.c_str()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* pathJ = json_object_get(rootJ, "folderPath");
		if (pathJ) {
			std::string dir = json_string_value(pathJ);
			if (!dir.empty())
				requestLoad(dir);
		}
	}

	void process(const ProcessArgs& args) override {
		// ===== Bank hand-off =====
		// Kill every grain before the old bank leaves: grains hold raw
		// pointers into its buffers, and the widget will free it.
		if (clearRequested.exchange(false)) {
			engine.reset();
			retire(activeBank);
			activeBank = NULL;
			applyBankCount();
		}
		if (litany::UserBank* b = pendingBank.exchange(NULL)) {
			engine.reset();
			retire(activeBank);
			activeBank = b;
			applyBankCount();
		}

		// Loop advance: button, trigger input, or the display (UI thread)
		bool adv = false;
		adv |= buttonTrigger.process(params[ADVANCE_PARAM].getValue() > 0.5f);
		adv |= trigTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		if (adv)
			advanceLoop();

		litany::LoopView view;
		const litany::Loop* lp = activeBank
			? activeBank->loop(currentLoop())
			: litany::SampleBank::get().loop(currentLoop());
		if (lp && lp->ready.load(std::memory_order_acquire) && lp->frames > 1) {
			view.l = lp->l.data();
			view.r = lp->r.data();
			view.frames = lp->frames;
			view.fileRate = lp->fileRate;
		}

		litany::GranularParams p;
		p.speed = cvParam(SPEED_PARAM, SPEED_INPUT, SPEED_ATT_PARAM, -2.f, 2.f);
		p.grain = cvParam(GRAIN_PARAM, GRAIN_INPUT, GRAIN_ATT_PARAM);
		p.morph = cvParam(MORPH_PARAM, MORPH_INPUT, MORPH_ATT_PARAM);
		p.scan = cvParam(SCAN_PARAM, SCAN_INPUT, SCAN_ATT_PARAM);
		p.texture = cvParam(TEXTURE_PARAM, TEXTURE_INPUT, TEXTURE_ATT_PARAM);

		litany::GranularEngine::Out out = engine.process(view, p, args.sampleRate);
		loopPhase.store(engine.phase01(), std::memory_order_relaxed);
		playSpeed.store(p.speed, std::memory_order_relaxed);
		if (out.eoc)
			eocPulse.trigger(1e-3f);

		float level = params[LEVEL_PARAM].getValue();
		float outL = out.l * 5.f * level;
		float outR = out.r * 5.f * level;
		if (outputs[OUTL_OUTPUT].isConnected() && !outputs[OUTR_OUTPUT].isConnected())
			outputs[OUTL_OUTPUT].setVoltage(0.5f * (outL + outR));
		else
			outputs[OUTL_OUTPUT].setVoltage(outL);
		outputs[OUTR_OUTPUT].setVoltage(outR);
		outputs[EOC_OUTPUT].setVoltage(eocPulse.process(args.sampleTime) ? 10.f : 0.f);

		lights[ADVANCE_LIGHT].setBrightnessSmooth(out.eoc || adv ? 1.f : 0.f, args.sampleTime * 0.5f);
	}

	void onReset() override {
		engine.reset();
	}
};

// ----- Loop name display: current loop + playhead hairline; click advances -----
struct LoopDisplay : TransparentWidget {
	LitanyEngine* module = NULL;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		NVGcolor color = eclipse::ACCENT_COLOR;
		float phase = 0.f;
		// Browser preview (no module): show the first loop of the onboard bank
		const litany::Loop* first = litany::SampleBank::get().loop(0);
		std::string text = first ? first->name : "NO LOOPS";
		if (module) {
			int state = module->loadState.load();
			std::string name = module->loopName(module->currentLoop());
			if (state == LitanyEngine::LOAD_LOADING) {
				text = "LOADING FOLDER";
				color = eclipse::LABEL_COLOR;
			}
			else if (state == LitanyEngine::LOAD_MISSING) {
				text = "FOLDER MISSING";
				color = eclipse::LABEL_COLOR;
			}
			else if (state == LitanyEngine::LOAD_BAD) {
				text = "NO WAVS IN FOLDER";
				color = eclipse::LABEL_COLOR;
			}
			else if (name.empty()) {
				text = "NO LOOPS";
				color = eclipse::LABEL_COLOR;
			}
			else {
				text = name;
				phase = module->loopPhase.load(std::memory_order_relaxed);
			}
		}

		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, eclipse::LABEL_SIZE);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, color);
		// Ellipsize to the bezel width
		float maxW = box.size.x - mm2px(2.f);
		float bounds[4];
		nvgTextBounds(args.vg, 0, 0, text.c_str(), NULL, bounds);
		if (bounds[2] - bounds[0] > maxW) {
			while (text.size() > 2) {
				text.pop_back();
				std::string t = text + "\xE2\x80\xA6";
				nvgTextBounds(args.vg, 0, 0, t.c_str(), NULL, bounds);
				if (bounds[2] - bounds[0] <= maxW) {
					text = t;
					break;
				}
			}
		}
		nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f, text.c_str(), NULL);

		// Playhead hairline along the bottom edge
		if (phase > 0.f) {
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, mm2px(1.f), box.size.y - mm2px(0.7f));
			nvgLineTo(args.vg, mm2px(1.f) + (box.size.x - mm2px(2.f)) * phase, box.size.y - mm2px(0.7f));
			nvgStrokeColor(args.vg, eclipse::DIM_COLOR);
			nvgStrokeWidth(args.vg, mm2px(0.35f));
			nvgStroke(args.vg);
		}
	}

	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			e.consume(this);
			module->advanceLoop();
		}
	}
};

// ----- Tape-loop lemniscate: a splice marker orbits an infinity path that
// threads the SPEED knob (the capstan). Position = loop phase, trail length and
// direction = playback speed; the splice ducks under the head as it passes. -----
struct LemniscateDisplay : TransparentWidget {
	LitanyEngine* module = NULL;
	static constexpr float A_MM = 15.f;        // Bernoulli scale: half-width = A*sqrt2, half-height = A/2 * Y_STRETCH
	static constexpr float Y_STRETCH = 1.f;
	static constexpr float HIDE_MM = 5.f;      // marker hidden under the TURN head at the crossing
	static const NVGcolor SPLICE_COLOR;        // Abyssal Harmonics red, matches the sigil

	Vec point(float t) {
		float s = std::sin(t), c = std::cos(t);
		float d = 1.f + s * s;
		float k = A_MM * 1.41421356f;
		return mm2px(Vec(k * c / d, Y_STRETCH * k * s * c / d)).plus(box.size.div(2.f));
	}

	// Phase 0 = the splice at the head, heading into the left lobe
	static float toT(float phase) {
		return 0.5f * (float)M_PI + 2.f * (float)M_PI * phase;
	}

	void draw(const DrawArgs& args) override {
		// Static tape path, drawn in layer 0 so the knob sits on top of it
		const int N = 180;
		nvgBeginPath(args.vg);
		for (int i = 0; i <= N; i++) {
			Vec p = point(2.f * (float)M_PI * i / N);
			if (i == 0)
				nvgMoveTo(args.vg, p.x, p.y);
			else
				nvgLineTo(args.vg, p.x, p.y);
		}
		nvgStrokeColor(args.vg, nvgTransRGBA(eclipse::ACCENT_COLOR, 200));
		nvgStrokeWidth(args.vg, mm2px(0.55f));
		nvgStroke(args.vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		float phase = 0.12f, speed = 1.f;
		if (module) {
			phase = module->loopPhase.load(std::memory_order_relaxed);
			speed = module->playSpeed.load(std::memory_order_relaxed);
		}
		const Vec c = box.size.div(2.f);
		const float hideR = mm2px(HIDE_MM);
		const float dir = speed < 0.f ? -1.f : 1.f;
		const float trail = 0.012f + 0.035f * std::fmin(std::fabs(speed), 2.f);

		// Motion trail: fades out behind the splice, longer at higher speed
		const int N = 20;
		Vec prev = point(toT(phase));
		for (int k = 1; k <= N; k++) {
			Vec p = point(toT(phase - dir * trail * k / N));
			float a = 1.f - (float)k / N;
			if (prev.minus(c).norm() > hideR && p.minus(c).norm() > hideR) {
				nvgBeginPath(args.vg);
				nvgMoveTo(args.vg, prev.x, prev.y);
				nvgLineTo(args.vg, p.x, p.y);
				nvgStrokeColor(args.vg, nvgTransRGBA(SPLICE_COLOR, (unsigned char)(230 * a)));
				nvgStrokeWidth(args.vg, mm2px(0.3f + 0.7f * a));
				nvgStroke(args.vg);
			}
			prev = p;
		}

		// The splice
		Vec m = point(toT(phase));
		if (m.minus(c).norm() > hideR) {
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, m.x, m.y, mm2px(1.8f));
			nvgFillColor(args.vg, nvgTransRGBA(SPLICE_COLOR, 80));
			nvgFill(args.vg);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, m.x, m.y, mm2px(0.9f));
			nvgFillColor(args.vg, SPLICE_COLOR);
			nvgFill(args.vg);
		}
	}
};

const NVGcolor LemniscateDisplay::SPLICE_COLOR = nvgRGB(0xff, 0x00, 0x00);

struct LitanyEngineWidget : ModuleWidget {
	// 20 HP. Left column (center LX) holds the voice; the right column is the
	// full-height patch bay (center BX) of jack + attenuverter pairs.
	static constexpr float LX = 36.f, BX = 85.2f;
	static constexpr float JACK_DX = -5.f, ATT_DX = 5.f;

	void addLabel(Vec mmPos, const std::string& text, float fontSize = eclipse::LABEL_SIZE,
	              NVGcolor color = eclipse::LABEL_COLOR) {
		eclipse::addLabel(this, mmPos, text, fontSize, color);
	}

	void addCvPair(float y, const char* label, int inputId, int attParamId, LitanyEngine* module) {
		addLabel(Vec(BX, y - 5.7f), label);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(BX + JACK_DX, y)), module, inputId));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(BX + ATT_DX, y)), module, attParamId));
	}

	LitanyEngineWidget(LitanyEngine* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/LitanyEngine.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		eclipse::addHeader(this, 50.8f, "L I T A N Y   E N G I N E");

		// ===== Left column =====
		LoopDisplay* display = new LoopDisplay;
		display->module = module;
		display->box.pos = mm2px(Vec(6.f, 15.5f));
		display->box.size = mm2px(Vec(60.f, 8.f));
		addChild(display);

		// Varispeed: the huge knob inside the sigil ring (ring in the panel SVG)
		addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(LX, 60.5f)), module, LitanyEngine::SPEED_PARAM));
		addLabel(Vec(LX, 76.6f), "SPEED", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);

		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(LX, 33.f)), module, LitanyEngine::LOOP_PARAM));
		addLabel(Vec(LX, 43.f), "LOOP");

		// Tape-loop lemniscate, TURN at the head
		LemniscateDisplay* tape = new LemniscateDisplay;
		tape->module = module;
		tape->box.pos = mm2px(Vec(LX - 23.f, 85.f - 9.f));
		tape->box.size = mm2px(Vec(46.f, 18.f));
		addChild(tape);
		addParam(createLightParamCentered<VCVLightBezel<RedLight>>(mm2px(Vec(LX, 85.f)), module,
			LitanyEngine::ADVANCE_PARAM, LitanyEngine::ADVANCE_LIGHT));
		addLabel(Vec(LX, 92.f), "TURN");

		// Grain row
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(14.f, 100.f)), module, LitanyEngine::GRAIN_PARAM));
		addLabel(Vec(14.f, 107.2f), "GRAIN");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(LX, 100.f)), module, LitanyEngine::MORPH_PARAM));
		addLabel(Vec(LX, 107.2f), "MORPH");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(58.f, 100.f)), module, LitanyEngine::SCAN_PARAM));
		addLabel(Vec(58.f, 107.2f), "SCAN");

		// Bottom row
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.f, 115.f)), module, LitanyEngine::TEXTURE_PARAM));
		addLabel(Vec(25.f, 122.2f), "TEXTURE");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(47.f, 115.f)), module, LitanyEngine::LEVEL_PARAM));
		addLabel(Vec(47.f, 122.2f), "LEVEL");

		// ===== Right column: patch bay =====
		addCvPair(24.f, "SPEED", LitanyEngine::SPEED_INPUT, LitanyEngine::SPEED_ATT_PARAM, module);
		addCvPair(38.f, "GRAIN", LitanyEngine::GRAIN_INPUT, LitanyEngine::GRAIN_ATT_PARAM, module);
		addCvPair(52.f, "MORPH", LitanyEngine::MORPH_INPUT, LitanyEngine::MORPH_ATT_PARAM, module);
		addCvPair(66.f, "SCAN", LitanyEngine::SCAN_INPUT, LitanyEngine::SCAN_ATT_PARAM, module);
		addCvPair(80.f, "TEXTURE", LitanyEngine::TEXTURE_INPUT, LitanyEngine::TEXTURE_ATT_PARAM, module);

		addLabel(Vec(BX - 6.f, 90.3f), "TRIG");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(BX - 6.f, 96.f)), module, LitanyEngine::TRIG_INPUT));
		addLabel(Vec(BX + 6.f, 90.3f), "EOC");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BX + 6.f, 96.f)), module, LitanyEngine::EOC_OUTPUT));

		addLabel(Vec(BX - 6.f, 106.3f), "OUT L", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BX - 6.f, 112.f)), module, LitanyEngine::OUTL_OUTPUT));
		addLabel(Vec(BX + 6.f, 106.3f), "OUT R", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BX + 6.f, 112.f)), module, LitanyEngine::OUTR_OUTPUT));
	}

	void step() override {
		ModuleWidget::step();
		LitanyEngine* m = getModule<LitanyEngine>();
		if (m)
			delete m->retiredBank.exchange(NULL);
	}

	static void openFolderDialog(LitanyEngine* module) {
		std::string dir = module->folderPath.empty() ? asset::user("") : module->folderPath;
		char* pathC = osdialog_file(OSDIALOG_OPEN_DIR, dir.c_str(), NULL, NULL);
		if (!pathC)
			return;
		std::string path = pathC;
		std::free(pathC);
		module->requestLoad(path);
	}

	void appendContextMenu(Menu* menu) override {
		LitanyEngine* module = getModule<LitanyEngine>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Sample bank"));
		menu->addChild(createMenuItem("Load sample folder (.wav)...", "", [module]() {
			openFolderDialog(module);
		}));
		menu->addChild(createMenuItem("Restore onboard litany", "", [module]() {
			module->requestLoad("");
		}, module->folderPath.empty()));
		std::string info = module->getBankInfo();
		if (!info.empty())
			menu->addChild(createMenuLabel(info));
		menu->addChild(createMenuLabel(string::f("Folder limits: %d files, %.0f s/file, %d MB",
			litany::MAX_FILES, litany::MAX_SECONDS_PER_FILE, (int)(litany::MAX_TOTAL_BYTES >> 20))));
	}
};

Model* modelLitanyEngine = createModel<LitanyEngine, LitanyEngineWidget>("LitanyEngine");
