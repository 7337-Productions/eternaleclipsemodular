#include "plugin.hpp"
#include "EclipseWidgets.hpp"

// Eternal Eclipse: a resizable blank panel bearing the EEM corona. No audio,
// no controls. Drag either edge to set the width from 3 HP upward; the width
// is saved with the patch.

struct EternalEclipse : Module {
	float width = 10 * RACK_GRID_WIDTH;

	EternalEclipse() {
		config(0, 0, 0, 0);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "width", json_real(width));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* widthJ = json_object_get(rootJ, "width");
		if (widthJ)
			width = json_number_value(widthJ);
	}
};

// The panel itself, drawn at runtime because the width is not fixed: abyss
// field, the corona scaled to fit, and the bottom brand mark (wordmark from
// 10 HP, sigil only below that -- the same rule the fixed panels follow).
struct CoronaPanel : Widget {
	std::shared_ptr<window::Svg> corona;
	std::shared_ptr<window::Svg> brand;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(0x00, 0x00, 0x00));
		nvgFill(args.vg);

		if (corona && corona->handle) {
			float cw = corona->handle->width;
			float ch = corona->handle->height;
			// Side margin keeps the corona's outer wisps off the edges; the
			// height cap keeps it inside the content floor above the brand mark.
			float availW = box.size.x - 2.f * mm2px(1.5f);
			float availH = mm2px(96.f);
			float s = std::min(availW / cw, availH / ch);
			nvgSave(args.vg);
			nvgTranslate(args.vg, (box.size.x - cw * s) / 2.f, (box.size.y - ch * s) / 2.f);
			nvgScale(args.vg, s, s);
			window::svgDraw(args.vg, corona->handle);
			nvgRestore(args.vg);
		}

		if (box.size.x >= 10 * RACK_GRID_WIDTH && brand && brand->handle) {
			float bw = brand->handle->width;
			float bh = brand->handle->height;
			nvgSave(args.vg);
			nvgTranslate(args.vg, (box.size.x - bw) / 2.f, box.size.y - bh);
			window::svgDraw(args.vg, brand->handle);
			nvgRestore(args.vg);
		}
		else {
			float cx = box.size.x / 2.f;
			float cy = mm2px(126.5f);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, cy, mm2px(1.3f));
			nvgStrokeWidth(args.vg, mm2px(0.3f));
			nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xc4, 0x64, 0xe6));
			nvgStroke(args.vg);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, cy, mm2px(0.33f));
			nvgFillColor(args.vg, nvgRGB(0xcd, 0x76, 0x2b));
			nvgFill(args.vg);
		}
	}
};

// One HP-wide grab strip on each edge. Dragging snaps the width to the HP
// grid and asks the rack whether the new footprint is free before committing.
struct ResizeHandle : OpaqueWidget {
	bool right = false;
	Vec dragPos;
	Rect originalBox;

	ResizeHandle() {
		box.size = Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT);
	}

	void onDragStart(const DragStartEvent& e) override {
		if (e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		dragPos = APP->scene->rack->getMousePos();
		ModuleWidget* mw = getAncestorOfType<ModuleWidget>();
		originalBox = mw->box;
	}

	void onDragMove(const DragMoveEvent& e) override {
		ModuleWidget* mw = getAncestorOfType<ModuleWidget>();
		EternalEclipse* module = dynamic_cast<EternalEclipse*>(mw->module);

		float deltaX = APP->scene->rack->getMousePos().x - dragPos.x;
		Rect newBox = originalBox;
		Rect oldBox = mw->box;
		const float minWidth = 3 * RACK_GRID_WIDTH;
		newBox.size.x += right ? deltaX : -deltaX;
		newBox.size.x = std::fmax(newBox.size.x, minWidth);
		newBox.size.x = std::round(newBox.size.x / RACK_GRID_WIDTH) * RACK_GRID_WIDTH;
		if (!right)
			newBox.pos.x = originalBox.pos.x + originalBox.size.x - newBox.size.x;

		mw->box = newBox;
		if (!APP->scene->rack->requestModulePos(mw, newBox.pos))
			mw->box = oldBox;
		if (module)
			module->width = mw->box.size.x;
	}

	void draw(const DrawArgs& args) override {
		// Two hairlines in the panel's hairline copper, quiet enough to read
		// as texture until you go looking for the grab strip.
		for (float x = 5.f; x <= 10.f; x += 5.f) {
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, x + 0.5f, mm2px(18.f));
			nvgLineTo(args.vg, x + 0.5f, mm2px(122.9f));
			nvgStrokeWidth(args.vg, 1.f);
			nvgStrokeColor(args.vg, nvgRGBA(0x38, 0x21, 0x10, 0x60));
			nvgStroke(args.vg);
		}
	}
};

struct EternalEclipseWidget : ModuleWidget {
	CoronaPanel* panel;
	ResizeHandle* rightHandle;
	Widget* topRightScrew;
	Widget* bottomRightScrew;

	EternalEclipseWidget(EternalEclipse* module) {
		setModule(module);
		box.size = Vec(10 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);

		panel = new CoronaPanel;
		panel->box.size = box.size;
		panel->corona = window::Svg::load(asset::plugin(pluginInstance, "res/EternalEclipse-corona.svg"));
		panel->brand = window::Svg::load(asset::plugin(pluginInstance, "res/EternalEclipse-brand.svg"));
		addChild(panel);

		ResizeHandle* leftHandle = new ResizeHandle;
		addChild(leftHandle);
		rightHandle = new ResizeHandle;
		rightHandle->right = true;
		addChild(rightHandle);

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		topRightScrew = createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0));
		addChild(topRightScrew);
		bottomRightScrew = createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH));
		addChild(bottomRightScrew);
	}

	void step() override {
		EternalEclipse* module = dynamic_cast<EternalEclipse*>(this->module);
		if (module)
			box.size.x = module->width;

		panel->box.size = box.size;
		rightHandle->box.pos.x = box.size.x - rightHandle->box.size.x;
		topRightScrew->box.pos.x = box.size.x - 2 * RACK_GRID_WIDTH;
		bottomRightScrew->box.pos.x = box.size.x - 2 * RACK_GRID_WIDTH;
		// Below 6 HP the right screws would sit on top of the left ones.
		bool showRightScrews = box.size.x >= 6 * RACK_GRID_WIDTH;
		topRightScrew->visible = showRightScrews;
		bottomRightScrew->visible = showRightScrews;

		ModuleWidget::step();
	}
};

Model* modelEternalEclipse = createModel<EternalEclipse, EternalEclipseWidget>("EternalEclipse");
