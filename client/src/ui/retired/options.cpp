#include "ui/options.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/keys.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// Tab titles and notes for the known owners; any other prefix gets its name capitalised.
		struct Section { const char* prefix; const char* title; const char* note; };
		const Section kSections[] = {
			{ "audio", "Audio", "" },
			{ "ui", "Interface", "" },
			{ "ui.colour", "Colours", "The flat style's colours: #rrggbb or #rrggbbaa. Some text colours change only at the next start." },
			{ "mod", "Mods", "Mod changes take effect at the next start." },
			{ "key", "Keys", "Click a box, then press the key (hold Ctrl, Alt or Shift with it). Escape cancels, a right click clears. Red: another action has it too." },
			{ "key.debug", "Debug keys", "The client's development keys. Red: a game key has it too." },
		};

		const Section* section(const std::string& prefix)
		{
			for (const Section& s : kSections) { if (prefix == s.prefix) { return &s; } }
			return nullptr;
		}

		// The longest known section the key is under ("ui.colour.Main" -> "ui.colour"), else its first part.
		std::string prefixOf(const std::string& key)
		{
			std::string best;
			for (const Section& s : kSections)
			{
				const std::string p = s.prefix;
				if (key.size() > p.size() && key.compare(0, p.size(), p) == 0 && key[p.size()] == '.' && p.size() > best.size()) { best = p; }
			}
			return best.empty() ? key.substr(0, key.find('.')) : best;
		}

		// A colour sample for the colour rows.
		class Swatch : public Widget
		{
		public:
			std::string value;

		protected:
			void paint() const override
			{
				Color c;
				const bool ok = Color::parse(value, c);
				const float b = std::max(1.0f, std::round(scale_));
				if (ok) { gfx::fill(screen_, c); }
				gfx::frame(screen_, b, res::colour("Main").alpha(ok ? 0.6f : 0.25f));
			}
		};

		// "audio.music" -> "Music", "ui.tooltip_delay" -> "Tooltip delay".
		std::string title(std::string s)
		{
			for (char& c : s) { if (c == '_' || c == '.') { c = ' '; } }
			if (!s.empty()) { s[0] = (char)toupper((unsigned char)s[0]); }
			return s;
		}

		double step(const cfg::Info& i) { return i.type == cfg::Type::kInt ? 1 : (i.max - i.min) / 100; }

		std::string number(const cfg::Info& i, double v)
		{
			char b[32];
			snprintf(b, sizeof b, i.type == cfg::Type::kInt ? "%.0f" : "%.3g", v);
			return b;
		}
	}

	Options::Options()
	{
		using A = Widget::Align;
		window_ = &root().add<CaptionWindow>("OPTIONS", [this] { show(false); }, 24);
		window_->halign = window_->valign = A::kCenter;
		window_->rect = { 0, 0, 720, CaptionWindow::kHeaderH + 562 };
		window_->visible = false;
		Panel& w = window_->body();
		w.rect.h = 562;
		// Tabs, rows, help line and buttons are placed from the edges, so the rows' view takes what is left.
		help_ = &w.add<Label>("", "Kenshi_StandardFont_Medium");
		help_->wrap = true;
		help_->halign = A::kStretch;
		help_->valign = A::kEnd;
		help_->rect = { 0, 56, 0, 0 };
		help_->colour = res::colour("Main").alpha(0.8f);
		Box& buttons = w.add<Box>(false, 10);
		buttons.halign = A::kEnd;
		buttons.valign = A::kEnd;
		Button& def = buttons.add<Button>("DEFAULT");
		def.tooltip = "Reset this tab's options to their defaults";
		def.onClicked = [this]
		{
			for (const Row& r : shownRows_) { if (!r.info.pinned) { cfg::setUser(r.info.key, r.info.def); } }
		};
		close_ = &buttons.add<Button>("CLOSE");
		close_->onClicked = [this] { show(false); };
		rows_ = &w.add<ScrollView>();
		rows_->halign = rows_->valign = A::kStretch;
		rows_->rect = { 0, 46, 0, 120 };
		build();
	}

	Options::~Options()
	{
		if (ready() && window_) { root().remove(window_); }
	}

	bool Options::shown() const { return window_->visible; }

	void Options::show(bool on)
	{
		window_->visible = on;
		if (on)
		{
			window_->raise();
			focus(window_);
		}
		else if (focused(window_)) { focus(nullptr); }
	}

	Widget* Options::keyField(const std::string& action, int slot) const
	{
		for (const Row& r : shownRows_) { if (r.info.key == "key." + action && slot >= 0 && slot < keys::kSlots) { return r.keys[slot]; } }
		return nullptr;
	}

	Widget* Options::control(const std::string& key) const
	{
		for (const Row& r : shownRows_) { if (r.info.key == key) { return r.control; } }
		return nullptr;
	}

	// The tab row, from the prefixes of the options declared now.
	void Options::build()
	{
		const std::vector<cfg::Info> all = cfg::list();
		optionCount_ = all.size();
		if (!tabView_)	// the tab row alone: the rows' view is rebuilt per tab
		{
			tabView_ = &window_->body().add<TabView>(0, 35);
			tabView_->halign = Widget::Align::kStretch;
			tabView_->rect.h = 35;
			tabView_->tabAlign = Widget::Align::kCenter;
			tabView_->onSelect = [this](size_t t) { select(t); };
		}
		tabView_->clearTabs();
		tabs_.clear();
		std::vector<std::string> prefixes;
		for (const Section& s : kSections)	// known owners first, in their order
		{
			for (const cfg::Info& i : all) { if (prefixOf(i.key) == s.prefix) { prefixes.push_back(s.prefix); break; } }
		}
		for (const cfg::Info& i : all)
		{
			const std::string p = prefixOf(i.key);
			if (!section(p) && (prefixes.empty() || prefixes.back() != p)) { prefixes.push_back(p); }
		}
		for (size_t t = 0; t < prefixes.size(); t++)
		{
			const Section* s = section(prefixes[t]);
			tabView_->add(s ? s->title : title(prefixes[t]));
			tabs_.push_back({ prefixes[t], tabView_->tab(t) });
		}
		if (tabs_.empty()) { select(0); }
		else { tabView_->select(std::min(current_, tabs_.size() - 1)); }
	}

	void Options::select(size_t t)
	{
		using A = Widget::Align;
		current_ = t;
		Widget& body = rows_->body();
		body.clear();
		shownRows_.clear();
		rows_->offset = 0;
		if (t >= tabs_.size()) { return; }
		const bool keyTab = tabs_[t].prefix == "key" || tabs_[t].prefix == "key.debug";
		std::vector<cfg::Info> infos;
		for (const cfg::Info& info : cfg::list()) { if (prefixOf(info.key) == tabs_[t].prefix) { infos.push_back(info); } }
		if (keyTab)	// the actions' order, not the keys'
		{
			auto at = [](const cfg::Info& i)
			{
				const std::vector<keys::Action>& a = keys::actions();
				return std::find_if(a.begin(), a.end(), [&](const keys::Action& x) { return "key." + x.name == i.key; }) - a.begin();
			};
			std::stable_sort(infos.begin(), infos.end(), [&](const cfg::Info& x, const cfg::Info& y) { return at(x) < at(y); });
		}
		for (const cfg::Info& info : infos)
		{
			Box& line = body.add<Box>(false, 12);
			line.halign = A::kStretch;
			const keys::Action* action = keyTab ? keys::find(info.key.substr(4)) : nullptr;
			std::string label = action ? action->title : title(info.key.substr(tabs_[t].prefix.size() + 1));
			if (action && label.compare(0, 7, "Debug: ") == 0) { label = title(label.substr(7)); }
			Label& name = line.add<Label>(label);
			name.fit = false;
			name.rect = { 0, 0, action ? 330.0f : 230.0f, 40 };
			Row r = { info, &line, nullptr, nullptr };
			const std::string key = info.key;
			if (action)	// a box per key slot
			{
				for (int slot = 0; slot < keys::kSlots; slot++)
				{
					KeyField& f = line.add<KeyField>(action->name, slot);
					f.rect = { 0, 0, 150, 40 };
					r.keys[slot] = &f;
				}
				r.control = r.keys[0];
				shownRows_.push_back(r);
				continue;
			}
			switch (info.type)
			{
				case cfg::Type::kBool:
				{
					TickBox& tb = line.add<TickBox>();
					tb.rect.h = 40;
					tb.onToggle = [key](bool on) { cfg::setUser(key, on ? "on" : "off"); };
					r.control = &tb;
					break;
				}
				case cfg::Type::kInt:
				case cfg::Type::kFloat:
				{
					Slider& s = line.add<Slider>(info.min, info.max, step(info));
					s.rect = { 0, 0, 300, 40 };
					s.onChange = [info](double v) { cfg::setUser(info.key, number(info, v)); };
					r.control = &s;
					r.value = &line.add<Label>();
					r.value->fit = false;
					r.value->rect = { 0, 0, 70, 40 };
					r.value->align = TextAlign::kEnd;
					break;
				}
				case cfg::Type::kText:
				{
					if (!info.choices.empty())	// a menu of the allowed values under the button
					{
						Button& b = line.add<Button>(info.value, Look::kButton, "Kenshi_StandardFont_Medium");
						b.rect = { 0, 0, 300, 40 };
						b.onClicked = [key, choices = info.choices, &b]
						{
							std::vector<Menu::Item> items;
							for (const std::string& c : choices) { items.push_back({ c, [key, c] { cfg::setUser(key, c); } }); }
							auto m = std::make_unique<Menu>(std::move(items));
							m->rect.w = b.screen().w / ui::scale();
							openMenu(std::move(m), b.screen().x, b.screen().bottom());
						};
						r.control = &b;
						break;
					}
					EditBox& e = line.add<EditBox>();
					e.rect = { 0, 0, tabs_[t].prefix == "ui.colour" ? 300.0f : 382.0f, 40 };
					e.onCommit = [key](const std::string& v) { cfg::setUser(key, v); };
					r.control = &e;
					if (tabs_[t].prefix == "ui.colour")
					{
						Swatch& sw = line.add<Swatch>();
						sw.rect = { 0, 0, 70, 40 };
						r.swatch = &sw;
					}
					break;
				}
			}
			shownRows_.push_back(r);
		}
		gen_ = 0;	// values next update
	}

	// Controls from the values in effect.
	void Options::refresh()
	{
		std::vector<cfg::Info> all = cfg::list();
		for (Row& r : shownRows_)
		{
			for (const cfg::Info& i : all)
			{
				if (i.key != r.info.key) { continue; }
				r.info = i;
				break;
			}
			const cfg::Info& i = r.info;
			const bool on = !i.pinned;
			if (auto* tb = dynamic_cast<TickBox*>(r.control)) { tb->checked = i.value == "on"; }
			if (auto* s = dynamic_cast<Slider*>(r.control)) { s->value = std::strtod(i.value.c_str(), nullptr); }
			if (auto* e = dynamic_cast<EditBox*>(r.control); e && !focused(e)) { e->text = i.value; }	// not while it is being typed in
			if (auto* b = dynamic_cast<Button*>(r.control); b && !dynamic_cast<TickBox*>(b)) { b->text = i.value; }
			if (r.swatch) { static_cast<Swatch*>(r.swatch)->value = i.value; }
			if (r.control) { r.control->enabled = on; }
			for (Widget* k : r.keys) { if (k) { k->enabled = on; } }
			if (r.value)
			{
				r.value->text = i.value;
				r.value->colour = on ? res::colour("Main") : res::colour("Main").alpha(0.5f);
			}
		}
	}

	void Options::update()
	{
		if (!window_->visible) { return; }
		if (cfg::list().size() != optionCount_) { build(); }	// an option was declared since (a module loaded late)
		const uint64_t g = cfg::generation();
		if (g != gen_)
		{
			gen_ = g;
			refresh();
		}
		// The help line shows the hovered row's help.
		float mx, my;
		mouse(mx, my);
		std::string help;
		for (const Row& r : shownRows_)
		{
			if (!r.box->shown() || !r.box->screen().contains(mx, my) || !rows_->screen().contains(mx, my)) { continue; }
			help = r.info.help;
			if (r.info.pinned) { help += " (set on the command line for this run)"; }
		}
		if (help.empty() && current_ < tabs_.size())
		{
			const Section* s = section(tabs_[current_].prefix);
			help = s ? s->note : "";
		}
		help_->text = help;
	}
}
