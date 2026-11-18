#include "serverrows.hpp"
#include <algorithm>
#include "config.hpp"

namespace
{
// Server-setting rows (docs/COMBAT_OPTIONS.md, kenshi-h2v.22.7.1): the options screen lists them like any cfg key; start_local_server() passes each
// as a OpenKenshi-server flag unless session.server_args already names it (a remote server has its owner's flags). Help = the doc's config-screen text.
const cfg::Bool kOptAttackSlots{ cfg::Group{ "Combat" }, "combat.fix_extra_attack_slots", true, "Fix extra attack slots. Large creatures can be attacked by more than one character at a time. In original Kenshi, the extra attackers never rotate out: whoever gets in first keeps attacking until the fight ends, and a slot can stay blocked by a knocked-out character. On: extra attackers take turns like the first one. Off: original behaviour. Takes effect when the next game starts." };
const cfg::Bool kOptRunaway{ cfg::Group{ "Skills" }, "xp.no_runaway_skills", true, "Stop runaway skills. In original Kenshi, a skill above 100 (Tinfist's unarmed, or one pushed there by an exploit) keeps rising with use, faster the higher it gets. On: skills above 100 stay where they are. Off: original behaviour. Takes effect when the next game starts." };
const cfg::Bool kOptFollowHeading{ cfg::Group{ "Squads and AI" }, "squad.follow_heading_fix", false, "Squad turn-around fix. In original Kenshi, when the squad leader turns straight back, followers keep heading for where the leader was going and overshoot before coming back. On: followers turn with the leader. Off: original behaviour. Takes effect when the next game starts." };
const cfg::Bool kOptTokenSpeeds{ cfg::Group{ "Squads and AI" }, "ai.token_speeds", false, "Fast off-screen travel. In original Kenshi squads that nobody is looking at stand still. On: they cross the world at about 55 units per second (25 inside a town), so a caravan or raid arrives in minutes instead of hours. Off: slow walk. Takes effect when the next game starts." };
const cfg::Bool kOptOffMedical{ cfg::Group{ "Off-screen world", "", "Game" }, "world.offscreen_medical", false, "Living off-screen. In original Kenshi, characters that are not near you are frozen: wounds do not bleed, nobody dies, and bodies vanish when you walk away. On: wounded characters keep bleeding, heal or die where they are, squadmates with a first-aid kit patch each other up, and the dead stay lootable until they rot (12 game hours). Off: original behaviour. Takes effect when the next game starts." };
const cfg::Bool kOptOffFights{ cfg::Group{ "Off-screen world", "", "Game" }, "world.offscreen_fights", false, "Fights off-screen. In original Kenshi, hostile squads that nobody is looking at pass through each other. On: when two hostile squads meet off-screen they fight a quick, strength-weighted battle: the loser loses half its people and runs home, the winner may lose one. Off: original behaviour. Takes effect when the next game starts." };
const cfg::Bool kOptVictoryAi{ cfg::Group{ "Off-screen world", "", "Game" }, "world.campaign_victory_ai", false, "Raiders stay after winning. In original Kenshi, a bug stops big faction raids (mostly Holy Nation assaults, but also Band of Bones, Southern Hive kidnappers, the Empire and Kral's Chosen) from ever using the 'victory: occupy the town' orders their data gives them: they win the fight, then always march home, however it went. Only bandit-style raiders (Dust Bandits, Crab Raiders, Rebel Farmers, Black Dragon Ninjas and similar) ever moved in. On: the winners of those raids stay and occupy the town until you kill them all; the town stays yours. Off: original behaviour. Takes effect when the next game starts." };
const cfg::Float kOptRetargetHold{ cfg::Group{ "Combat" }, "combat.retarget_hold_s", 0.0, 0.0, 10.0, "Target switching delay. In original Kenshi a fighter re-evaluates whom to attack every moment, so in a crowd it can swap targets very often. A delay makes fighters stick with their target for that many seconds before they may swap. 0: original behaviour. Takes effect when the next game starts." };
struct Row { const char* flag; const cfg::Bool* on; };
const Row kRows[] = {
	{ "--fix-extra-attack-slots", &kOptAttackSlots }, { "--no-runaway-skills", &kOptRunaway }, { "--follow-heading-fix", &kOptFollowHeading },
	{ "--token-speeds", &kOptTokenSpeeds }, { "--offscreen-medical", &kOptOffMedical }, { "--offscreen-fights", &kOptOffFights },
	{ "--campaign-victory-ai", &kOptVictoryAi },
};
}

void serverrows::append_flags(std::vector<std::string>& args)
{
	const auto named = [&](const char* flag) { return std::find(args.begin(), args.end(), flag) != args.end(); };
	for (const Row& r : kRows)
	{
		if (named(r.flag)) { continue; }
		args.push_back(r.flag);
		args.push_back(r.on->get() ? "on" : "off");
	}
	if (!named("--combat-retarget-hold") && kOptRetargetHold.get() > 0)
	{
		args.push_back("--combat-retarget-hold");
		args.push_back(std::to_string(kOptRetargetHold.get()));
	}
}
