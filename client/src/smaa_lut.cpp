// SMAA lookup-texture generators (AreaTex, SearchTex), computed at runtime.
//
// Ported from iryoku/smaa Scripts/AreaTex.py and Scripts/SearchTex.py
// (https://github.com/iryoku/smaa), distributed under the MIT licence:
//
// Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
// Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
// Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
// Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
// Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software. As clarification, there is no
// requirement that the copyright notice and permission be included in binary
// distributions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// All math is IEEE double like the Python. Results depend on the compiler not
// fusing multiply-adds: build with -ffp-contract=off on targets that have FMA
// enabled by default (plain x86-64 is fine).

#include "smaa_lut.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>

namespace smaa_lut
{
namespace
{
	struct vec2
	{
		double x, y;
	};

	vec2 operator+(vec2 a, vec2 b) { return { a.x + b.x, a.y + b.y }; }
	vec2 operator-(vec2 a, vec2 b) { return { a.x - b.x, a.y - b.y }; }
	vec2 operator*(vec2 a, double s) { return { s * a.x, s * a.y }; }
	vec2 operator/(vec2 a, double s) { return { a.x / s, a.y / s }; }

	constexpr double kSubsampleOrtho[7] = { 0.0, -0.25, 0.25, -0.125, 0.125, -0.375, 0.375 };
	constexpr vec2 kSubsampleDiag[5] = { { 0.0, 0.0 }, { 0.25, -0.25 }, { -0.25, 0.25 }, { 0.125, -0.125 }, { -0.125, 0.125 } };

	constexpr int kSizeOrtho = 16, kSizeDiag = 20;
	constexpr int kSamplesDiag = 30;
	constexpr double kSmoothMaxDistance = 32.0;

	// Where each pattern's subtexture sits in the 4D texture, in subtexture units.
	constexpr int kEdgesOrtho[16][2] = { { 0, 0 }, { 3, 0 }, { 0, 3 }, { 3, 3 }, { 1, 0 }, { 4, 0 }, { 1, 3 }, { 4, 3 },
	                                     { 0, 1 }, { 3, 1 }, { 0, 4 }, { 3, 4 }, { 1, 1 }, { 4, 1 }, { 1, 4 }, { 4, 4 } };
	constexpr int kEdgesDiag[16][2] = { { 0, 0 }, { 1, 0 }, { 0, 2 }, { 1, 2 }, { 2, 0 }, { 3, 0 }, { 2, 2 }, { 3, 2 },
	                                    { 0, 1 }, { 1, 1 }, { 0, 3 }, { 1, 3 }, { 2, 1 }, { 3, 1 }, { 2, 3 }, { 3, 3 } };

	// Python: int(255.0 * a), truncating toward zero.
	uint8_t toByte(double a) { return static_cast<uint8_t>(static_cast<int>(255.0 * a)); }

	double lerp(double a, double b, double p) { return a + (b - a) * p; }
	vec2 lerp(vec2 a, vec2 b, double p) { return a + (b - a) * p; }

	double saturate(double a) { return std::min(std::max(a, 0.0), 1.0); }

	// Smoothing for small u-patterns.
	void smootharea(double d, vec2& a1, vec2& a2)
	{
		vec2 b1 = vec2{ std::sqrt(a1.x * 2.0), std::sqrt(a1.y * 2.0) } * 0.5;
		vec2 b2 = vec2{ std::sqrt(a2.x * 2.0), std::sqrt(a2.y * 2.0) } * 0.5;
		double p = saturate(d / kSmoothMaxDistance);
		a1 = lerp(b1, a1, p);
		a2 = lerp(b2, a2, p);
	}

	//------------------------------------------------------------------
	// Horizontal/vertical areas

