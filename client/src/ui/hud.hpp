#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/cast.hpp"
#include "ui/jobs.hpp"
#include "ui/widget.hpp"

// The in-game HUD (docs/UI.md "First screens"): time / speed panel, squad portrait bar, combat toggles for the selection. It knows nothing about
// SIMLINK: the host fills a HudState from its sim view every frame and turns HudActions into orders. kui_demo drives it with made-up data.
namespace ui
{
	struct HudPart
	{
		std::string name;	// e.g. "Left Arm"
		float health = 0, stun = 0, max = 100;	// health - stun is what counts (KO at 0); health goes down to -3 x max
		float bandage = 0, splint = 0;	// the original's bandaging and jury-rigging points (yellow / white layers); 0 until the wire has them
		bool robot = false;	// a replaced (robotic) limb: drawn blue
		bool crushed = false;	// a stump or a crushed limb: the crushed overlay
	};

	// The selection's order ticks: the original's OrdersPanel order (Kenshi_MainPanel.layout), then RUN (the bottom-centre row).
	enum class HudToggle { kBlock, kHold, kPassive, kJobs, kRanged, kTaunt, kSneak, kRun };
	constexpr int kHudToggles = 8;

	struct HudMember
	{
		uint64_t id = 0;
		std::string name;	// empty: "#id"
		bool selected = false;
		float health = 1;	// 0..1 of the whole body (bar)
		bool down = false;	// KO or dead: dimmed portrait
		bool toggles[kHudToggles] = {};	// by HudToggle: a tick is checked when every selected member has it
		std::vector<HudPart> parts;	// the medical panel, in anatomy order (up to 9); empty: no medical data yet
		float blood = -1;	// < 0: unknown
		float bloodMax = -1;	// the race's max blood, the Blood bar's scale; < 0: unknown, the value only (the wire has none yet)
		float bleed = 0;	// blood lost a second: one '<' per 0.05 on the Blood row
		float hunger = -1;	// 0..3 (the original's; Hungry below 2, Starving below 1); < 0: unknown, "-" (the wire has none yet)
		bool fed = false;	// food being digested: '>' on the Hunger row
		bool resting = false;	// in a bed: no falling '<'
		bool robot = false;	// a robotic race: "Oil" for Blood, Hunger "-", every part blue
		std::string status;	// the medical panel's status box (HealthText: "Normal", "Discovered!"); empty: hidden
		std::string statusColour;	// a palette name ("TipBad" for Discovered!); empty: the box's ink
		std::string statusTip;	// the status box's hover text, a health sentence (the tip box); empty: none
		dl::TexId portrait = 0;	// the rendered head (Portraits::get) under the host's id; 0: none yet, the frame shows the initial
		float portraitUv[4] = { 0, 0, 1, 1 };	// u0, v0 (top), u1, v1 (bottom)
		// The portrait's game-state layers (ui::PortraitLayers, kenshi-rwh.112), each the host's: the mark over the face (the original: Eaten >
		// Prison > Slave, Slave = wears shackles), the overlay behind it (Combat > Hunger), the face dimmed to 0.25 (carried, in a bed or put in
		// something), the pulsing Hurt background.
		PortraitMark mark = PortraitMark::kNone;
		PortraitBack back = PortraitBack::kNone;
		bool dim = false;
		bool hurt = false;
	};

	// A tab over the portrait bar: one of the player's squads (darkui_squads_0926: EXPEDITION / TOWN). Picking one shows its members in the bar.
	struct HudSquadTab
	{
		uint64_t id = 0;
		std::string name;	// shown in capitals
	};

	// A row of the status panel: label left, value right.
	struct HudLine
	{
		std::string label, value;
		std::string colour = {};	// the value's palette name ("StatUp" for a modified stat); empty: the panel's ink
		std::string tip = {};	// its hover text (the tip box); empty: none
		bool operator==(const HudLine&) const = default;
	};

	// A line of a hover text: the label left (it wraps when there is no value), the value right-aligned; both empty: a gap. Colours are palette
	// names or "#rrggbb"; empty: the tip's ink (the value's: the label's).
	struct HudTipLine
	{
		std::string label, value = {};
		std::string labelColour = {}, valueColour = {};
		bool operator==(const HudTipLine&) const = default;
	};

