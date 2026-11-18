// knav_draw: a top-down picture of a world bake (knav_bake --world DIR) over a box, for looking at what the bake did.
//   knav_draw OUT.ppm (--navworld DIR | --mesh FILE [--components] | --native) --box X0 Z0 X1 Z1 [--scale U] [--y Y0 Y1] [--path X0 Z0 X1 Z1 ...]
// --mesh FILE draws one .knav (a navcache zone entry) instead of --navworld; --components colours its polygons by connected set
// (and prints the count); --probe X Z lists the drawn polygons over that point (height range, corners), for any source.
// --native draws the original's shipped navtiles over the box instead (knav::native, the grader): faces grey, doors green
// (open) / dark green (closed), interiors light purple, wall edges (no opposite face, no streaming link) white; no --path.
// --height colours every polygon by its mean height across the --y band instead (blue low, red high), for stairs.
// One pixel per U world units (default 0.5), z downward. Polygons by area: Land grey, Edge (the wall band) orange, Shallow
// light blue, Swim blue, doors green (open) / dark green (closed), pruned (flags 0: no seed reaches them) dark red; the
// higher polygon drawn last. --y keeps the polygons with a corner between those heights. Each --path is a route of the
// default TravelFilter between the surfaces nearest those points (red, start and end marked), printed with its length.

#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "kworld/zones.hpp"

#include <DetourNavMesh.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Rgb = std::array<unsigned char, 3>;

struct Canvas
{
	int w = 0;
	int h = 0;
	double x0 = 0.0;
	double z0 = 0.0;
	double scale = 0.5;
	std::vector<Rgb> px;

	void set(int x, int y, const Rgb& c)
	{
		if (x >= 0 && y >= 0 && x < w && y < h)
			px[static_cast<std::size_t>(y * w + x)] = c;
	}
	double toX(double wx) const { return (wx - x0) / scale; }
	double toY(double wz) const { return (wz - z0) / scale; }

	template <typename V>
	void triangle(const V* a, const V* b, const V* c, const Rgb& colour)
	{
		const double ax = toX(a[0]), ay = toY(a[2]), bx = toX(b[0]), by = toY(b[2]), cx = toX(c[0]), cy = toY(c[2]);
		const double area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
		if (area == 0.0)
			return;
		const int minX = std::max(0, static_cast<int>(std::floor(std::min({ ax, bx, cx }))));
		const int maxX = std::min(w - 1, static_cast<int>(std::ceil(std::max({ ax, bx, cx }))));
		const int minY = std::max(0, static_cast<int>(std::floor(std::min({ ay, by, cy }))));
		const int maxY = std::min(h - 1, static_cast<int>(std::ceil(std::max({ ay, by, cy }))));
		for (int y = minY; y <= maxY; ++y)
		{
			for (int x = minX; x <= maxX; ++x)
			{
				const double px_ = x + 0.5, py = y + 0.5;
				const double u = ((px_ - ax) * (cy - ay) - (cx - ax) * (py - ay)) / area;
				const double v = ((bx - ax) * (py - ay) - (px_ - ax) * (by - ay)) / area;
				if (u >= 0.0 && v >= 0.0 && u + v <= 1.0)
					set(x, y, colour);
			}
		}
	}

	void line(double wx0, double wz0, double wx1, double wz1, const Rgb& colour, int thick)
	{
		const double ax = toX(wx0), ay = toY(wz0), bx = toX(wx1), by = toY(wz1);
		const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(bx - ax), std::fabs(by - ay)))));
		for (int i = 0; i <= steps; ++i)
		{
			const double t = static_cast<double>(i) / steps;
			const int x = static_cast<int>(ax + (bx - ax) * t);
			const int y = static_cast<int>(ay + (by - ay) * t);
			for (int dy = -thick / 2; dy <= thick / 2; ++dy)
				for (int dx = -thick / 2; dx <= thick / 2; ++dx)
					set(x + dx, y + dy, colour);
		}
	}
};

