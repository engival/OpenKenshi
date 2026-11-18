#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/widget.hpp"

// The animation lab's window (docs/UI.md "Animation browser"; docs/CLIENT_GOALS.md "Animation lab"; client coordinator
// 2026-11-07): a movable, resizable dev window, its body from client/data/ui/animbrowser.layout. Left: race and sex, a name filter, the clip
// list (a group dropdown by the name filter, both kept; a row's groups faint at its right; at most kMaxRows rows; PREV / NEXT walk the whole filtered list, wrapping), "N of M". Centre: the host's offscreen render (a texture
// filled every frame, like a portrait; a drag orbits, the wheel zooms), the transport (step, play / pause, a frame scrub, speed 0.1-2x with a
// notch at 1x), the host's reply line. Right: camera presets, the plant point (four sole points, none, or a bone from a dropdown), the
// overlays, MEASURE / MARK, the readout and COPY. Keys while it has the focus (not the filter): Space play / pause, comma / period a frame,
// M mark, Escape closes. The host renders, plays and measures; this only shows its state and reports the clicks.
namespace ui
{
	struct AnimClip
	{
		std::string name;
		std::string kind;	// "record" (an ANIMATION record) | "raw" (a clip of the skeleton)
		float length = 0;	// seconds
		int frames = 0;
		std::vector<std::string> groups;	// the data groups it is in (AnimBrowserState::groups), in order
		std::string usedBy;	// one line: what names it (records, techniques, species)
		std::string clip;	// the skeleton clip it plays (a record's anim name)
		bool relocates = false;	// its ANIMATION record relocates
		bool autoFollow = false;	// the game locks its root (Follow root greyed)
		bool operator==(const AnimClip&) const = default;
	};

	struct AnimBrowserState
	{
		PortraitRef view;	// the offscreen render; tex 0: a "no render" box
		std::vector<std::string> races;
		std::string race;
		bool female = false;
		std::vector<AnimClip> clips;	// the race's; the window filters by group and name itself
		std::vector<std::string> groups;	// the group dropdown's vocabulary, after its "All"
		std::string group;	// the current group ("": all); not in groups: nothing listed
		std::string clip;	// the current one; "": none
		bool playing = false;
		float time = 0;	// seconds into the clip
		int frame = 0, frames = 0;
		double speed = 1;	// 0.1 .. 2
		std::string camera = "side";	// "side" | "front" | "top" | "orbit"
		std::string plant;	// "lheel" | "lball" | "rheel" | "rball" | a bone name | "": none
		std::vector<std::string> bones;	// the skeleton's, for the bone dropdown
		bool trails = true, rootPath = true, boneNames = false;	// the overlays
		bool follow = false;	// the root lock (the node carries the root's travel)
		float travel = 0;	// u per loop (the root text carries it; not shown on its own)
		std::string root;	// the root line ("travels 50.6 u per loop (root-locked)"); "": hidden
		std::string readout;	// the measurement, multi-line
		std::string reply;	// one status line ("marked walk t=0.47 f14"); "": hidden
		bool operator==(const AnimBrowserState&) const = default;
	};

	struct AnimBrowserActions
	{
		std::function<void(const std::string&)> race;
		std::function<void(bool)> female;
		std::function<void(const std::string&)> group;	// the group dropdown: a name, "" for All
		std::function<void(bool)> follow;	// Follow root (the host refuses it on an autoFollow clip, with a reply)
		std::function<void(const std::string&)> select;	// a clip by name (a list click, PREV / NEXT)
		std::function<void(bool)> play;	// true: play
		std::function<void(int)> step;	// -1 | +1 frame (pauses)
		std::function<void(float)> seek;	// seconds (the scrub)
		std::function<void(double)> speed;
		std::function<void(const std::string&)> camera;
		std::function<void(float, float)> orbit;	// a drag on the render, units right / down
		std::function<void(float)> zoom;	// the wheel over the render (> 0: up)
		std::function<void(const std::string&)> plant;	// as AnimBrowserState::plant
		std::function<void(const std::string&, bool)> overlay;	// "trails" | "root" | "bones"
		std::function<void()> measure, mark;
		std::function<void(const std::string&)> copy;	// the readout, for the clipboard
		std::function<void(int, int)> viewSize;	// the render box in pixels, whenever it changes: render 1:1
		std::function<void()> close;	// the X or Escape (the window hides itself first)
	};

