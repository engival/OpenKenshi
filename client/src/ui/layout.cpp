#include "ui/layout.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

#include "app/config.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const cfg::Bool kReload{ cfg::Group{ "UI development", "", "Debug" }, "ui.layout_reload", true, "Layout files (client/data/ui) rebuild their screen when saved" };
		std::string gDir;
		std::vector<Layout*> gLive;
		using Clock = std::chrono::steady_clock;
		Clock::time_point gLastPoll;

		const std::map<std::string, Look>& looks()
		{
			static const std::map<std::string, Look> k = { { "none", Look::kNone }, { "window", Look::kWindow }, { "panel", Look::kPanel },
				{ "inset", Look::kInset }, { "field", Look::kField }, { "button", Look::kButton }, { "hud", Look::kHudPanel } };
			return k;
		}

		bool number(const std::string& s, float& out)
		{
			char* end = nullptr;
			out = std::strtof(s.c_str(), &end);
			return !s.empty() && end && *end == 0;
		}

		bool align(const std::string& s, Widget::Align& out)
		{
			static const std::map<std::string, Widget::Align> k = { { "start", Widget::Align::kStart }, { "end", Widget::Align::kEnd },
				{ "center", Widget::Align::kCenter }, { "stretch", Widget::Align::kStretch } };
			auto it = k.find(s);
			if (it == k.end()) { return false; }
			out = it->second;
			return true;
		}
	}

	void setLayoutDir(const std::string& dataDir) { gDir = dataDir; }

	struct Layout::Node
	{
		std::string kind, text;
		bool hasText = false;
		std::map<std::string, std::string> attrs;
		int line = 0, indent = 0;
		std::vector<Node> kids;
	};

	Layout::Layout(Widget& parent, std::string name, std::function<void(Layout&)> onBuild)
		: parent_(parent), name_(std::move(name)), onBuild_(std::move(onBuild))
	{
		gLive.push_back(this);
		reload();
	}

	Layout::~Layout()
	{
		std::erase(gLive, this);
		if (!ready()) { return; }
		for (Widget* w : roots_) { parent_.remove(w); }
		if (errorBox_) { parent_.remove(errorBox_); }
	}

	Widget* Layout::find(const std::string& id) const
	{
		auto it = ids_.find(id);
		return it == ids_.end() ? nullptr : it->second;
	}

	void Layout::missing(const std::string& id)
	{
		if (error_.empty()) { error_ = path_ + ": the screen needs a widget id=" + id + " of another kind or missing"; }
		fprintf(stderr, "kui: %s: no usable id=%s\n", path_.c_str(), id.c_str());
		showError();
	}

	void Layout::showError()
	{
		if (errorBox_) { parent_.remove(errorBox_); }
		errorBox_ = nullptr;
		if (error_.empty()) { return; }
		Panel& box = parent_.add<Panel>(Look::kInset, 10);
		box.halign = Widget::Align::kStretch;
		box.tint = res::colour("Bad");
		Label& l = box.add<Label>(error_, "Kenshi_StandardFont_Medium");
		l.colour = res::colour("BadBright");
		l.halign = Widget::Align::kStretch;
		l.wrap = true;
		box.solid = true;
		errorBox_ = &box;
	}

	void Layout::reload()
	{
		path_ = (gDir.empty() ? std::string("<no layout dir>") : gDir) + "/ui/" + name_ + ".layout";
		std::error_code ec;
		stamp_ = std::filesystem::last_write_time(path_, ec);
		const bool good = build();
		showError();
		if (good && onBuild_) { onBuild_(*this); }
	}

	void Layout::pollAll()
	{
		if (!kReload.get() || Clock::now() - gLastPoll < std::chrono::milliseconds(500)) { return; }
		gLastPoll = Clock::now();
		for (Layout* l : std::vector<Layout*>(gLive))
		{
			std::error_code ec;
			const auto t = std::filesystem::last_write_time(l->path_, ec);
			if (!ec && t != l->stamp_) { l->reload(); }
		}
	}

	// Parses the file into nodes, then builds them; on any error the old tree stays and error_ says file:line.
	bool Layout::build()
	{
		auto fail = [this](int line, const std::string& what)
		{
			error_ = path_ + (line > 0 ? ":" + std::to_string(line) : std::string()) + ": " + what;
			fprintf(stderr, "kui: %s\n", error_.c_str());
			return false;
		};
		if (gDir.empty()) { return fail(0, "no layout dir (the host calls ui::setLayoutDir)"); }
		std::ifstream in(path_);
		if (!in) { return fail(0, "can't open the file"); }

		// ---- parse ----
		Node top;
		top.indent = -1;
		std::vector<Node*> stack = { &top };
		std::string raw;
		for (int ln = 1; std::getline(in, raw); ln++)
		{
			if (!raw.empty() && raw.back() == '\r') { raw.pop_back(); }
			size_t i = 0;
			int indent = 0;
			for (; i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'); i++) { indent += raw[i] == '\t' ? 4 : 1; }
			if (i == raw.size() || raw[i] == '#') { continue; }
			Node n;
			n.line = ln;
			n.indent = indent;
			bool first = true;
			while (i < raw.size())
			{
				while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) { i++; }
				if (i == raw.size()) { break; }
				std::string key, value;
				bool quoted = false;
				auto word = [&](std::string& out)
				{
					if (raw[i] == '"')
					{
						const size_t end = raw.find('"', i + 1);
						if (end == std::string::npos) { return false; }
						out = raw.substr(i + 1, end - i - 1);
						i = end + 1;
						quoted = true;
						return true;
					}
					const size_t start = i;
					while (i < raw.size() && raw[i] != ' ' && raw[i] != '\t' && raw[i] != '=') { i++; }
					out = raw.substr(start, i - start);
					return true;
				};
				if (!word(key)) { return fail(ln, "a quote isn't closed"); }
				if (!quoted && i < raw.size() && raw[i] == '=')
				{
					i++;
					quoted = false;
					if (i == raw.size() || raw[i] == ' ' || raw[i] == '\t') { return fail(ln, key + "= has no value"); }
					if (!word(value)) { return fail(ln, "a quote isn't closed"); }
					if (first) { return fail(ln, "a line starts with its kind, not " + key + "="); }
					if (n.attrs.count(key)) { return fail(ln, key + " given twice"); }
					n.attrs[key] = value;
				}
				else if (first) { n.kind = key; }
				else if (quoted && !n.hasText) { n.text = key; n.hasText = true; }
				else { return fail(ln, "'" + key + "': text goes in quotes, once; settings are key=value"); }
				first = false;
			}
			while (stack.back()->indent >= indent) { stack.pop_back(); }
			stack.back()->kids.push_back(std::move(n));
			stack.push_back(&stack.back()->kids.back());
		}
		if (top.kids.empty()) { return fail(0, "no widgets in the file"); }

		// ---- build (into a holder, so a failure leaves the old tree) ----
		Widget holder;
		std::map<std::string, Widget*> ids;
		std::set<std::string> slots;
		std::string err;
		int errLine = 0;
		std::function<bool(const Node&, Widget&, const Flex*)> make = [&](const Node& n, Widget& into, const Flex* flex) -> bool
		{
			auto bad = [&](const std::string& what) { err = what; errLine = n.line; return false; };
			std::map<std::string, std::string> a = n.attrs;
			auto take = [&](const char* k, std::string& out) { auto it = a.find(k); if (it == a.end()) { return false; } out = it->second; a.erase(it); return true; };
			auto num = [&](const char* k, float& out) -> int	// 0 absent, 1 read, -1 bad
			{
				std::string v;
				if (!take(k, v)) { return 0; }
				return number(v, out) ? 1 : -1;
			};
			std::string font;
			take("font", font);
			auto fontOr = [&](const char* def) { return font.empty() ? std::string(def) : font; };
			const bool leaf = n.kind == "label" || n.kind == "button" || n.kind == "spacer" || n.kind == "tabs" || n.kind == "edit" || n.kind == "check"
				|| n.kind == "dropdown" || n.kind == "slider";
			if (leaf && !n.kids.empty()) { return bad(n.kind + " can't hold widgets"); }
			if (n.hasText && n.kind != "label" && n.kind != "button" && n.kind != "check" && n.kind != "dropdown" && n.kind != "edit") { return bad(n.kind + " takes no text"); }
			float gap = 0, pad = 0;
			if (num("gap", gap) < 0 || num("pad", pad) < 0) { return bad("gap / pad must be numbers"); }
			Widget* w = nullptr;
			Widget* inner = nullptr;	// where the node's children go
			FlexItem item;
			bool hasItem = false;
			if (n.kind == "row" || n.kind == "col")
			{
				Flex& f = flex ? const_cast<Flex*>(flex)->add<Flex>(FlexItem{}, n.kind == "col", gap, pad) : into.add<Flex>(n.kind == "col", gap, pad);
				std::string j;
				if (take("justify", j) && !align(j, f.justify)) { return bad("justify=" + j + ": start, center or end"); }
				w = inner = &f;
			}
			else if (n.kind == "stack" || n.kind == "slot" || n.kind == "spacer") { w = inner = flex ? &const_cast<Flex*>(flex)->add<Widget>(FlexItem{}) : &into.add<Widget>(); }
			else if (n.kind == "panel")
			{
				std::string l = "panel";
				take("look", l);
				auto it = looks().find(l);
				if (it == looks().end()) { return bad("look=" + l + ": none, window, panel, inset, field, button or hud"); }
				w = inner = flex ? &const_cast<Flex*>(flex)->add<Panel>(FlexItem{}, it->second, pad) : &into.add<Panel>(it->second, pad);
			}
			else if (n.kind == "scroll")
			{
				ScrollView& s = flex ? const_cast<Flex*>(flex)->add<ScrollView>(FlexItem{}) : into.add<ScrollView>();
				static_cast<Box&>(s.body()).spacing = gap;
				w = &s;
				inner = &s.body();
			}
			else if (n.kind == "tabs")
			{
				std::string side = "top";
				take("side", side);
				if (side != "top" && side != "left") { return bad("side=" + side + ": top or left"); }
				const Tabs::Side sd = side == "left" ? Tabs::Side::kLeft : Tabs::Side::kTop;
				w = flex ? &const_cast<Flex*>(flex)->add<Tabs>(FlexItem{}, sd, fontOr("Kenshi_StandardFont_Medium")) : &into.add<Tabs>(sd, fontOr("Kenshi_StandardFont_Medium"));
			}
			else if (n.kind == "label")
			{
				const std::string f = fontOr("Kenshi_StandardFont_Medium");
				Label& l = flex ? const_cast<Flex*>(flex)->add<Label>(FlexItem{}, n.text, f) : into.add<Label>(n.text, f);
				std::string v;
				if (take("colour", v))
				{
					Color c;
					l.colour = Color::parse(v, c) ? c : res::colour(v);
				}
				float alpha = 1;
				if (const int r = num("alpha", alpha); r) { if (r < 0 || alpha < 0 || alpha > 1) { return bad("alpha must be 0..1"); } l.colour = l.colour.alpha(alpha); }
				auto textAlign = [](const std::string& t, TextAlign& out)
				{
					if (t == "start") { out = TextAlign::kStart; } else if (t == "center") { out = TextAlign::kCenter; } else if (t == "end") { out = TextAlign::kEnd; } else { return false; }
					return true;
				};
				if (take("text_align", v) && !textAlign(v, l.align)) { return bad("text_align=" + v + ": start, center or end"); }
				if (take("text_valign", v) && !textAlign(v, l.vAlign)) { return bad("text_valign=" + v + ": start, center or end"); }
				if (take("wrap", v))
				{
					if (v != "on" && v != "off") { return bad("wrap=" + v + ": on or off"); }
					l.wrap = v == "on";	// still fit: size=auto is the wrapped text's height at its width
				}
				w = &l;
			}
			else if (n.kind == "button")
			{
				const std::string f = fontOr("Kenshi_PaintedTextFont_Small");
				Button& b = flex ? const_cast<Flex*>(flex)->add<Button>(FlexItem{}, n.text, Look::kButton, f) : into.add<Button>(n.text, Look::kButton, f);
				w = &b;
			}
			else if (n.kind == "edit")
			{
				const std::string f = fontOr("Kenshi_StandardFont_Medium");
				w = flex ? &const_cast<Flex*>(flex)->add<EditBox>(FlexItem{}, "", f) : &into.add<EditBox>("", f);
				static_cast<EditBox*>(w)->hint = n.text;
			}
			else if (n.kind == "check")
			{
				const std::string f = fontOr("Kenshi_StandardFont_Medium");
				w = flex ? &const_cast<Flex*>(flex)->add<TickBox>(FlexItem{}, n.text, f) : &into.add<TickBox>(n.text, f);
			}
			else if (n.kind == "dropdown")
			{
				const std::string f = fontOr("Kenshi_StandardFont_Medium");
				w = flex ? &const_cast<Flex*>(flex)->add<Dropdown>(FlexItem{}, n.text, f) : &into.add<Dropdown>(n.text, f);
			}
			else if (n.kind == "slider")
			{
				float lo = 0, hi = 100, st = 1;
				if (num("min", lo) < 0 || num("max", hi) < 0 || num("step", st) < 0) { return bad("min / max / step must be numbers"); }
				if (hi <= lo || st < 0) { return bad("a slider wants min < max and step >= 0"); }
				Slider& sl = flex ? const_cast<Flex*>(flex)->add<Slider>(FlexItem{}, lo, hi, st) : into.add<Slider>(lo, hi, st);
				sl.value = lo;
				w = &sl;
			}
			else { return bad("unknown kind '" + n.kind + "'"); }
			// Common settings.
			std::string v;
			if (take("id", v))
			{
				if (ids.count(v)) { return bad("id=" + v + " is used twice"); }
				ids[v] = w;
				if (n.kind == "slot") { slots.insert(v); }
			}
			if (num("w", w->rect.w) < 0 || num("h", w->rect.h) < 0 || num("x", w->rect.x) < 0 || num("y", w->rect.y) < 0) { return bad("w / h / x / y must be numbers"); }
			if (take("tip", v)) { w->tooltip = v; }
			if (take("visible", v)) { w->visible = v != "off"; }
			// In a row / col: size along it, align across it (default stretch). Elsewhere: halign / valign (default stretch).
			const Widget::Align def = Widget::Align::kStretch;
			w->halign = w->valign = def;
			if (flex)
			{
				if (take("size", v))
				{
					hasItem = true;
					if (v == "auto") { item.basis = -1; }
					else if (v == "fill") { item.fill = 1; }
					else if (v.rfind("fill:", 0) == 0) { if (!number(v.substr(5), item.fill) || item.fill <= 0) { return bad("size=" + v + ": fill:N wants a weight above 0"); } }
					else if (!number(v, item.basis)) { return bad("size=" + v + ": a number, auto, fill or fill:N"); }
				}
				if (const int r = num("min", item.min); r) { if (r < 0) { return bad("min must be a number"); } hasItem = true; }
				if (const int r = num("max", item.max); r) { if (r < 0) { return bad("max must be a number"); } hasItem = true; }
				if (hasItem) { const_cast<Flex*>(flex)->set(*w, item); }
				Widget::Align c = def;
				if (take("align", v) && !align(v, c)) { return bad("align=" + v + ": start, center, end or stretch"); }
				(flex->vertical ? w->halign : w->valign) = c;
			}
			else
			{
				if (take("halign", v) && !align(v, w->halign)) { return bad("halign=" + v + ": start, center, end or stretch"); }
				if (take("valign", v) && !align(v, w->valign)) { return bad("valign=" + v + ": start, center, end or stretch"); }
			}
			if (!a.empty()) { return bad("'" + a.begin()->first + "' isn't a setting of " + n.kind); }
			for (const Node& k : n.kids) { if (!make(k, *inner, dynamic_cast<Flex*>(inner))) { return false; } }
			return true;
		};
		for (const Node& n : top.kids) { if (!make(n, holder, nullptr)) { return fail(errLine, err); } }

		// ---- swap in: the slots' children move to the new slots of the same id ----
		for (const std::string& id : slots_)
		{
			if (!slots.count(id)) { continue; }	// gone from the file: its children go with the old tree
			Widget* from = ids_.at(id);
			Widget* to = ids.at(id);
			while (!from->children().empty()) { to->adopt(from->take(from->children().front().get())); }
		}
		for (Widget* r : roots_) { parent_.remove(r); }
		roots_.clear();
		while (!holder.children().empty())
		{
			Widget* c = holder.children().front().get();
			roots_.push_back(&parent_.adopt(holder.take(c)));
		}
		ids_ = std::move(ids);
		spares_.clear();
		error_.clear();
		slots_ = std::move(slots);
		return true;
	}
}
