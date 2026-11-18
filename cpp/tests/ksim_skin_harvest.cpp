// ksim_skin_harvest (kenshi-cym.28, rwh.97): the Skin Bandits' skin-harvest chain. PROBE (SkinHouse, KSIM_SKINHOUSE).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/save/save_import.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/soak.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <cstdlib>
#include "ksim/world/towncircle.hpp"
#include <iostream>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

using namespace ksim;

int main()
{
	try
	{
		const char* skin = std::getenv("KSIM_SKINHOUSE");
		if (skin == nullptr || *skin == 0)
		{
			std::cout << "ksim_skin_harvest: SKIPPED: KSIM_SKINHOUSE not set\n";
			return 77;
		}
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		const auto gd = sim.gameData();
		const auto data = CombatData::build(*gd, KENSHI_DATA);
		sim.enableCombat(data, CombatSettings{});
		const char* bake = std::getenv("KSIM_NAVWORLD");
		if (bake == nullptr || *bake == 0)
		{
			std::cout << "ksim_skin_harvest: SKIPPED: KSIM_NAVWORLD not set\n";
			return 77;
		}
		sim.enableNavigation(MeshNavigator::openWorld(bake, {{simlink::kHubZoneX, simlink::kHubZoneZ}}, KENSHI_DATA));
		const saveimport::Report report = saveimport::importSave(sim, *gd, *data, skin);
		AiSystem::Config ai;
		ai.registry = std::make_shared<const BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		sim.enableAi(std::move(ai));
		sim.enableDialogue(DialogueBook::build(gd));
		const std::uint64_t engival = report.selected;
		sim.setSpeechObserver([&](const Simulation::SpeechEvent& s)
		{
			if (!s.end && s.target == engival)
				std::cout << "tick " << sim.clockTick() << " speech " << s.speaker << " dialogue " << s.dialogue << " line " << s.line << " \"" << s.text << "\"\n";
		});
		sim.setDialogueObserver([&](const crime::DialogueEvent& e)
		{
			if (e.subject == engival)
				std::cout << "tick " << e.tick << " event " << e.event << " speaker " << e.speaker << "\n";
		});
		sim.setTaskLog([&](const std::string& line)
		{
			if (line.find("kidnap 254") != std::string::npos || line.find("cage ") != std::string::npos || line.find("sever") != std::string::npos)
				std::cout << "task " << sim.clockTick() << ": " << line << "\n";
		});
		SoakOptions soak;
		soak.days = 2.0 / 24.0;
		runSoak(sim, soak);
		const state::Fighter* f = sim.combat()->find(engival);
		std::cout << "engival " << engival << " at " << f->x << "," << f->z << " sneak " << f->sneak << " blood " << f->blood << " down " << f->down << " faction " << f->faction << "\n";
		for (const DialoguePick& pick : sim.dialogue()->log())
		{
			if (pick.event.target == engival)
				std::cout << "pick tick " << pick.event.tick << " event " << int(pick.event.event) << " speaker " << pick.event.speaker << " result " << int(pick.result) << " candidates " << pick.candidates << " passed " << pick.passed << "\n";
		}
		for (const auto& m : sim.combat()->members())
		{
			if (m.fighter.building == 0 || std::hypot(m.fighter.x - f->x, m.fighter.z - f->z) > 300.0)
				continue;
			const auto* site = sim.world().buildingSite(m.fighter.building);
			std::cout << "bsite " << m.fighter.building << " home " << m.fighter.homeBuilding << " site " << (site != nullptr) << " tmpl '" << (site ? site->squadTemplate : "") << "' pub " << sim.world().openToPublic(m.fighter.building) << "\n";
			break;
		}
		sim.world().ecs().each([&](const state::TownState& t)
		{
			const double d = std::hypot(t.x - f->x, t.z - f->z);
			if (d < 40000.0)
				std::cout << "town " << t.rootSid << " active " << t.activeSid << " d " << d << " owner " << t.owner.id << " r " << towncircle::of(*gd, t).radius << " xz " << t.x << "," << t.z << "\n";
		});
		for (const BuildingSite& b : sim.world().buildingSites())
		{
			const double d = std::hypot(b.cx - 73345.0, b.cz - 95141.0);
			if (d < 1500.0)
				std::cout << "site " << b.key << " " << b.buildingSid << " d " << d << " at " << b.cx << "," << b.cz << " tmpl '" << b.squadTemplate << "' layout '" << b.interiorLayout << "' interior " << b.hasInterior << " ruined " << b.ruined << " label '" << b.label << "'\n";
		}
		int near = 0;
		for (const auto& m : sim.combat()->members())
		{
			if (std::hypot(m.fighter.x - f->x, m.fighter.z - f->z) > 300.0 || m.id == engival)
				continue;
			++near;
			bool sees = false;
			for (const auto& r : m.fighter.sight)
				sees = sees || (r.target == engival && r.seen);
			std::cout << "  near " << m.id << " fac " << m.fighter.faction << " sees " << sees << " building " << m.fighter.building << " target " << m.fighter.target << " engaged " << m.fighter.engaged.size() << " state " << int(m.fighter.state) << "\n";
		}
		std::cout << near << " members within 300 u\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_skin_harvest: " << e.what() << "\n";
		return 1;
	}
}