	// The status panel, bottom left (Kenshi_MainPanel.layout InformationPanel; kenshi-inspect ui_captures/hud_panel16.md), for the character the
	// host picks (the selection's first). Every text is the host's; an empty one leaves its place blank or hidden. Colours are palette names:
	// HudGood / HudBad / HudNeutral / HudTag / HudFaint are the original's #a8b774 / #59231a / #afa68b / #492620 / #444444.
	struct HudInfo
	{
		bool valid = false;	// false: no panel
		// The tag row over the plate (BountyPanel; kenshi-inspect hud_panel16.md (5), hud16_static.md 5): one tag at a time, the host picks the
		// first branch that applies, in this order, and fills these. tag empty: the row is hidden.
		//   War Leader / Leader     tag "War Leader" / "Leader", HudTag; tip: what a leader is.
		//   Kidnapped               tag "Kidnapped", HudTag; tip: the kidnapping, who will help.
		//   Diplomatic Status       tag "Diplomatic Status", HudTag; tip: attacking them angers their faction.
		//   slave state 1 / 2 / 3   tag "[SLAVE]" / "[ESCAPED SLAVE]" / "[EX-SLAVE]", HudBad, the brackets too (live, Dark UI: the user's
		//                           kenshi-8x6 shot 23; hud16_static reads only the inner text); tip: whose slave (the owner faction), states 2 and 3 add "Situation:" Seen! (HudBad) / Undiscovered
		//                           (HudGood), "Looks like a slave:" N%, "Time remaining:" N hours; then a HudTag hint line; then the
		//                           "Bounty:" lines when wanted.
		//   committing a crime      tag "Committing Crime!", HudTag, tagValue "(N)" (the crime's counter, right-aligned); tip: the crime's name,
		//                           unseen / caught, the local bounty when there is one.
		//   escaped prisoner        tag "[ESCAPED PRISONER]", HudTag; tip: what guards do, "Expires in:" N hours.
		//   bounty                  tag "Bounty:", HudTag, tagValue "c.N": the local faction's bounty in HudNeutral, else the largest other
		//                           faction's in HudFaint; tip: one line per faction (faction left, "c.N" right: HudNeutral for the local
		//                           faction, HudFaint for others), "Wanted for:" and the crimes, the imprisonment, the expiry ("Never expires"
		//                           when notorious; "Not wanted here" when no local bounty).
		//   Kidnapped (again)       as above, when nothing else applies.
		std::string tag, tagColour;	// the left text and its ink (empty: the row's); the rest after blank
		std::string name, faction, nameColour;	// the name plate: name left, faction right
		std::vector<HudLine> state;	// StateText: "State:" "Defensive", "Goal:", "Encumbrance:", and the skill row in use (or none)
		std::vector<HudLine> extended;	// ExtendedStatsText, shown when extended: ATHLETICS XP, STRENGTH XP, TOUGHNESS XP, KO POINT, RUN SPEED, HUNGER RATE
		std::vector<HudLine> stats;	// StatsText by weapon class: "MARTIAL ARTS:" "24 (+4)" (HudGood; "N (-M)" HudBad) ...
		bool blank = false;	// valid: drawn as empty frames, no tag, plate text, state or stats (an unloaded inspect focus)
		std::string tagValue = {}, tagValueColour = {};	// right-aligned ("(3)", "c.500"); its ink (empty: tagColour's)
		std::vector<HudTipLine> tagTip = {};	// the tag's hover text; empty: none
	};

	// A selected building (kenshi-inspect ui_captures/genbuilding_*.png): the same status and medical panels with other contents. The plate
	// holds its name and owner; the medical status box its state ("No workers", "Output full"); the state area rows ("Town:") over full-width
	// buttons ([UPGRADE], Dismantle); the stats panel rows (Workers, Efficiency, Max efficiency, a mine's resource quality) over ticks (Battery,
	// Power); the medical bars its bars (Condition, Power, a material's have/need while under construction), the rest of the 9 drawn as empty
// slats. Every text, colour and fill is the host's (kenshi-inspect ui_captures/hud_building.md, hud30_static.md); a colour is a palette name
// or "#rrggbb" (the mine quality's ink is computed host-side).
	struct HudBuildingBar
	{
		std::string label, right, value;	// the label centred in the bar, right at its right end ("Inactive"), value in the column beside
		float fraction = 0;	// 0..1 of the bar filled
		std::string colour = {};	// the fill's layer: BarLayerGreen (empty: the default; Condition, Power), BarLayerYellow (a generator's, a battery's) ...
		bool operator==(const HudBuildingBar&) const = default;
	};