	// Area under the line p1->p2 for the pixel x..x+1.
	vec2 areaOrthoSeg(vec2 p1, vec2 p2, int px)
	{
		vec2 d = p2 - p1;
		double x1 = px;
		double x2 = px + 1.0;
		double y1 = p1.y + d.y * (x1 - p1.x) / d.x;
		double y2 = p1.y + d.y * (x2 - p1.x) / d.x;

		bool inside = (x1 >= p1.x && x1 < p2.x) || (x2 > p1.x && x2 <= p2.x);
		if (!inside) { return { 0.0, 0.0 }; }

		bool istrapezoid = std::copysign(1.0, y1) == std::copysign(1.0, y2) || std::fabs(y1) < 1e-4 || std::fabs(y2) < 1e-4;
		if (istrapezoid)
		{
			double a = (y1 + y2) / 2.0;
			if (a < 0.0) { return { std::fabs(a), 0.0 }; }
			return { 0.0, std::fabs(a) };
		}

		// Two triangles. Python's modf(x)[0] is the fractional part, same as C's.
		double xc = -p1.y * d.x / d.y + p1.x;
		double ip;
		double frac = std::modf(xc, &ip);
		double a1 = xc > p1.x ? y1 * frac / 2.0 : 0.0;
		double a2 = xc < p2.x ? y2 * (1.0 - frac) / 2.0 : 0.0;
		double a = std::fabs(a1) > std::fabs(a2) ? a1 : -a2;
		if (a < 0.0) { return { std::fabs(a1), std::fabs(a2) }; }
		return { std::fabs(a2), std::fabs(a1) };
	}

	vec2 areaOrtho(int pattern, int left, int right, double offset)
	{
		int d = left + right + 1;
		double dh = d / 2.0;
		double o1 = 0.5 + offset;
		double o2 = 0.5 + offset - 1.0;
		auto seg = [&](vec2 p1, vec2 p2) { return areaOrthoSeg(p1, p2, left); };

		switch (pattern)
		{
		case 1: return left <= right ? seg({ 0.0, o2 }, { dh, 0.0 }) : vec2{ 0.0, 0.0 };
		case 2: return left >= right ? seg({ dh, 0.0 }, { double(d), o2 }) : vec2{ 0.0, 0.0 };
		case 3:
		{
			vec2 a1 = seg({ 0.0, o2 }, { dh, 0.0 });
			vec2 a2 = seg({ dh, 0.0 }, { double(d), o2 });
			smootharea(d, a1, a2);
			return a1 + a2;
		}
		case 4: return left <= right ? seg({ 0.0, o1 }, { dh, 0.0 }) : vec2{ 0.0, 0.0 };
		case 6:
		{
			// Blend the full offset Z with partially offset L patterns to avoid discontinuities.
			if (std::fabs(offset) > 0.0)
			{
				vec2 a1 = seg({ 0.0, o1 }, { double(d), o2 });
				vec2 a2 = seg({ 0.0, o1 }, { dh, 0.0 });
				a2 = a2 + seg({ dh, 0.0 }, { double(d), o2 });
				return (a1 + a2) / 2.0;
			}
			return seg({ 0.0, o1 }, { double(d), o2 });
		}
		case 7: return seg({ 0.0, o1 }, { double(d), o2 });
		case 8: return left >= right ? seg({ dh, 0.0 }, { double(d), o1 }) : vec2{ 0.0, 0.0 };
		case 9:
		{
			if (std::fabs(offset) > 0.0)
			{
				vec2 a1 = seg({ 0.0, o2 }, { double(d), o1 });
				vec2 a2 = seg({ 0.0, o2 }, { dh, 0.0 });
				a2 = a2 + seg({ dh, 0.0 }, { double(d), o1 });
				return (a1 + a2) / 2.0;
			}
			return seg({ 0.0, o2 }, { double(d), o1 });
		}
		case 11: return seg({ 0.0, o2 }, { double(d), o1 });
		case 12:
		{
			vec2 a1 = seg({ 0.0, o1 }, { dh, 0.0 });
			vec2 a2 = seg({ dh, 0.0 }, { double(d), o1 });
			smootharea(d, a1, a2);
			return a1 + a2;
		}
		case 13: return seg({ 0.0, o2 }, { double(d), o1 });
		case 14: return seg({ 0.0, o1 }, { double(d), o2 });
		default: return { 0.0, 0.0 }; // 0, 5, 10, 15: no filtering
		}
	}

	//------------------------------------------------------------------
	// Diagonal areas