	class AnimBrowser
	{
	public:
		static constexpr size_t kMaxRows = 400;	// clip rows drawn; past it a "narrow the filter" line
		explicit AnimBrowser(AnimBrowserActions actions = {});
		~AnimBrowser();
		void update(const AnimBrowserState& s);	// every frame while shown (cheap when nothing changed)
		void show(bool on);
		bool shown() const;
		// For scripted tests.
		CaptionWindow* window() const { return win_; }
		const Layout& layout() const { return *layout_; }
		size_t filtered() const { return match_.size(); }	// clips the filter keeps
		Button* clipRow(size_t i) const { return i < rows_.size() ? rows_[i] : nullptr; }	// the i-th row shown
		Widget* moreLine() const { return more_; }	// the "narrow the filter" line; null when every match has a row
		Label* emptyLine() const { return empty_; }	// "no clips in this group for <race>" / "no clips match"; null while any match
		Dropdown* groupBox() const { return group_; }
		Button* followBox() const { return follow_; }
		Label* usedBy() const { return usedBy_; }
		Label* rootText() const { return rootText_; }
		EditBox* filter() const { return filter_; }
		Dropdown* raceBox() const { return race_; }
		Dropdown* boneBox() const { return bone_; }
		Button* femaleBox() const { return female_; }
		Button* button(const std::string& id) const;	// a layout button by id (prev, next, play, stepback, stepfwd, side, ..., lheel, ..., none, measure, mark, copy)
		Button* overlayBox(const std::string& id) const;	// trails, root, bones
		Slider* scrub() const { return scrub_; }
		Slider* speedSlider() const { return speed_; }
		Label* count() const { return count_; }
		Label* frameText() const { return frameText_; }
		Label* speedText() const { return speedText_; }
		Label* readout() const { return readout_; }
		Label* reply() const { return reply_; }
		Widget* view() const { return view_; }
		ScrollView* clipList() const { return clips_; }

	private:
		class Win;
		class View;
		void bind(Layout& l);
		void rebuildClips();	// match_ and the rows from last_.clips and the filter
		void refresh();	// everything else from last_
		void walk(int dir);	// PREV / NEXT
		bool key(int k, bool down);	// the window's keys; false: not ours
		CaptionWindow* win_ = nullptr;
		std::unique_ptr<Layout> layout_;
		AnimBrowserActions act_;
		AnimBrowserState last_;
		bool built_ = false, clipsDirty_ = true, reveal_ = true;
		std::string filterText_;
		std::vector<size_t> match_;	// clip indices the filter keeps
		std::vector<Button*> rows_;
		Widget* more_ = nullptr;
		Label* empty_ = nullptr;
		Dropdown *race_ = nullptr, *bone_ = nullptr, *group_ = nullptr;
		Button *female_ = nullptr, *follow_ = nullptr;
		Label *usedBy_ = nullptr, *rootText_ = nullptr;
		EditBox* filter_ = nullptr;
		ScrollView *clips_ = nullptr, *readoutBox_ = nullptr;
		Slider *scrub_ = nullptr, *speed_ = nullptr;
		Label *count_ = nullptr, *frameText_ = nullptr, *speedText_ = nullptr, *readout_ = nullptr, *reply_ = nullptr;
		Widget* view_ = nullptr;
		std::vector<std::pair<std::string, Button*>> buttons_, overlays_;
		int sentW_ = 0, sentH_ = 0;	// the last viewSize sent
	};
}