	struct HudBuildingTick
	{
		std::string id, text;	// "battery" "Battery"
		bool checked = false;
		bool operator==(const HudBuildingTick&) const = default;
	};

	struct HudBuildingButton
	{
		std::string id, text;	// "dismantle" "Dismantle"
		std::string colour = {};	// the text's palette name ("BadBright"); empty: the button's
		std::string confirm = {};	// non-empty: asked first (a Yes / No prompt with this text); Yes sends it
		bool tag = false;	// a plain centred line instead of a button ("[PAUSED]")
		bool operator==(const HudBuildingButton&) const = default;
	};

	struct HudBuildingInfo
	{
		bool valid = false;	// true: it takes the status and medical panels instead of the character's
		uint64_t id = 0;
		std::string name, faction;	// the plate
		std::string status, statusColour;	// the status box; empty: hidden
		std::vector<HudLine> state;	// "Town:" "Desert Paradise"
		std::vector<HudBuildingButton> buttons;	// under the state rows: [UPGRADE], [DOWNGRADE], PAUSE CONSTRUCTION, [PAUSED], Dismantle
		std::vector<HudLine> stats;	// "Workers:" "0/2" (HudBad) ...
		std::vector<HudBuildingTick> ticks;	// under the stats rows
		std::vector<HudBuildingBar> bars;	// up to 9 (Hud::kLimbs)
		bool operator==(const HudBuildingInfo&) const = default;
	};

	// Spectator mode (client coordinator 2026-11-07): the character being watched, the "Watching" panel's text. Every text is the host's.
	struct HudWatch
	{
		uint64_t id = 0;	// 0: nobody ("Watching: nobody")
		std::string name, faction;	// an empty name: "#id"
		std::string goal, task;	// one clipped line each, the full text in the tip; empty: "-" (the task waits on the sim's AI rows)
		bool following = false;	// the camera follows it: "following" and STOP
		bool operator==(const HudWatch&) const = default;
	};

	struct HudState
	{
		bool clockValid = false;
		uint32_t day = 0;
		float hour = 0;		// 0..24
		float speed = 1;	// game speed multiplier
		bool paused = false;
		std::vector<HudMember> squad;	// the player's characters, in portrait order (the active squad's)
		// The inspect focus (an LMB on an NPC): the medical panel draws it instead of the first selected member; no parts: an empty frame
		// (unloaded). Not selected, so the portraits ignore it.
		HudMember focus;
		bool focusSet = false;
		std::vector<HudSquadTab> squadTabs;	// none: no tab row
		uint64_t activeSquad = 0;	// its tab is checked
		HudInfo info;
		HudBuildingInfo building;	// valid: shown instead of info and the medical panel
		bool toggleEnabled[kHudToggles] = { true, false, false, false, false, false, true, true };	// by HudToggle; off: drawn disabled (no order yet)
		bool medicEnabled = false, rescueEnabled = false, prospectEnabled = true;	// the OrdersPanel's buttons
		bool jobsValid = false;	// the jobs list shows (the host: one character selected, say)
		std::vector<Job> jobs;	// its jobs in priority order
		int64_t money = -1;	// the player faction's cats, in the money box; below 0: hidden (the wire has no money outside TRADE yet)
		// The floor diamond, over the game speed buttons' left end while the host shows it (WorldBuildings::floorStatus; the host keeps it up):
		// floor counts from 0, as does the label (the ground reads "Floor 0", as in the original; user 2026-11-04); floorUp / floorDown: the
		// arrow can step. floorName: the building R / F act on (the lead's, or the one the cursor peeks into), in the tooltips. No target (both
		// arrows off, no name): the label reads "Floor", no number.
		bool floorValid = false;
		int floor = 0;
		std::string floorName;
		bool floorUp = true, floorDown = true;
		// The primary selection's move speed, the speed diamond's picture (kenshi-inspect ui_captures/hud_floor_speed.md): 0 walk, 1 jog,
		// 2 run (the default), 3 group (everyone at the slowest member's pace).
		int moveSpeed = 2;
		// Direct drive: the camera drives one of your own characters (the host: app.follow on a player character). Both diamonds draw
		// active (a gold rim, the face lit); the host draws no tag of its own (k16.3.32).
		bool driving = false;
		// Spectator mode: no player character. Hidden: the squad bar, its tabs, the order ticks and OrdersPanel, the money and build boxes, the
		// move speed diamond, INV and SQD. Shown: the Watching panel (top centre; PAUSED moves under it) and the CAST window (opens on its own
		// each time spectating turns on; the X hides it until CAST). The status and medical panels draw the focus as usual.
		bool spectating = false;
		HudWatch watch;
		std::vector<HudCastRow> cast;	// the CAST window's rows, any order (it sorts by name)
		// A playtest run is on: the NOTES button (left of the ShortcutsPanel, at the screen's foot) shows, checked while notesOpen.
		bool playtestRun = false, notesOpen = false;
	};

