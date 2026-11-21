#pragma once

// The host's UI wiring: init_ui (and its init_menus), update_ui and the progress-bar level sync it shares with the logic frame's scenario start
// (kenshi-k16.2.369.10; the slice plan on kenshi-k16.2.369.2). The App state is host/app.hpp's.

#include <cstdint>
#include <functional>
#include <string>

#include "app/config.hpp"
#include "host/app.hpp"
#include "ui/build.hpp"
#include "ui/buildingwindow.hpp"
#include "ui/creator.hpp"
#include "ui/gear.hpp"
#include "ui/hud.hpp"
#include "ui/itemwindows.hpp"
#include "ui/overview.hpp"
#include "ui/prospecting.hpp"
#include "ui/research.hpp"
#include "ui/squads.hpp"
#include "ui/stats.hpp"
#include "ui/widget.hpp"

namespace host
{

void init_ui();
void update_ui(double dt);
void progress_level_sync(bool force = false);

// Defined in main.cpp, used by init_menus / init_ui / update_ui
bool hold_menu_action(int button, const std::function<void()>& run);
const char* container_refusal(const std::string& r);
const char* speed_event(float speed);
extern const cfg::Bool kTitleAtStart;
extern const double kContainerOpen;
extern const double kProducerOpen;
extern const std::string kNewGameFaction;
int move_speed();
std::string client_data_dir();
std::string job_text(const simlink::SimLinkDecoder::JobRow& row);
ui::BuildState build_state();
ui::CreatorActions creator_actions();
ui::PortraitLayers portrait_layers(uint64_t id);
ui::ProspectState prospect_state();
ui::ResearchActions research_actions();
ui::ResearchState research_state();
ui::SquadsActions squads_actions();
ui::SquadsState squads_state();
void begin_session_load();
bool session_load_expected();	// this run will show a session load screen (session.loading_screen, scripted or not)
void camera_sidecar_delete(const std::string& name);
void camera_sidecar_save(const std::string& name);
void cont_close(uint64_t id);
void cont_close_all();
void creator_frame(double dt = 0);
void debug_tree_frame();
void debug_tree_open(uint8_t kind, uint64_t id);
void dialogue_frame();
void discovery_log();
void crime_log();
void lock_too_hard_log();
void map_places(ui::MapState& ms);
void map_view_frame();
void menu_pause(bool on);
void name_labels_frame();
void ng_begin(const std::string& id);
void ng_load(int i);
void open_creator(const std::string& what);
void prospect_paint();
void set_camera_target(const char* reason);
void shortcut(const std::string& action);
void speech_frame();
void start_local_server();

}
