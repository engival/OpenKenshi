// Retired by kenshi-k16.2.166: WorldBuildings::setProps matched a container to a layout piece by position (740bb8b7, kenshi-8vb.1).
// The SIMLINK kind 3 tail (building doorKey + piece index) names the piece now.
		if (placed < 0 && !p.item && !p.placedOnly)	// a container the layout already draws (the Hub bar's counter, a cabinet): the sim sends it with yaw 0, a second copy at 90 degrees to the layout piece (kenshi-8vb.1)
		{
			for (int dx = -1; dx <= 1 && placed < 0; dx++)
			{
				for (int dz = -1; dz <= 1 && placed < 0; dz++)
				{
					const auto g = furnGrid_.find(furnKey(p.pos[0] + 4 * dx, p.pos[2] + 4 * dz));
					if (g == furnGrid_.end()) { continue; }
					for (const uint32_t i : g->second)
					{
						const BuildingInstance& f = data_.instances[i];
						if (std::hypot(f.x - p.pos[0], f.z - p.pos[2]) < 1.0 && std::fabs(f.y - p.pos[1]) < 2.0) { placed = (int)i; break; }
					}
				}
			}
		}
