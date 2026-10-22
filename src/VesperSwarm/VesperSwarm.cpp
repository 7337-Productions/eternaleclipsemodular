#include <atomic>
#include "plugin.hpp"
#include "EclipseWidgets.hpp"
#include "VesperSwarm/dsp/Choir.hpp"

// Vesper Swarm (OMN-106): an eight-voice formant choir in the shape of the
// Dewanatron Swarmatron. One PITCH, one SWARM: at rest the eight singers hold
// the same note a few cents apart; SWARM fans them out in equal steps to a
// two-octave cluster, or (from the menu) into whole-number multiples of the
// pitch like the original's oscillators. Each singer is a band-limited glottal
// pulse through a five-formant soprano filter bank with her own vocal tract,
// vibrato and place in the stereo field; VOWEL morphs A E I O U and BREATH
// adds air. The mix passes a 4-pole lowpass and a gate-driven ADSR that
// drones while GATE is unpatched. Every knob has a CV jack. The singers stand
// at the points of the evening star, Venus's eight-pointed sign; a click on a
// point silences or wakes one.

using vesper::VOICES;

// Abyssal Harmonics red for the singers' lights: pure #ff0000, the colour of
// the Syzygy beads and the Coronal sigil, not Rack's stock red.
struct AbyssalLight : GrayModuleLightWidget {
	AbyssalLight() {
		addBaseColor(nvgRGB(0xff, 0x00, 0x00));
	}
};

struct VesperSwarm : Module {
	enum ParamId {
		PITCH_PARAM,
		GLIDE_PARAM,
		SWARM_PARAM,
		VOWEL_PARAM,
		BREATH_PARAM,
		CUTOFF_PARAM,
		RES_PARAM,
		ATTACK_PARAM,
		DECAY_PARAM,
		SUSTAIN_PARAM,
		RELEASE_PARAM,
		ENV_DEST_PARAM,
		ENUMS(VOICE_PARAMS, VOICES),
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		GATE_INPUT,
		GLIDE_INPUT,
		SWARM_INPUT,
		VOWEL_INPUT,
		BREATH_INPUT,
		CUTOFF_INPUT,
		RES_INPUT,
		ATTACK_INPUT,
		DECAY_INPUT,
		SUSTAIN_INPUT,
		RELEASE_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTL_OUTPUT,
		OUTR_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(VOICE_LIGHTS, VOICES),
		LIGHTS_LEN
	};

	vesper::Choir choir;
	vesper::ChoirParams prm;
	dsp::ClockDivider divider;
	dsp::SchmittTrigger gateTrig;
	float glided = 0.f;
	bool glideInit = false;

	// UI -> audio
	std::atomic<int> law{vesper::LAW_EQUAL};

	// audio -> UI
	std::atomic<float> dispVowel{0.f};
	std::atomic<uint8_t> dispMask{0xFF};
	std::atomic<float> dispLevel[VOICES];