	// Fraction of the pixel at p covered by the half-plane left of p1->p2,
	// by brute-force supersampling (kSamplesDiag^2 samples, as upstream).
	double areaDiagSample(vec2 p1, vec2 p2, vec2 p)
	{
		bool degenerate = p1.x == p2.x && p1.y == p2.y;
		vec2 m = (p1 + p2) / 2.0;
		double a = p2.y - p1.y;
		double b = p1.x - p2.x;

		int count = 0;
		for (int sx = 0; sx < kSamplesDiag; ++sx)
		{
			for (int sy = 0; sy < kSamplesDiag; ++sy)
			{
				vec2 o = vec2{ double(sx), double(sy) } / double(kSamplesDiag - 1);
				vec2 q = p + o;
				count += degenerate || a * (q.x - m.x) + b * (q.y - m.y) > 0;
			}
		}
		return double(count) / (kSamplesDiag * kSamplesDiag);
	}

	vec2 areaDiagLine(int pattern, vec2 p1, vec2 p2, int left, vec2 offset)
	{
		if (kEdgesDiag[pattern][0] > 0) { p1 = p1 + offset; }
		if (kEdgesDiag[pattern][1] > 0) { p2 = p2 + offset; }
		double l = left;
		double a1 = areaDiagSample(p1, p2, vec2{ 1.0, 0.0 } + vec2{ l, l });
		double a2 = areaDiagSample(p1, p2, vec2{ 1.0, 1.0 } + vec2{ l, l });
		return { 1.0 - a1, a2 };
	}

	// Each pattern is a (start1, end1) line, optionally blended with a second
	// possibility for the unknown line endings ("black magic" in upstream).
	struct DiagSpec
	{
		vec2 s1, e1;
		bool blend;
		vec2 s2, e2;
	};

	// End points are given without the (d, d) extent, which areaDiag adds.
	constexpr DiagSpec kDiagSpec[16] = {
		{ { 1, 1 }, { 1, 1 }, true,  { 1, 0 }, { 1, 0 } }, //  0
		{ { 1, 0 }, { 0, 0 }, true,  { 1, 0 }, { 1, 0 } }, //  1
		{ { 0, 0 }, { 1, 0 }, true,  { 1, 0 }, { 1, 0 } }, //  2
		{ { 1, 0 }, { 1, 0 }, false, { 0, 0 }, { 0, 0 } }, //  3
		{ { 1, 1 }, { 0, 0 }, true,  { 1, 1 }, { 1, 0 } }, //  4
		{ { 1, 1 }, { 0, 0 }, true,  { 1, 0 }, { 1, 0 } }, //  5
		{ { 1, 1 }, { 1, 0 }, false, { 0, 0 }, { 0, 0 } }, //  6
		{ { 1, 1 }, { 1, 0 }, true,  { 1, 0 }, { 1, 0 } }, //  7
		{ { 0, 0 }, { 1, 1 }, true,  { 1, 0 }, { 1, 1 } }, //  8
		{ { 1, 0 }, { 1, 1 }, false, { 0, 0 }, { 0, 0 } }, //  9
		{ { 0, 0 }, { 1, 1 }, true,  { 1, 0 }, { 1, 0 } }, // 10
		{ { 1, 0 }, { 1, 1 }, true,  { 1, 0 }, { 1, 0 } }, // 11
		{ { 1, 1 }, { 1, 1 }, false, { 0, 0 }, { 0, 0 } }, // 12
		{ { 1, 1 }, { 1, 1 }, true,  { 1, 0 }, { 1, 1 } }, // 13
		{ { 1, 1 }, { 1, 1 }, true,  { 1, 1 }, { 1, 0 } }, // 14
		{ { 1, 1 }, { 1, 1 }, true,  { 1, 0 }, { 1, 0 } }, // 15
	};

	vec2 areaDiag(int pattern, int left, int right, vec2 offset)
	{
		double d = left + right + 1;
		vec2 ext = { d, d };
		const DiagSpec& s = kDiagSpec[pattern];
		vec2 a1 = areaDiagLine(pattern, s.s1, s.e1 + ext, left, offset);
		if (!s.blend) { return a1; }
		return (a1 + areaDiagLine(pattern, s.s2, s.e2 + ext, left, offset)) / 2.0;
	}

	//------------------------------------------------------------------
	// Assembly

	void put(std::vector<uint8_t>& tex, int x, int y, vec2 v)
	{
		size_t i = (size_t(y) * kAreaW + x) * 2;
		tex[i] = toByte(v.x);
		tex[i + 1] = toByte(v.y);
	}

