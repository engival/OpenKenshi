#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The new game's ADVANCED OPTIONS window (docs/UI.md), after Kenshi_AdvancedOptionsWindow.layout and kinspect's live capture
// (findings/ui_captures/advopts.md; boxes x 1.5): 576 x 478.5, no close button, kept under the panel it opens below (the NEW GAME or IMPORT
// panel, same left edge). One flat list of rows: a slider row (the name, clipped; the slider, its arrows a step each; the value box, typed
// values clamped but not stepped) or a tick row; the hovered name's tip in the box at the bottom, which keeps the last. Host-fed: the values
// are the host's (per new game / per save); every change is an action and the next update shows it.
namespace ui
{
	struct AdvOption
	{
		enum class Kind { kSlider, kTick };
		std::string id, label;
		Kind kind = Kind::kSlider;
		double min = 0, max = 1, step = 0.01;	// a slider's
		double value = 0;	// a tick: 0 / 1
		std::string tip;	// rich text; empty: the tip box keeps what it showed
		bool operator==(const AdvOption&) const = default;
	};

	struct AdvancedActions
	{
		std::function<void(const std::string& id, double value)> change;
	};

	class AdvancedOptions
	{
	public:
		static constexpr float kW = 576, kH = 478.5f;	// Root: 384 x 319 at 720
		static constexpr float kSliderH = 34.5f, kTickH = 31.5f;	// row pitches (23 / 21 at 720)
		explicit AdvancedOptions(AdvancedActions actions = {});
		~AdvancedOptions();
		void update(const std::vector<AdvOption>& rows);	// any time (cheap when nothing changed); a typed value box is left alone
		void openBelow(const Widget& panel);	// shown under panel, following it while both show
		void show(bool on);
		bool shown() const;
		static std::string number(double v);	// the value box's text: the shortest decimal ("1", "0.5", "1.05", "0.253")
		// For scripted tests.
		Widget* window() const;
		Widget* name(size_t i) const;	// row i's name (hover it for the tip)
		Slider* slider(size_t i) const;	// null on a tick row
		EditBox* valueBox(size_t i) const;
		Button* tick(size_t i) const;	// null on a slider row
		RichText* tipText() const { return tip_; }

	private:
		class Frame;
		struct Row
		{
			Widget* name;
			Slider* slider;
			EditBox* box;
			Button* tick;
		};
		Frame* win_ = nullptr;
		Box* list_ = nullptr;
		RichText* tip_ = nullptr;
		std::vector<Row> rows_;
		std::vector<AdvOption> last_;
		AdvancedActions act_;
		void rebuild();
		void hover();
	};
}
