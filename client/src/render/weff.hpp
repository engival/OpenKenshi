#pragma once
#include <string>

// A weather-style particle system pinned at the pivot + (DX, DZ), its y the terrain's (`debug weff SCRIPT DX DZ`, a dev A/B aid, k16.2.11.3).
// App holds the list (App::weff) and the dev `debug weff` op appends to it (kenshi-k16.2.153.21), so the record lives here.
struct Weff
{
	std::string script;
	double off[2] = {};
};
