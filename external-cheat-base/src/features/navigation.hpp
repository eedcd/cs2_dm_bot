#pragma once

#include "utils/math/vector.hpp"

#include <string>
#include <vector>

namespace navigation
{
    // Loads the top-down radar mask for the given map id. Radar images contain
    // exactly the walkable floor, so they double as a navigation grid. Returns
    // false when the map has no bundled data.
    bool prepare(const std::string& mapId);

    // True when walkable map data for this map could be loaded. When false,
    // callers should fall back to another visibility heuristic.
    bool isAvailable(const std::string& mapId);

    // True when a straight world-space segment does not cross a non-walkable
    // (wall/void) area on the map mask. Returns true when no map data exists.
    bool hasLineOfSight(
        const std::string& mapId,
        float startX,
        float startY,
        float goalX,
        float goalY);

    // Height-aware variant. Layered maps (Nuke/Train/Vertigo/Baggage) use the
    // radar mask that matches each endpoint's Z band, falling back to either
    // mask when the endpoints are on different floors (stairs, ramps).
    bool hasLineOfSight3D(
        const std::string& mapId,
        float startX,
        float startY,
        float startZ,
        float goalX,
        float goalY,
        float goalZ);

    // Finds a world-space path from start to goal on the current map. The
    // returned list excludes the start point. Empty when no path exists.
    std::vector<vec2> findPath(
        const std::string& mapId,
        float startX,
        float startY,
        float goalX,
        float goalY);
}
