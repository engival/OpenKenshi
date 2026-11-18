// config_save (kenshi-k16.2.243.1): config.cpp's save() drops a line from kclient.cfg when its value equals the option's static
// compiled-in default (so a future default change reaches the user) and keeps everything else: comments, unknown keys (another
// build's), free-form raw keys (window.<layout>.*) and any value set away from the default. Built twice by run.sh, the second time
// with a different t.int default, to show a saved away value outlives a changed default. The defaults are the two macros below,
// standing in for the client's literal declarations; a value detected at runtime is never an option default and has no option here.
#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef T_INT_DEFAULT
#define T_INT_DEFAULT 5
#endif
#ifndef T_BOOL_DEFAULT
#define T_BOOL_DEFAULT 0
#endif

namespace
{
	const cfg::Bool kBool{ cfg::Group{ "Test", "", "Debug" }, "t.bool", T_BOOL_DEFAULT != 0, "Test: a tick box with a static default" };
	const cfg::Int kInt{ cfg::Group{ "Test", "", "Debug" }, "t.int", T_INT_DEFAULT, 0, 10, "Test: a slider with a static default" };
	int failures = 0;

	void check(bool ok, const std::string& what)
	{
		if (ok) { printf("ok: %s\n", what.c_str()); }
		else { ++failures; fprintf(stderr, "FAIL: %s\n", what.c_str()); }
	}

	std::string read(const std::string& p)
	{
		std::ifstream in(p);
		std::stringstream s;
		s << in.rdbuf();
		return s.str();
	}

	void write(const std::string& p, const std::string& text)
	{
		std::filesystem::create_directories(std::filesystem::path(p).parent_path());
		std::ofstream f(p);
		f << text;
	}

	bool hasLine(const std::string& text, const std::string& line)
	{
		std::istringstream in(text);
		for (std::string l; std::getline(in, l); ) { if (l == line) { return true; } }
		return false;
	}

	bool hasKey(const std::string& text, const std::string& key)	// any line that sets key
	{
		std::istringstream in(text);
		for (std::string l; std::getline(in, l); ) { if (l.rfind(key + "=", 0) == 0) { return true; } }
		return false;
	}

	std::string defOf(const std::string& key)
	{
		for (const cfg::Info& i : cfg::list()) { if (i.key == key) { return i.def; } }
		return "";
	}

	std::string opposite(const std::string& onOff) { return onOff == "on" ? "off" : "on"; }
}

int main(int argc, char** argv)
{
	const std::string mode = argc > 1 ? argv[1] : "drop";
	const std::string path = cfg::path();
	if (mode == "drop")
	{
		const std::string bdef = defOf("t.bool"), idef = defOf("t.int");
		const std::string away = opposite(bdef);
		// t.bool is in the file away from its default, t.int equal to its own: after a toggle back both must drop out.
		write(path, "# a comment kept by a save\n"
			"t.bool=" + away + "\n"
			"t.int=" + idef + "\n"
			"another.build.key=1\n"
			"t.gone=off\n"
			"window.aaaa1111.rect=0,0,1280,720\n"
			"window.aaaa1111.monitor=DP-1\n");
		cfg::retire("t.gone", "t.gone is gone (a retired key, kenshi-jj3.30)");
		cfg::load();
		check(kBool.get() == (away == "on"), "the file's away value is in effect after load");
		cfg::setUser("t.bool", away);	// the user toggles it back to the default
		cfg::setUser("t.bool", bdef);
		check(cfg::save(), "save after toggling back to the default");
		std::string f = read(path);
		check(!hasKey(f, "t.bool"), "a line equal to the static default is dropped");
		check(!hasKey(f, "t.int"), "another option's line equal to its own default is dropped too");
		check(hasLine(f, "# a comment kept by a save"), "a comment survives the save");
		check(hasLine(f, "another.build.key=1"), "an unknown key survives the save");
		check(hasLine(f, "window.aaaa1111.rect=0,0,1280,720"), "a window.<layout>.rect line survives the save");
		check(hasLine(f, "window.aaaa1111.monitor=DP-1"), "a window.<layout>.monitor line survives the save");
		cfg::load();
		check(kBool.get() == (bdef == "on"), "after the drop the default is in effect again");
		cfg::setUser("t.int", "7");	// away from its default, and its own line was just dropped: written anew
		check(cfg::save(), "save with t.int away from its default");
		f = read(path);
		check(hasLine(f, "t.int=7"), "an option set away from its default is saved");
		check(!hasKey(f, "t.bool"), "the dropped key is not written back on a later save");
		check(!hasKey(f, "t.gone") && hasKey(f, "another.build.key"), "a retired key's line is gone after a save, another build's key stays (kenshi-jj3.30)");
		// Grouped layout (kenshi-k16.2.244.1): the comment first, the option under its "# Section / Group" header, unknown keys at the end.
		const size_t pComment = f.find("# a comment kept by a save"), pHead = f.find("# Debug / Test\nt.int=7"), pUnknown = f.find("another.build.key=1"), pWin = f.find("window.aaaa1111.rect");
		check(pComment == 0, "the leading comment stays on top");
		check(pHead != std::string::npos && pComment < pHead, "the saved option is under its '# Debug / Test' header");
		check(pUnknown != std::string::npos && pHead < pUnknown && pUnknown < pWin, "unknown and window.<layout>.* keys keep their place at the end");
		cfg::setUser("t.bool", opposite(bdef));
		cfg::save();
		const std::string g2 = read(path);
		check(g2.find("# Debug / Test") != std::string::npos && g2.find("# Debug / Test", g2.find("# Debug / Test") + 1) == std::string::npos, "a second save does not duplicate the header");
		check(g2.find("t.bool=" + opposite(bdef)) < g2.find("t.int=7"), "the group's keys are in key order under one header");
		cfg::setUser("t.bool", bdef);
		cfg::save();
		check(cfg::problems().empty(), "every option of this test has a section and a group");
	}
	else if (mode == "survive")
	{
		check(defOf("t.int") == "6", "this build's t.int default (6) differs from the build that saved the file (5)");
		cfg::load();
		check(kInt.get() == 7, "a value saved away from the old default is in effect under the changed default");
		std::string f = read(path);
		check(hasLine(f, "t.int=7"), "the saved away value's line is in the file before a save");
		cfg::setUser("t.bool", opposite(defOf("t.bool")));
		check(cfg::save(), "save under the changed default");
		f = read(path);
		check(hasLine(f, "t.int=7"), "the saved away value's line survives a save under the changed default");
		check(kInt.get() == 7, "and stays in effect");
	}
	else
	{
		fprintf(stderr, "config_save: unknown mode '%s' (drop|survive)\n", mode.c_str());
		return 2;
	}
	if (failures) { fprintf(stderr, "config_save %s: %d failed\n", mode.c_str(), failures); return 1; }
	printf("config_save %s: PASS\n", mode.c_str());
	return 0;
}
