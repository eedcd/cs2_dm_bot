#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "navigation.hpp"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr int kGridSize = 512;
    constexpr int kGridCells = kGridSize * kGridSize;

    struct MapDefinition
    {
        const char* id;
        float originX;
        float originY;
        float scale;
        // Below this world Z the "lower" radar mask applies. NaN means the map
        // has a single level.
        float lowerZ;
    };

    constexpr float kNoLowerLevel =
        std::numeric_limits<float>::quiet_NaN();

    // Source-style overview transform:
    //   pixelX = (worldX - originX) / scale
    //   pixelY = (originY - worldY) / scale
    constexpr std::array<MapDefinition, 16> kMaps{ {
        { "ar_baggage", -1316.0f, 1288.0f, 2.539062f, -5.0f },
        { "ar_shoots", -1368.0f, 1952.0f, 2.6875f, kNoLowerLevel },
        { "ar_shoots_night", -1368.0f, 1952.0f, 2.6875f, kNoLowerLevel },
        { "cs_italy", -2647.0f, 2592.0f, 4.6f, kNoLowerLevel },
        { "cs_office", -1838.0f, 1858.0f, 4.1f, kNoLowerLevel },
        { "de_ancient", -2953.0f, 2164.0f, 5.0f, kNoLowerLevel },
        { "de_ancient_night", -2953.0f, 2164.0f, 5.0f, kNoLowerLevel },
        { "de_anubis", -2796.0f, 3328.0f, 5.22f, kNoLowerLevel },
        { "de_cache", -2000.0f, 3250.0f, 5.5f, kNoLowerLevel },
        { "de_dust2", -2476.0f, 3239.0f, 4.4f, kNoLowerLevel },
        { "de_inferno", -2087.0f, 3870.0f, 4.9f, kNoLowerLevel },
        { "de_mirage", -3230.0f, 1713.0f, 5.0f, kNoLowerLevel },
        { "de_nuke", -3453.0f, 2887.0f, 7.0f, -495.0f },
        { "de_overpass", -4831.0f, 1781.0f, 5.2f, kNoLowerLevel },
        { "de_train", -2308.0f, 2078.0f, 4.082077f, -50.0f },
        { "de_vertigo", -3168.0f, 1762.0f, 4.0f, 11700.0f }
    } };

    const MapDefinition* findMap(const std::string& mapId)
    {
        for (const MapDefinition& map : kMaps) {
            if (mapId == map.id) {
                return &map;
            }
        }
        return nullptr;
    }

    void ensureComInitialized()
    {
        static thread_local bool initialized = false;
        if (!initialized) {
            initialized = true;
            // RPC_E_CHANGED_MODE is harmless: COM is already usable.
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        }
    }

    std::filesystem::path executableDirectory()
    {
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(
            nullptr,
            path.data(),
            static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return {};
        }
        return std::filesystem::path(
            std::wstring_view(path.data(), length)).parent_path();
    }

    bool decodePng(
        const std::filesystem::path& path,
        std::vector<std::uint8_t>& pixels,
        UINT& width,
        UINT& height)
    {
        ensureComInitialized();

        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)))) {
            return false;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(
                path.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                &decoder))) {
            return false;
        }

        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) {
            return false;
        }

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom))) {
            return false;
        }

        if (FAILED(frame->GetSize(&width, &height)) ||
            width == 0 || height == 0) {
            return false;
        }

        const UINT stride = width * 4;
        pixels.resize(static_cast<std::size_t>(stride) * height);
        return SUCCEEDED(converter->CopyPixels(
            nullptr,
            stride,
            static_cast<UINT>(pixels.size()),
            pixels.data()));
    }

    struct LoadedMap
    {
        std::string id;
        bool attempted = false;
        bool ready = false;
        float originX = 0.0f;
        float originY = 0.0f;
        float scale = 1.0f;
        float cellWorldSize = 1.0f;
        UINT sourceWidth = 0;
        UINT sourceHeight = 0;
        std::vector<std::uint8_t> walkable;
        int walkableCount = 0;
        bool hasLower = false;
        float lowerZ = kNoLowerLevel;
        std::vector<std::uint8_t> walkableLower;
        // Full-resolution obstacle masks (1 = blocked). Radar art only paints
        // walkable floor, so every transparent pixel is solid geometry. Using
        // the source resolution keeps thin walls that a 512-cell downsample
        // would drop entirely.
        std::vector<std::uint8_t> solid;
        std::vector<std::uint8_t> solidLower;
        UINT lowerWidth = 0;
        UINT lowerHeight = 0;
    };

    LoadedMap g_map;

    bool buildMask(
        const std::vector<std::uint8_t>& pixels,
        const UINT width,
        const UINT height,
        std::vector<std::uint8_t>& walkable)
    {
        walkable.assign(kGridCells, 0);

        const float cellPixelsX =
            static_cast<float>(width) / kGridSize;
        const float cellPixelsY =
            static_cast<float>(height) / kGridSize;

        for (int cy = 0; cy < kGridSize; ++cy) {
            const UINT sourceY = std::min(
                height - 1,
                static_cast<UINT>(
                    (cy + 0.5f) * cellPixelsY));
            for (int cx = 0; cx < kGridSize; ++cx) {
                const UINT sourceX = std::min(
                    width - 1,
                    static_cast<UINT>(
                        (cx + 0.5f) * cellPixelsX));
                const std::size_t offset =
                    (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
                // Radar images only paint the walkable floor; everything else
                // is transparent.
                if (pixels[offset + 3] > 128) {
                    walkable[cy * kGridSize + cx] = 1;
                }
            }
        }

        // Erode by one cell so paths keep a small distance from walls.
        std::vector<std::uint8_t> eroded = walkable;
        for (int cy = 1; cy < kGridSize - 1; ++cy) {
            for (int cx = 1; cx < kGridSize - 1; ++cx) {
                const int index = cy * kGridSize + cx;
                if (!walkable[index]) {
                    continue;
                }
                bool solid = true;
                for (int dy = -1; dy <= 1 && solid; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!walkable[
                                (cy + dy) * kGridSize + (cx + dx)]) {
                            solid = false;
                            break;
                        }
                    }
                }
                if (!solid) {
                    eroded[index] = 0;
                }
            }
        }
        walkable.swap(eroded);

        int count = 0;
        for (const std::uint8_t cell : walkable) {
            count += cell ? 1 : 0;
        }
        return count > 0;
    }

    void buildSolid(
        const std::vector<std::uint8_t>& pixels,
        const UINT width,
        const UINT height,
        std::vector<std::uint8_t>& solid)
    {
        solid.assign(static_cast<std::size_t>(width) * height, 0);
        for (UINT y = 0; y < height; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * width;
            for (UINT x = 0; x < width; ++x) {
                // Nearly transparent pixels are walls/void. The threshold is
                // low on purpose: lower-floor radars are drawn semi-faded, so a
                // high cutoff would turn their walkable floor into walls.
                solid[row + x] =
                    pixels[(row + x) * 4 + 3] < 16 ? 1 : 0;
            }
        }
    }

    bool isWalkableMask(
        const std::vector<std::uint8_t>& mask,
        const int x,
        const int y)
    {
        if (x < 0 || y < 0 || x >= kGridSize || y >= kGridSize) {
            return false;
        }
        return mask[y * kGridSize + x] != 0;
    }

    bool isWalkable(const LoadedMap& map, const int x, const int y)
    {
        return isWalkableMask(map.walkable, x, y);
    }

    void worldToCell(
        const LoadedMap& map,
        const float worldX,
        const float worldY,
        int& cellX,
        int& cellY)
    {
        const float pixelX = (worldX - map.originX) / map.scale;
        const float pixelY = (map.originY - worldY) / map.scale;
        const float cellsX =
            pixelX / map.sourceWidth * kGridSize;
        const float cellsY =
            pixelY / map.sourceHeight * kGridSize;
        cellX = static_cast<int>(std::lround(cellsX));
        cellY = static_cast<int>(std::lround(cellsY));
    }

    void cellToWorld(
        const LoadedMap& map,
        const int cellX,
        const int cellY,
        float& worldX,
        float& worldY)
    {
        const float pixelX =
            (cellX + 0.5f) / kGridSize * map.sourceWidth;
        const float pixelY =
            (cellY + 0.5f) / kGridSize * map.sourceHeight;
        worldX = map.originX + pixelX * map.scale;
        worldY = map.originY - pixelY * map.scale;
    }

    int nearestWalkable(
        const LoadedMap& map,
        const int cellX,
        const int cellY)
    {
        if (isWalkable(map, cellX, cellY)) {
            return cellY * kGridSize + cellX;
        }
        for (int radius = 1; radius <= 24; ++radius) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (std::abs(dx) != radius && std::abs(dy) != radius) {
                        continue;
                    }
                    if (isWalkable(map, cellX + dx, cellY + dy)) {
                        return (cellY + dy) * kGridSize + (cellX + dx);
                    }
                }
            }
        }
        return -1;
    }

    bool hasLineOfSight(
        const LoadedMap& map,
        const int fromX,
        const int fromY,
        const int toX,
        const int toY)
    {
        int x = fromX;
        int y = fromY;
        const int dx = std::abs(toX - fromX);
        const int dy = std::abs(toY - fromY);
        const int stepX = (toX > fromX) ? 1 : -1;
        const int stepY = (toY > fromY) ? 1 : -1;
        int error = dx - dy;

        while (true) {
            if (!isWalkable(map, x, y)) {
                return false;
            }
            if (x == toX && y == toY) {
                return true;
            }
            const int doubled = error * 2;
            if (doubled > -dy) {
                error -= dy;
                x += stepX;
            }
            if (doubled < dx) {
                error += dx;
                y += stepY;
            }
        }
    }

    struct SearchNode
    {
        float score;
        int index;
        bool operator>(const SearchNode& other) const
        {
            return score > other.score;
        }
    };

    std::vector<int> runAStar(
        const LoadedMap& map,
        const int startIndex,
        const int goalIndex)
    {
        static thread_local std::vector<float> gScore;
        static thread_local std::vector<std::int32_t> cameFrom;
        static thread_local std::vector<std::uint8_t> closed;
        gScore.assign(kGridCells, std::numeric_limits<float>::infinity());
        cameFrom.assign(kGridCells, -1);
        closed.assign(kGridCells, 0);

        const int goalX = goalIndex % kGridSize;
        const int goalY = goalIndex / kGridSize;

        const auto heuristic = [goalX, goalY](const int index) {
            const int x = index % kGridSize;
            const int y = index / kGridSize;
            const int dx = std::abs(x - goalX);
            const int dy = std::abs(y - goalY);
            const int diagonal = std::min(dx, dy);
            return static_cast<float>(diagonal) * 1.41421356f +
                static_cast<float>(std::max(dx, dy) - diagonal);
        };

        std::priority_queue<
            SearchNode,
            std::vector<SearchNode>,
            std::greater<SearchNode>> open;
        gScore[startIndex] = 0.0f;
        open.push(SearchNode{ heuristic(startIndex), startIndex });

        constexpr std::array<std::array<int, 2>, 8> kDirections{ {
            { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
            { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }
        } };

        int visited = 0;
        while (!open.empty()) {
            const SearchNode current = open.top();
            open.pop();
            const int index = current.index;
            if (closed[index]) {
                continue;
            }
            closed[index] = 1;
            if (index == goalIndex) {
                break;
            }
            if (++visited > 500000) {
                return {};
            }

            const int x = index % kGridSize;
            const int y = index / kGridSize;
            for (std::size_t direction = 0;
                 direction < kDirections.size();
                 ++direction) {
                const int nx = x + kDirections[direction][0];
                const int ny = y + kDirections[direction][1];
                if (!isWalkable(map, nx, ny)) {
                    continue;
                }
                const bool diagonal = direction >= 4;
                if (diagonal &&
                    (!isWalkable(map, x + kDirections[direction][0], y) ||
                     !isWalkable(map, x, y + kDirections[direction][1]))) {
                    continue;
                }
                const int neighbor = ny * kGridSize + nx;
                if (closed[neighbor]) {
                    continue;
                }
                const float stepCost = diagonal ? 1.41421356f : 1.0f;
                const float tentative = gScore[index] + stepCost;
                if (tentative < gScore[neighbor]) {
                    gScore[neighbor] = tentative;
                    cameFrom[neighbor] = index;
                    open.push(SearchNode{
                        tentative + heuristic(neighbor),
                        neighbor });
                }
            }
        }

        if (cameFrom[goalIndex] < 0 && goalIndex != startIndex) {
            return {};
        }

        std::vector<int> cells;
        int cursor = goalIndex;
        while (cursor >= 0) {
            cells.push_back(cursor);
            if (cursor == startIndex) {
                break;
            }
            cursor = cameFrom[cursor];
        }
        std::reverse(cells.begin(), cells.end());
        return cells;
    }

    bool maskLineOfSight(
        const LoadedMap& map,
        const std::vector<std::uint8_t>& mask,
        const float startX,
        const float startY,
        const float goalX,
        const float goalY)
    {
        int startCellX = 0;
        int startCellY = 0;
        int goalCellX = 0;
        int goalCellY = 0;
        worldToCell(map, startX, startY, startCellX, startCellY);
        worldToCell(map, goalX, goalY, goalCellX, goalCellY);

        int x = startCellX;
        int y = startCellY;
        const int dx = std::abs(goalCellX - startCellX);
        const int dy = std::abs(goalCellY - startCellY);
        const int stepX = (goalCellX > startCellX) ? 1 : -1;
        const int stepY = (goalCellY > startCellY) ? 1 : -1;
        int error = dx - dy;

        const int totalSteps = std::max(dx, dy);
        int step = 0;
        while (true) {
            // Ignore cells right next to either endpoint: the player capsule
            // may be overlapping a wall edge and still see past it.
            const bool nearEndpoint =
                step <= 2 || step >= totalSteps - 2;
            if (!nearEndpoint && !isWalkableMask(mask, x, y)) {
                return false;
            }
            if (x == goalCellX && y == goalCellY) {
                return true;
            }
            const int doubled = error * 2;
            if (doubled > -dy) {
                error -= dy;
                x += stepX;
            }
            if (doubled < dx) {
                error += dx;
                y += stepY;
            }
            ++step;
        }
    }

    // True when the full-resolution segment does not touch any solid pixel.
    // The first/last few world units are ignored so a player whose capsule
    // overlaps a wall edge can still see past it.
    bool solidLineClear(
        const LoadedMap& map,
        const std::vector<std::uint8_t>& solid,
        const UINT width,
        const UINT height,
        const float startX,
        const float startY,
        const float goalX,
        const float goalY)
    {
        if (solid.empty() || width == 0 || height == 0) {
            return false;
        }

        const float startPixelX = (startX - map.originX) / map.scale;
        const float startPixelY = (map.originY - startY) / map.scale;
        const float goalPixelX = (goalX - map.originX) / map.scale;
        const float goalPixelY = (map.originY - goalY) / map.scale;
        const float deltaX = goalPixelX - startPixelX;
        const float deltaY = goalPixelY - startPixelY;
        const float length = std::sqrt(deltaX * deltaX + deltaY * deltaY);
        if (length < 0.5f) {
            return true;
        }

        const float marginPixels = std::max(1.0f, 12.0f / map.scale);
        if (length <= marginPixels * 2.0f) {
            return true;
        }

        // Walk the segment at sub-pixel steps so no one-pixel wall is skipped.
        const int steps = static_cast<int>(
            std::ceil(length / 0.35f));
        for (int index = 1; index < steps; ++index) {
            const float t = static_cast<float>(index) /
                static_cast<float>(steps);
            const float along = t * length;
            if (along <= marginPixels ||
                along >= length - marginPixels) {
                continue;
            }
            const float pixelX = startPixelX + deltaX * t;
            const float pixelY = startPixelY + deltaY * t;
            const int x = static_cast<int>(std::floor(pixelX));
            const int y = static_cast<int>(std::floor(pixelY));
            if (x < 0 || y < 0 ||
                x >= static_cast<int>(width) ||
                y >= static_cast<int>(height)) {
                return false;
            }
            if (solid[static_cast<std::size_t>(y) * width + x]) {
                return false;
            }
        }
        return true;
    }
}

