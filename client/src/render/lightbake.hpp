#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// CPU light bake for one building type (kenshi-jj3.1.1): a BVH over the type's triangles and a grid of probes holding the SKY TRANSFER, how much of a
// uniform white sky (radiance 1) reaches the probe from each direction, bounces included, as L1 spherical harmonics. It stores transfer, not light:
// at run time it multiplies the client's own ambient, so one bake is right at every time of day and weather (kenshi-lab docs/CLIENT_INTEGRATION_PLAN.md
// section 2). Ported from kenshi-lab stormview/src/bake.cpp (074dc80, our own code; the lamp of the lab bake is left out). Pure CPU, no sokol: thread
// safe, runs on a worker thread (WorldBuildings).
// The SUN TRANSFER (bakeSun, render.sun_bounce, kenshi-jj3.4.1) is the same grid for a white sun of strength 1 from one direction, in RGB (the tint is
// the albedo of what the light bounced off). It depends on where the sun is, so it is baked per direction and cross-faded at run time.
// Units: building-local mesh units (before the instance scale).
namespace lightbake
{

struct Tri
{
	float p[3][3];
	float n[3];		// unit normal on the side the vertex normals face
	float albedo;	// what the surface bounces (the luminance of its diffuse texture, raw like the client's textures): the sky transfer, grey
	float alb[3] = {};	// the diffuse texture's own RGB (raw, linear like the client's textures): the sun transfer's tint
};

// Probe (x, y, z) sits at min + spacing * (x, y, z), x fastest. sh: 4 floats per probe, the sky transfer in the order L00, L1(y), L1(z), L1(x).
// aux: 2 floats per probe: [0] enclosure (share of the upper hemisphere the building hides: 0 in the open, 1 under a ceiling),
// [1] usable (1; 0 inside a wall or under the ground: those hold no light and every lookup leaves them out).
struct Grid
{
	int dim[3] = {};
	float min[3] = {};
	float spacing = 1;
	std::vector<float> sh, aux;
	double seconds = 0;		// the bake's wall time (a cache load keeps the original's)
	bool fromCache = false;
	size_t triangles = 0;

	// The sun transfer (bakeSun; render.sun_bounce): 12 floats per probe, R, G, B, each L1 (L00, L1y, L1z, L1x), the irradiance a white sun of strength 1
	// from sunDir (building-local, unit) delivers after bouncing. Empty: no sun bake for this grid.
	std::vector<float> sun;
	float sunDir[3] = {};
	float sunGround[3] = {};	// the ground albedo the sun bake used (dark ground, dark bounce)
	double sunSeconds = 0;		// the sun part alone
	size_t sunVpls = 0;			// the virtual point lights the sun patches were merged into
	bool sunValid() const { return sun.size() == count() * 12; }

	size_t count() const { return (size_t)dim[0] * dim[1] * dim[2]; }
	// how much of the sky reaches the probe, normalised so a probe in the open (the upper half of the sphere white) reads 1 and one sealed in 0
	float level(size_t i) const { return sh[i * 4] * 0.282095f * 2.0f; }
	float enclosure(size_t i) const { return aux[i * 2]; }
	bool usable(size_t i) const { return aux[i * 2 + 1] > 0.5f; }
	size_t usableCount() const;
	void position(size_t i, float out[3]) const;
};

// Bakes over [mn, mx] (ground and margin already included) with the ground as the plane y = groundY. threads: the total, caller included.
Grid bake(std::vector<Tri> tris, const float mn[3], const float mx[3], float spacing, float groundY, float groundAlbedo, int threads);

// Lamp occlusion (render.lamp_occlusion, kenshi-jj3.9.1): the type's triangles in a BVH, kept for the visibility of one placement's lamps. Thread safe once built (const).
constexpr int kMaxLamps = 8;
class Occluder
{
public:
	explicit Occluder(std::vector<Tri> tris);
	~Occluder();
	Occluder(const Occluder&) = delete;
	Occluder& operator=(const Occluder&) = delete;
	// For each probe of g (x fastest like g.sh), kMaxLamps bytes: 255 when the probe sees lamp k (building-local positions, the grid's units), else 0 (0 for an unusable probe and a missing lamp).
	std::vector<uint8_t> lampVisibility(const Grid& g, const std::vector<std::array<float, 3>>& lamps) const;

private:
	struct Impl;
	Impl* impl_;
};

// The SUN part alone (kenshi-jj3.4.1): same grid, same tris, `groundAlbedo` in RGB; fills Grid::sun with the transfer for a white sun of strength 1 from
// `sunDir` (building-local, normalised here). A sun at or below the horizon leaves `sun` empty (the term is zero then; no bake). What the lab's bake.cpp
// does for the sun: rays from the sun on a 384 x 384 grid find the sunlit surfaces indoors, merged into virtual point lights (5-unit cells) that every
// probe connects to directly; the probes' own rays find the sunlit surfaces outdoors; two more bounce passes; then the 1-2-1 soften over indoor probes.
Grid bakeSun(std::vector<Tri> tris, const float mn[3], const float mx[3], float spacing, float groundY, const float groundAlbedo[3], const float sunDir[3], int threads);

// Cancels the bakes in flight and every later one (shutdown): their parallel loops skip the remaining chunks, so a join takes milliseconds. A bake that
// ran while cancelled() is true returns a partial grid the caller must drop.
void cancel();
bool cancelled();

// The cache (derived data, never a repo): the per-user cache root's kclient/lightbake.<name>.<key>.bin (app/userdirs.hpp: $XDG_CACHE_HOME or
// ~/.cache; Windows %LOCALAPPDATA%\OpenKenshi\cache), the key a content hash of the type's mesh files.
std::string cachePath(const std::string& name, uint64_t key);
bool load(const std::string& path, Grid& out);
bool save(const std::string& path, const Grid& g);
uint64_t hashBytes(uint64_t h, const void* data, size_t n);	// FNV-1a; start from kHashSeed
constexpr uint64_t kHashSeed = 1469598103934665603ull;

}