	// One independent unit of work: all pixels of one (offset row, pattern) pair.
	// Writes touch disjoint texels, so threads need no locking.
	void runJob(std::vector<uint8_t>& tex, int job)
	{
		int pattern = job % 16;
		int row = job / 16;
		if (row < 7)
		{
			// Orthogonal distances are compressed quadratically: texel (l, r) holds
			// the area for distances (l^2, r^2).
			for (int l = 0; l < kSizeOrtho; ++l)
			{
				for (int r = 0; r < kSizeOrtho; ++r)
				{
					int x = l + kSizeOrtho * kEdgesOrtho[pattern][0];
					int y = 5 * kSizeOrtho * row + r + kSizeOrtho * kEdgesOrtho[pattern][1];
					put(tex, x, y, areaOrtho(pattern, l * l, r * r, kSubsampleOrtho[row]));
				}
			}
		} else {
			int dy = row - 7;
			for (int l = 0; l < kSizeDiag; ++l)
			{
				for (int r = 0; r < kSizeDiag; ++r)
				{
					int x = 5 * kSizeOrtho + l + kSizeDiag * kEdgesDiag[pattern][0];
					int y = 4 * kSizeDiag * dy + r + kSizeDiag * kEdgesDiag[pattern][1];
					put(tex, x, y, areaDiag(pattern, l, r, kSubsampleDiag[dy]));
				}
			}
		}
	}

	// Bilinear fetch for an edge combination at the search sample position.
	double bilinear(const std::array<int, 4>& e)
	{
		double a = lerp(e[0], e[1], 1.0 - 0.25);
		double b = lerp(e[2], e[3], 1.0 - 0.25);
		return lerp(a, b, 1.0 - 0.125);
	}

	// Reverse lookup of bilinear(): the edges that produce texcoord t, if any.
	bool edgeFor(double t, std::array<int, 4>& out)
	{
		for (int i = 0; i < 16; ++i)
		{
			std::array<int, 4> e = { (i >> 3) & 1, (i >> 2) & 1, (i >> 1) & 1, i & 1 };
			if (bilinear(e) == t) { out = e; return true; }
		}
		return false;
	}

	// Distance to add in the last step of a search to the left.
	int deltaLeft(const std::array<int, 4>& left, const std::array<int, 4>& top)
	{
		int d = 0;
		if (top[3] == 1) { d += 1; }
		if (d == 1 && top[2] == 1 && left[1] != 1 && left[3] != 1) { d += 1; }
		return d;
	}

	// Distance to add in the last step of a search to the right.
	int deltaRight(const std::array<int, 4>& left, const std::array<int, 4>& top)
	{
		int d = 0;
		if (top[3] == 1 && left[1] != 1 && left[3] != 1) { d += 1; }
		if (d == 1 && top[2] == 1 && left[0] != 1 && left[2] != 1) { d += 1; }
		return d;
	}
}

std::vector<uint8_t> area()
{
	std::vector<uint8_t> tex(size_t(kAreaW) * kAreaH * 2, 0);
	constexpr int kJobs = (7 + 5) * 16;
	std::atomic<int> next{ 0 };
	auto worker = [&]
	{
		for (int j = next++; j < kJobs; j = next++) { runJob(tex, j); }
	};

	unsigned n = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
	std::vector<std::thread> pool;
	for (unsigned i = 1; i < n; ++i) { pool.emplace_back(worker); }
	worker();
	for (std::thread& t : pool) { t.join(); }
	return tex;
}

std::vector<uint8_t> search()
{
	// Upstream builds a 66x33 image (left deltas | right deltas), then crops rows
	// 17..32 of the first 64 columns and flips vertically. Output row j is
	// therefore source row 32 - j, and only columns 0..63 are kept.
	std::vector<uint8_t> tex(size_t(kSearchW) * kSearchH, 0);
	for (int j = 0; j < kSearchH; ++j)
	{
		int y = 32 - j;
		for (int x = 0; x < 33; ++x)
		{
			std::array<int, 4> l, t;
			if (!edgeFor(0.03125 * x, l) || !edgeFor(0.03125 * y, t)) { continue; }
			tex[size_t(j) * kSearchW + x] = 127 * deltaLeft(l, t);
			if (33 + x < kSearchW) { tex[size_t(j) * kSearchW + 33 + x] = 127 * deltaRight(l, t); }
		}
	}
	return tex;
}
}