	struct VowelQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			char buf[16];
			vesper::vowelName(getValue(), buf, sizeof buf);
			return buf;
		}
	};

	VesperSwarm() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PITCH_PARAM, -24.f, 24.f, 0.f, "Pitch", " semitones");
		configParam(GLIDE_PARAM, 0.f, 1.f, 0.f, "Glide", " ms", 400.f, 5.f);
		configParam(SWARM_PARAM, 0.f, 1.f, 0.15f, "Swarm", "%", 0.f, 100.f);
		configParam<VowelQuantity>(VOWEL_PARAM, 0.f, 4.f, 0.f, "Vowel");
		configParam(BREATH_PARAM, 0.f, 1.f, 0.2f, "Breath", "%", 0.f, 100.f);
		configParam(CUTOFF_PARAM, 0.f, 1.f, 1.f, "Cutoff", " Hz", 1024.f, 20.f);
		configParam(RES_PARAM, 0.f, 1.f, 0.f, "Resonance", "%", 0.f, 100.f);
		configParam(ATTACK_PARAM, 0.f, 1.f, 0.455f, "Attack", " ms", 60000.f, 1.f);
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay", " ms", 10000.f, 1.f);
		configParam(SUSTAIN_PARAM, 0.f, 1.f, 1.f, "Sustain", "%", 0.f, 100.f);
		configParam(RELEASE_PARAM, 0.f, 1.f, 0.65f, "Release", " ms", 10000.f, 1.f);
		configSwitch(ENV_DEST_PARAM, 0.f, 1.f, 0.f, "Envelope destination",
			{"VCA: the envelope shapes the level", "VCF: the envelope opens the filter, the level stays up"});
		for (int i = 0; i < VOICES; i++)
			configSwitch(VOICE_PARAMS + i, 0.f, 1.f, 1.f, string::f("Singer %d", i + 1), {"Silent", "Singing"});

		configInput(VOCT_INPUT, "V/oct");
		configInput(GATE_INPUT, "Gate (unpatched: the choir drones)");
		configInput(GLIDE_INPUT, "Glide CV (10 V = full range)");
		configInput(SWARM_INPUT, "Swarm CV (10 V = full range)");
		configInput(VOWEL_INPUT, "Vowel CV (10 V = A to U)");
		configInput(BREATH_INPUT, "Breath CV (10 V = full range)");
		configInput(CUTOFF_INPUT, "Cutoff CV (1 V/oct)");
		configInput(RES_INPUT, "Resonance CV (10 V = full range)");
		configInput(ATTACK_INPUT, "Attack CV (10 V = full range)");
		configInput(DECAY_INPUT, "Decay CV (10 V = full range)");
		configInput(SUSTAIN_INPUT, "Sustain CV (10 V = full range)");
		configInput(RELEASE_INPUT, "Release CV (10 V = full range)");
		configOutput(OUTL_OUTPUT, "Left");
		configOutput(OUTR_OUTPUT, "Right");

		divider.setDivision(vesper::BLOCK);
		for (int i = 0; i < VOICES; i++)
			dispLevel[i].store(0.f);
	}

	void onReset() override {
		glideInit = false;
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "law", json_integer(law.load()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* lawJ = json_object_get(rootJ, "law");
		if (lawJ)
			law.store(clamp((int)json_integer_value(lawJ), 0, 1));
	}

	// Knob plus CV: 10 V sweeps the knob's whole range, bipolar, clamped
	float knobCv(int paramId, int inputId, float lo, float hi) {
		float v = params[paramId].getValue();
		if (inputs[inputId].isConnected())
			v += vesper::clampf(inputs[inputId].getVoltage(), -10.f, 10.f) * 0.1f * (hi - lo);
		return vesper::clampf(v, lo, hi);
	}

	// Envelope knobs: attack 1 ms .. 60 s, decay and release 1 ms .. 10 s
	static float attackSeconds(float x) {
		return 0.001f * std::pow(60000.f, x);
	}
	static float envSeconds(float x) {
		return 0.001f * std::pow(10000.f, x);
	}

	void block(const ProcessArgs& args) {
		float sr = args.sampleRate;
		float blockDt = (float)vesper::BLOCK / sr;

		bool gate = true;
		if (inputs[GATE_INPUT].isConnected()) {
			gateTrig.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f);
			gate = gateTrig.isHigh();
		}

		// Pitch with glide: 5 ms .. 2 s exponential approach, off at zero
		float target = vesper::clampf(inputs[VOCT_INPUT].getVoltage(), -10.f, 10.f) + params[PITCH_PARAM].getValue() / 12.f;
		float glide = knobCv(GLIDE_PARAM, GLIDE_INPUT, 0.f, 1.f);
		if (!glideInit || glide <= 0.f) {
			glided = target;
			glideInit = true;
		}
		else {
			float tau = 0.005f * std::pow(400.f, glide);
			glided += (target - glided) * (1.f - std::exp(-blockDt / tau));
		}

		prm.pitchVolts = glided;
		prm.swarm = knobCv(SWARM_PARAM, SWARM_INPUT, 0.f, 1.f);
		prm.law = law.load(std::memory_order_relaxed);
		prm.vowel = knobCv(VOWEL_PARAM, VOWEL_INPUT, 0.f, 4.f);
		prm.breath = knobCv(BREATH_PARAM, BREATH_INPUT, 0.f, 1.f);
		uint8_t mask = 0;
		for (int i = 0; i < VOICES; i++)
			if (params[VOICE_PARAMS + i].getValue() > 0.5f)
				mask |= (uint8_t)(1u << i);
		prm.voiceMask = mask;
		prm.gate = gate;

		float cutoffExp = params[CUTOFF_PARAM].getValue() * 10.f
			+ vesper::clampf(inputs[CUTOFF_INPUT].getVoltage(), -10.f, 10.f);
		prm.cutoff = vesper::clampf(20.f * std::exp2(cutoffExp), 20.f, std::min(20000.f, sr * 0.45f));
		prm.res = knobCv(RES_PARAM, RES_INPUT, 0.f, 1.f);
		prm.attack = attackSeconds(knobCv(ATTACK_PARAM, ATTACK_INPUT, 0.f, 1.f));
		prm.decay = envSeconds(knobCv(DECAY_PARAM, DECAY_INPUT, 0.f, 1.f));
		prm.sustain = knobCv(SUSTAIN_PARAM, SUSTAIN_INPUT, 0.f, 1.f);
		prm.release = envSeconds(knobCv(RELEASE_PARAM, RELEASE_INPUT, 0.f, 1.f));
		prm.envToFilter = params[ENV_DEST_PARAM].getValue() > 0.5f;

		choir.setBlock(prm, sr);

		// With the envelope on the filter the level is always up
		float amp = prm.envToFilter ? 1.f : choir.env.level;
		dispVowel.store(prm.vowel, std::memory_order_relaxed);
		dispMask.store(mask, std::memory_order_relaxed);
		for (int i = 0; i < VOICES; i++) {
			bool on = (mask >> i) & 1u;
			float lvl = on ? vesper::clampf(choir.voicePeak[i] * amp * 2.f, 0.f, 1.f) : 0.f;
			dispLevel[i].store(lvl, std::memory_order_relaxed);
			lights[VOICE_LIGHTS + i].setBrightness(on ? (amp > 0.01f ? 1.f : 0.25f) : 0.f);
		}
	}

	void process(const ProcessArgs& args) override {
		if (divider.process())
			block(args);
		float l, r;
		choir.process(l, r);
		outputs[OUTL_OUTPUT].setVoltage(5.f * l);
		outputs[OUTR_OUTPUT].setVoltage(5.f * r);
	}
};

