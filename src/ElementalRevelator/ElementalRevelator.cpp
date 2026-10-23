#include "plugin.hpp"
#include "EclipseWidgets.hpp"
#include "ChordBank.hpp"
#include "DaemonNames.hpp"
#include "dsp/AlgorithmEngine.hpp"
#include "dsp/WavetableOsc.hpp"
#include "dsp/CrossMod.hpp"
#include "dsp/LadderFilter.hpp"
#include "dsp/StereoDelay.hpp"

using namespace dsputil;

// Shared wavetable data (~4MB), generated once on first module instantiation
static const WavetableBank& wavetables() {
	static WavetableBank bank;
	return bank;
}

struct ElementalRevelator : Module {
	enum Elem { AIR, WATER, EARTH, FIRE };

	enum ParamId {
		AIR_PITCH_PARAM,
		WATER_PITCH_PARAM,
		EARTH_PITCH_PARAM,
		FIRE_PITCH_PARAM,
		AIR_WT_PARAM,
		WATER_WT_PARAM,
		EARTH_WT_PARAM,
		FIRE_WT_PARAM,
		SPIRIT_PARAM,
		SPIRIT_MODE_PARAM,
		OFFERING_PARAM,
		SIGIL_PARAM,
		RITE_PARAM,
		VEIL_PARAM,
		DAEMON_PARAM,
		OMEN_PARAM,
		CUTOFF_PARAM,
		RES_PARAM,
		TIME_PARAM,
		FEED_PARAM,
		MIX_PARAM,
		// CV attenuverters (appended to keep earlier param indices stable)
		VOCT_ATT_PARAM,
		SIGIL_ATT_PARAM,
		VEIL_ATT_PARAM,
		RITE_ATT_PARAM,
		DAEMON_ATT_PARAM,
		OFFERING_ATT_PARAM,
		OMEN_ATT_PARAM,
		SPIRIT_ATT_PARAM,
		AIRWT_ATT_PARAM,
		WATWT_ATT_PARAM,
		ERHWT_ATT_PARAM,
		FIRWT_ATT_PARAM,
		TIME_ATT_PARAM,
		FEED_ATT_PARAM,
		CUTOFF_ATT_PARAM,
		RES_ATT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		SIGIL_INPUT,
		VEIL_INPUT,
		RITE_INPUT,
		DAEMON_INPUT,
		OFFERING_INPUT,
		OMEN_INPUT,
		SPIRIT_INPUT,
		AIRWT_INPUT,
		WATWT_INPUT,
		ERHWT_INPUT,
		FIRWT_INPUT,
		TIME_INPUT,
		FEED_INPUT,
		CUTOFF_INPUT,
		RES_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTL_OUTPUT,
		OUTR_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		SPIRIT_LIGHT,
		LIGHTS_LEN
	};

	WavetableOsc osc[4];
	LadderFilter ladderL, ladderR;
	StereoDelay delay;
	AlgorithmEngine engine;

	float lastOut[4] = {};
	float slewedSemi[4] = {};
	bool semiInit = false;
	float smoothedTime = -1.f;

	// Per-element WT knob/CV/attenuverter ids in element order
	static constexpr int WT_PARAM[4] = {AIR_WT_PARAM, WATER_WT_PARAM, EARTH_WT_PARAM, FIRE_WT_PARAM};
	static constexpr int WT_INPUT[4] = {AIRWT_INPUT, WATWT_INPUT, ERHWT_INPUT, FIRWT_INPUT};
	static constexpr int WT_ATT[4] = {AIRWT_ATT_PARAM, WATWT_ATT_PARAM, ERHWT_ATT_PARAM, FIRWT_ATT_PARAM};
	static constexpr int PITCH_PARAM[4] = {AIR_PITCH_PARAM, WATER_PITCH_PARAM, EARTH_PITCH_PARAM, FIRE_PITCH_PARAM};

	// SIGIL ring: each element is modulated by its predecessor
	// (AIR -> WATER -> FIRE -> EARTH -> AIR)
	static constexpr int RING_PRED[4] = {EARTH, AIR, FIRE, WATER};

	// VEIL detune spread (approximately zero-sum: pitch center stays put)
	static constexpr float DETUNE_COEF[4] = {1.f, -0.4f, -1.f, 0.4f};
	// Element wavetable-position spread positions
	static constexpr float MORPH_COEF[4] = {-1.f, -0.33f, 0.33f, 1.f};
	// Stereo placement: AIR/EARTH left, WATER/FIRE right
	static constexpr float PAN_COEF[4] = {-0.8f, 0.4f, -0.4f, 0.8f};