	struct HudActions
	{
		std::function<void(uint64_t id, bool add)> select;	// portrait click (add: Shift held)
		std::function<void(uint64_t id)> focus;			// portrait double click; the host resolves it through hints_for_portrait (a player's: select and centre; anyone else's: inspect and follow)
		std::function<void(HudToggle which, bool value)> toggle;	// a tick for the selection (the host maps it to its order)
		std::function<void()> stop;			// STOP: order 4 for the selection
		std::function<void()> prospect, medic, rescue;	// the OrdersPanel's buttons
		std::function<void(bool paused)> pause;			// order 40
		std::function<void(float speed)> setSpeed;		// order 41
		std::function<void(uint64_t squad)> setActive;	// a squad tab (UNSURE until kinspect: a plain click, nothing else)
		std::function<void(int delta)> floor;	// the floor diamond: +1 up, -1 down (the host steps and clamps it, as R / F)
		std::function<void(int mode)> moveSpeed;	// the speed diamond: the mode its arrow picks (left -1, right +1, wrapping 0..3)
		// A ShortcutsPanel button: the key action it stands for (Hud::kShortcuts), so the host handles it as that key.
		std::function<void(const std::string& action)> shortcut;
		std::function<void(bool extended)> extend;	// the status panel's ^ / v button (the original plays Stats_Up / Stats_Down)
		std::function<void(uint64_t job)> removeJob;	// the jobs list's X
		std::function<void(uint64_t job, int index)> moveJob;	// a jobs row dragged to its new place in the order
		std::function<void(uint64_t building, const std::string& button)> buildingButton;	// a HudBuildingButton's id
		std::function<void(uint64_t building, const std::string& tick, bool on)> buildingTick;	// the host decides; the next state shows it
		std::function<void(uint64_t id)> watch;	// spectating: a cast row clicked (the inspect focus)
		std::function<void(uint64_t id)> follow;	// spectating: the camera follows it (a cast row's double click, FOLLOW); 0: stop (STOP)
		std::function<void()> notes;	// the NOTES button: the host toggles the notes window (opening it: NotesWindow::focusEntry)
	};

	class Prompt;
	class Diamond;

	class Hud
	{
	public:
		static constexpr float kSpeeds[3] = { 1, 2, 5 };	// the speed buttons after pause: the original's 1 / 2 / 5 (TimeSpeedButton2..4 user data, live; kenshi-inspect audio_load_pause.md)
		// The ShortcutsPanel's buttons, top to bottom (INV STA MAP TCH SQD HLP): their key actions.
		static constexpr const char* kShortcuts[6] = { "toggle_inventory", "toggle_stats", "toggle_map", "toggle_research", "toggle_squads", "toggle_help" };

