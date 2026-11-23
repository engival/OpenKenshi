// Inspect focus skill-in-use row (kenshi-5j3n): every CHAR_STATS row 6 kind, incl. 4 (ranged) and 6 (swimming) which the sim does not send yet.
#include <cstdio>
#include "host/skillrow.hpp"

static int fails = 0;
static void check(uint8_t kind, uint8_t skill, float extra, const char* label, const char* value)
{
	simlink::CharStats cs; cs.extraKind = kind; cs.extraSkill = skill; cs.extra = extra;
	ui::HudInfo info; host::skill_in_use_row(cs, info);
	const bool none = label == nullptr;
	const bool ok = none ? info.state.empty() : (info.state.size() == 1 && info.state[0].label == label && info.state[0].value == value);
	if (!ok) { ++fails; std::printf("FAIL kind %d skill %d: got %s\n", kind, skill, info.state.empty() ? "<none>" : (info.state[0].label + " " + info.state[0].value).c_str()); }
}
int main()
{
	check(0, 0xFF, 5, nullptr, nullptr);
	check(1, 16, 54.6f, "Stealth:", "55");
	check(2, 3, 12.2f, "Athletics:", "12");
	check(3, 17, 55.f, "Science:", "55");
	check(3, 18, 7.f, "Labouring:", "7");
	check(3, 0xFF, 9.f, "skill 255:", "9");
	check(5, 7, 40.f, "Katanas:", "40");
	check(4, 0xFF, 3.4f, "Accuracy Error:", "3 degrees");
	check(6, 0xFF, 5.f, "Swimming:", "5 mph");
	check(9, 0xFF, 1.f, "skill 9:", "1");
	std::printf("skillrow_test %s\n", fails ? "FAIL" : "ok");
	return fails != 0;
}