	// OMEN variation: deterministic per-seed targets, smoothed to avoid clicks
	int lastSeed = -1;
	float varDetuneT[4] = {}, varDetune[4] = {};   // per-element detune, semitones
	float varPhaseT[4] = {}, varPhase[4] = {};     // per-element phase offset, 0..1
	float varMorphT[4] = {}, varMorph[4] = {};     // per-element morph skew
	float varSigilT = 1.f, varSigil = 1.f;         // sigil depth multiplier
	float varTimeT = 1.f, varTime = 1.f;           // delay time multiplier
	float varWidthT = 0.f, varWidth = 0.f;         // stereo width offset
	float varCutoffT = 0.f, varCutoff = 0.f;       // cutoff nudge, octaves

	void computeSeedTargets(int seed) {
		if (seed == 0) {
			// Seed 0 is the untouched voice
			for (int i = 0; i < 4; i++) {
				varDetuneT[i] = 0.f;
				varPhaseT[i] = 0.f;
				varMorphT[i] = 0.f;
			}
			varSigilT = 1.f;
			varTimeT = 1.f;
			varWidthT = 0.f;
			varCutoffT = 0.f;
			return;
		}
		uint32_t h = (uint32_t)seed * 2654435761u;
		auto next = [&h]() {
			h = h * 1664525u + 1013904223u;
			return (float)((h >> 8) & 0xFFFF) / 65535.f; // 0..1
		};
		for (int i = 0; i < 4; i++) {
			varDetuneT[i] = (next() - 0.5f) * 0.7f;   // ±0.35 semitones
			varPhaseT[i] = next();                     // 0..1
			varMorphT[i] = (next() - 0.5f) * 0.16f;   // ±0.08
		}
		varSigilT = 0.8f + next() * 0.4f;             // 0.8..1.2
		varTimeT = 0.9f + next() * 0.2f;              // 0.9..1.1
		varWidthT = (next() - 0.5f) * 0.4f;           // ±0.2
		varCutoffT = (next() - 0.5f) * 0.5f;          // ±0.25 octaves
	}

	// OFFERING scan character: warp the wavetable position from smooth
	// interpolation (s=0) toward stepped, hard-switched waves (s=1).
	// e is the crossfade band half-width at each cell midpoint -- it only
	// depends on s, so the caller computes it once per sample (see
	// offeringEdge) instead of per element.
	static float offeringEdge(float s) {
		// s=0 -> 0.5 (whole cell = identity), s=1 -> 0.01 (near-instant,
		// but still click-free under morph CV)
		return 0.5f * std::pow(1.f - s, 1.5f) + 0.01f;
	}
	static float offeringQuantize(float m, float s, float e) {
		if (s <= 0.f)
			return m;
		const float N = 15.f; // NUM_WAVES - 1 cells
		float w = m * N;
		float cell = std::floor(w);
		float frac = w - cell;
		float t = clampf((frac - (0.5f - e)) / (2.f * e), 0.f, 1.f);
		t = t * t * (3.f - 2.f * t); // smoothstep: steps land with glue
		float q = (cell + t) / N;
		return lerp(m, q, std::min(1.f, s * 8.f));
	}