// Panel geometry (mm). The star of Ishtar is drawn in the SVG; the singers'
// toggles sit just beyond its points and the display breathes in its heart.
static constexpr float STAR_X = 4.f, STAR_Y = 16.f, STAR_W = 100.f, STAR_H = 106.5f;
static constexpr float STAR_CX = 54.f, STAR_CY = 69.25f, STAR_R = 40.f;
static constexpr float BUTTON_R = 46.f; // in the ring between the SVG's circles at 41 and 51

static Vec starPoint(int i, float r) {
	float a = -0.5f * (float)M_PI + (float)i * 0.25f * (float)M_PI;
	return Vec(STAR_CX + r * std::cos(a), STAR_CY + r * std::sin(a));
}

// Overlay on the star: rays from the centre toward each singing voice, a
// glow that breathes with the envelope, and the vowel in the middle.
struct StarDisplay : TransparentWidget {
	VesperSwarm* module = NULL;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		float vowel = 0.5f;
		uint8_t mask = 0xDB; // preview: singers 3 and 6 silent
		float level[VOICES];
		for (int i = 0; i < VOICES; i++)
			level[i] = 0.55f;
		if (module) {
			vowel = module->dispVowel.load(std::memory_order_relaxed);
			mask = module->dispMask.load(std::memory_order_relaxed);
			for (int i = 0; i < VOICES; i++)
				level[i] = module->dispLevel[i].load(std::memory_order_relaxed);
		}

		nvgSave(args.vg);
		nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);
		Vec c = mm2px(Vec(STAR_CX - STAR_X, STAR_CY - STAR_Y));

		// Rays along the points of the singing voices
		nvgStrokeWidth(args.vg, mm2px(0.8f));
		nvgLineCap(args.vg, NVG_ROUND);
		for (int i = 0; i < VOICES; i++) {
			if (!((mask >> i) & 1u))
				continue;
			Vec p0 = mm2px(starPoint(i, 13.f).minus(Vec(STAR_X, STAR_Y)));
			Vec p1 = mm2px(starPoint(i, STAR_R - 3.f).minus(Vec(STAR_X, STAR_Y)));
			nvgStrokeColor(args.vg, nvgRGBAf(1.f, 0.77f, 0.39f, 0.08f + 0.7f * level[i]));
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, p0.x, p0.y);
			nvgLineTo(args.vg, p1.x, p1.y);
			nvgStroke(args.vg);
		}

		// The vowel being sung
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font) {
			char buf[16];
			vesper::vowelName(vowel, buf, sizeof buf);
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, eclipse::TITLE_SIZE);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(args.vg, eclipse::LABEL_COLOR);
			nvgText(args.vg, c.x, c.y, buf, NULL);
		}

		nvgResetScissor(args.vg);
		nvgRestore(args.vg);
	}
};

struct VesperSwarmWidget : ModuleWidget {
	// 40 HP. The star on the left, controls in the middle, patch bay right.
	static constexpr float BAY_C0 = 166.f, BAY_C1 = 178.6f, BAY_C2 = 191.2f;
	static constexpr float BAY_ROW0 = 27.f, BAY_DY = 19.f;

	void addLabel(Vec mmPos, const std::string& text, float fontSize = eclipse::LABEL_SIZE,
	              NVGcolor color = eclipse::LABEL_COLOR) {
		eclipse::addLabel(this, mmPos, text, fontSize, color);
	}

