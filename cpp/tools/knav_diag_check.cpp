// knav_diag_check: dev tool (kenshi-ffmi.24.1). Which diagonal of the 36 u terrain lattice the shipped navtile's world mesh follows. Of the
// shipped world-mesh vertices inside a cell whose height differs by over 1 u between the two splits, how many lie (within 0.3 u) on the
// (i,j)-(i+1,j+1) surface, on the other one, and on the smaller-height-step choice the generator used before.
//   knav_diag_check I,J ...
#include "knav/native.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
	try
	{
		const std::string data = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(data + "/newland/land/fullmap.tif");
		const std::string dir = knav::native::navtilesDir(KENSHI_DATA);
		const int cx = terrain.sampleCountX() - 1, cz = terrain.sampleCountZ() - 1;
		const auto sample = [&](int i, int j) { return terrain.sampleHeight(std::clamp(2 * i, 0, cx), std::clamp(2 * j, 0, cz)); };
		std::printf("zone\tdiffering\ta-d\tb-c\told_rule\n");
		for (int k = 1; k < argc; ++k)
		{
			int zx, zz;
			if (std::sscanf(argv[k], "%d,%d", &zx, &zz) != 2)
				continue;
			int n = 0, onAd = 0, onBc = 0, onOld = 0;
			for (const knav::native::Mesh& m : knav::native::loadTile(dir, zx, zz))
			{
				if (!m.info.empty())
					continue;
				for (const auto& v : m.vertices)
				{
					const double s = 2 * kworld::kHeightmapSpacing, fx = (v[0] - kworld::kWorldMin) / s, fz = (v[2] - kworld::kWorldMin) / s;
					const int i = (int)std::floor(fx), j = (int)std::floor(fz);
					const double u = fx - i, w = fz - j;
					const double a = sample(i, j), b = sample(i + 1, j), c = sample(i, j + 1), d = sample(i + 1, j + 1);
					const double ad = u >= w ? a + u * (b - a) + w * (d - b) : a + u * (d - c) + w * (c - a);
					const double bc = u + w <= 1 ? a + u * (b - a) + w * (c - a) : d + (1 - u) * (c - d) + (1 - w) * (b - d);
					if (std::fabs(ad - bc) <= 1)
						continue;
					++n;
					onAd += std::fabs(v[1] - ad) <= 0.3;
					onBc += std::fabs(v[1] - bc) <= 0.3;
					onOld += std::fabs(v[1] - (std::fabs(a - d) <= std::fabs(b - c) ? ad : bc)) <= 0.3;
				}
			}
			std::printf("%d.%d\t%d\t%d\t%d\t%d\n", zx, zz, n, onAd, onBc, onOld);
		}
	} catch (const std::exception& e) {
		std::cerr << e.what() << "\n";
		return 1;
	}
}
