#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "ui/layout.hpp"
#include "ui/widget.hpp"

// The debug panel (docs/UI.md; the user, 2026-11-05: the old key list (Ctrl+F1, now F8) was a wall of text): a movable DEBUG window, top left. On its
// left (client/data/ui/debug.layout; resizable) the host's debug toggles (a tick box each, its key beside it) and the debug scenes (offline only)
// in a list that scrolls when they outgrow the frame, under it the host's extra rich text, always in view (k16.3.45); beside them the commands, time and weather. No key list (k16.2.352): the Options Keys tab lists every binding.
namespace ui
{
	struct DebugToggle
	{
		std::string id, label;
		bool on = false;
		std::string key;	// the keys:: action whose chord shows beside it; "" none
		bool operator==(const DebugToggle&) const = default;
	};

	// Where a command shows (k16.2.364): kWindow: a WINDOWS button (opens a window); kAction: an ACTIONS button; kCharacter: not a column
	// button but one on the CHARACTER toolbar (acts on the selection; k16.2.393.1: a window of its own, toggled by the panel's "Character...").
	enum class DebugGroup { kAction, kWindow, kCharacter };

	struct DebugCommand
	{
		std::string id, label;
		bool enabled = true;
		DebugGroup group = DebugGroup::kAction;
		bool operator==(const DebugCommand&) const = default;
	};

	struct DebugState
	{
		std::vector<DebugToggle> toggles;
		std::vector<std::string> scenes;	// scene names; empty: the list hides (the host's, offline only)
		std::string extra;	// rich text under the toggles (the squad overlay's sources); "": hidden
		std::vector<DebugCommand> commands;	// a button each, in order
		std::vector<std::string> weathers;	// the chooser's names, after its own "auto" row
		std::string weather;	// the current one; "auto" or "": normal weather
		bool weatherEnabled = false, timeEnabled = false;	// off: the chooser / the time buttons and the dial greyed, no clicks
		float hour = -1;	// the game clock, hours 0..24: the dial's hand; < 0 no hand
		float sunrise = 6, sunset = 20;	// hours: the dial's daylight arc
		std::string reply;	// the host's last answer, under the ACTIONS; "": hidden
		bool operator==(const DebugState&) const = default;
	};

	struct DebugActions
	{
		std::function<void(const std::string& id)> toggle;
		std::function<void(const std::string& name)> scene;
		std::function<void(const std::string& id)> command;
		std::function<void(const std::string& name)> weather;	// "auto": normal weather
		std::function<void(const std::string& preset)> time;	// "dawn", "noon", "dusk", "midnight", "+1h" (the host maps them to hours), or the dial's hour as a number ("14.25")
		std::function<void()> close;	// the close button or Escape (the panel hides itself first)
		std::function<void()> settings;	// "Settings...": the host opens the DEBUG SETTINGS window (ui::Options, OptionsScope::kDebug; k16.2.419.1); unset: no button
	};

	// The Time row's 24 h dial (k16.3.37): midnight at the bottom, noon at the top, hours running clockwise (6 on the left, 18 on the right),
	// the sunrise..sunset arc shaded as daylight, a hand at the game hour. A click or a drag on the face picks the hour under the pointer,
	// snapped to 15 minutes; onPick gets it on release (a drag shows its pick as a gold hand until then). The tooltip names the hour.
	class ClockDial : public Widget
	{
	public:
		ClockDial();
		float hour = -1, sunrise = 6, sunset = 20;	// hours; hour < 0: no hand
		std::function<void(float hour)> onPick;
		float hourAt(float x, float y) const;	// pixels: the snapped hour 0..23.75 under (x, y); < 0 off the face or at its centre
		Widget* hit(float x, float y) override;
		bool onPress(Btn b, float x, float y) override;
		void onDrag(float x, float y) override;
		void onRelease(Btn b, float x, float y) override;
		std::string tip() const override;

	protected:
		void paint() const override;

	private:
		float pick_ = -1;	// the hour a press or drag points at, until the release
	};