namespace navigation
{
    bool prepare(const std::string& mapId)
    {
        if (mapId.empty()) {
            return false;
        }
        if (g_map.attempted && g_map.id == mapId) {
            return g_map.ready;
        }

        g_map = LoadedMap{};
        g_map.id = mapId;
        g_map.attempted = true;

        const MapDefinition* definition = findMap(mapId);
        if (!definition) {
            return false;
        }

        const std::filesystem::path path =
            executableDirectory() / L"maps" / (mapId + ".png");
        std::vector<std::uint8_t> pixels;
        UINT width = 0;
        UINT height = 0;
        if (!decodePng(path, pixels, width, height)) {
            return false;
        }

        g_map.originX = definition->originX;
        g_map.originY = definition->originY;
        g_map.scale = definition->scale;
        g_map.sourceWidth = width;
        g_map.sourceHeight = height;
        g_map.cellWorldSize = definition->scale *
            (static_cast<float>(width) / kGridSize);

        g_map.ready = buildMask(
            pixels,
            width,
            height,
            g_map.walkable);
        buildSolid(pixels, width, height, g_map.solid);
        g_map.walkableCount = 0;
        for (const std::uint8_t cell : g_map.walkable) {
            g_map.walkableCount += cell ? 1 : 0;
        }

        if (g_map.ready && std::isfinite(definition->lowerZ)) {
            const std::filesystem::path lowerPath =
                executableDirectory() / L"maps" /
                (mapId + "_lower.png");
            std::vector<std::uint8_t> lowerPixels;
            UINT lowerWidth = 0;
            UINT lowerHeight = 0;
            if (decodePng(
                    lowerPath,
                    lowerPixels,
                    lowerWidth,
                    lowerHeight) &&
                buildMask(
                    lowerPixels,
                    lowerWidth,
                    lowerHeight,
                    g_map.walkableLower)) {
                g_map.hasLower = true;
                g_map.lowerZ = definition->lowerZ;
                g_map.lowerWidth = lowerWidth;
                g_map.lowerHeight = lowerHeight;
                buildSolid(
                    lowerPixels,
                    lowerWidth,
                    lowerHeight,
                    g_map.solidLower);
            }
        }
        return g_map.ready;
    }

