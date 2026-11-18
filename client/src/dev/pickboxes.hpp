#pragma once
// dump pickboxes (kenshi-k16.2.260.1): dev-only, built with KCLIENT_DEV_TOOLS (src/dev/).
#include <cstdint>
#include <string>

class WorldBuildings;

namespace dev
{
// An instance's pick volume against the mesh the renderer draws, part by part. The pick tests each part's box in the instance's mesh frame
// (WorldBuildings::instanceRay: (world - origin) turned by the inverse rotation and divided by scale x stretch, against the mesh's uploaded vertex
// bounds grown by toMesh(offset)). The drawn side takes every triangle vertex of the same mesh through WorldBuildings::partToWorld (the transform the
// instance vertex shader uses). Per part: the two world boxes and `frame`, how far a drawn vertex lies outside the pick box in the mesh frame (0: the
// pick box holds every drawn vertex, so it is turned, offset and scaled like the mesh). The summary line carries the instance's building name, its
// floor and shown floor, the part count, the worst gap / frame and `inside` (every drawn vertex and the drawn box's centre inside the pick box).
// screen: the shot's world-to-screen (null: no corners), printing the pick box's 8 corners as `box screen x y ...` (bit0 = x, bit1 = y, bit2 = z).
std::string pickBoxReport(WorldBuildings& wb, uint32_t index, bool (*screen)(const double p[3], float& x, float& y));
}
