#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/widget.hpp"

// The light debugger window (kenshi-jj3.27.3; docs/UI.md "lightspanel.*"): a see-through dev window over the scene, one row per light term of the host's
// dev::lightTerms (client/src/dev/lightterms.hpp, kenshi-jj3.27.1; the UI never includes it): a tick (on / off) and SOLO per row, grouped under the host's
// groups (sun, ambient, lamps, emissive, fog, sky, air, post; any other after, as first seen), ALL ON at the top. The body is client/data/ui/lightspanel.layout.
// Host-fed: the state in every frame while shown (the lamp rows change as lamps come into view), actions out.
namespace ui
{
	struct LightRow
	{
		std::string name;	// "sun_direct", ..., "lamp:<id>": what set / solo get back
		std::string group;
		bool on = true;
		bool operator==(const LightRow&) const = default;
	};

	struct LightsPanelState
	{
		std::vector<LightRow> terms;	// the host's order (lamps nearest first)
		bool operator==(const LightsPanelState&) const = default;
	};

	struct LightsPanelActions
	{
		std::function<void(const std::string&, bool)> set;	// a tick: that term on / off
		std::function<void(const std::string&)> solo;	// only that term (and what it needs to show)
		std::function<void()> allOn;
		std::function<void()> close;	// the X or Escape (the window hides itself first)
	};

	class LightsPanel
	{
	public:
		explicit LightsPanel(LightsPanelActions actions = {});
		~LightsPanel();
		void update(const LightsPanelState& s);	// every frame while shown (cheap when nothing changed)
		void show(bool on);
		bool shown() const;
		// For scripted tests.
		CaptionWindow* window() const { return win_; }
		const Layout& layout() const { return *layout_; }
		Button* allButton() const { return all_; }
		Label* countLabel() const { return count_; }	// "all on" / "N off"
		ScrollView* list() const { return list_; }
		size_t rows() const { return ticks_.size(); }	// term rows (no group headers)
		size_t headers() const { return headers_.size(); }
		Label* header(size_t i) const { return i < headers_.size() ? headers_[i] : nullptr; }	// in the shown order
		TickBox* tick(const std::string& name) const;	// null: no such row
		Button* solo(const std::string& name) const;

	private:
		void bind(Layout& l);
		void rebuild();	// the rows from last_: only when the row set (names, groups, order) changes
		void refresh();	// the ticks and the count into the kept rows
		CaptionWindow* win_ = nullptr;
		std::unique_ptr<Layout> layout_;
		Button* all_ = nullptr;
		Label* count_ = nullptr;
		ScrollView* list_ = nullptr;
		LightsPanelState last_;
		LightsPanelActions act_;
		std::vector<std::string> names_;	// per term row, the shown order
		std::vector<TickBox*> ticks_;
		std::vector<Button*> solos_;
		std::vector<Label*> headers_;
		bool dirty_ = true;
	};

	std::vector<std::string> lightGroups(const std::vector<LightRow>& terms);	// the sections in the shown order: the known ones first, then any other as first seen
}