    bool isAvailable(const std::string& mapId)
    {
        return prepare(mapId) && g_map.ready;
    }

    bool hasLineOfSight(
        const std::string& mapId,
        const float startX,
        const float startY,
        const float goalX,
        const float goalY)
    {
        if (!prepare(mapId) || !g_map.ready) {
            return true;
        }
        return maskLineOfSight(
            g_map,
            g_map.walkable,
            startX,
            startY,
            goalX,
            goalY);
    }

    bool hasLineOfSight3D(
        const std::string& mapId,
        const float startX,
        const float startY,
        const float startZ,
        const float goalX,
        const float goalY,
        const float goalZ)
    {
        if (!prepare(mapId) || !g_map.ready) {
            return true;
        }

        // Preferred path: exact, full-resolution obstacle test. This rejects
        // one-pixel walls that the 512-cell navigation grid can lose.
        if (!g_map.solid.empty()) {
            const bool lowerSolid = g_map.hasLower &&
                !g_map.solidLower.empty();
            if (!g_map.hasLower || !lowerSolid) {
                return solidLineClear(
                    g_map,
                    g_map.solid,
                    g_map.sourceWidth,
                    g_map.sourceHeight,
                    startX,
                    startY,
                    goalX,
                    goalY);
            }

            const bool startLower = startZ < g_map.lowerZ;
            const bool goalLower = goalZ < g_map.lowerZ;
            if (startLower == goalLower) {
                return startLower
                    ? solidLineClear(
                        g_map,
                        g_map.solidLower,
                        g_map.lowerWidth,
                        g_map.lowerHeight,
                        startX,
                        startY,
                        goalX,
                        goalY)
                    : solidLineClear(
                        g_map,
                        g_map.solid,
                        g_map.sourceWidth,
                        g_map.sourceHeight,
                        startX,
                        startY,
                        goalX,
                        goalY);
            }

            // Different floors: stairs and ramps may still connect them, so a
            // clear segment on either level's mask counts.
            return solidLineClear(
                    g_map,
                    g_map.solid,
                    g_map.sourceWidth,
                    g_map.sourceHeight,
                    startX,
                    startY,
                    goalX,
                    goalY) ||
                solidLineClear(
                    g_map,
                    g_map.solidLower,
                    g_map.lowerWidth,
                    g_map.lowerHeight,
                    startX,
                    startY,
                    goalX,
                    goalY);
        }

        // Fallback for images that could not be decoded at source resolution.
        if (!g_map.hasLower) {
            return maskLineOfSight(
                g_map,
                g_map.walkable,
                startX,
                startY,
                goalX,
                goalY);
        }

        const bool startLower = startZ < g_map.lowerZ;
        const bool goalLower = goalZ < g_map.lowerZ;
        if (startLower == goalLower) {
            return maskLineOfSight(
                g_map,
                startLower ? g_map.walkableLower : g_map.walkable,
                startX,
                startY,
                goalX,
                goalY);
        }

        return maskLineOfSight(
                g_map,
                g_map.walkable,
                startX,
                startY,
                goalX,
                goalY) ||
            maskLineOfSight(
                g_map,
                g_map.walkableLower,
                startX,
                startY,
                goalX,
                goalY);
    }