Rgb areaColour(const dtPoly& poly)
{
	if (poly.flags == 0)
		return { 110, 30, 30 };
	switch (static_cast<knav::Area>(poly.getArea()))
	{
	case knav::Area::Land: return { 190, 190, 190 };
	case knav::Area::Shallow: return { 150, 200, 240 };
	case knav::Area::Swim: return { 60, 110, 220 };
	case knav::Area::DoorOpen: return { 60, 200, 60 };
	case knav::Area::DoorClosed: return { 20, 110, 20 };
	case knav::Area::Edge: return { 240, 150, 40 };
	}
	return { 255, 0, 255 };
}

// A polygon to draw: its corners and, per edge (corner i to i + 1), whether it is drawn as a wall.
struct Shape
{
	float top = 0.0f;
	Rgb colour{};
	std::vector<std::array<double, 3>> corners;
	std::vector<bool> walls;
};

Rgb nativeColour(const knav::native::Mesh& mesh, const knav::native::Face& face)
{
	if (face.data == 4)
		return { 60, 200, 60 };
	if (face.data == 5)
		return { 20, 110, 20 };
	return mesh.interior() ? Rgb{ 200, 180, 220 } : Rgb{ 190, 190, 190 };
}

int zoneOf(double v)
{
	return static_cast<int>(std::floor((v - kworld::kWorldMin) / kworld::kZoneSize));
}