	class DebugPanel
	{
	public:
		static constexpr float kW = 570, kH = 640, kSideW = 240, kMidW = 270;
		explicit DebugPanel(DebugActions actions = {});
		~DebugPanel();
		void update(const DebugState& s);	// every frame while shown (cheap when nothing changed)
		void show(bool on);
		bool shown() const;
		// For scripted tests.
		Widget* window() const;
		Button* toggle(size_t i) const;	// a TickBox, checked = on
		Button* scene(size_t i) const;
		Widget* scenesBox() const;
		Button* command(size_t i) const;	// the WINDOWS buttons then the ACTIONS ones, in the commands' order (kCharacter ones are not here)
		Widget* windowsBox() const { return windowsBox_; }	// the WINDOWS heading and buttons: hidden while no command is kWindow
		Button* settingsButton() const { return settings_; }	// "Settings...": hidden while DebugActions::settings is unset
		Button* characterButton() const { return character_; }	// "Character...": toggles the toolbar; hidden while no command is kCharacter
		// The CHARACTER toolbar (k16.2.393.1): a small movable window of the kCharacter commands, apart from the panel: once shown it stays up
		// (a click elsewhere, Escape on another window or the panel's close leave it) until its own close button or Character... again. Whether
		// it is shown and where it was dragged are client settings (debug.character_bar*, the Debug section), so a restart puts it back.
		CaptionWindow* characterBar() const;	// the toolbar while shown, else null
		Button* characterCommand(size_t i) const;	// the toolbar's i-th button (the kCharacter commands in order) while shown, else null
		void showCharacter(bool on);	// as the toggle: shown (when there are kCharacter commands) and remembered
		// The host feeds update() while this is true: the panel or the toolbar is up (or the toolbar is remembered shown and waits for its commands).
		bool wantsUpdate() const;
		Button* weatherRow(size_t i) const;
		ScrollView* weatherList() const { return weathers_; }	// 0: Auto, then the names; checked = current
		Button* timeButton(size_t i) const;	// dawn, noon, dusk, midnight, +1h
		ClockDial* timeDial() const { return dial_; }
		Label* reply() const { return reply_; }
		Widget* extra() const;	// the RichText
		ScrollView* sideList() const { return side_; }	// the toggles and scenes: scrolls when they outgrow the frame (k16.3.45)
		// "Overlay range" (kenshi-e155): a slider bound to debug.overlay_range, under the navmesh / collision toggles; null while that
		// setting is not declared (a build without the overlays). Its value label shows the range, "no limit" at 0.
		Slider* rangeSlider() const { return range_; }
		Label* rangeValue() const { return rangeValue_; }
		const Layout& layout() const { return *layout_; }

	private:
		void rebuildSide();
		void rebuildMid();	// the command buttons and the weather rows
		void refreshMid();	// enabled / checked / the reply, from last_
		void addRange(const cfg::Info& info);	// the "Overlay range" row, appended to toggles_
		void refreshRange();	// its thumb and label, from the setting
		CaptionWindow* win_ = nullptr;
		void bind(Layout& l);	// the file's widgets, after each (re)build
		std::unique_ptr<Layout> layout_;
		Widget *toggles_ = nullptr, *scenes_ = nullptr;
		Widget* scenesBox_ = nullptr;
		Widget* extraBox_ = nullptr;
		ScrollView* side_ = nullptr;
		Slider* range_ = nullptr;
		Label* rangeValue_ = nullptr;
		uint64_t rangeGen_ = 0;	// cfg::generation() the row last showed
		RichText* extra_ = nullptr;
		std::vector<Button*> toggleButtons_, sceneButtons_, commandButtons_, weatherButtons_;
		Widget *commands_ = nullptr, *windows_ = nullptr, *windowsBox_ = nullptr;
		Button* character_ = nullptr;
		Button* settings_ = nullptr;
		class CharBar;
		CharBar* bar_ = nullptr;	// the toolbar window, built hidden with the panel
		std::vector<size_t> cmdOf_;	// commandButtons_[i] -> last_.commands index
		ScrollView* weathers_ = nullptr;
		Label* reply_ = nullptr;
		Button* time_[5] = {};
		ClockDial* dial_ = nullptr;
		bool revealWeather_ = false;
		DebugState last_;
		DebugActions act_;
		bool built_ = false;
	};
}