	void addBayInput(float x, int row, const char* label, int inputId, VesperSwarm* module) {
		float y = BAY_ROW0 + BAY_DY * row;
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, inputId));
		addLabel(Vec(x, y - 5.7f), label);
	}

	VesperSwarmWidget(VesperSwarm* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VesperSwarm.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		eclipse::addHeader(this, 101.6f, "V E S P E R   S W A R M");

		// ===== The star =====
		StarDisplay* display = new StarDisplay;
		display->module = module;
		display->box.pos = mm2px(Vec(STAR_X, STAR_Y));
		display->box.size = mm2px(Vec(STAR_W, STAR_H));
		addChild(display);
		for (int i = 0; i < VOICES; i++)
			addParam(createLightParamCentered<VCVLightBezelLatch<AbyssalLight>>(
				mm2px(starPoint(i, BUTTON_R)), module, VesperSwarm::VOICE_PARAMS + i, VesperSwarm::VOICE_LIGHTS + i));

		// ===== Controls =====
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(120.5f, 31.f)), module, VesperSwarm::PITCH_PARAM));
		addLabel(Vec(120.5f, 41.f), "PITCH");
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(142.f, 31.f)), module, VesperSwarm::GLIDE_PARAM));
		addLabel(Vec(142.f, 37.3f), "GLIDE");

		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(120.5f, 57.f)), module, VesperSwarm::SWARM_PARAM));
		addLabel(Vec(120.5f, 67.f), "SWARM");
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(141.5f, 57.f)), module, VesperSwarm::VOWEL_PARAM));
		addLabel(Vec(141.5f, 67.f), "VOWEL");

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(118.f, 84.f)), module, VesperSwarm::BREATH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(131.f, 84.f)), module, VesperSwarm::CUTOFF_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(144.f, 84.f)), module, VesperSwarm::RES_PARAM));
		addLabel(Vec(118.f, 91.2f), "BREATH");
		addLabel(Vec(131.f, 91.2f), "CUTOFF");
		addLabel(Vec(144.f, 91.2f), "RES");

		static const float EX[4] = {115.5f, 125.8f, 136.2f, 146.5f};
		static const int EP[4] = {VesperSwarm::ATTACK_PARAM, VesperSwarm::DECAY_PARAM,
		                          VesperSwarm::SUSTAIN_PARAM, VesperSwarm::RELEASE_PARAM};
		static const char* EL[4] = {"ATK", "DEC", "SUS", "REL"};
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(EX[i], 104.5f)), module, EP[i]));
			addLabel(Vec(EX[i], 110.7f), EL[i]);
		}
		addParam(createParamCentered<eclipse::CKSSHorizontal>(mm2px(Vec(131.f, 117.8f)), module, VesperSwarm::ENV_DEST_PARAM));
		addLabel(Vec(119.8f, 117.8f), "VCA", eclipse::FINE_SIZE);
		addLabel(Vec(142.2f, 117.8f), "VCF", eclipse::FINE_SIZE);

		// ===== Bay =====
		addBayInput(BAY_C0, 0, "V/OCT", VesperSwarm::VOCT_INPUT, module);
		addBayInput(BAY_C1, 0, "GATE", VesperSwarm::GATE_INPUT, module);
		addBayInput(BAY_C2, 0, "GLIDE", VesperSwarm::GLIDE_INPUT, module);
		addBayInput(BAY_C0, 1, "SWARM", VesperSwarm::SWARM_INPUT, module);
		addBayInput(BAY_C1, 1, "VOWEL", VesperSwarm::VOWEL_INPUT, module);
		addBayInput(BAY_C2, 1, "BREATH", VesperSwarm::BREATH_INPUT, module);
		addBayInput(BAY_C0, 2, "CUTOFF", VesperSwarm::CUTOFF_INPUT, module);
		addBayInput(BAY_C1, 2, "RES", VesperSwarm::RES_INPUT, module);
		addBayInput(BAY_C2, 2, "ATK", VesperSwarm::ATTACK_INPUT, module);
		addBayInput(BAY_C0, 3, "DEC", VesperSwarm::DECAY_INPUT, module);
		addBayInput(BAY_C1, 3, "SUS", VesperSwarm::SUSTAIN_INPUT, module);
		addBayInput(BAY_C2, 3, "REL", VesperSwarm::RELEASE_INPUT, module);

		addLabel(Vec(172.3f, 104.3f), "OUT L", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addLabel(Vec(184.9f, 104.3f), "OUT R", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(172.3f, 110.f)), module, VesperSwarm::OUTL_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(184.9f, 110.f)), module, VesperSwarm::OUTR_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		VesperSwarm* module = getModule<VesperSwarm>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Swarm law",
			{"Equal steps: the singers fan out in semitones around the pitch",
			 "Harmonic: whole-number multiples of the pitch, like the Swarmatron"},
			[module]() { return (size_t)module->law.load(); },
			[module](size_t i) { module->law.store((int)i); }));
	}
};

Model* modelVesperSwarm = createModel<VesperSwarm, VesperSwarmWidget>("VesperSwarm");