int usage()
{
	std::cerr << "usage: knav_draw OUT.ppm (--navworld DIR | --mesh FILE [--components] | --native) --box X0 Z0 X1 Z1 [--scale U] [--y Y0 Y1 [--height]] [--path X0 Z0 X1 Z1 ...]\n";
	return 2;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
		return usage();
	try
	{
		const std::string out = argv[1];
		std::string navworld;
		std::string meshFile; // --mesh: one .knav (a navcache zone entry)
		bool components = false; // --components: colour by connected set
		std::vector<std::array<double, 2>> probes; // --probe X Z: list the drawn polygons over that point
		bool native = false;
		bool byHeight = false;
		double box[4] = {};
		bool haveBox = false;
		double scale = 0.5;
		float y0 = -1e9f, y1 = 1e9f;
		std::vector<std::array<double, 4>> paths;
		for (int i = 2; i < argc; ++i)
		{
			const std::string_view arg = argv[i];
			if (arg == "--navworld" && i + 1 < argc)
				navworld = argv[++i];
			else if (arg == "--mesh" && i + 1 < argc)
				meshFile = argv[++i];
			else if (arg == "--probe" && i + 2 < argc)
			{
				probes.push_back({ std::atof(argv[i + 1]), std::atof(argv[i + 2]) });
				i += 2;
			}
			else if (arg == "--components")
				components = true;
			else if (arg == "--native")
				native = true;
			else if (arg == "--height")
				byHeight = true;
			else if (arg == "--box" && i + 4 < argc)
			{
				for (double& v : box)
					v = std::atof(argv[++i]);
				haveBox = true;
			}
			else if (arg == "--scale" && i + 1 < argc)
				scale = std::atof(argv[++i]);
			else if (arg == "--y" && i + 2 < argc)
			{
				y0 = static_cast<float>(std::atof(argv[++i]));
				y1 = static_cast<float>(std::atof(argv[++i]));
			}
			else if (arg == "--path" && i + 4 < argc)
			{
				std::array<double, 4> p{};
				for (double& v : p)
					v = std::atof(argv[++i]);
				paths.push_back(p);
			}
			else
				return usage();
		}
		if (navworld.empty() + meshFile.empty() + !native != 2 || !haveBox || scale <= 0.0 || (native && !paths.empty()) || (byHeight && y0 < -1e8f))
			return usage();

		Canvas canvas;
		canvas.x0 = box[0];
		canvas.z0 = box[1];
		canvas.scale = scale;
		canvas.w = static_cast<int>(std::ceil((box[2] - box[0]) / scale));
		canvas.h = static_cast<int>(std::ceil((box[3] - box[1]) / scale));
		canvas.px.assign(static_cast<std::size_t>(canvas.w * canvas.h), Rgb{ 20, 20, 28 });

		const auto inBand = [&](float y) { return y >= y0 && y <= y1; };
		std::vector<Shape> shapes;
		std::optional<knav::NavMesh> mesh;
		if (native)
		{
			const std::string dir = knav::native::navtilesDir(KENSHI_DATA);
			for (int j = zoneOf(box[1]) - 1; j <= zoneOf(box[3]) + 1; ++j)
			{
				for (int i = zoneOf(box[0]) - 1; i <= zoneOf(box[2]) + 1; ++i)
				{
					for (const knav::native::Mesh& m : knav::native::loadTile(dir, i, j))
					{
						for (const knav::native::Face& face : m.faces)
						{
							Shape shape;
							shape.top = -1e9f;
							shape.colour = nativeColour(m, face);
							bool band = false, inside = false;
							for (std::uint32_t e = face.startEdge; e < face.startEdge + face.numEdges; ++e)
							{
								const std::array<double, 3>& v = m.vertices[m.edges[e].a];
								shape.corners.push_back(v);
								shape.walls.push_back(m.wall(e));
								shape.top = std::max(shape.top, static_cast<float>(v[1]));
								band = band || inBand(static_cast<float>(v[1]));
								inside = inside || (v[0] >= box[0] && v[0] <= box[2] && v[2] >= box[1] && v[2] <= box[3]);
							}
							if (band && inside && shape.corners.size() >= 3)
								shapes.push_back(std::move(shape));
						}
					}
				}
			}
		} else {
			if (meshFile.empty())
			{
				const double regionSize = kworld::kZoneSize * knav::kRegionZones;
				const int rx = static_cast<int>(std::floor(((box[0] + box[2]) / 2 - kworld::kWorldMin) / regionSize));
				const int rz = static_cast<int>(std::floor(((box[1] + box[3]) / 2 - kworld::kWorldMin) / regionSize));
				mesh.emplace(knav::NavMesh::loadRegion(navworld, rx, rz, 4));
			} else {
				mesh.emplace(knav::NavMesh::load(meshFile));
			}
			knav::PolyComponents sets;
			if (components)
			{
				sets = knav::PolyComponents(*mesh, 65535);
				std::cout << sets.count() << " connected sets\n";
			}
			const dtNavMesh& detour = mesh->detour();
			for (int t = 0; t < detour.getMaxTiles(); ++t)
			{
				const dtMeshTile* tile = detour.getTile(t);
				if (tile == nullptr || tile->header == nullptr || tile->header->bmax[0] < box[0] || tile->header->bmin[0] > box[2]
					|| tile->header->bmax[2] < box[1] || tile->header->bmin[2] > box[3])
					continue;
				for (int p = 0; p < tile->header->polyCount; ++p)
				{
					const dtPoly& poly = tile->polys[p];
					if (poly.getType() != DT_POLYTYPE_GROUND)
						continue;
					Shape shape;
					shape.top = -1e9f;
					shape.colour = areaColour(poly);
					if (components)
					{
						const std::uint32_t label = sets.at(tile->header->x, tile->header->y, p);
						const std::uint32_t h = label * 2654435761u;
						shape.colour = { static_cast<unsigned char>(60 + (h >> 8) % 196), static_cast<unsigned char>(60 + (h >> 16) % 196),
							static_cast<unsigned char>(60 + (h >> 24) % 196) };
					}
					bool band = false;
					for (int v = 0; v < poly.vertCount; ++v)
					{
						const float* c = &tile->verts[poly.verts[v] * 3];
						shape.corners.push_back({ c[0], c[1], c[2] });
						shape.walls.push_back(false);
						shape.top = std::max(shape.top, c[1]);
						band = band || inBand(c[1]);
					}
					if (band)
						shapes.push_back(std::move(shape));
				}
			}
		}
		std::sort(shapes.begin(), shapes.end(), [](const Shape& a, const Shape& b) { return a.top < b.top; });
		for (const std::array<double, 2>& at : probes)
		{
			std::cout << "probe " << at[0] << " " << at[1] << ":\n";
			for (const Shape& shape : shapes)
			{
				bool pos = false, neg = false;
				double lo = 1e9;
				for (std::size_t v = 0; v < shape.corners.size(); ++v)
				{
					const std::array<double, 3>& a = shape.corners[v];
					const std::array<double, 3>& b = shape.corners[(v + 1) % shape.corners.size()];
					lo = std::min(lo, a[1]);
					((b[0] - a[0]) * (at[1] - a[2]) - (b[2] - a[2]) * (at[0] - a[0]) < 0 ? neg : pos) = true;
				}
				if (!(pos && neg))
				{
					std::cout << "  y " << lo << " .. " << shape.top << ", " << shape.corners.size() << " corners:";
					for (const std::array<double, 3>& c : shape.corners)
						std::cout << " (" << c[0] << " " << c[1] << " " << c[2] << ")";
					std::cout << "\n";
				}
			}
		}
		if (byHeight)
		{
			for (Shape& shape : shapes)
			{
				double mean = 0.0;
				for (const std::array<double, 3>& c : shape.corners)
					mean += c[1];
				const double t = std::clamp((mean / static_cast<double>(shape.corners.size()) - y0) / (y1 - y0), 0.0, 1.0);
				shape.colour = { static_cast<unsigned char>(40 + 215 * t), static_cast<unsigned char>(60 + 120 * (1 - std::fabs(2 * t - 1))),
					static_cast<unsigned char>(255 - 215 * t) };
			}
		}
		for (const Shape& shape : shapes)
		{
			for (std::size_t v = 2; v < shape.corners.size(); ++v)
				canvas.triangle(shape.corners[0].data(), shape.corners[v - 1].data(), shape.corners[v].data(), shape.colour);
			const Rgb rim{ static_cast<unsigned char>(shape.colour[0] * 0.55), static_cast<unsigned char>(shape.colour[1] * 0.55),
				static_cast<unsigned char>(shape.colour[2] * 0.55) };
			for (std::size_t v = 0; v < shape.corners.size(); ++v)
			{
				const std::array<double, 3>& a = shape.corners[v];
				const std::array<double, 3>& b = shape.corners[(v + 1) % shape.corners.size()];
				if (shape.walls[v])
					canvas.line(a[0], a[2], b[0], b[2], { 255, 255, 255 }, 2);
				else
					canvas.line(a[0], a[2], b[0], b[2], rim, 1);
			}
		}

		for (const auto& p : paths)
		{
			const knav::Point from{ static_cast<float>(p[0]), (y0 + y1) / 2 > -1e8f ? (y0 + y1) / 2 : 0.0f, static_cast<float>(p[1]) };
			const knav::Point to{ static_cast<float>(p[2]), from[1], static_cast<float>(p[3]) };
			const knav::PathResult route = knav::NavQuery(*mesh, 65535).findPath(from, to, { 4.0f, y0 > -1e8f ? (y1 - y0) / 2 + 10.0f : 5000.0f, 4.0f });
			std::cout << "path " << p[0] << " " << p[1] << " -> " << p[2] << " " << p[3] << ": "
				<< (route.complete ? "complete" : route.found ? "partial" : "none") << ", length " << route.length << ", "
				<< route.points.size() << " corners\n";
			for (std::size_t i = 1; i < route.points.size(); ++i)
				canvas.line(route.points[i - 1][0], route.points[i - 1][2], route.points[i][0], route.points[i][2], { 230, 30, 30 }, 3);
			for (const knav::Point& end : { route.points.empty() ? from : route.points.front(), route.points.empty() ? to : route.points.back() })
				for (int dy = -3; dy <= 3; ++dy)
					for (int dx = -3; dx <= 3; ++dx)
						canvas.set(static_cast<int>(canvas.toX(end[0])) + dx, static_cast<int>(canvas.toY(end[2])) + dy, { 255, 255, 255 });
		}

		std::ofstream file(out, std::ios::binary);
		file << "P6\n" << canvas.w << " " << canvas.h << "\n255\n";
		file.write(reinterpret_cast<const char*>(canvas.px.data()), static_cast<std::streamsize>(canvas.px.size() * 3));
		std::cout << "wrote " << out << " (" << canvas.w << " x " << canvas.h << ", " << shapes.size() << " polygons)\n";
		return file ? 0 : 1;
	} catch (const std::exception& e) {
		std::cerr << "knav_draw: " << e.what() << "\n";
		return 1;
	}
}
