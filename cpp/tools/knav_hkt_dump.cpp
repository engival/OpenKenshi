// knav_hkt_dump: dev tool (kenshi-k16.7.138). Reads shipped navtiles and prints each mesh's scalar header members
// (erosionRadius, ...) or its boundary edges.
//   knav_hkt_dump scalars I0 J0 I1 J1     one line per mesh: tile, info, faces, then name=value
//   knav_hkt_dump faces I J [INFO]       every face's vertex ring in nav_faces.tsv columns (face_id n x y z ...), world coords, for tools/nav_stair_coverage.py
//   knav_hkt_dump edges I J [INFO]        boundary (wall) edges of the meshes of tile I.J (the one whose info holds INFO), world coords
#include "knav/native.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	try
	{
		if (argc < 6 && !(argc >= 4 && (std::string(argv[1]) == "edges" || std::string(argv[1]) == "faces")))
			return 2;
		const std::string dir = knav::native::navtilesDir(KENSHI_DATA);
		const std::string mode = argv[1];
		if (mode == "scalars")
		{
			for (int j = std::atoi(argv[3]); j <= std::atoi(argv[5]); ++j)
				for (int i = std::atoi(argv[2]); i <= std::atoi(argv[4]); ++i)
					for (const knav::native::Mesh& m : knav::native::loadTile(dir, i, j))
					{
						std::printf("%d.%d\t%s\t%zu", i, j, m.info.c_str(), m.faces.size());
						for (const auto& [k, v] : m.scalars)
							std::printf("\t%s=%g", k.c_str(), v);
						std::printf("\n");
					}
		} else if (mode == "faces") {
			std::printf("# face_id vertex_count then x y z per vertex (knav_hkt_dump faces)\n");
			const std::string want = argc > 4 ? argv[4] : "";
			for (const knav::native::Mesh& m : knav::native::loadTile(dir, std::atoi(argv[2]), std::atoi(argv[3])))
			{
				if (!want.empty() && m.info.find(want) == std::string::npos)
					continue;
				for (std::size_t f = 0; f < m.faces.size(); ++f)
				{
					std::printf("%zu\t%d", f, int(m.faces[f].numEdges));
					for (std::uint32_t e = m.faces[f].startEdge; e < m.faces[f].startEdge + m.faces[f].numEdges; ++e)
					{
						const auto& a = m.vertices[m.edges[e].a];
						std::printf("\t%.4f\t%.4f\t%.4f", a[0], a[1], a[2]);
					}
					std::printf("\n");
				}
			}
		} else {
			const std::string want = argc > 4 ? argv[4] : "";
			for (const knav::native::Mesh& m : knav::native::loadTile(dir, std::atoi(argv[2]), std::atoi(argv[3])))
			{
				if (!want.empty() && m.info.find(want) == std::string::npos)
					continue;
				for (std::size_t e = 0; e < m.edges.size(); ++e)
				{
					if (!m.wall(e))
						continue;
					const auto& a = m.vertices[m.edges[e].a];
					const auto& b = m.vertices[m.edges[e].b];
					std::printf("%s\t%.3f %.3f %.3f\t%.3f %.3f %.3f\n", m.info.empty() ? "world" : m.info.c_str(), a[0], a[1], a[2], b[0], b[1], b[2]);
				}
			}
		}
	} catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
	return 0;
}
