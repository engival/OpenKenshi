// knav_native: the original's shipped navtiles converted to Detour (knav/native_tiles.hpp), written as world bake region files
// (the knav_bake --world layout), so knav_draw --navworld, knav_compare and the sim read them like our bake.
//   knav_native OUTDIR --region RX0 RZ0 [RX1 RZ1] [--navtiles DIR]
// Prints per region the faces, polygons, the fullest tile and what did not convert cleanly (NativeTileStats).

#include "knav/native_tiles.hpp"
#include "kworld/zones.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int usage()
{
	std::cerr << "usage: knav_native OUTDIR --region RX0 RZ0 [RX1 RZ1] [--navtiles DIR]\n";
	return 2;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2)
			return usage();
		const std::string out = argv[1];
		std::string navtiles = knav::native::navtilesDir(KENSHI_DATA);
		int rx0 = 0, rz0 = 0, rx1 = -1, rz1 = -1;
		for (int i = 2; i < argc; ++i)
		{
			const std::string arg = argv[i];
			if (arg == "--navtiles" && i + 1 < argc)
				navtiles = argv[++i];
			else if (arg == "--region" && i + 2 < argc)
			{
				rx0 = rx1 = std::stoi(argv[++i]);
				rz0 = rz1 = std::stoi(argv[++i]);
				if (i + 2 < argc && std::string(argv[i + 1]).substr(0, 2) != "--")
				{
					rx1 = std::stoi(argv[++i]);
					rz1 = std::stoi(argv[++i]);
				}
			} else {
				return usage();
			}
		}
		if (rx1 < rx0 || rz1 < rz0)
			return usage();
		std::filesystem::create_directories(out);

		for (int rz = rz0; rz <= rz1; ++rz)
			for (int rx = rx0; rx <= rx1; ++rx)
			{
				const kworld::ZoneCell min{ rx * knav::kRegionZones, rz * knav::kRegionZones };
				const kworld::ZoneCell max{ min.x + knav::kRegionZones - 1, min.z + knav::kRegionZones - 1 };
				// One zone further each way: an interior stored next door may reach in.
				std::vector<knav::native::Mesh> meshes;
				for (int j = min.z - 1; j <= max.z + 1; ++j)
					for (int i = min.x - 1; i <= max.x + 1; ++i)
						for (knav::native::Mesh& mesh : knav::native::loadTile(navtiles, i, j))
							meshes.push_back(std::move(mesh));
				const knav::NavConfig config = knav::configForWorldZones(min, max);
				std::vector<knav::TileCoord> coords;
				for (int z = config.window.z0; z <= config.window.z1; ++z)
					for (int x = config.window.x0; x <= config.window.x1; ++x)
						coords.push_back({ x, z });
				knav::NativeTileStats stats;
				std::vector<knav::BakedTile> tiles = knav::nativeTiles(config, meshes, coords, {}, &stats);
				const std::size_t count = tiles.size();
				knav::NavMesh::fromTiles(config, std::move(tiles)).save(out + "/" + knav::regionFileName(rx, rz));
				std::cout << "region " << rx << " " << rz << ": " << count << " tiles, " << stats.faces << " faces, " << stats.polys
						  << " polys (fullest tile " << stats.maxPolys << "), over-full " << stats.overfull << ", slivers " << stats.slivers << ", fanned " << stats.fanned
						  << ", open links " << stats.openLinks << ", open shared " << stats.openShared << "\n";
			}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_native: " << e.what() << "\n";
		return 1;
	}
}
