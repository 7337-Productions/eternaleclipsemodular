#pragma once
#include <rack.hpp>

using namespace rack;

// Shared Eternal Eclipse panel style: copper-on-abyss (pure black panels,
// tiered copper/gold text), matching the Church of the Eternal Eclipse
// website (assets/css/tokens.css). Labels are runtime-drawn (Rack's SVG
// renderer ignores <text> elements). Contrast rule carried from the site:
// every text tier stays >= ~4:1 on the panel background; quietness comes from
// size and tracking, never from dimming.
namespace eclipse {

static const NVGcolor LABEL_COLOR = nvgRGB(0xff, 0xee, 0xb8);   // copper-flash cream
static const NVGcolor ACCENT_COLOR = nvgRGB(0xff, 0xc4, 0x64);  // copper-bright
static const NVGcolor DIM_COLOR = nvgRGB(0x9c, 0x6a, 0x3b);     // ash -- glyphs/graphics only, never text

// Type scale (px at 100% zoom). VCV Library review requires text readable at
// 100% on a normal-DPI monitor; these sizes track VCV Fundamental's density.
// FINE_SIZE is the floor -- nothing on a panel goes smaller.
static const float TITLE_SIZE = 12.f;
static const float LABEL_SIZE = 9.f;
static const float FINE_SIZE = 7.f;

struct PanelLabel : TransparentWidget {
	std::string text;
	float fontSize;
	NVGcolor color;

	void draw(const DrawArgs& args) override {
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, fontSize);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, color);
		nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f, text.c_str(), NULL);
	}
};

inline void addLabel(Widget* parent, Vec mmPos, const std::string& text,
                     float fontSize = LABEL_SIZE, NVGcolor color = LABEL_COLOR) {
	PanelLabel* label = new PanelLabel;
	label->box.size = mm2px(Vec(30.f, 5.f));
	label->box.pos = mm2px(mmPos).minus(label->box.size.div(2));
	label->text = text;
	label->fontSize = fontSize;
	label->color = color;
	parent->addChild(label);
}

// Standard header block: title at y=7.3, sized for the chamber-glyph divider
// at y=10.8 in the panel SVG. Every module uses this geometry. The brand mark
// lives at the bottom edge of the panel SVG.
inline void addHeader(Widget* parent, float centerX, const std::string& title) {
	addLabel(parent, Vec(centerX, 7.3f), title, TITLE_SIZE, ACCENT_COLOR);
}

// House light buttons: the black bezel with Rack's stock red light, as on
// Elemental Revelator's SPIRIT button. Every lit button on an Eternal
// Eclipse panel uses one of these two, so the glow reads the same across a
// rack. Drive them at 1.0 lit and 0 off (a quarter for armed-but-idle).
using LightButton = VCVLightBezelLatch<RedLight>;        // latching
using MomentaryLightButton = VCVLightBezel<RedLight>;    // momentary

// Two-position horizontal toggle (left = 0, right = 1), built from the
// component library's three-position horizontal switch frames.
struct CKSSHorizontal : app::SvgSwitch {
	CKSSHorizontal() {
		shadow->opacity = 0.0;
		addFrame(Svg::load(asset::system("res/ComponentLibrary/CKSSThreeHorizontal_0.svg")));
		addFrame(Svg::load(asset::system("res/ComponentLibrary/CKSSThreeHorizontal_2.svg")));
	}
};

} // namespace eclipse