		explicit Hud(HudActions actions);
		~Hud();
		void update(const HudState& s);	// every frame, before ui::frame
		bool shown() const;
		void show(bool on);
		// For scripted tests and tutorials; null when out of range.
		Widget* portrait(size_t i) const;
		Widget* portraitMark(size_t i) const;	// the Image over its face (HudMember::mark)
		const PortraitFace* portraitFace(size_t i) const;	// its layers
		const Label* portraitInitial(size_t i) const;	// the initial shown without a portrait image
		Widget* speedButton(int i) const { return i >= 0 && i < 4 ? speed_[i] : nullptr; }	// 0 pause, 1..3 kSpeeds
		Widget* toggleButton(HudToggle t) const { return toggle_[(int)t]; }
		Widget* ordersPanel() const { return orders_; }
		Widget* medicButton() const { return medic_; }
		Widget* shortcutsPanel() const { return shortcuts_; }
		Widget* rescueButton() const { return rescue_; }
		Widget* prospectButton() const { return prospect_; }
		Widget* squadTab(size_t i) const { return i < tabs_.size() ? tabs_[i] : nullptr; }
		Widget* hintBox() const { return hintBox_; }	// the fixed tip box (ui.hud_tips=box)
		const std::string& hintText() const;
		Widget* medicalPanel() const { return medical_; }	// shown for the focus, else the first selected member with parts
		const std::string& medicalName() const { return medicalName_; }	// whose panel it is (not drawn: the name is on the status panel's plate)
		Widget* medicalBar(int i) const;
		Widget* medicalValue(int i) const;
		Label* medicalRight(int i) const;	// a building bar's right text
		float medicalFill(int i) const;	// the bar's fraction
		bool buildingShown() const { return building_; }
		Prompt* buildingPrompt() const { return prompt_.get(); }	// a button's confirm; null until the first
		Button* buildingButton(size_t i) const { return i < bldButtons_.size() ? bldButtons_[i] : nullptr; }
		Button* buildingTick(size_t i) const { return i < bldTicks_.size() ? bldTicks_[i] : nullptr; }
		Widget* medicalStatus() const { return status_; }	// the status box over the medical panel (a Panel holding a Label)
		Widget* infoPanel() const { return info_; }
		Widget* moneyBox() const { return money_; }
		Button* buildButton() const { return build_; }	// the BuildPanel: toggle_build through HudActions::shortcut
		const std::string& moneyText() const { return moneyAmount_->text; }
		bool moneyShown() const { return moneyAmount_->visible; }	// the money row; the box stays for the day and time
		const std::string& dayText() const { return day_->text; }
		const std::string& clockText() const { return clock_->text; }
		Widget* infoTag() const { return tag_; }
		std::string infoTagText() const;	// the tag row as drawn: "[SLAVE]", "Committing Crime! (3)"
		Color infoTagInk(bool value) const;	// the tag's (inside the brackets) or its value's ink
		const std::string& infoTagTip() const { return tag_->tooltip; }	// the hover text's markup
		Widget* infoPlate() const { return plate_; }
		const std::string& infoName() const { return name_->text; }
		size_t infoRows() const;	// the state, extended and stats rows
		Widget* infoStats() const { return statsPanel_; }
		Widget* infoButton() const { return extButton_; }
		// The status panel's ^ button: the tag, plate and state slide up over kSlideTime to show the extended rows above the stats. Starts
		// collapsed, one flag for every character, not saved (as the original).
		static constexpr double kSlideTime = 0.5;
		bool extended() const { return extended_; }
		float slide() const { return slide_; }	// 0 collapsed .. 1 extended, eased
		Widget* infoExtended() const { return extPanel_; }	// the sliding panel: state rows over the extended rows

		Widget* pausedPanel() const { return pausedPanel_; }	// for scripted tests
		Widget* shortcutPanel() const { return shortcuts_; }
		Widget* timePanel() const { return time_; }
		// The control hints panel's dock (ui::dockHints, kenshi-k16.2.294.10.1): w x h pixels, its right edge on the cluster's (the BuildPanel's),
		// its foot one panel gap (the money box to the BuildPanel, from the layout) over the highest shown cluster panel under it: the speed /
		// money row, or a diamond where the panel is wide enough to reach it. Empty while the HUD is hidden.
		Rect hintsDock(float w, float h) const;
		float panelGap() const;	// the gap the bottom-right panels keep between each other, pixels
		Diamond* floorDiamond() const { return floor_; }
		Diamond* moveSpeedDiamond() const { return moveSpeed_; }
		bool speedAtlas() const { return speedAtlas_; }	// the speed diamond shows the atlas's icons (ui.speed_icons atlas and a themed atlas)
		JobList* jobList() const { return jobs_; }
		Widget* shortcutButton(int i) const { return i >= 0 && i < 6 ? shortcut_[i] : nullptr; }	// kShortcuts order
		Widget* watchPanel() const { return watch_; }	// spectating: top centre
		const std::string& watchText() const;	// its first line
		const std::string& watchGoalText() const;	// its second line (goal and task)
		Button* followButton() const { return followBtn_; }	// FOLLOW / STOP
		Button* castButton() const { return castBtn_; }
		Button* notesButton() const { return notesBtn_; }	// in its own panel (notesButton()->parent())
		CastWindow* castWindow() const { return cast_.get(); }	// null until spectating first turns on
		Widget* squadBar() const { return squad_; }
		Widget* toggleRow() const { return toggles_; }	// RUN, STOP
		Widget* buildPanel() const { return build_ ? build_->parent() : nullptr; }

