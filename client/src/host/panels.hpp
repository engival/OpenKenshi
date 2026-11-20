#pragma once

// The host's panels: the creator's state, the stats window and status panel rows, the grid inventory (gear, containers, the producer's machine
// window and focus panel), the item windows' actions and the gear-order queue (kenshi-k16.2.369.11; the slice plan on kenshi-k16.2.369.2). The App
// state is host/app.hpp's.

#include <cstdint>

#include "app/config.hpp"
#include "host/app.hpp"
#include "ui/buildingwindow.hpp"
#include "ui/creator.hpp"
#include "ui/gear.hpp"
#include "ui/hud.hpp"
#include "ui/itemwindows.hpp"
#include "ui/stats.hpp"

namespace host
{

// Defined in main.cpp, used by the panels
extern const cfg::Text kSpeedUnit;
float speedScale();
uint64_t cont_actor(uint64_t owner);

bool container_state(const App::OpenCont& c, ui::ContainerState& s);
ui::BuildingWindowActions machine_actions();
ui::BuildingWindowState machine_state();
ui::CreatorState creator_state();
ui::GearState gear_state(uint64_t owner);
ui::ItemWindowsActions item_actions();
void stats_info(const simlink::CharStats& cs, ui::HudInfo& info, bool playerFaction);
void stats_window(uint64_t id, ui::StatsState& s);
void gear_orders_clear();
void gear_orders_frame();
ui::HudBuildingInfo machine_panel();

}
