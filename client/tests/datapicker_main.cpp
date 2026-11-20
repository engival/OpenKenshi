// Test driver for datapicker_check.sh: datasetting::resolve exactly as main.cpp calls it, built without the dev build's KENSHI_DATA default.
#include <cstdio>
#include <string>
#include "platform/datasetting.hpp"
int main(int argc, char** argv)
{
	std::string out, why;
	if (!datasetting::resolve(argc > 1 ? argv[1] : "", out, why)) { fprintf(stderr, "kclient: %s", why.c_str()); return 1; }
	printf("data=%s\n", out.c_str());
	return 0;
}