	private:
		void showStatus(const std::string& text, const std::string& colour);
		struct Portrait;
		HudActions act_;
		Widget* top_ = nullptr;	// all HUD widgets hang under this one child of ui::root()
		Widget* pausedPanel_ = nullptr;	// "PAUSED", top centre while paused
		Widget* time_ = nullptr;	// the game speed buttons, bottom right over the OrdersPanel
		Widget* diamonds_ = nullptr;	// left of them: the floor diamond over the move speed diamond
		Diamond *floor_ = nullptr, *moveSpeed_ = nullptr;
		bool speedAtlas_ = false;
		Widget* shortcuts_ = nullptr;
		Widget* money_ = nullptr;
		Button* build_ = nullptr;
		Label* moneyLabel_ = nullptr;
		Label* moneyAmount_ = nullptr;
		Label* day_ = nullptr;	// "Day: N" under the money
		Label* clock_ = nullptr;	// "hh:mm" right of it
		JobList* jobs_ = nullptr;
		Button* shortcut_[6] = {};
		Button* speed_[4] = {};
		Box* squad_ = nullptr;
		Box* tabRow_ = nullptr;
		std::vector<Button*> tabs_;
		std::vector<uint64_t> tabIds_;
		std::vector<std::string> tabNames_;
		Box* toggles_ = nullptr;	// the bottom-centre row: RUN, STOP
		Widget* orders_ = nullptr;	// OrdersPanel, bottom right
		Widget* ticks_ = nullptr;	// its tick column
		Button* toggle_[kHudToggles] = {};
		Button *medic_ = nullptr, *rescue_ = nullptr, *prospect_ = nullptr;
		std::vector<Portrait*> portraits_;
		struct Limb;
		static constexpr int kLimbs = 9;	// the original panel's LifeBar1..9
		Widget* medical_ = nullptr;
		std::string medicalName_;
		Panel* hintBox_ = nullptr;
		RichText* hint_ = nullptr;
		Panel* status_ = nullptr;
		Label* statusText_ = nullptr;
		class Rows;
		class TagRow;
		Widget* info_ = nullptr;
		Panel *tag_ = nullptr, *plate_ = nullptr, *statsPanel_ = nullptr;
		TagRow* tagRow_ = nullptr;
		Label *name_ = nullptr, *faction_ = nullptr;
		Rows *state_ = nullptr, *ext_ = nullptr, *stats_ = nullptr;
		Widget* slider_ = nullptr;	// tag, plate and extPanel_, moved up by the slide
		Panel* extPanel_ = nullptr;
		Button* extButton_ = nullptr;
		bool extended_ = false;
		float slide_ = 0, slideFrom_ = 0;
		double slideAt_ = -1;	// ui::now() when the button was pressed; < 0: at rest
		void place();	// the slider's place for slide_
		void updateInfo(const HudInfo& info);
		void updateBuilding(const HudBuildingInfo& b);
		bool building_ = false;
		uint64_t buildingId_ = 0;
		Box *bldButtonBox_ = nullptr, *bldTickBox_ = nullptr;
		std::vector<Button*> bldButtons_, bldTicks_;
		std::vector<HudBuildingButton> bldButtonDefs_;
		std::vector<HudBuildingTick> bldTickDefs_;
		std::unique_ptr<Prompt> prompt_;
		Limb* limbs_[kLimbs] = {};
		Panel* watch_ = nullptr;
		Label *watchName_ = nullptr, *watchGoal_ = nullptr;
		Button *followBtn_ = nullptr, *castBtn_ = nullptr, *notesBtn_ = nullptr;
		std::unique_ptr<CastWindow> cast_;
		bool castOpen_ = false, spectating_ = false;
		void updateSpectator(const HudState& s);
		HudState last_;
	};
}
