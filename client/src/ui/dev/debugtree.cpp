#include "ui/dev/debugtree.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "sokol_app.h"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		constexpr float kW = 720, kBodyH = 600, kRowH = 24, kIndent = 16;

		bool holds(const std::string& hay, const std::string& lowNeedle)
		{
			std::string h = hay;
			for (char& c : h) { c = (char)std::tolower((unsigned char)c); }
			return h.find(lowNeedle) != std::string::npos;
		}

		// "{#rrggbb}Name" -> the colour and "Name"; no prefix: false and the title as is.
		bool swatchOf(const std::string& title, Color& c, std::string& rest)
		{
			rest = title;
			if (title.size() < 9 || title[0] != '{' || title[1] != '#') { return false; }
			const size_t end = title.find('}');
			if (end == std::string::npos || !Color::parse(title.substr(1, end - 1), c)) { return false; }
			rest = title.substr(end + 1);
			while (!rest.empty() && rest[0] == ' ') { rest.erase(0, 1); }
			return true;
		}

		class Swatch : public Widget
		{
		public:
			Color colour;

		protected:
			void paint() const override
			{
				gfx::fill(screen_, colour);
				gfx::frame(screen_, std::max(1.0f, std::round(scale_)), res::colour("Main").alpha(0.6f));
			}
		};
	}

	// A node's open / closed arrow: click-only (a ray pointer works); Shift: the whole subtree.
	class DebugTree::Toggle : public Widget
	{
	public:
		Toggle(DebugTree& t, size_t row, bool open) : tree_(t), row_(row), open_(open) { solid = true; tooltip = "Shift+click: the whole branch"; }
		void onClick(Btn b) override { if (b == Btn::kLeft) { tree_.flip(row_, (mods() & SAPP_MODIFIER_SHIFT) != 0); } }

	protected:
		void paint() const override
		{
			const Color c = hovered(this) ? res::colour("Accent") : res::colour("Main").alpha(0.7f);
			const float cx = screen_.x + screen_.w / 2, cy = screen_.y + screen_.h / 2, a = std::min(screen_.w, screen_.h) * 0.22f;
			if (open_) { gfx::triangle(cx - a, cy - a * 0.6f, cx + a, cy - a * 0.6f, cx, cy + a * 0.7f, c); }
			else { gfx::triangle(cx - a * 0.6f, cy - a, cx - a * 0.6f, cy + a, cx + a * 0.7f, cy, c); }
		}

	private:
		DebugTree& tree_;
		size_t row_;
		bool open_;
	};

	DebugTree::DebugTree(DebugTreeActions actions) : act_(std::move(actions))
	{
		win_ = &root().add<CaptionWindow>("DEBUG VIEW", [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		}, 0);
		win_->halign = Widget::Align::kEnd;
		win_->rect = { 8, 120, kW, CaptionWindow::kHeaderH + kBodyH };
		win_->resizable = true;
		win_->minSize = { 420, 300 };
		win_->visible = false;
		win_->body().rect.h = kBodyH;
		Swatch& sw = static_cast<Widget*>(win_->caption().parent())->add<Swatch>();
		sw.rect = { 12, 0, 14, 14 };
		sw.valign = Widget::Align::kCenter;
		sw.visible = false;
		swatch_ = &sw;
		layout_ = std::make_unique<Layout>(win_->body(), "debugtree", [this](Layout& l) { bind(l); });
		if (!list_) { bind(*layout_); }	// the file failed: spares, so the window works under the red box
	}

	DebugTree::~DebugTree()
	{
		layout_.reset();	// before its parent goes
		if (ready() && win_) { root().remove(win_); }
	}

	void DebugTree::bind(Layout& l)
	{
		tick_ = &l.at<Label>("tick");
		filter_ = &l.at<EditBox>("filter");
		filter_->text = filterText_;
		filter_->onChange = [this](const std::string& t) { filterText_ = t; dirty_ = true; };
		filter_->onCommit = filter_->onChange;
		auto_ = &l.at<TickBox>("auto");
		auto_->onToggle = [this](bool on) { if (act_.setAuto) { act_.setAuto(on); } };
		refresh_ = &l.at<Button>("refresh");
		refresh_->onClicked = [this] { if (act_.refresh) { act_.refresh(); } };
		copy_ = &l.at<Button>("copy");
		copy_->onClicked = [this] { if (act_.copy) { act_.copy(text()); } };
		list_ = &l.at<ScrollView>("rows");
		static_cast<Box&>(list_->body()).spacing = 0;
		rowWidgets_.clear();
		toggles_.clear();
		tick_->text = "Tick " + std::to_string(last_.tick);
		auto_->checked = last_.autoOn;
		dirty_ = true;
	}

	bool DebugTree::open(size_t r) const
	{
		auto it = open_.find(paths_[r]);
		return it != open_.end() ? it->second : last_.rows[r].depth == 0;
	}

	void DebugTree::flip(size_t r, bool subtree)
	{
		if (r >= last_.rows.size()) { return; }
		const bool to = !open(r);
		open_[paths_[r]] = to;
		if (subtree)
		{
			for (size_t i = r + 1; i < last_.rows.size() && last_.rows[i].depth > last_.rows[r].depth; i++) { open_[paths_[i]] = to; }
		}
		dirty_ = true;
	}

	void DebugTree::update(const DebugTreeState& s)
	{
		if (!(s == last_))
		{
			if (s.rows != last_.rows) { dirty_ = true; }
			last_ = s;
			Color c;
			std::string name;
			swatch_->visible = swatchOf(s.title, c, name);
			static_cast<Swatch*>(swatch_)->colour = c;
			win_->caption().text = name.empty() ? "DEBUG VIEW" : name;
			tick_->text = "Tick " + std::to_string(s.tick);
			auto_->checked = s.autoOn;
			// Key paths: parent path / key, a repeated key under the same parent "#n".
			const size_t n = s.rows.size();
			paths_.assign(n, {});
			parent_.assign(n, -1);
			std::vector<int> stack;
			std::map<std::string, int> seen;
			for (size_t i = 0; i < n; i++)
			{
				const int d = std::max(0, s.rows[i].depth);
				while ((int)stack.size() > d) { stack.pop_back(); }
				parent_[i] = stack.empty() ? -1 : stack.back();
				std::string p = (parent_[i] < 0 ? std::string() : paths_[parent_[i]] + "/") + s.rows[i].key;
				const int k = seen[p]++;
				if (k > 0) { p += "#" + std::to_string(k); }
				paths_[i] = p;
				while ((int)stack.size() < d) { stack.push_back(stack.empty() ? -1 : stack.back()); }	// a jump in depth: no row between
				stack.push_back((int)i);
			}
		}
		if (dirty_) { rebuild(); }
	}

	void DebugTree::rebuild()
	{
		dirty_ = false;
		const std::vector<DebugTreeRow>& rows = last_.rows;
		const size_t n = rows.size();
		std::string needle = filterText_;
		for (char& c : needle) { c = (char)std::tolower((unsigned char)c); }
		std::vector<char> keep(n, needle.empty());
		if (!needle.empty())
		{
			for (size_t i = 0; i < n; i++)
			{
				if (!holds(rows[i].key, needle) && !holds(rows[i].value, needle)) { continue; }
				for (int j = (int)i; j >= 0 && !keep[j]; j = parent_[j]) { keep[j] = 1; }
			}
		}
		shown_.clear();
		for (size_t i = 0; i < n; i++)
		{
			if (!keep[i]) { continue; }
			bool vis = true;
			if (needle.empty()) { for (int j = parent_[i]; j >= 0 && vis; j = parent_[j]) { vis = open(j); } }	// a filter opens the kept ancestors
			if (vis) { shown_.push_back(i); }
		}
		Widget& body = list_->body();
		body.clear();
		rowWidgets_.clear();
		toggles_.clear();
		for (size_t i : shown_)
		{
			const DebugTreeRow& r = rows[i];
			const bool kids = i + 1 < n && rows[i + 1].depth > r.depth;
			Flex& line = body.add<Flex>(false, 8);
			line.halign = Widget::Align::kStretch;
			line.rect.h = kRowH;
			Widget& keyCell = line.add<Widget>(FlexItem{ .fill = 4 });
			keyCell.valign = Widget::Align::kStretch;
			keyCell.clipChildren = true;
			const float x = std::max(0, r.depth) * kIndent;
			Widget* t = nullptr;
			if (kids)
			{
				t = &keyCell.add<Toggle>(*this, i, !needle.empty() || open(i));
				t->rect = { x, 0, kIndent, kRowH };
			}
			Label& k = keyCell.add<Label>(r.key);
			k.rect.x = x + kIndent + 2;
			k.valign = Widget::Align::kCenter;
			k.colour = res::colour("Main").alpha(0.65f);
			Widget& valCell = line.add<Widget>(FlexItem{ .fill = 6 });
			valCell.valign = Widget::Align::kStretch;
			valCell.clipChildren = true;
			valCell.solid = true;
			valCell.tooltip = r.value;
			Label& v = valCell.add<Label>(r.value);
			v.valign = Widget::Align::kCenter;
			v.colour = res::colour("Main");
			rowWidgets_.push_back(&line);
			toggles_.push_back(t);
		}
	}

	Widget* DebugTree::toggle(size_t i) const { return i < toggles_.size() ? toggles_[i] : nullptr; }

	std::string DebugTree::text() const
	{
		std::string out = win_->caption().text + " (tick " + std::to_string(last_.tick) + ")\n";
		for (size_t i : shown_)
		{
			const DebugTreeRow& r = last_.rows[i];
			out += std::string(2 * std::max(0, r.depth), ' ') + r.key + (r.value.empty() ? "" : ": " + r.value) + "\n";
		}
		return out;
	}

	void DebugTree::show(bool on)
	{
		if (win_->visible == on) { return; }
		win_->visible = on;
		if (on)
		{
			win_->raise();
			focus(win_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	bool DebugTree::shown() const { return win_->visible; }
}
