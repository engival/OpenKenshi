// A sleeper's pose is a use node of the bed, never its origin (kinspect findings/bed_seat_pose.md 2; client kenshi-9bre, kenshi-k16.6.693):
// every bed furniture that has use nodes keeps all of them, the default site point is one, and a sleeper takes the node nearest it.

#include "ksim/ai/routines.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ksim::routines;

int main()
{
	try
	{
		const kdata::GameData data = kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA));
		const LayoutLibrary library = LayoutLibrary::load(KENSHI_DATA);
		std::size_t beds = 0, withNodes = 0;
		for (const Layout& layout : library.all())
		{
			std::vector<const LayoutPiece*> origins;
			for (const LayoutPiece& piece : layout.pieces)
				if (const kdata::Record* record = data.find(piece.target); record != nullptr && classifyPiece(data, *record) == SiteKind::Bed)
					origins.push_back(&piece);
			const std::vector<Piece> out = bedPieces(data, layout);
			test::require(out.size() == origins.size(), "a bed piece per bed in " + layout.sid);
			for (std::size_t i = 0; i < out.size(); ++i)
			{
				++beds;
				if (out[i].nodes.empty())
					continue;
				++withNodes;
				const bool onNode = std::any_of(out[i].nodes.begin(), out[i].nodes.end(), [&](const BedNode& n) { return std::hypot(n.x - out[i].x, n.z - out[i].z) < 1e-6; });
				test::require(onNode, "the bed's point is one of its use nodes, never its origin: " + layout.sid + " bed " + std::to_string(i));
			}
		}
		std::printf("ok  %zu beds, %zu with use nodes, none on the origin\n", beds, withNodes);

		// The Hub bar's Bed (two nodes, 11 u either side x scale): two sleepers take different nodes, each its own facing.
		const Layout* hubBar = library.find("3384-TwoStorey.mod-Bar");
		test::require(hubBar != nullptr, "the TwoStorey Bar layout");
		const std::vector<Piece> hub = bedPieces(data, *hubBar);
		test::require(!hub.empty() && hub[0].nodes.size() == 2, "the Hub bar's first bed has two use nodes");
		const LayoutPiece* origin = nullptr;
		for (const LayoutPiece& piece : hubBar->pieces)
			if (const kdata::Record* record = data.find(piece.target); record != nullptr && classifyPiece(data, *record) == SiteKind::Bed)
			{
				origin = &piece;
				break;
			}
		const BedNode& a = hub[0].nodes[0];
		const BedNode& b = hub[0].nodes[1];
		std::printf("ok  Hub bar bed 0: origin (%.2f, %.2f) node A (%.2f, %.2f) off (%.2f, %.2f) face (%.3f, %.3f); node B (%.2f, %.2f) off (%.2f, %.2f)\n",
			origin->x, origin->z, a.x, a.z, a.x - origin->x, a.z - origin->z, a.faceX, a.faceZ, b.x, b.z, b.x - origin->x, b.z - origin->z);
		test::require(std::hypot(a.x - b.x, a.z - b.z) > 15.0, "the two nodes are apart");
		SiteEntry bed;
		bed.x = hub[0].x;
		bed.z = hub[0].z;
		bed.nodes = hub[0].nodes;
		const BedNode first = bedNodeNear(bed, a.x + 1.0, a.z);
		const BedNode second = bedNodeNear(bed, b.x, b.z - 1.0);
		test::require(first.x == a.x && second.x == b.x && first.x != second.x, "two sleepers take different nodes");
		// yaw node - 90: the facing is the piece's rotation of the node's -X (the sleeper's +Z maps to node -X).
		const kdata::Record* record = data.find(hub[0].record);
		const std::vector<UseNode> poses = useNodePoses(data, *record);
		const LayoutPiece& p = *origin;
		const std::array<double, 3> face = rotate({p.qw, p.qx, p.qy, p.qz}, rotate(poses[0].q, {-1.0, 0.0, 0.0}));
		const std::array<double, 3> face1 = rotate({p.qw, p.qx, p.qy, p.qz}, rotate(poses[1].q, {-1.0, 0.0, 0.0}));
		const double len = std::hypot(face[0], face[2]), len1 = std::hypot(face1[0], face1[2]);
		const bool match = (std::hypot(a.faceX - face[0] / len, a.faceZ - face[2] / len) < 1e-6 && std::hypot(b.faceX - face1[0] / len1, b.faceZ - face1[2] / len1) < 1e-6);
		test::require(match, "each node's facing is its own -X");
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
	std::puts("ksim_bed_pose: all passed");
	return 0;
}