    std::vector<vec2> findPath(
        const std::string& mapId,
        const float startX,
        const float startY,
        const float goalX,
        const float goalY)
    {
        if (!prepare(mapId) || !g_map.ready) {
            return {};
        }

        int startCellX = 0;
        int startCellY = 0;
        int goalCellX = 0;
        int goalCellY = 0;
        worldToCell(g_map, startX, startY, startCellX, startCellY);
        worldToCell(g_map, goalX, goalY, goalCellX, goalCellY);

        const int start = nearestWalkable(
            g_map, startCellX, startCellY);
        const int goal = nearestWalkable(
            g_map, goalCellX, goalCellY);
        if (start < 0 || goal < 0) {
            return {};
        }

        const std::vector<int> cells = runAStar(g_map, start, goal);
        if (cells.size() < 2) {
            return {};
        }

        // Smooth the grid path using line-of-sight jumps, then convert to
        // world coordinates.
        std::vector<int> smoothed;
        smoothed.push_back(cells.front());
        std::size_t anchor = 0;
        while (anchor + 1 < cells.size()) {
            std::size_t farthest = anchor + 1;
            for (std::size_t candidate = cells.size() - 1;
                 candidate > anchor + 1;
                 --candidate) {
                if (hasLineOfSight(
                        g_map,
                        cells[anchor] % kGridSize,
                        cells[anchor] / kGridSize,
                        cells[candidate] % kGridSize,
                        cells[candidate] / kGridSize)) {
                    farthest = candidate;
                    break;
                }
            }
            smoothed.push_back(cells[farthest]);
            anchor = farthest;
        }

        std::vector<vec2> path;
        path.reserve(smoothed.size());
        for (std::size_t index = 1; index < smoothed.size(); ++index) {
            float worldX = 0.0f;
            float worldY = 0.0f;
            cellToWorld(
                g_map,
                smoothed[index] % kGridSize,
                smoothed[index] / kGridSize,
                worldX,
                worldY);
            path.push_back(vec2{ worldX, worldY });
        }
        return path;
    }
}
