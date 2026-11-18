#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/widget.hpp"

// The debug panel (docs/UI.md; the user, 2026-11-05: the old key list (Ctrl+F1, now F8) was a wall of text): a movable DEBUG window, top left. On its
// left (client/data/ui/debug.layout; resizable) the host's debug toggles (a tick box each, its key beside it), under them the host's extra rich text,
// then the debug scenes (offline only); beside them the commands, time and weather. No key list (k16.2.352): the Options Keys tab lists every binding.
namespace ui
{
	struct DebugToggle
	{
		std::string id, label;
		bool on = false;
		std::string key;	// the keys:: action whose chord shows beside it; "" none
		bool operator==(const DebugToggle&) const = default;
	};

	struct DebugCommand
	{
		std::string id, label;
		bool enabled = true;
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
		std::string reply;	// the host's last answer, under the commands; "": hidden
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
		Button* command(size_t i) const;
		Button* weatherRow(size_t i) const;
		ScrollView* weatherList() const { return weathers_; }	// 0: Auto, then the names; checked = current
		Button* timeButton(size_t i) const;	// dawn, noon, dusk, midnight, +1h
		ClockDial* timeDial() const { return dial_; }
		Label* reply() const { return reply_; }
		Widget* extra() const;	// the RichText
		const Layout& layout() const { return *layout_; }

	private:
		void rebuildSide();
		void rebuildMid();	// the command buttons and the weather rows
		void refreshMid();	// enabled / checked / the reply, from last_
		CaptionWindow* win_ = nullptr;
		void bind(Layout& l);	// the file's widgets, after each (re)build
		std::unique_ptr<Layout> layout_;
		Widget *toggles_ = nullptr, *scenes_ = nullptr;
		Widget* scenesBox_ = nullptr;
		Widget* extraBox_ = nullptr;
		RichText* extra_ = nullptr;
		std::vector<Button*> toggleButtons_, sceneButtons_, commandButtons_, weatherButtons_;
		Widget* commands_ = nullptr;
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
