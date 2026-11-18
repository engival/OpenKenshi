// SMAA lookup-texture generators (AreaTex, SearchTex), computed at runtime.
// Ported from iryoku/smaa Scripts/AreaTex.py and Scripts/SearchTex.py (MIT).
// Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia,
// Fernando Navarro, Diego Gutierrez. Full licence text is in smaa_lut.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace smaa_lut
{
	constexpr int kAreaW = 160, kAreaH = 560, kSearchW = 64, kSearchH = 16;

	// RG8, rows top to bottom as in upstream's AreaTex.h (kAreaW * kAreaH * 2 bytes)
	std::vector<uint8_t> area();
	// R8, rows top to bottom as in upstream's SearchTex.h (kSearchW * kSearchH bytes)
	std::vector<uint8_t> search();
}