	ElementalRevelator() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(AIR_PITCH_PARAM, -54.f, 54.f, -12.f, "Air (pitch / chord root)", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
		configParam(WATER_PITCH_PARAM, -54.f, 54.f, -12.f, "Water (pitch)", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
		configParam(EARTH_PITCH_PARAM, -54.f, 54.f, -12.f, "Earth (pitch)", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
		configParam(FIRE_PITCH_PARAM, -54.f, 54.f, -12.f, "Fire (pitch)", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
		configParam(AIR_WT_PARAM, 0.f, 1.f, 0.f, "Air wavetable position", "%", 0.f, 100.f);
		configParam(WATER_WT_PARAM, 0.f, 1.f, 0.f, "Water wavetable position", "%", 0.f, 100.f);
		configParam(EARTH_WT_PARAM, 0.f, 1.f, 0.f, "Earth wavetable position", "%", 0.f, 100.f);
		configParam(FIRE_WT_PARAM, 0.f, 1.f, 0.f, "Fire wavetable position", "%", 0.f, 100.f);
		configParam<SpiritQuantity>(SPIRIT_PARAM, 0.f, 23.f, 2.f, "Spirit (chord)");
		paramQuantities[SPIRIT_PARAM]->snapEnabled = true;
		configSwitch(SPIRIT_MODE_PARAM, 0.f, 1.f, 0.f, "Spirit (chord mode)", {"Off", "On"});
		configParam(OFFERING_PARAM, 0.f, 1.f, 0.f, "Offering (scan: smooth \xE2\x86\x90 / stepped \xE2\x86\x92)", "%", 0.f, 100.f);
		configParam(SIGIL_PARAM, -1.f, 1.f, 0.f, "Sigil (elemental ring: PM \xE2\x86\x90 / FM \xE2\x86\x92)", "%", 0.f, 100.f);
		configParam(RITE_PARAM, 0.f, 1.f, 0.f, "Rite (opposing-pair ring mod)", "%", 0.f, 100.f);
		configParam(VEIL_PARAM, 0.f, 1.f, 0.1f, "Veil (detune / stereo width)", "%", 0.f, 100.f);
		configParam<DaemonQuantity>(DAEMON_PARAM, 0.f, 71.f, 0.f, "Daemon");
		paramQuantities[DAEMON_PARAM]->snapEnabled = true;
		configParam(OMEN_PARAM, 0.f, 63.f, 0.f, "Omen (variation)");
		paramQuantities[OMEN_PARAM]->snapEnabled = true;
		configParam(CUTOFF_PARAM, 0.f, 1.f, 1.f, "Cutoff", " Hz", 1024.f, 20.f);
		configParam(RES_PARAM, 0.f, 1.f, 0.f, "Resonance", "%", 0.f, 100.f);
		configParam(TIME_PARAM, 0.f, 1.f, 0.5f, "Time (delay time)", "%", 0.f, 100.f);
		configParam(FEED_PARAM, 0.f, 1.f, 0.3f, "Feed (feedback / diffusion)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix (delay dry/wet)", "%", 0.f, 100.f);

		configParam(VOCT_ATT_PARAM, -1.f, 1.f, 0.f, "V/oct CV amount", "%", 0.f, 100.f);
		configParam(SIGIL_ATT_PARAM, -1.f, 1.f, 0.f, "Sigil CV amount", "%", 0.f, 100.f);
		configParam(VEIL_ATT_PARAM, -1.f, 1.f, 0.f, "Veil CV amount", "%", 0.f, 100.f);
		configParam(RITE_ATT_PARAM, -1.f, 1.f, 0.f, "Rite CV amount", "%", 0.f, 100.f);
		configParam(DAEMON_ATT_PARAM, -1.f, 1.f, 1.f, "Daemon scan CV amount", "%", 0.f, 100.f);
		configParam(OFFERING_ATT_PARAM, -1.f, 1.f, 0.f, "Offering CV amount", "%", 0.f, 100.f);
		configParam(OMEN_ATT_PARAM, -1.f, 1.f, 0.f, "Omen CV amount", "%", 0.f, 100.f);
		configParam(SPIRIT_ATT_PARAM, -1.f, 1.f, 0.f, "Spirit CV amount", "%", 0.f, 100.f);
		configParam(AIRWT_ATT_PARAM, -1.f, 1.f, 0.f, "Air WT CV amount", "%", 0.f, 100.f);
		configParam(WATWT_ATT_PARAM, -1.f, 1.f, 0.f, "Water WT CV amount", "%", 0.f, 100.f);
		configParam(ERHWT_ATT_PARAM, -1.f, 1.f, 0.f, "Earth WT CV amount", "%", 0.f, 100.f);
		configParam(FIRWT_ATT_PARAM, -1.f, 1.f, 0.f, "Fire WT CV amount", "%", 0.f, 100.f);
		configParam(TIME_ATT_PARAM, -1.f, 1.f, 0.f, "Time CV amount", "%", 0.f, 100.f);
		configParam(FEED_ATT_PARAM, -1.f, 1.f, 0.f, "Feed CV amount", "%", 0.f, 100.f);
		configParam(CUTOFF_ATT_PARAM, -1.f, 1.f, 0.f, "Cutoff CV amount", "%", 0.f, 100.f);
		configParam(RES_ATT_PARAM, -1.f, 1.f, 0.f, "Resonance CV amount", "%", 0.f, 100.f);

		configInput(VOCT_INPUT, "1V/octave pitch (poly with Spirit off: channels 1-4 play Air, Water, Earth, Fire)");
		configInput(SIGIL_INPUT, "Sigil CV");
		configInput(VEIL_INPUT, "Veil CV");
		configInput(RITE_INPUT, "Rite CV");
		configInput(DAEMON_INPUT, "Daemon scan CV (10V sweeps all 72 daemons; overrides knob)");
		configInput(OFFERING_INPUT, "Offering CV");
		configInput(OMEN_INPUT, "Omen CV (10V sweeps all 64 steps)");
		configInput(SPIRIT_INPUT, "Spirit chord CV (10V sweeps all 24 chords; active in Spirit mode)");
		configInput(AIRWT_INPUT, "Air wavetable CV");
		configInput(WATWT_INPUT, "Water wavetable CV");
		configInput(ERHWT_INPUT, "Earth wavetable CV");
		configInput(FIRWT_INPUT, "Fire wavetable CV");
		configInput(TIME_INPUT, "Time CV");
		configInput(FEED_INPUT, "Feed CV");
		configInput(CUTOFF_INPUT, "Cutoff CV (1V/octave)");
		configInput(RES_INPUT, "Resonance CV");
		configOutput(OUTL_OUTPUT, "Left audio");
		configOutput(OUTR_OUTPUT, "Right audio");

		// Force wavetable generation now rather than on the audio thread
		wavetables();
	}

	void onReset() override {
		for (int i = 0; i < 4; i++) {
			osc[i].reset();
			lastOut[i] = 0.f;
		}
		ladderL.reset();
		ladderR.reset();
		delay.clear();
		semiInit = false;
		smoothedTime = -1.f;
	}

	// Blend two algorithm configs: floats interpolate, ints/bools snap to new
	static AlgorithmEngine::AlgorithmConfig blendConfig(
		const AlgorithmEngine::AlgorithmConfig& a,
		const AlgorithmEngine::AlgorithmConfig& b, float t) {
		AlgorithmEngine::AlgorithmConfig c = b;
		c.detuneScale = lerp(a.detuneScale, b.detuneScale, t);
		c.morphSpread = lerp(a.morphSpread, b.morphSpread, t);
		c.sigilScale = lerp(a.sigilScale, b.sigilScale, t);
		c.cutoffOffset = lerp(a.cutoffOffset, b.cutoffOffset, t);
		c.resBias = lerp(a.resBias, b.resBias, t);
		c.keyTrack = lerp(a.keyTrack, b.keyTrack, t);
		c.delayTimeBase = lerp(a.delayTimeBase, b.delayTimeBase, t);
		c.delayFeedback = lerp(a.delayFeedback, b.delayFeedback, t);
		c.diffusionBase = lerp(a.diffusionBase, b.diffusionBase, t);
		c.freqShiftRange = lerp(a.freqShiftRange, b.freqShiftRange, t);
		c.elementTilt = lerp(a.elementTilt, b.elementTilt, t);
		c.riteScale = lerp(a.riteScale, b.riteScale, t);
		c.outputGain = lerp(a.outputGain, b.outputGain, t);
		return c;
	}

	void process(const ProcessArgs& args) override {
		// ===== Daemon =====
		int daemonIdx = (int)std::round(params[DAEMON_PARAM].getValue());
		// DAEMON CV scans all 72 daemons and overrides the knob while patched
		if (inputs[DAEMON_INPUT].isConnected()) {
			float scan = clampf(inputs[DAEMON_INPUT].getVoltage() / 10.f
				* params[DAEMON_ATT_PARAM].getValue(), 0.f, 1.f);
			daemonIdx = std::min((int)(scan * 72.f), 71);
		}
		engine.setIndex(daemonIdx);
		engine.setSampleRate(args.sampleRate);
		float blend = engine.advanceCrossfade();
		AlgorithmEngine::AlgorithmConfig cfg = engine.isCrossfading()
			? blendConfig(engine.getPrevConfig(), engine.getConfig(), blend)
			: engine.getConfig();

		// ===== Omen variation =====
		int seed = (int)std::round(clampf(params[OMEN_PARAM].getValue()
			+ inputs[OMEN_INPUT].getVoltage() / 10.f * params[OMEN_ATT_PARAM].getValue() * 63.f, 0.f, 63.f));
		if (seed != lastSeed) {
			lastSeed = seed;
			computeSeedTargets(seed);
		}
		float sm = std::min(40.f / args.sampleRate, 1.f); // ~25ms smoothing
		for (int i = 0; i < 4; i++) {
			varDetune[i] += (varDetuneT[i] - varDetune[i]) * sm;
			varPhase[i] += (varPhaseT[i] - varPhase[i]) * sm;
			varMorph[i] += (varMorphT[i] - varMorph[i]) * sm;
		}
		varSigil += (varSigilT - varSigil) * sm;
		varTime += (varTimeT - varTime) * sm;
		varWidth += (varWidthT - varWidth) * sm;
		varCutoff += (varCutoffT - varCutoff) * sm;

		// ===== Spirit chord mode & pitch =====
		bool spiritOn = params[SPIRIT_MODE_PARAM].getValue() > 0.5f;
		lights[SPIRIT_LIGHT].setBrightness(spiritOn ? 1.f : 0.f);
		float voct = inputs[VOCT_INPUT].getVoltage() * params[VOCT_ATT_PARAM].getValue();
		// Spirit off: a poly V/OCT gives each element its own channel (AIR 1,
		// WATER 2, EARTH 3, FIRE 4); with fewer channels the elements wrap
		// round the ones there are, so a mono cable moves all four together
		float elemVoct[4] = {voct, voct, voct, voct};
		int voctChannels = inputs[VOCT_INPUT].getChannels();
		if (!spiritOn && voctChannels > 1) {
			for (int i = 0; i < 4; i++)
				elemVoct[i] = inputs[VOCT_INPUT].getVoltage(i % voctChannels) * params[VOCT_ATT_PARAM].getValue();
		}

		float targetSemi[4];
		float veilDetuneScale = 1.f;
		if (spiritOn) {
			// AIR stays live as the chord root, quantized to semitones;
			// the other three tunings are taken over by the chord
			float rootSemi = std::round(params[AIR_PITCH_PARAM].getValue());
			int chordIdx = (int)std::round(clampf(params[SPIRIT_PARAM].getValue()
				+ inputs[SPIRIT_INPUT].getVoltage() / 10.f * params[SPIRIT_ATT_PARAM].getValue() * 23.f, 0.f, 23.f));
			for (int i = 0; i < 4; i++)
				targetSemi[i] = rootSemi + kChords[chordIdx].offsets[i];
			veilDetuneScale = 0.15f; // chords stay chromatic; VEIL is mostly width
		}
		else {
			for (int i = 0; i < 4; i++)
				targetSemi[i] = params[PITCH_PARAM[i]].getValue();
		}
		// ~5ms slew: chord changes and mode toggling never zipper
		if (!semiInit) {
			semiInit = true;
			for (int i = 0; i < 4; i++)
				slewedSemi[i] = targetSemi[i];
		}
		float semiSm = std::min(200.f / args.sampleRate, 1.f);
		for (int i = 0; i < 4; i++)
			slewedSemi[i] += (targetSemi[i] - slewedSemi[i]) * semiSm;

		// ===== Veil: detune spread + stereo width =====
		float veil = clampf(params[VEIL_PARAM].getValue()
			+ inputs[VEIL_INPUT].getVoltage() / 10.f * params[VEIL_ATT_PARAM].getValue(), 0.f, 1.f);
		float freq[4];
		for (int i = 0; i < 4; i++) {
			float detuneSemi = veil * cfg.detuneScale * 5.f * DETUNE_COEF[i] * veilDetuneScale + varDetune[i];
			freq[i] = dsp::FREQ_C4 * std::pow(2.f, slewedSemi[i] / 12.f + elemVoct[i] + detuneSemi / 12.f);
			freq[i] = clampf(freq[i], 0.1f, args.sampleRate * 0.25f);
		}

		// ===== Offering + per-element wavetable position =====
		float offering = clampf(params[OFFERING_PARAM].getValue()
			+ inputs[OFFERING_INPUT].getVoltage() / 10.f * params[OFFERING_ATT_PARAM].getValue(), 0.f, 1.f);
		float offeringE = offeringEdge(offering);
		float morph[4];
		for (int i = 0; i < 4; i++) {
			float m = clampf(params[WT_PARAM[i]].getValue()
				+ inputs[WT_INPUT[i]].getVoltage() / 10.f * params[WT_ATT[i]].getValue()
				+ cfg.morphSpread * MORPH_COEF[i] + varMorph[i], 0.f, 1.f);
			morph[i] = offeringQuantize(m, offering, offeringE);
		}

		// ===== Sigil: elemental ring cross-mod (1-sample feedback) =====
		float sigil = clampf((params[SIGIL_PARAM].getValue()
			+ inputs[SIGIL_INPUT].getVoltage() / 5.f * params[SIGIL_ATT_PARAM].getValue())
			* cfg.sigilScale * varSigil, -1.f, 1.f);
		float out[4];
		for (int i = 0; i < 4; i++) {
			float w = (cfg.sigilAlternate && (i == WATER || i == EARTH)) ? -sigil : sigil;
			CrossMod::CrossModResult cm = CrossMod::process(w, lastOut[RING_PRED[i]], freq[i]);
			out[i] = osc[i].processWithPhaseMod(freq[i] + cm.freqOffset, morph[i],
				cfg.wavebank[i], args.sampleRate, wavetables(), cm.phaseMod + varPhase[i]);
			out[i] = CrossMod::wavefold(out[i], cm.foldAmount);
		}
		for (int i = 0; i < 4; i++)
			lastOut[i] = out[i];

		// ===== Mix: element tilt, stereo placement =====
		float width = clampf(veil * 0.9f + varWidth, 0.f, 1.f);
		float gAF = std::min(1.f, 1.f + cfg.elementTilt);
		float gWE = std::min(1.f, 1.f - cfg.elementTilt);
		float gain[4] = {gAF, gWE, gWE, gAF};
		float sigL = 0.f, sigR = 0.f;
		for (int i = 0; i < 4; i++) {
			float s = out[i] * gain[i] * 0.5f;
			sigL += s * 0.5f * (1.f - PAN_COEF[i] * width);
			sigR += s * 0.5f * (1.f + PAN_COEF[i] * width);
		}

		// ===== Rite: opposing-pair ring mod =====
		float rite = clampf((params[RITE_PARAM].getValue()
			+ inputs[RITE_INPUT].getVoltage() / 10.f * params[RITE_ATT_PARAM].getValue())
			* cfg.riteScale, 0.f, 1.f);
		float ringAF = out[AIR] * out[FIRE];
		float ringWE = out[WATER] * out[EARTH];
		float ringSum = 0.7f * (ringAF + ringWE);
		float ringL = lerp(ringSum, 1.2f * ringAF, 0.5f * width);
		float ringR = lerp(ringSum, 1.2f * ringWE, 0.5f * width);
		sigL = lerp(sigL, ringL, rite);
		sigR = lerp(sigR, ringR, rite);

		// ===== Ladder filter =====
		float cutExp = params[CUTOFF_PARAM].getValue() * 10.f
			+ inputs[CUTOFF_INPUT].getVoltage() * params[CUTOFF_ATT_PARAM].getValue()
			+ cfg.cutoffOffset + varCutoff
			+ cfg.keyTrack * (slewedSemi[AIR] / 12.f + voct);
		float fc = clampf(20.f * std::pow(2.f, cutExp), 20.f, std::min(20000.f, args.sampleRate * 0.45f));
		float res = clampf(params[RES_PARAM].getValue()
			+ inputs[RES_INPUT].getVoltage() / 10.f * params[RES_ATT_PARAM].getValue()
			+ cfg.resBias, 0.f, 1.05f);
		sigL = ladderL.process(sigL, fc, res, args.sampleRate);
		sigR = ladderR.process(sigR, fc, res, args.sampleRate);

		// ===== Delay / diffusion FX =====
		float timeParam = clampf(params[TIME_PARAM].getValue()
			+ inputs[TIME_INPUT].getVoltage() / 10.f * params[TIME_ATT_PARAM].getValue(), 0.f, 1.f);
		float delayTime = cfg.delayTimeBase * varTime * std::pow(4.f, 2.f * timeParam - 1.f); // 0.25x..4x
		// Slew delay time to avoid hard jumps in the read head
		if (smoothedTime < 0.f)
			smoothedTime = delayTime;
		smoothedTime += (delayTime - smoothedTime) * (10.f / args.sampleRate);

		float feed = clampf(params[FEED_PARAM].getValue()
			+ inputs[FEED_INPUT].getVoltage() / 10.f * params[FEED_ATT_PARAM].getValue(), 0.f, 1.f);
		float diffusion = clampf(cfg.diffusionBase + feed * 0.3f, 0.f, 1.f);
		float feedback = clampf(feed * 0.9f + cfg.delayFeedback * 0.3f, 0.f, 0.95f);
		float shift = cfg.freqShiftRange * (0.25f + 0.75f * feed);

		StereoDelay::StereoFrame fx = delay.process(sigL, sigR, diffusion, smoothedTime,
			feedback, shift, params[MIX_PARAM].getValue(), args.sampleRate);

		// ===== Output =====
		float outL = softClip(fx.L * cfg.outputGain) * 5.f;
		float outR = softClip(fx.R * cfg.outputGain) * 5.f;

		if (outputs[OUTL_OUTPUT].isConnected() && !outputs[OUTR_OUTPUT].isConnected())
			outputs[OUTL_OUTPUT].setVoltage(0.5f * (outL + outR));
		else
			outputs[OUTL_OUTPUT].setVoltage(outL);
		outputs[OUTR_OUTPUT].setVoltage(outR);
	}
};

constexpr int ElementalRevelator::WT_PARAM[4];
constexpr int ElementalRevelator::WT_INPUT[4];
constexpr int ElementalRevelator::WT_ATT[4];
constexpr int ElementalRevelator::PITCH_PARAM[4];
constexpr int ElementalRevelator::RING_PRED[4];
constexpr float ElementalRevelator::DETUNE_COEF[4];
constexpr float ElementalRevelator::MORPH_COEF[4];
constexpr float ElementalRevelator::PAN_COEF[4];

struct ElementalRevelatorWidget : ModuleWidget {
	// Pentagram zone: all ten knobs sit on one ring (r=44 around 60,70),
	// evenly spaced 36 degrees apart. Golden-Dawn attribution for the
	// five points (SPIRIT top, AIR upper-left, WATER upper-right, EARTH
	// lower-left, FIRE lower-right); the five modifiers take the
	// midpoint angles, each wired to its inner pentagram corner.
	static constexpr float SPIRIT_X = 60.f, SPIRIT_Y = 26.f;
	static constexpr float AIR_X = 18.15f, AIR_Y = 56.4f;
	static constexpr float WATER_X = 101.85f, WATER_Y = 56.4f;
	static constexpr float EARTH_X = 34.14f, EARTH_Y = 105.6f;
	static constexpr float FIRE_X = 85.86f, FIRE_Y = 105.6f;
	static constexpr float CENTER_X = 60.f, CENTER_Y = 70.f;
	// Filter/FX column center
	static constexpr float FX_X = 139.5f;
	// Patch bay column centers and jack/attenuverter pair offsets
	static constexpr float BAY_XL = 175.f, BAY_XR = 196.5f;
	static constexpr float JACK_DX = -4.8f, ATT_DX = 5.2f;

	void addLabel(Vec mmPos, const std::string& text, float fontSize = eclipse::LABEL_SIZE,
	              NVGcolor color = eclipse::LABEL_COLOR) {
		eclipse::addLabel(this, mmPos, text, fontSize, color);
	}

	void addCvPair(float x, float y, int inputId, int attParamId, ElementalRevelator* module) {
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x + JACK_DX, y)), module, inputId));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(x + ATT_DX, y)), module, attParamId));
	}

	ElementalRevelatorWidget(ElementalRevelator* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ElementalRevelator.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Title
		eclipse::addHeader(this, 106.68f, "E L E M E N T A L   R E V E L A T O R");

		// ===== Zone 1: pentagram =====
		// Five points
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(SPIRIT_X, SPIRIT_Y)), module, ElementalRevelator::SPIRIT_PARAM));
		addLabel(Vec(SPIRIT_X, SPIRIT_Y - 10.f), "SPIRIT");
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(AIR_X, AIR_Y)), module, ElementalRevelator::AIR_PITCH_PARAM));
		addLabel(Vec(AIR_X, AIR_Y + 8.8f), "AIR");
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(WATER_X, WATER_Y)), module, ElementalRevelator::WATER_PITCH_PARAM));
		addLabel(Vec(WATER_X, WATER_Y + 8.8f), "WATER");
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(EARTH_X, EARTH_Y)), module, ElementalRevelator::EARTH_PITCH_PARAM));
		addLabel(Vec(EARTH_X, EARTH_Y + 8.8f), "EARTH");
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(FIRE_X, FIRE_Y)), module, ElementalRevelator::FIRE_PITCH_PARAM));
		addLabel(Vec(FIRE_X, FIRE_Y + 8.8f), "FIRE");

		// Spirit latch at the pentagram center
		addParam(createLightParamCentered<VCVLightBezelLatch<RedLight>>(mm2px(Vec(CENTER_X, CENTER_Y)), module,
			ElementalRevelator::SPIRIT_MODE_PARAM, ElementalRevelator::SPIRIT_LIGHT));

		// Per-element wavetable trimpots, on the ring beside their elements
		addParam(createParamCentered<Trimpot>(mm2px(Vec(16.f, 70.f)), module, ElementalRevelator::AIR_WT_PARAM));
		addLabel(Vec(16.f, 75.5f), "WT", eclipse::FINE_SIZE);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(104.f, 70.f)), module, ElementalRevelator::WATER_WT_PARAM));
		addLabel(Vec(104.f, 75.5f), "WT", eclipse::FINE_SIZE);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(24.4f, 95.86f)), module, ElementalRevelator::EARTH_WT_PARAM));
		addLabel(Vec(24.4f, 101.4f), "WT", eclipse::FINE_SIZE);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(95.6f, 95.86f)), module, ElementalRevelator::FIRE_WT_PARAM));
		addLabel(Vec(95.6f, 101.4f), "WT", eclipse::FINE_SIZE);

		// Modifier knobs on the ring at the midpoint angles
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(34.14f, 34.4f)), module, ElementalRevelator::SIGIL_PARAM));
		addLabel(Vec(34.14f, 41.6f), "SIGIL");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(85.86f, 34.4f)), module, ElementalRevelator::VEIL_PARAM));
		addLabel(Vec(85.86f, 41.6f), "VEIL");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(18.15f, 83.6f)), module, ElementalRevelator::OFFERING_PARAM));
		addLabel(Vec(18.15f, 90.8f), "OFFERING");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(101.85f, 83.6f)), module, ElementalRevelator::RITE_PARAM));
		addLabel(Vec(101.85f, 90.8f), "RITE");

		// Daemon knob at the ring's bottom
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(60.f, 114.f)), module, ElementalRevelator::DAEMON_PARAM));
		// Abyssal Harmonics seal red (label logo on the Church music page);
		// the matching red rings live in the panel SVG.
		addLabel(Vec(60.f, 123.f), "DAEMON", eclipse::LABEL_SIZE, nvgRGB(0xff, 0x00, 0x00));

		// ===== Zone 2: filter/FX column =====
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(FX_X, 28.f)), module, ElementalRevelator::OMEN_PARAM));
		addLabel(Vec(FX_X, 36.8f), "OMEN");
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(FX_X, 50.f)), module, ElementalRevelator::CUTOFF_PARAM));
		addLabel(Vec(FX_X, 58.8f), "CUTOFF", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(FX_X, 70.f)), module, ElementalRevelator::RES_PARAM));
		addLabel(Vec(FX_X, 78.8f), "RES");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(FX_X, 92.f)), module, ElementalRevelator::TIME_PARAM));
		addLabel(Vec(FX_X, 100.8f), "TIME");
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(131.f, 110.f)), module, ElementalRevelator::FEED_PARAM));
		addLabel(Vec(131.f, 115.7f), "FEED");
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(148.f, 110.f)), module, ElementalRevelator::MIX_PARAM));
		addLabel(Vec(148.f, 115.7f), "MIX");

		// ===== Zone 3: patch bay =====
		static const float ROW_Y[8] = {26.f, 37.f, 48.f, 59.f, 70.f, 81.f, 92.f, 103.f};
		struct BayEntry {
			const char* label;
			int inputId;
			int attId;
		};
		static const BayEntry LEFT[8] = {
			{"V/OCT", ElementalRevelator::VOCT_INPUT, ElementalRevelator::VOCT_ATT_PARAM},
			{"SIGIL", ElementalRevelator::SIGIL_INPUT, ElementalRevelator::SIGIL_ATT_PARAM},
			{"VEIL", ElementalRevelator::VEIL_INPUT, ElementalRevelator::VEIL_ATT_PARAM},
			{"RITE", ElementalRevelator::RITE_INPUT, ElementalRevelator::RITE_ATT_PARAM},
			{"DAEMON", ElementalRevelator::DAEMON_INPUT, ElementalRevelator::DAEMON_ATT_PARAM},
			{"OFFERING", ElementalRevelator::OFFERING_INPUT, ElementalRevelator::OFFERING_ATT_PARAM},
			{"OMEN", ElementalRevelator::OMEN_INPUT, ElementalRevelator::OMEN_ATT_PARAM},
			{"CUTOFF", ElementalRevelator::CUTOFF_INPUT, ElementalRevelator::CUTOFF_ATT_PARAM},
		};
		static const BayEntry RIGHT[8] = {
			{"AIR WT", ElementalRevelator::AIRWT_INPUT, ElementalRevelator::AIRWT_ATT_PARAM},
			{"ERH WT", ElementalRevelator::ERHWT_INPUT, ElementalRevelator::ERHWT_ATT_PARAM},
			{"FIR WT", ElementalRevelator::FIRWT_INPUT, ElementalRevelator::FIRWT_ATT_PARAM},
			{"WAT WT", ElementalRevelator::WATWT_INPUT, ElementalRevelator::WATWT_ATT_PARAM},
			{"SPIRIT", ElementalRevelator::SPIRIT_INPUT, ElementalRevelator::SPIRIT_ATT_PARAM},
			{"TIME", ElementalRevelator::TIME_INPUT, ElementalRevelator::TIME_ATT_PARAM},
			{"FEED", ElementalRevelator::FEED_INPUT, ElementalRevelator::FEED_ATT_PARAM},
			{"RES", ElementalRevelator::RES_INPUT, ElementalRevelator::RES_ATT_PARAM},
		};
		for (int i = 0; i < 8; i++) {
			addLabel(Vec(BAY_XL, ROW_Y[i] - 5.7f), LEFT[i].label);
			addCvPair(BAY_XL, ROW_Y[i], LEFT[i].inputId, LEFT[i].attId, module);
			addLabel(Vec(BAY_XR, ROW_Y[i] - 5.7f), RIGHT[i].label);
			addCvPair(BAY_XR, ROW_Y[i], RIGHT[i].inputId, RIGHT[i].attId, module);
		}

		// Stereo outputs
		addLabel(Vec(BAY_XL, 111.6f), "OUT L", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addLabel(Vec(BAY_XR, 111.6f), "OUT R", eclipse::LABEL_SIZE, eclipse::ACCENT_COLOR);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BAY_XL, 117.6f)), module, ElementalRevelator::OUTL_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BAY_XR, 117.6f)), module, ElementalRevelator::OUTR_OUTPUT));
	}
};

Model* modelElementalRevelator = createModel<ElementalRevelator, ElementalRevelatorWidget>("ElementalRevelator");
