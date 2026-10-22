#pragma once
#include <cmath>

namespace vesper {

// Gate-driven ADSR, analog style: the attack starts from wherever the level
// is, decay and release are exponential approaches. Times are reached, not
// time constants: attack hits 1 after `attack` seconds, decay and release
// are 95% done after theirs.
struct Adsr {
	enum Stage { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };
	int stage = IDLE;
	float level = 0.f;
	float aCoef = 0.f, dCoef = 0.f, rCoef = 0.f, sus = 1.f;
	bool lastGate = false;

	// Once per block; dt is the sample period.
	void setBlock(float attack, float decay, float sustain, float release, float dt) {
		aCoef = 1.f - std::exp(-dt * 1.6094f / std::fmax(attack, 1e-4f)); // ln 5
		dCoef = 1.f - std::exp(-dt * 2.9957f / std::fmax(decay, 1e-4f));  // ln 20
		rCoef = 1.f - std::exp(-dt * 2.9957f / std::fmax(release, 1e-4f));
		sus = sustain;
	}

	float process(bool gate) {
		if (gate && !lastGate)
			stage = ATTACK;
		else if (!gate && lastGate && stage != IDLE)
			stage = RELEASE;
		lastGate = gate;

		switch (stage) {
			case ATTACK:
				level += (1.25f - level) * aCoef;
				if (level >= 1.f) {
					level = 1.f;
					stage = DECAY;
				}
				break;
			case DECAY:
				level += (sus - level) * dCoef;
				if (std::fabs(level - sus) < 1e-4f)
					stage = SUSTAIN;
				break;
			case SUSTAIN:
				level += (sus - level) * dCoef;
				break;
			case RELEASE:
				level -= level * rCoef;
				if (level < 1e-5f) {
					level = 0.f;
					stage = IDLE;
				}
				break;
			default:
				level = 0.f;
				break;
		}
		return level;
	}

	void reset() {
		stage = IDLE;
		level = 0.f;
		lastGate = false;
	}
};

} // namespace vesper
