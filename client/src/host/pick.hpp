#pragma once

// The host's picking and context menu (kenshi-k16.2.369.4; the slice plan on kenshi-k16.2.369.2): the click ray's hit, the item name labels,
// pick_at / pick_screen, the hold-RMB menu and the F12 pick report. Moved out of main.cpp unchanged; the state they read is host/app.hpp's.

#include <cstdint>
#include <string>
#include <vector>

#include "dev/devhooks.hpp"
#include "host/app.hpp"
#include "input/pick.hpp"
#include "ui/menus.hpp"

namespace host
{

// A collision shape the click ray passed over and why (kenshi-k16.2.259): the F12 shot's pick report and `dump pick` name each.
struct PickSkip { std::string name; double at[3]; std::string why; };

constexpr double kPropBoxSlack = 0.5;	// how far past a container's drawn box a click still counts as the container (the sim's collision proxy for the piece sits a thousandth of a unit nearer than its mesh box, k16.2.239.1)

// Ground item name labels (the original's DropItemLabels; kenshi-inspect picker.md): every item within kLabelRange shows one while ALT (the
// original's `highlight` key) is held, otherwise only the hovered item's, which stays while the mouse is on the label itself. The box is the
// hit area, edges included: kLabelHeight of the screen height high, the text plus kLabelPad wide, standing on the item position + 4 y, clamped
// on screen. LMB on a label picks the item up (Shift queues), and the world pick under a label is skipped. With all labels shown, overlaps
// are pushed apart as the original's 140163b70 does (spread_labels). Nearest item last, so it draws on top and is hit first.
struct ItemLabel { uint64_t id; float x, y, w, h; std::string text; double dist; };
constexpr float kLabelHeight = 0.0259f, kLabelPad = 6.0f;	// pad: pixels at 720p (Cup 27, Hemp 38, Wooden Bowl 80 wide)
constexpr double kLabelRange = 707, kLabelLift = 4, kContainerLabelLift = 10, kProducerLabelLift = 20;	// producers: MINE	// containers: MINE (above a crate; the original's container label is not measured)

// The pick chance the hover tag shows: order 99 on the cage key asked once per (cage, acting character); its MENU_ENTRIES carries the chance.
struct CageHover { uint64_t cage = 0, actor = 0; uint32_t seq = 0; float chance = -1; };

// The held right-click's menu: the character's answer (bit 0 Talk To) and, on a locked cage's prisoner, the cage's (order 99 on the cage key: bits 16 pick
// lock, 17 use tools, 18 use strength and the pick chance) are both asked; it opens when everything asked is in or at the deadline.
struct PendingMenu
{
	bool on = false;
	Pick p;
	double x = 0, y = 0, deadline = 0;
	uint32_t seq = 0, cageSeq = 0;
	uint64_t cage = 0;
	std::string cageTitle;
	bool charDone = true, cageDone = true;
	int entries = -1, cageEntries = -1;	// -1: not answered (Talk offered; all three cage rows offered)
	float chance = -1;
};

// Hold RMB on a target: the original's action menu (FEATURES "RMB default action"). A title row names the target; actions the server
// does not do yet are shown disabled. kHoldMenu: the original's, read live by kenshi-inspect.
constexpr double kHoldMenu = 0.2;

void build_mvp(float mvp[16], float nearZ, float farZ);	// main.cpp
bool cut_hidden(const double pos[3]);	// main.cpp
bool labels_shown();	// main.cpp
void mouse_ray(double mx, double my, double dir[3]);	// main.cpp
double camera_near();	// main.cpp
extern std::string hoverRule;	// main.cpp
void order_sent(bool sent);	// main.cpp
bool open_container(uint64_t id, bool queue);	// main.cpp
bool use_producer(uint64_t id, bool queue);	// main.cpp
bool open_body(uint64_t id, bool queue);	// main.cpp
bool ray_ground(const double origin[3], const double dir[3], double hit[3]);	// main.cpp
bool see_through(const double p[3], double bottom = 1e30, std::string* why = nullptr);	// main.cpp
bool ray_hit(const double origin[3], const double dir[3], double hit[3], bool* building, const double* eye = nullptr, std::vector<PickSkip>* skipped = nullptr, float* hitBox = nullptr);
double ground_under(double x, double z, const char** on);
float label_text_width(const std::string& text);
float label_height();
std::vector<ItemLabel> item_labels(bool all, const float* cam = nullptr);	// cam: the mvp to project with (record_ui: the packet's); null: build_mvp
uint64_t label_at(double mx, double my);
Pick pick_at(const double dir[3], std::vector<PickSkip>* skipped = nullptr);
Pick pick_screen(double mx, double my, std::vector<PickSkip>* skipped = nullptr);
std::vector<ui::Menu::Item> menu_items(const Pick& p, int entries = -1);
void menu_query_frame();
void rmb_hold_menu();
std::string pick_owner_report(const Pick& p, double mx, double my, const std::vector<dev::PickSkip>& skipped);	// the F12 sidecar's owner / primitive / rule lines

extern CageHover cageHover;
extern PendingMenu menuPending;

}
