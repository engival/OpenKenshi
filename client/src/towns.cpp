#include "towns.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <map>

#include "kworld/towns.hpp"
#include "kworld/zones.hpp"

namespace
{

// kworld::Towns::placements() already resolves layer stacking; a TOWN placed again within 100 units in another layer
// keeps the later layer's position (kworld::zoneLayerRank: later ranks override earlier ones).
int layer_rank(const std::string& layer)
{
	try { return kworld::zoneLayerRank(layer); }
	catch (const std::exception&) { return -1; }
}

}

TownSet TownSet::load(const std::string& dataDir)
{
	TownSet set;
	const auto t0 = std::chrono::steady_clock::now();
	try
	{
		const kworld::Towns kt = kworld::Towns::load(dataDir);
		std::map<std::string, int> used;
		int generic = 0;
		for (const kworld::TownRecord& rec : kt.towns())
		{
			// A TOWN record can be placed many times (Fishing Village, Swamp Village, ...): keep one placement
			// per distinct position (within 10 m), the latest layer first.
			std::vector<const kworld::TownPlacement*> ps;
			for (size_t pi : rec.placementIndices) { ps.push_back(&kt.placements()[pi]); }
			std::stable_sort(ps.begin(), ps.end(), [](auto* a, auto* b) { return layer_rank(a->layer) > layer_rank(b->layer); });
			if (rec.name.starts_with("MARKER")) { generic += (int)ps.size(); continue; }	// spawn markers, not places
			std::vector<const kworld::TownPlacement*> kept;
			for (const kworld::TownPlacement* p : ps)
			{
				if (std::none_of(kept.begin(), kept.end(), [&](auto* k) { return std::hypot(k->x - p->x, k->z - p->z) < 100; })) { kept.push_back(p); }
			}
			for (const kworld::TownPlacement* p : kept)
			{
				used[p->layer]++;
				set.towns.push_back({ rec.name.empty() ? rec.stringID : rec.name, rec.stringID, p->x, p->z });
			}
		}
		char buf[256];
		snprintf(buf, sizeof buf, "%zu TOWN records, %zu placements, %zu records without placement, %d MARKER placements skipped; distinct positions by layer:",
			kt.towns().size(), kt.placements().size(), kt.townsWithoutPlacement(), generic);
		set.report = buf;
		for (const auto& [layer, n] : used) { set.report += " " + layer + "=" + std::to_string(n); }
	}
	catch (const std::exception& e) { set.report = std::string("town load failed: ") + e.what(); }
	set.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return set;
}

int TownSet::nearest(double x, double z) const
{
	int best = -1;
	double bestD = 0;
	for (int i = 0; i < (int)towns.size(); i++)
	{
		const double d = std::hypot(towns[i].x - x, towns[i].z - z);
		if (best < 0 || d < bestD) { best = i; bestD = d; }
	}
	return best;
}
