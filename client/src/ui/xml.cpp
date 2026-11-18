#include "ui/xml.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

namespace ui::xml
{
	namespace
	{
		struct Reader
		{
			const std::string& s;
			size_t p = 0;
			std::string err;

			bool eof() const { return p >= s.size(); }
			void blanks() { while (!eof() && strchr(" \t\r\n", s[p])) { p++; } }
			bool skipPast(const char* end)
			{
				const size_t q = s.find(end, p);
				if (q == std::string::npos) { return false; }
				p = q + strlen(end);
				return true;
			}
			std::string name()
			{
				const size_t b = p;
				while (!eof() && !strchr(" \t\r\n/>=", s[p])) { p++; }
				return s.substr(b, p - b);
			}
			static std::string unescape(const std::string& v)
			{
				std::string o;
				for (size_t i = 0; i < v.size(); i++)
				{
					if (v[i] != '&') { o += v[i]; continue; }
					const size_t semi = v.find(';', i);
					const std::string e = semi == std::string::npos ? "" : v.substr(i + 1, semi - i - 1);
					const char c = e == "amp" ? '&' : e == "lt" ? '<' : e == "gt" ? '>' : e == "quot" ? '"' : e == "apos" ? '\'' : 0;
					if (!c) { o += v[i]; continue; }
					o += c;
					i = semi;
				}
				return o;
			}

			// Skips text, comments and declarations up to the next element tag; false at the end of input.
			bool toTag()
			{
				for (;;)
				{
					const size_t q = s.find('<', p);
					if (q == std::string::npos) { p = s.size(); return false; }
					p = q;
					if (s.compare(p, 4, "<!--") == 0) { if (!skipPast("-->")) { err = "unterminated comment"; return false; } continue; }
					if (s.compare(p, 2, "<?") == 0 || s.compare(p, 2, "<!") == 0) { if (!skipPast(">")) { err = "unterminated declaration"; return false; } continue; }
					return true;
				}
			}

			// At '<' of a start tag.
			std::unique_ptr<Node> element()
			{
				auto n = std::make_unique<Node>();
				p++;
				n->name = name();
				if (n->name.empty()) { err = "empty element name at " + std::to_string(p); return nullptr; }
				for (;;)
				{
					blanks();
					if (eof()) { err = "unterminated tag <" + n->name; return nullptr; }
					if (s[p] == '/')
					{
						if (s.compare(p, 2, "/>") != 0) { err = "bad '/' in <" + n->name; return nullptr; }
						p += 2;
						return n;
					}
					if (s[p] == '>') { p++; break; }
					std::string key = name();
					blanks();
					if (key.empty() || eof() || s[p] != '=') { err = "bad attribute in <" + n->name; return nullptr; }
					p++;
					blanks();
					if (eof() || (s[p] != '"' && s[p] != '\'')) { err = "unquoted attribute " + key; return nullptr; }
					const char q = s[p++];
					const size_t e = s.find(q, p);
					if (e == std::string::npos) { err = "unterminated attribute " + key; return nullptr; }
					n->attrs.emplace_back(std::move(key), unescape(s.substr(p, e - p)));
					p = e + 1;
				}
				for (;;)
				{
					if (!toTag()) { if (err.empty()) { err = "missing </" + n->name + ">"; } return nullptr; }
					if (s.compare(p, 2, "</") == 0)
					{
						p += 2;
						const std::string close = name();
						if (close != n->name) { err = "</" + close + "> closes <" + n->name + ">"; return nullptr; }
						if (!skipPast(">")) { err = "unterminated </" + close; return nullptr; }
						return n;
					}
					auto c = element();
					if (!c) { return nullptr; }
					n->children.push_back(std::move(c));
				}
			}
		};
	}

	const std::string& Node::attr(const char* key) const
	{
		static const std::string none;
		for (const auto& [k, v] : attrs) { if (k == key) { return v; } }
		return none;
	}

	bool Node::has(const char* key) const
	{
		for (const auto& kv : attrs) { if (kv.first == key) { return true; } }
		return false;
	}

	std::vector<const Node*> Node::all(const char* childName) const
	{
		std::vector<const Node*> out;
		for (const auto& c : children) { if (c->name == childName) { out.push_back(c.get()); } }
		return out;
	}

	const Node* Node::first(const char* childName) const
	{
		for (const auto& c : children) { if (c->name == childName) { return c.get(); } }
		return nullptr;
	}

	std::unique_ptr<Node> parse(const std::string& text, std::string& err)
	{
		Reader r{ text, 0, {} };
		if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) { r.p = 3; }
		if (!r.toTag()) { err = r.err.empty() ? "no element" : r.err; return nullptr; }
		auto root = r.element();
		if (!root) { err = r.err; }
		return root;
	}

	std::unique_ptr<Node> load(const std::string& path, std::string& err)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f) { err = "cannot read " + path; return nullptr; }
		std::stringstream ss;
		ss << f.rdbuf();
		auto n = parse(ss.str(), err);
		if (!n) { err = path + ": " + err; }
		return n;
	}
}
