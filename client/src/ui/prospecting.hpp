#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The prospecting results window (docs/UI.md), after Kenshi_ProspectingWindow and Kenshi_ProspectingWindowResourceLine, placed as kinspect's
// live capture (findings/ui_captures/prospect.md): PROSPECTING RESULTS, 594 x 550 centred, opened by the host when a prospecting task ends.
// OVERVIEW across the top (two columns of label / value lines in an inset box); RESOURCES lower left (a row per resource: a radio-style
// button with its name, the value in a small flat box); the selected resource's map lower right (the host's texture region, the camera
// marker over it). One resource is selected at a time; picking another is the host's order (`select`) and the next state shows it.
namespace ui
{
	struct ProspectLine
	{
		std::string label, value;
		Color labelColour = { 0, 0, 0, 0 }, valueColour = { 0, 0, 0, 0 };	// alpha 0 = the inset's ink; the host's: style ProspectZone / ProspectEnv
		bool operator==(const ProspectLine&) const = default;
	};

	struct ProspectResource
	{
		std::string id, name;
		int value = 0;	// 0..100
		Color colour = { 0, 0, 0, 0 };	// the value's; alpha 0 = the style's ProspectLow (< 50) / ProspectMid (50..75) / ProspectHigh (> 75)
		bool operator==(const ProspectResource&) const = default;
	};

	struct ProspectState
	{
		std::vector<ProspectLine> overview[2];	// OverviewDatapanel1 / 2
		std::vector<ProspectResource> resources;
		std::string selected;	// the resource whose map shows (its button lit)
		PortraitRef map = {};	// the selected resource's map: tex + uv; tex 0: an empty map box
		float markerU = -1, markerV = 0;	// the camera in the map's 0..1 (u < 0: no marker)
		float markerAngle = 0;	// radians, clockwise on the screen from the marker image's up
		bool operator==(const ProspectState&) const = default;
	};

	struct ProspectActions
	{
		std::function<void(const std::string& id)> select;	// another resource picked
		std::function<void()> close;	// the close button or Escape (the window hides itself first)
	};

	class Prospecting
	{
	public:
		static constexpr float kW = 594, kH = 550, kRowH = 30;	// the layout's 0.309 x 0.509 of 1920 x 1080 (live: 396 x 366 at 720); a line 30
		explicit Prospecting(ProspectActions actions = {});
		~Prospecting();
		void update(const ProspectState& s);	// every frame while shown (cheap when nothing changed)
		void show(bool on);
		bool shown() const;
		// For scripted tests.
		Widget* window() const;
		Button* tick(size_t i) const;	// resource i's button (checked = selected)
		Label* value(size_t i) const;
		Label* overviewLabel(int col, size_t i) const;
		Label* overviewValue(int col, size_t i) const;
		Widget* map() const;
		Rect marker() const;	// the marker's box on the screen, pixels (empty: none)

	private:
		class MapBox;
		struct Row;
		CaptionWindow* win_ = nullptr;
		Box* cols_[2] = {};
		ScrollView* list_ = nullptr;
		MapBox* map_ = nullptr;
		std::vector<Row*> rows_;
		std::vector<std::pair<Label*, Label*>> lines_[2];
		ProspectState last_;
		ProspectActions act_;
	};
}
