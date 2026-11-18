#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/widget.hpp"

// The spectator's cast list (docs/UI.md "Spectator mode"; client coordinator 2026-11-07): a movable, resizable CAST window, its body from
// client/data/ui/cast.layout. A filter box (name or faction, any case), a table Name / Faction / Id (sorted by name; a header click sorts,
// Id by number), and a count line. A click asks watch(id) (the inspect focus), a double click or Enter follow(id). The highlight is the
// host's watched id. The Hud owns it and says when it shows; the X or Escape hides it and calls close(). Non-modal.
namespace ui
{
	struct HudCastRow
	{
		uint64_t id = 0;
		std::string name;	// empty: "#id"
		std::string faction;
		bool down = false;	// KO or dead: dimmed
		bool operator==(const HudCastRow&) const = default;
	};

	struct CastActions
	{
		std::function<void(uint64_t id)> watch, follow;
		std::function<void()> close;
	};

	class CastWindow
	{
	public:
		explicit CastWindow(CastActions actions = {});
		~CastWindow();
		void update(const std::vector<HudCastRow>& cast, uint64_t watched);	// every frame (cheap when nothing changed)
		void show(bool on);
		bool shown() const;
		// For scripted tests.
		CaptionWindow* window() const { return win_; }
		const Layout& layout() const { return *layout_; }
		Table* table() const { return table_; }
		EditBox* filter() const { return filter_; }
		const std::string& countText() const { return count_->text; }

	private:
		void bind(Layout& l);
		void rebuild();
		CaptionWindow* win_ = nullptr;
		std::unique_ptr<Layout> layout_;
		CastActions act_;
		std::vector<HudCastRow> cast_;
		uint64_t watched_ = 0;
		bool built_ = false, syncing_ = false;
		std::string filterText_;
		EditBox* filter_ = nullptr;
		Table* table_ = nullptr;
		Label* count_ = nullptr;
	};
}
