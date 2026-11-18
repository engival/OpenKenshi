#include "ui/dev/lightspanel.hpp"

#include <algorithm>
#include <cctype>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		constexpr float kW = 340, kBodyH = 880, kRowH = 26, kHeadH = 24, kSoloW = 60;
		constexpr float kSeeThrough = 0.8f;	// the window's fill alpha: the scene shows through while a term is switched
		const char* const kGroups[] = { "sun", "ambient", "lamps", "emissive", "fog", "sky", "air", "post" };	// the host's (lightterms.hpp), in this order
	}

	std::vector<std::string> lightGroups(const std::vector<LightRow>& terms)
	{
		std::vector<std::string> out;
		for (const char* g : kGroups)
		{
			if (std::any_of(terms.begin(), terms.end(), [&](const LightRow& t) { return t.group == g; })) { out.push_back(g); }
		}
		for (const LightRow& t : terms)
		{
			if (std::find(out.begin(), out.end(), t.group) == out.end()) { out.push_back(t.group); }
		}
		return out;
	}

	LightsPanel::LightsPanel(LightsPanelActions actions) : act_(std::move(actions))
	{
		win_ = &root().add<CaptionWindow>("LIGHTS", [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		}, 0);
		win_->rect = { 400, 100, kW, CaptionWindow::kHeaderH + kBodyH };
		win_->visible = false;
		win_->tint = Color{}.alpha(kSeeThrough);
		win_->body().tint = Color{}.alpha(kSeeThrough);
		win_->body().rect.h = kBodyH;
		layout_ = std::make_unique<Layout>(win_->body(), "lightspanel", [this](Layout& l) { bind(l); });
		if (!list_) { bind(*layout_); }	// the file failed: spares, so the window works under the red box
	}

	LightsPanel::~LightsPanel()
	{
		layout_.reset();	// before its parent goes
		if (ready() && win_) { root().remove(win_); }
	}

	void LightsPanel::bind(Layout& l)
	{
		all_ = &l.at<Button>("all");
		all_->onClicked = [this] { if (act_.allOn) { act_.allOn(); } };
		count_ = &l.at<Label>("count");
		list_ = &l.at<ScrollView>("rows");
		static_cast<Box&>(list_->body()).spacing = 0;
		names_.clear();
		ticks_.clear();
		solos_.clear();
		headers_.clear();
		dirty_ = true;
	}

	void LightsPanel::update(const LightsPanelState& s)
	{
		if (!(s == last_))
		{
			const bool sameRows = std::equal(s.terms.begin(), s.terms.end(), last_.terms.begin(), last_.terms.end(),
				[](const LightRow& a, const LightRow& b) { return a.name == b.name && a.group == b.group; });
			if (!sameRows) { dirty_ = true; }	// the ticks alone: in place, so a press held across frames still clicks
			last_ = s;
			if (!dirty_) { refresh(); }
		}
		if (dirty_)
		{
			rebuild();
			refresh();
		}
	}

	void LightsPanel::refresh()
	{
		size_t off = 0;
		for (const LightRow& t : last_.terms)
		{
			if (!t.on) { off++; }
			const auto it = std::find(names_.begin(), names_.end(), t.name);
			if (it != names_.end()) { ticks_[it - names_.begin()]->checked = t.on; }
		}
		count_->text = off ? std::to_string(off) + " off" : "all on";
		count_->colour = off ? res::colour("BadBright") : res::colour("Main").alpha(0.75f);	// something switched off is what the user must not forget
	}

	void LightsPanel::rebuild()
	{
		dirty_ = false;
		Widget& body = list_->body();
		body.clear();
		names_.clear();
		ticks_.clear();
		solos_.clear();
		headers_.clear();
		for (const std::string& g : lightGroups(last_.terms))
		{
			std::string caps = g;
			for (char& c : caps) { c = (char)std::toupper((unsigned char)c); }
			Label& h = body.add<Label>(caps, "Kenshi_StandardFont_MediumSmall");
			h.rect.h = kHeadH;
			h.halign = Widget::Align::kStretch;
			h.colour = res::colour("Accent");
			headers_.push_back(&h);
			for (const LightRow& t : last_.terms)
			{
				if (t.group != g) { continue; }
				Widget& row = body.add<Widget>();
				row.rect = { 0, 0, kW - 40, kRowH };
				row.halign = Widget::Align::kStretch;
				TickBox& b = row.add<TickBox>(t.name);
				b.rect = { 8, 0, kW - 48 - kSoloW - 8, kRowH };
				b.checkable = false;	// the host's state ticks it
				b.checked = t.on;
				b.tooltip = "Switch " + t.name + " on or off";
				b.onClicked = [this, name = t.name]
				{
					const auto it = std::find_if(last_.terms.begin(), last_.terms.end(), [&](const LightRow& r) { return r.name == name; });
					if (it != last_.terms.end() && act_.set) { act_.set(name, !it->on); }
				};
				Button& s = row.add<Button>("SOLO", Look::kButton, "Kenshi_StandardFont_MediumSmall");
				s.rect = { 0, 2, kSoloW, kRowH - 4 };
				s.halign = Widget::Align::kEnd;
				s.tooltip = "Only " + t.name + " (and what it needs to show); ALL ON undoes it";
				s.onClicked = [this, name = t.name] { if (act_.solo) { act_.solo(name); } };
				names_.push_back(t.name);
				ticks_.push_back(&b);
				solos_.push_back(&s);
			}
		}
	}

	TickBox* LightsPanel::tick(const std::string& name) const
	{
		const auto it = std::find(names_.begin(), names_.end(), name);
		return it == names_.end() ? nullptr : ticks_[it - names_.begin()];
	}

	Button* LightsPanel::solo(const std::string& name) const
	{
		const auto it = std::find(names_.begin(), names_.end(), name);
		return it == names_.end() ? nullptr : solos_[it - names_.begin()];
	}

	void LightsPanel::show(bool on)
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

	bool LightsPanel::shown() const { return win_->visible; }
}
