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
		// Notes for the groups that need one, shown under the group's header.
		struct GroupNote { const char* group; const char* note; };
		const GroupNote kGroupNotes[] = {
			{ "Colours", "The flat style's colours: #rrggbb or #rrggbbaa. Some text colours change only at the next start." },
			{ "Installed mods", "Mod changes take effect at the next start." },
			{ "Keys", "Click a box, then press the key (hold Ctrl, Alt or Shift with it). Escape cancels, a right click clears. Red: another action has it too." },
			{ "Debug keys", "The client's development keys. Red: a game key has it too." },
		};

		const char* groupNote(const std::string& group)
		{
			for (const GroupNote& n : kGroupNotes) { if (group == n.group) { return n.note; } }
			return "";
		}

		// The prefixes a row's label drops beyond the key's first part ("ui.colour.Main" -> "Main", not "Colour Main").
		const char* const kLongPrefixes[] = { "ui.colour", "key.debug" };

		bool keyRow(const cfg::Info& i) { return i.key.compare(0, 4, "key.") == 0 && keys::find(i.key.substr(4)); }

		constexpr float kW = 960, kBodyH = 620, kMinW = 700, kMinH = 420, kRowH = 40, kHeaderH = 38, kNeedsH = 22;
		constexpr const char* kPinnedTip = "set on the command line for this run; changing it here saves it";	// a --set row's tooltip (k16.2.303.1)

		// A group's header in the rows list: its name in the Accent colour (Title is a dark red under the original style) over a hairline rule, the same in both styles.
		class GroupHeader : public Widget
		{
		public:
			explicit GroupHeader(const std::string& text)
			{
				Label& l = add<Label>(text, "Kenshi_StandardFont_Medium");
				l.colour = res::colour("Accent");
				l.halign = Align::kStretch;
				l.valign = Align::kStretch;
				l.fit = false;
				l.vAlign = TextAlign::kEnd;
				l.rect = { 0, 0, 0, 0 };
				label = &l;
			}
			Label* label = nullptr;

			void place(const Rect& px, float s) override
			{
				Widget::place(px, s);
				label->place({ px.x + 2 * s, px.y, px.w - 2 * s, px.h - 6 * s }, s);
			}

		protected:
			void paint() const override
			{
				const float b = std::max(1.0f, std::round(scale_));
				gfx::fill({ screen_.x, screen_.bottom() - 3 * scale_ - b, screen_.w, b }, res::colour("Accent").alpha(0.45f));
			}
		};

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

		// A row: a Flex that draws the find's highlight behind its widgets.
		class Line : public Flex
		{
		public:
			Line() : Flex(false, 12) {}
			bool hit = false;

		protected:
			void paint() const override
			{
				if (!hit) { return; }
				const float b = std::max(1.0f, std::round(scale_ * 2));
				const Rect r = { screen_.x - 6 * scale_, screen_.y, screen_.w + 12 * scale_, screen_.h };
				gfx::fill(r, res::colour("Accent").alpha(0.22f));
				gfx::frame(r, b, res::colour("Accent"));
			}
		};

		std::string lower(std::string s)
		{
			for (char& c : s) { c = (char)tolower((unsigned char)c); }
			return s;
		}

		// q (lowercase) starts a word of s (lowercase): at its start or after a character that is not a letter or digit ("sun" in "render.sun_shafts").
		bool wordStart(const std::string& s, const std::string& q)
		{
			for (size_t p = s.find(q); p != std::string::npos; p = s.find(q, p + 1))
			{
				if (p == 0 || !isalnum((unsigned char)s[p - 1])) { return true; }
			}
			return false;
		}

		// How well an option matches the find (k16.2.227.5, user: "SCORE and SORT the matches"): 0 is best, -1 no match. The label first (equal,
		// starts with, a word starts with, contains), then the cfg key's words, then the help's (a word starts with, contains), the key anywhere last.
		int score(const std::string& label, const std::string& key, const std::string& help, const std::string& q)
		{
			if (label == q) { return 0; }
			if (label.compare(0, q.size(), q) == 0) { return 1; }
			if (wordStart(label, q)) { return 2; }
			if (label.find(q) != std::string::npos) { return 3; }
			if (wordStart(key, q)) { return 4; }
			if (wordStart(help, q)) { return 5; }
			if (help.find(q) != std::string::npos) { return 6; }
			if (key.find(q) != std::string::npos) { return 7; }
			return -1;
		}

		// "audio.music" -> "Music", "ui.tooltip_delay" -> "Tooltip delay".
		std::string title(std::string s)
		{
			for (char& c : s) { if (c == '_' || c == '.') { c = ' '; } }
			if (!s.empty()) { s[0] = (char)toupper((unsigned char)s[0]); }
			return s;
		}

		// The row's end: a reset button in a fixed cell, so the rows' right edges line up whether it shows or not.
		Button& resetButton(Flex& line, const cfg::Info& info)
		{
			Widget& cell = line.add<Widget>(FlexItem{ 26 });
			cell.valign = Widget::Align::kStretch;
			Button& b = cell.add<Button>("", Look::kReset);
			b.rect = { 0, 0, 26, 26 };
			b.halign = b.valign = Widget::Align::kCenter;
			b.tooltip = "Reset to the default: " + (info.def.empty() ? std::string("none") : info.def);
			b.onClicked = [key = info.key, def = info.def] { cfg::setUser(key, def); };
			return b;
		}

		// A "needs KEY", "needs KEY below N" / "above N" note (cfg::Group::needs) against the values in effect: false while the option it needs
		// is off (a Bool off, a number 0, or outside the bound). Any other note, or a key not declared, counts as met (never dimmed).
		bool needsMet(const std::string& needs, const std::vector<cfg::Info>& all)
		{
			char key[96], dir[16];
			double bound = 0;
			const int n = sscanf(needs.c_str(), "needs %95[A-Za-z0-9_.] %15s %lf", key, dir, &bound);
			if (n < 1) { return true; }
			for (const cfg::Info& i : all)
			{
				if (i.key != key) { continue; }
				if (i.type == cfg::Type::kBool) { return i.value == "on"; }
				if (i.type == cfg::Type::kText) { return true; }
				const double v = std::strtod(i.value.c_str(), nullptr);
				if (n == 3 && std::string(dir) == "below") { return v < bound; }
				if (n == 3 && std::string(dir) == "above") { return v > bound; }
				return v != 0;
			}
			return true;
		}

		double stepOf(const cfg::Info& i) { return i.type == cfg::Type::kInt ? 1 : (i.max - i.min) / 100; }

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
		window_ = &root().add<CaptionWindow>("OPTIONS", [this] { show(false); }, 16);
		window_->halign = window_->valign = A::kCenter;
		window_->rect = { 0, 0, kW, CaptionWindow::kHeaderH + kBodyH };
		window_->resizable = true;
		window_->minSize = { kMinW, kMinH };
		window_->visible = false;
		window_->body().rect.h = kBodyH;
		// Ctrl+F while the window itself has the keyboard (not a row's field or key box) focuses the find field.
		window_->keyHook = [this](int key, bool down)
		{
			if (key != SAPP_KEYCODE_F || !(mods() & SAPP_MODIFIER_CTRL)) { return false; }
			if (down) { focusSearch(); }
			return true;
		};
		layout_ = std::make_unique<Layout>(window_->body(), "options", [this](Layout& l) { bind(l); });
		if (!tabBar_) { bind(*layout_); }	// the file failed: spares, so the screen works under the red box
	}

	void Options::bind(Layout& l)
	{
		tabBar_ = &l.at<Tabs>("tabs");
		tabBar_->onSelect = [this](size_t t) { select(t); };	// the find stays; its highlight shows when its row is on the tab
		rows_ = &l.at<ScrollView>("rows");
		help_ = &l.at<Label>("help");
		search_ = &l.at<EditBox>("find");
		search_->text = query_;	// a reload keeps the find
		search_->keepsKeyboard = true;
		search_->onChange = [this](const std::string& text) { find(text); };
		search_->onEnter = [this] { step(!(mods() & SAPP_MODIFIER_SHIFT)); };
		search_->onEscape = [this]	// the first Escape clears the find, the next (an empty field) closes the window as the window's own does
		{
			if (search_->text.empty()) { show(false); }
			else
			{
				find("");
				focus(window_);
			}
			return true;
		};
		prev_ = &l.at<Button>("find_prev");
		prev_->onClicked = [this] { step(false); };
		next_ = &l.at<Button>("find_next");
		next_->onClicked = [this] { step(true); };
		count_ = &l.at<Label>("find_count");
		l.at<Button>("default").onClicked = [this]
		{
			for (const Row& r : shownRows_) { cfg::setUser(r.info.key, r.info.def); }	// pinned rows too: the user's choice ends the pin (k16.2.303.1)
		};
		close_ = &l.at<Button>("close");
		close_->onClicked = [this] { show(false); };
		shownRows_.clear();
		build();
	}

	Options::~Options()
	{
		layout_.reset();	// before its parent goes
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
		else
		{
			if (focused(window_) || focused(search_)) { focus(nullptr); }
			if (!query_.empty()) { find(""); }	// a reopen starts without one
		}
	}

	void Options::focusSearch()
	{
		focus(search_);
		search_->select(0, search_->text.size());
	}

	void Options::find(const std::string& text)
	{
		if (search_->text != text) { search_->text = text; }
		if (text == query_) { return; }
		query_ = text;
		collect();
		hit_ = -1;
		if (!hits_.empty()) { go(0); }	// the best match, whichever tab it is on
		else { mark(); }
	}

	void Options::step(bool forward)
	{
		if (hits_.empty()) { return; }
		const int n = (int)hits_.size();
		go(hit_ < 0 ? (forward ? 0 : n - 1) : (hit_ + (forward ? 1 : n - 1)) % n);
	}

	void Options::collect()
	{
		hits_.clear();
		if (query_.empty()) { return; }
		const std::string q = lower(query_);
		for (size_t t = 0; t < tabs_.size(); t++)
		{
			for (const cfg::Info& info : options(t))
			{
				const int sc = score(lower(label(info)), lower(info.key), lower(info.help), q);
				if (sc >= 0) { hits_.push_back({ t, info.key, sc }); }
			}
		}
		std::stable_sort(hits_.begin(), hits_.end(), [](const Hit& a, const Hit& b) { return a.score < b.score; });	// ties: tab, then row
	}

	void Options::go(int h)
	{
		hit_ = h;
		if (hits_[h].tab != current_ || tabBar_->selected() != current_) { tabBar_->select(hits_[h].tab); }	// select() through onSelect
		mark();
	}

	void Options::mark()
	{
		const std::string key = hit_ >= 0 && hits_[hit_].tab == current_ ? hits_[hit_].key : "";
		for (size_t i = 0; i < shownRows_.size(); i++)
		{
			const bool on = shownRows_[i].info.key == key;
			static_cast<Line*>(shownRows_[i].box)->hit = on;
			if (on) { reveal(key); }
		}
		if (!count_) { return; }
		if (query_.empty()) { count_->text.clear(); }
		else if (hits_.empty()) { count_->text = "0 matches"; }
		else if (hit_ < 0) { count_->text = std::to_string(hits_.size()) + (hits_.size() == 1 ? " match" : " matches"); }
		else { count_->text = std::to_string(hit_ + 1) + " of " + std::to_string(hits_.size()); }
		count_->colour = !query_.empty() && hits_.empty() ? res::colour("Bad") : res::colour("Main").alpha(0.8f);
		prev_->enabled = next_->enabled = !hits_.empty();
	}

	const std::string& Options::countText() const { return count_->text; }

	std::string Options::highlighted() const
	{
		for (const Row& r : shownRows_) { if (static_cast<const Line*>(r.box)->hit) { return r.info.key; } }
		return "";
	}

	bool Options::rowInView(const std::string& key) const
	{
		for (const Row& r : shownRows_)
		{
			if (r.info.key != key) { continue; }
			const Rect a = r.box->screen(), v = rows_->screen();
			return r.box->shown() && a.h > 0 && a.y >= v.y - 0.5f && a.bottom() <= v.bottom() + 0.5f;
		}
		return false;
	}

	void Options::reveal(const std::string& key)
	{
		const auto& kids = rows_->body().children();
		for (const Row& r : shownRows_)
		{
			if (r.info.key != key) { continue; }
			for (size_t c = 0; c < kids.size(); c++) { if (kids[c].get() == r.box) { rows_->reveal(c); } }
		}
	}

	std::vector<std::string> Options::groups() const
	{
		std::vector<std::string> out;
		for (const auto& c : rows_->body().children()) { if (const auto* h = dynamic_cast<const GroupHeader*>(c.get())) { out.push_back(h->label->text); } }
		return out;
	}

	Label* Options::needsLabel(const std::string& key) const
	{
		for (const Row& r : shownRows_) { if (r.info.key == key) { return r.needs; } }
		return nullptr;
	}

	Widget* Options::keyField(const std::string& action, int slot) const
	{
		for (const Row& r : shownRows_) { if (r.info.key == "key." + action && slot >= 0 && slot < keys::kSlots) { return r.keys[slot]; } }
		return nullptr;
	}

	Button* Options::reset(const std::string& key) const
	{
		for (const Row& r : shownRows_) { if (r.info.key == key) { return r.reset; } }
		return nullptr;
	}

	const std::string& Options::helpText() const
	{
		return help_->text;
	}

	Widget* Options::value(const std::string& key) const
	{
		for (const Row& r : shownRows_) { if (r.info.key == key) { return r.value; } }
		return nullptr;
	}

	Widget* Options::control(const std::string& key) const
	{
		for (const Row& r : shownRows_) { if (r.info.key == key) { return r.control; } }
		return nullptr;
	}

	// The tab row: one per cfg section that has an option now, in cfg::sectionNames() order (kenshi-k16.2.244.2).
	void Options::build()
	{
		const std::vector<cfg::Info> all = cfg::list();
		optionCount_ = all.size();
		tabBar_->clear();
		tabs_.clear();
		// The client's development options (the Debug section: debug.*, the debug keys, ...): built only with the debug gate (--debug), and never in a
		// build without KCLIENT_DEV_TOOLS (kenshi-k16.2.153.2).
#ifdef KCLIENT_DEV_TOOLS
		const bool devBuild = true;
#else
		const bool devBuild = false;
#endif
		const char* heading = nullptr;
		for (const std::string& sec : cfg::sectionNames())
		{
			const bool development = sec == "Debug";
			if (development && (!devBuild || !keys::debug())) { continue; }
			if (std::none_of(all.begin(), all.end(), [&](const cfg::Info& i) { return i.section == sec; })) { continue; }
			const char* g = development ? "CLIENT" : "SETTINGS";	// the game's own options, then the client's development ones
			if (g != heading) { tabBar_->heading(g); }
			heading = g;
			tabBar_->add(sec);
			tabs_.push_back({ sec, sec, tabBar_->tab(tabs_.size()) });
		}
		if (tabs_.empty()) { select(0); }
		else { tabBar_->select(std::min(current_, tabs_.size() - 1)); }
		if (!query_.empty())	// the options changed: the matches again, the same one shown when it still is one
		{
			const std::string key = hit_ >= 0 ? hits_[hit_].key : "";
			collect();
			hit_ = -1;
			for (size_t i = 0; i < hits_.size(); i++) { if (hits_[i].key == key) { hit_ = (int)i; } }
			mark();
		}
	}

	// The tab's rows: by group (by name, the order of the saved file and --print-defaults), then by key; key rows in the actions' order.
	std::vector<cfg::Info> Options::options(size_t t) const
	{
		std::vector<cfg::Info> infos;
		if (t >= tabs_.size()) { return infos; }
		for (const cfg::Info& info : cfg::list()) { if (info.section == tabs_[t].section) { infos.push_back(info); } }
		const std::vector<keys::Action>& acts = keys::actions();
		auto at = [&](const cfg::Info& i)
		{
			if (!keyRow(i)) { return acts.size(); }
			return (size_t)(std::find_if(acts.begin(), acts.end(), [&](const keys::Action& x) { return "key." + x.name == i.key; }) - acts.begin());
		};
		std::stable_sort(infos.begin(), infos.end(), [&](const cfg::Info& x, const cfg::Info& y)
		{
			if (x.group != y.group) { return x.group < y.group; }
			return at(x) < at(y);	// cfg::list() is in key order already
		});
		return infos;
	}

	std::string Options::label(const cfg::Info& info) const
	{
		const keys::Action* action = keyRow(info) ? keys::find(info.key.substr(4)) : nullptr;
		if (action) { return action->title.compare(0, 7, "Debug: ") == 0 ? title(action->title.substr(7)) : action->title; }
		for (const char* p : kLongPrefixes)
		{
			const std::string pre = std::string(p) + ".";
			if (info.key.compare(0, pre.size(), pre) == 0 && info.key.size() > pre.size()) { return title(info.key.substr(pre.size())); }
		}
		const size_t dot = info.key.find('.');
		return title(dot == std::string::npos || dot + 1 == info.key.size() ? info.key : info.key.substr(dot + 1));
	}

	void Options::select(size_t t)
	{
		current_ = t;
		Widget& body = rows_->body();
		body.clear();
		shownRows_.clear();
		rows_->offset = 0;
		const std::string* group = nullptr;
		const std::vector<cfg::Info> infos = options(t);
		for (const cfg::Info& info : infos)
		{
			if (!group || *group != info.group)	// a header per group, its note (if any) under it
			{
				group = &info.group;
				GroupHeader& h = body.add<GroupHeader>(info.group);
				h.halign = Widget::Align::kStretch;
				h.rect.h = kHeaderH;
				if (const char* note = groupNote(info.group); *note)
				{
					Label& l = body.add<Label>(note, "Kenshi_StandardFont_MediumSmall");
					l.halign = Widget::Align::kStretch;
					l.wrap = true;
					l.colour = res::colour("Main").alpha(0.7f);
				}
			}
			addRow(info, t);
		}
		mark();
		gen_ = 0;	// values next update
	}

	// One row into the list: the name, the control, the reset button.
	void Options::addRow(const cfg::Info& info, size_t t)
	{
		using A = Widget::Align;
		Flex& line = rows_->body().add<Line>();
		line.halign = A::kStretch;
		line.rect.h = kRowH;
		const keys::Action* action = keyRow(info) ? keys::find(info.key.substr(4)) : nullptr;
		Label& name = line.add<Label>(FlexItem{ action ? 330.0f : 230.0f }, label(info));
		name.fit = false;
		name.valign = A::kStretch;
		auto fills = [&line](Widget& w, float max = 1e9f) { line.set(w, { .fill = 1, .min = 120, .max = max }); w.valign = A::kCenter; w.rect.h = kRowH; };
		Row r = { info, t, &line, nullptr, nullptr };
		r.name = &name;
		const std::string key = info.key;
		if (action)	// a box per key slot
		{
			for (int slot = 0; slot < keys::kSlots; slot++)
			{
				KeyField& f = line.add<KeyField>(FlexItem{ 150 }, action->name, slot);
				f.rect.h = kRowH;
				r.keys[slot] = &f;
			}
			r.control = r.keys[0];
			r.reset = &resetButton(line, info);
			r.needs = needsNote(info, t);
			shownRows_.push_back(r);
			return;
		}
		switch (info.type)
		{
			case cfg::Type::kBool:
			{
				line.add<Widget>(FlexItem{ .fill = 1 });	// the tick at the right edge, as the original
				TickBox& tb = line.add<TickBox>();
				tb.rect.h = kRowH;
				tb.onToggle = [key](bool on) { cfg::setUser(key, on ? "on" : "off"); };
				r.control = &tb;
				break;
			}
			case cfg::Type::kInt:
			case cfg::Type::kFloat:
			{
				Slider& s = line.add<Slider>(info.min, info.max, stepOf(info));
				fills(s, 520);
				s.notches.push_back(std::strtod(info.def.c_str(), nullptr));	// the default, and 0 when it is inside the range
				if (info.min < 0 && info.max > 0 && s.notches.back() != 0) { s.notches.push_back(0); }
				s.onChange = [info](double v) { cfg::setUser(info.key, number(info, v)); };
				r.control = &s;
				r.value = &line.add<Label>(FlexItem{ 70 });
				r.value->fit = false;
				r.value->valign = A::kStretch;
				r.value->align = TextAlign::kEnd;
				break;
			}
			case cfg::Type::kText:
			{
				if (!info.choices.empty())	// a menu of the allowed values under the button
				{
					Button& b = line.add<Button>(info.value, Look::kButton, "Kenshi_StandardFont_Medium");
					fills(b, 420);
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
				fills(e);
				e.onCommit = [key](const std::string& v) { cfg::setUser(key, v); };
				r.control = &e;
				if (info.key.compare(0, 10, "ui.colour.") == 0)
				{
					Swatch& sw = line.add<Swatch>(FlexItem{ 70 });
					sw.rect.h = kRowH;
					sw.valign = A::kCenter;
					r.swatch = &sw;
				}
				break;
			}
		}
		r.reset = &resetButton(line, info);
		r.needs = needsNote(info, t);
		shownRows_.push_back(r);
	}

	// A needs note as the player reads it (k16.2.244.7): every declared key in it named by its row label, with its tab when that is not tab t
	// ("needs render.cut_shadows" -> "needs Cut shadows", "needs Indoor light (Graphics) below 1"). The dimming and the find keep the raw key.
	std::string Options::needsText(const std::string& needs, size_t t) const
	{
		const std::vector<cfg::Info> all = cfg::list();
		std::string out;
		size_t i = 0;
		while (i < needs.size())
		{
			const size_t end = needs.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.", i);
			const size_t n = (end == std::string::npos ? needs.size() : end) - i;
			if (n == 0) { out += needs[i++]; continue; }
			std::string word = needs.substr(i, n), tail;
			while (!word.empty() && word.back() == '.') { word.pop_back(); tail += '.'; }	// a sentence's full stop is not part of the key
			const auto it = std::find_if(all.begin(), all.end(), [&](const cfg::Info& x) { return x.key == word; });
			if (it == all.end()) { out += word; }
			else
			{
				out += label(*it);
				if (t >= tabs_.size() || it->section != tabs_[t].section)
				{
					const auto tab = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& x) { return x.section == it->section; });
					out += " (" + (tab == tabs_.end() ? it->section : tab->title) + ")";
				}
			}
			out += tail;
			i += n;
		}
		return out;
	}

	// The option's needs note (cfg::Group::needs), a line under its row lined up with the control; null when it has none.
	Label* Options::needsNote(const cfg::Info& info, size_t t)
	{
		if (info.needs.empty()) { return nullptr; }
		Flex& line = rows_->body().add<Flex>(false, 12);
		line.halign = Widget::Align::kStretch;
		line.rect.h = kNeedsH;
		line.add<Widget>(FlexItem{ keyRow(info) ? 330.0f : 230.0f });
		Label& l = line.add<Label>(FlexItem{ .fill = 1 }, needsText(info.needs, t), "Kenshi_StandardFont_MediumSmall");
		l.fit = false;
		l.valign = Widget::Align::kStretch;
		l.vAlign = TextAlign::kStart;
		return &l;
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
			if (auto* tb = dynamic_cast<TickBox*>(r.control)) { tb->checked = i.value == "on"; }
			if (auto* s = dynamic_cast<Slider*>(r.control)) { s->value = std::strtod(i.value.c_str(), nullptr); }
			if (auto* e = dynamic_cast<EditBox*>(r.control); e && !focused(e)) { e->text = i.value; }	// not while it is being typed in
			if (auto* b = dynamic_cast<Button*>(r.control); b && !dynamic_cast<TickBox*>(b)) { b->text = i.value; }
			if (r.swatch) { static_cast<Swatch*>(r.swatch)->value = i.value; }
			// A command-line pin (--set) blocks nothing: the row works as any other, and a change ends the pin and is saved (cfg::setUser,
			// k16.2.303.1). Only the tooltip on the name and control says where the value came from.
			const std::string tip = i.pinned ? kPinnedTip : "";
			if (r.name) { r.name->tooltip = tip; }
			if (r.control) { r.control->tooltip = tip; }
			if (r.reset) { r.reset->visible = i.value != i.def; }
			if (r.value) { r.value->text = i.value; }
			if (r.needs) { r.needs->colour = res::colour("Main").alpha(needsMet(i.needs, all) ? 0.75f : 0.4f); }	// dimmed while what it needs is off
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
			if (r.info.pinned) { help += std::string(" (") + kPinnedTip + ")"; }
		}
		help_->text = help;
	}
}
