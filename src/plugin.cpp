#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(modelElementalRevelator);
	p->addModel(modelMoonPhaseDistortion);
	p->addModel(modelLiminalVast);
	p->addModel(modelSaros);
	p->addModel(modelSephirothicModulator);
	p->addModel(modelCosmicClock);
	p->addModel(modelCoronalAnnihilator);
	p->addModel(modelLitanyEngine);
	p->addModel(modelSyzygy);
}
