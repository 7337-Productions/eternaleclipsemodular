#pragma once
#include <cstdint>

// Syzygy scale bank: the fourteen modes of ABreakpoint 2, in its original
// order. deg[d] = 1 when semitone d above the root belongs to the scale.
// IONIAN duplicates MAJOR and AEOLIAN duplicates MINOR on purpose -- the
// list is kept intact so a SCALE sweep lands where the original did.
namespace syzygy {

static constexpr int NUM_SCALES = 14;

struct Scale {
	const char* name;
	uint8_t deg[12];
};

static const Scale kScales[NUM_SCALES] = {
	{"MAJOR",        {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1}},
	{"MINOR",        {1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0}},
	{"HARM MINOR",   {1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1}},
	{"HARM MINOR 4", {1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1}},
	{"DORIAN",       {1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1, 0}},
	{"PHRYGIAN",     {1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0}},
	{"LYDIAN",       {1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1}},
	{"MIXOLYDIAN",   {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0}},
	{"IONIAN",       {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1}},
	{"AEOLIAN",      {1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0}},
	{"LOCRIAN",      {1, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0}},
	{"PROMETHEAN",   {1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 0}},
	{"ENIGMATIC",    {1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1}},
	{"CHROMATIC",    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}},
};

static const char* kRootNames[12] = {
	"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// 12-bit mask, bit d = semitone d above the root
inline uint16_t scaleMask(int s) {
	if (s < 0 || s >= NUM_SCALES)
		s = 0;
	uint16_t m = 0;
	for (int d = 0; d < 12; d++)
		if (kScales[s].deg[d])
			m |= (uint16_t)(1u << d);
	return m;
}

// Nearest scale degree at or above `semi` (the original scaleQuant walks up)
inline int quantizeUp(int semi, uint16_t mask, int root) {
	for (int k = 0; k < 12; k++) {
		int d = ((semi + k - root) % 12 + 12) % 12;
		if (mask & (1u << d))
			return semi + k;
	}
	return semi;
}

} // namespace syzygy
