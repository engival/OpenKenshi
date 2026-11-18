// kwindowgeom_test: unit check of the window geometry memory's pure half (windowgeom.hpp): the layout fingerprint and the rect parse.
// Run: kwindowgeom_test (exit 0 and "kwindowgeom_test ok").
#include <cstdio>

#include "platform/windowgeom.hpp"

using namespace windowgeom;

static int fails = 0;
static void check(bool ok, const char* what) { if (!ok) { fprintf(stderr, "kwindowgeom_test FAIL: %s\n", what); fails++; } }

int main()
{
	const std::vector<Monitor> a = { { "DP-1", 0, 0, 2560, 1440 }, { "HDMI-1", 2560, 0, 1920, 1080 }, { "eDP-1", -1920, 0, 1920, 1200 } };
	const std::vector<Monitor> b = { a[2], a[0], a[1] }, c = { a[1], a[2], a[0] };
	const std::string fa = fingerprint(a);
	check(fa.size() == 16, "fingerprint is 16 characters");
	check(fa.find_first_not_of("0123456789abcdef") == std::string::npos, "fingerprint is lowercase hex");
	check(fa == fingerprint(b) && fa == fingerprint(c), "fingerprint is order-independent");
	std::vector<Monitor> moved = a, resized = a, renamed = a, fewer = { a[0], a[1] };
	moved[1].x = 2561;
	resized[0].h = 1600;
	renamed[2].name = "eDP-2";
	check(fa != fingerprint(moved), "fingerprint changes with a position");
	check(fa != fingerprint(resized), "fingerprint changes with a size");
	check(fa != fingerprint(renamed), "fingerprint changes with a name");
	check(fa != fingerprint(fewer), "fingerprint changes with the monitor count");
	check(fingerprint({}) .size() == 16, "empty layout still has a fingerprint");

	Rect r;
	check(parseRect("100,200,1280,720", r) && r == Rect{ 100, 200, 1280, 720 }, "parse a plain rect");
	check(parseRect("-1920,-30,800,600", r) && r == Rect{ -1920, -30, 800, 600 }, "parse a negative position");
	check(!parseRect("", r), "reject empty");
	check(!parseRect("1,2,3", r), "reject three fields");
	check(!parseRect("1,2,3,4,5", r), "reject five fields");
	check(!parseRect("1,2,3,4x", r), "reject trailing junk");
	check(!parseRect("1,2,0,4", r), "reject a zero width");
	check(!parseRect("1,2,3,-4", r), "reject a negative height");
	check(formatRect({ -5, 6, 700, 800 }) == "-5,6,700,800", "format");

	check(monitorAtCentre(a, { 100, 100, 800, 600 }) && monitorAtCentre(a, { 100, 100, 800, 600 })->name == "DP-1", "centre on DP-1");
	check(monitorAtCentre(a, { -1900, 10, 400, 300 }) && monitorAtCentre(a, { -1900, 10, 400, 300 })->name == "eDP-1", "centre on a monitor left of the origin");
	check(!monitorAtCentre(fewer, { -1900, 10, 400, 300 }), "centre off every connected monitor");
	check(!monitorAtCentre(a, { 9000, 9000, 100, 100 }), "centre far away");
	printf("kwindowgeom_test %s\n", fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}
