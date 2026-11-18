#include "app/steamvdf.hpp"

#include <algorithm>
#include <cctype>

namespace
{
struct Token
{
	enum Kind { Str, Open, Close, End } kind = End;
	std::string s;
};

struct Lexer
{
	const std::string& t;
	std::size_t i = 0;
	explicit Lexer(const std::string& text) : t(text) {}

	Token next()
	{
		for (;;)
		{
			while (i < t.size() && isspace((unsigned char)t[i])) { i++; }
			if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '/')
			{
				while (i < t.size() && t[i] != '\n') { i++; }
				continue;
			}
			break;
		}
		Token k;
		if (i >= t.size()) { return k; }
		const char c = t[i];
		if (c == '{') { i++; k.kind = Token::Open; return k; }
		if (c == '}') { i++; k.kind = Token::Close; return k; }
		k.kind = Token::Str;
		if (c == '"')
		{
			i++;
			while (i < t.size() && t[i] != '"')
			{
				if (t[i] == '\\' && i + 1 < t.size())
				{
					const char e = t[i + 1];
					k.s += e == 'n' ? '\n' : e == 't' ? '\t' : e;	// \\ and \" are the ones that matter; any other escape keeps its letter
					i += 2;
				}
				else { k.s += t[i++]; }
			}
			if (i < t.size()) { i++; }
		}
		else
		{
			while (i < t.size() && !isspace((unsigned char)t[i]) && t[i] != '{' && t[i] != '}') { k.s += t[i++]; }
		}
		return k;
	}
};

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}

bool numeric(const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; }

// Walks the file; calls onPair(stack of enclosing keys, key, value) for every "key" "value" pair.
template <class F> void walk(const std::string& text, F onPair)
{
	Lexer lx(text);
	std::vector<std::string> stack;
	for (;;)
	{
		Token a = lx.next();
		if (a.kind == Token::End) { return; }
		if (a.kind == Token::Close) { if (!stack.empty()) { stack.pop_back(); } continue; }
		if (a.kind == Token::Open) { stack.push_back(std::string()); continue; }	// a block with no key: keep the depth right
		Token b = lx.next();
		if (b.kind == Token::End) { return; }
		if (b.kind == Token::Open) { stack.push_back(lower(a.s)); }
		else if (b.kind == Token::Str) { onPair(stack, a.s, b.s); }
		else if (!stack.empty()) { stack.pop_back(); }	// key then '}'
	}
}
}

std::vector<std::string> steamvdf::libraryPaths(const std::string& text)
{
	std::vector<std::string> out;
	const auto add = [&](const std::string& p)
	{
		if (!p.empty() && std::find(out.begin(), out.end(), p) == out.end()) { out.push_back(p); }
	};
	walk(text, [&](const std::vector<std::string>& st, const std::string& key, const std::string& val)
	{
		if (st.size() == 1 && st[0] == "libraryfolders" && numeric(key)) { add(val); }
		else if (st.size() == 2 && st[0] == "libraryfolders" && numeric(st[1]) && lower(key) == "path") { add(val); }
	});
	return out;
}

std::string steamvdf::installDir(const std::string& text)
{
	std::string out;
	walk(text, [&](const std::vector<std::string>& st, const std::string& key, const std::string& val)
	{
		if (out.empty() && st.size() == 1 && st[0] == "appstate" && lower(key) == "installdir") { out = val; }
	});
	return out;
}
