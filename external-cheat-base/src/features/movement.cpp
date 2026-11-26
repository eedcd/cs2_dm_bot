#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "movement.hpp"
#include "aimbot.hpp"
#include "esp.hpp"
#include "navigation.hpp"
#include "core/memory/memory.hpp"
#include "client_dll.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    using SteadyClock = std::chrono::steady_clock;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kReachRadius = 150.0f;
    constexpr float kAxisDeadband = 60.0f;
    constexpr float kStuckDistance = 25.0f;
    constexpr auto kStuckCheckInterval = std::chrono::milliseconds(250);
    constexpr auto kDetourDuration = std::chrono::milliseconds(700);
    constexpr auto kQuickStopDuration = std::chrono::milliseconds(120);

    // Patrol offsets forming a closed loop around the anchor position. The
    // route is intentionally generic: without navmesh access the bot combines
    // this circuit with collision detection, detours and jumping.
    constexpr std::array<std::array<float, 2>, 8> kRouteOffsets{ {
        { 0.0f, 900.0f },
        { 900.0f, 900.0f },
        { 900.0f, 0.0f },
        { 900.0f, -900.0f },
        { 0.0f, -900.0f },
        { -900.0f, -900.0f },
        { -900.0f, 0.0f },
        { -900.0f, 900.0f }
    } };

    enum KeyId : std::size_t
    {
        KeyW = 0,
        KeyA = 1,
        KeyS = 2,
        KeyD = 3,
        KeyCount = 4
    };

    constexpr std::array<WORD, KeyCount> kScanCodes{ {
        0x11, // W
        0x1E, // A
        0x1F, // S
        0x20  // D
    } };

    bool pressed[KeyCount]{};
    bool haveAnchor = false;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    std::size_t waypointIndex = 0;

    // Collision recovery state.
    SteadyClock::time_point lastStuckCheck{};
    float lastSampleX = 0.0f;
    float lastSampleY = 0.0f;
    int stuckCount = 0;
    float detourOffsetDegrees = 0.0f;
    SteadyClock::time_point detourUntil{};

    // Combat quick-stop state.
    bool quickStopping = false;
    SteadyClock::time_point quickStopUntil{};
    bool combatStopHeld = false;

    // View stability: pause movement for a tick when the aimbot performs a
    // large flick, which would otherwise rotate the movement direction.
    bool haveViewYaw = false;
    float lastViewYaw = 0.0f;

    // Map path following state.
    std::vector<vec2> routePath;
    std::size_t routePathIndex = 0;
    SteadyClock::time_point lastPathTime{};
    float lastGoalX = 0.0f;
    float lastGoalY = 0.0f;
    std::string pathMapId;
    std::string cachedMapId;
    SteadyClock::time_point lastMapQuery{};

    float normalizeAngle(float angle)
    {
        while (angle > 180.0f) angle -= 360.0f;
        while (angle < -180.0f) angle += 360.0f;
        return angle;
    }

    void sendKey(const std::size_t index, const bool down)
    {
        if (pressed[index] == down) {
            return;
        }
        pressed[index] = down;

        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = 0;
        input.ki.wScan = kScanCodes[index];
        input.ki.dwFlags = KEYEVENTF_SCANCODE |
            (down ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &input, sizeof(INPUT));
    }

    bool anyPressed()
    {
        return pressed[KeyW] || pressed[KeyA] ||
            pressed[KeyS] || pressed[KeyD];
    }

    void beginQuickStop()
    {
        if (quickStopping) {
            return;
        }

        bool previous[KeyCount]{};
        for (std::size_t index = 0; index < KeyCount; ++index) {
            previous[index] = pressed[index];
            sendKey(index, false);
        }

        // Counter-strafe: tap the opposite direction briefly so the player
        // decelerates before the shot instead of sliding.
        for (std::size_t index = 0; index < KeyCount; ++index) {
            if (previous[index]) {
                sendKey(index ^ 2U, true); // W<->S, A<->D
            }
        }

        quickStopping = true;
        quickStopUntil = SteadyClock::now() + kQuickStopDuration;
    }

    void endQuickStop()
    {
        for (std::size_t index = 0; index < KeyCount; ++index) {
            sendKey(index, false);
        }
        quickStopping = false;
    }

    void resetState()
    {
        haveAnchor = false;
        waypointIndex = 0;
        lastStuckCheck = {};
        stuckCount = 0;
        detourUntil = {};
        detourOffsetDegrees = 0.0f;
        haveViewYaw = false;
        combatStopHeld = false;
        routePath.clear();
        routePathIndex = 0;
        lastPathTime = {};
        pathMapId.clear();
        cachedMapId.clear();
        lastMapQuery = {};
        if (quickStopping) {
            endQuickStop();
        }
    }

    const std::string& currentMapIdCached(const SteadyClock::time_point now)
    {
        if (cachedMapId.empty() ||
            now - lastMapQuery >= std::chrono::seconds(1)) {
            lastMapQuery = now;
            cachedMapId = esp::currentMapId();
        }
        return cachedMapId;
    }
}

namespace movement
{
    void stop()
    {
        for (std::size_t index = 0; index < KeyCount; ++index) {
            sendKey(index, false);
        }
    }

    void update(const bool enabled)
    {
        if (!enabled) {
            stop();
            resetState();
            return;
        }

        LocalPlayerCache local;
        {
            std::lock_guard<std::mutex> lock(esp::dataMutex);
            local = esp::localPlayer;
        }
        if (!local.isValid || local.pawn == 0) {
            stop();
            haveAnchor = false;
            return;
        }

        int32_t health = 0;
        if (!memory::TryRead(
                local.pawn +
                    cs2_dumper::schemas::client_dll::
                        C_BaseEntity::m_iHealth,
                health) ||
            health <= 0) {
            stop();
            haveAnchor = false;
            return;
        }

        const auto now = SteadyClock::now();

        // Combat: stop moving (with counter-strafe) whenever the crosshair is
        // on a target or the triggerbot is preparing a shot.
        if (aimbot::isEngaging()) {
            if (!combatStopHeld) {
                combatStopHeld = true;
                beginQuickStop();
            }
            if (quickStopping && now >= quickStopUntil) {
                endQuickStop();
            }
            return;
        }
        if (combatStopHeld) {
            combatStopHeld = false;
            if (quickStopping) {
                endQuickStop();
            }
        }

        // Pause for a tick if the aimbot just snapped the view hard. Movement
        // is view-relative, so a large flick would flip the key directions.
        const float yawDelta = haveViewYaw
            ? normalizeAngle(local.viewAngle.y - lastViewYaw)
            : 0.0f;
        lastViewYaw = local.viewAngle.y;
        haveViewYaw = true;
        if (std::abs(yawDelta) > 12.0f) {
            stop();
            return;
        }

        if (!haveAnchor) {
            anchorX = local.position.x;
            anchorY = local.position.y;
            waypointIndex = 0;
            haveAnchor = true;
            lastSampleX = local.position.x;
            lastSampleY = local.position.y;
            lastStuckCheck = now;
        }
        if (waypointIndex >= kRouteOffsets.size()) {
            waypointIndex = 0;
        }

        // Collision detection: if movement keys are held but the player has not
        // moved, treat this as a wall and start a perpendicular detour.
        if (lastStuckCheck.time_since_epoch().count() == 0) {
            lastStuckCheck = now;
            lastSampleX = local.position.x;
            lastSampleY = local.position.y;
        } else if (now - lastStuckCheck >= kStuckCheckInterval) {
            const float movedX = local.position.x - lastSampleX;
            const float movedY = local.position.y - lastSampleY;
            const float moved = std::sqrt(movedX * movedX + movedY * movedY);
            if (anyPressed() && moved < kStuckDistance) {
                ++stuckCount;
                detourOffsetDegrees = (stuckCount % 2 == 0) ? 90.0f : -90.0f;
                detourUntil = now + kDetourDuration;
                if (stuckCount >= 3) {
                    // Give up on this waypoint and try the next one.
                    waypointIndex =
                        (waypointIndex + 1) % kRouteOffsets.size();
                    stuckCount = 0;
                }
                // Invalidate the grid path so it is recomputed from the new
                // position after the detour.
                routePath.clear();
                routePathIndex = 0;
            } else {
                stuckCount = 0;
            }
            lastSampleX = local.position.x;
            lastSampleY = local.position.y;
            lastStuckCheck = now;
        }

        // Prefer walking toward the nearest living player. Enemy and teammate
        // positions are always on reachable map ground, so chasing them follows
        // the real map layout far better than an abstract loop.
        bool chasing = false;
        float targetX = 0.0f;
        float targetY = 0.0f;
        const esp::EnemySnapshot enemies = esp::getEnemySnapshot();
        if (enemies) {
            float bestDistanceSquared = 6000.0f * 6000.0f;
            for (const EnemyInfo& enemy : *enemies) {
                if (enemy.health <= 0) {
                    continue;
                }
                const float toX = enemy.position.x - local.position.x;
                const float toY = enemy.position.y - local.position.y;
                const float distanceSquared = toX * toX + toY * toY;
                if (distanceSquared < bestDistanceSquared) {
                    bestDistanceSquared = distanceSquared;
                    targetX = enemy.position.x;
                    targetY = enemy.position.y;
                    chasing = true;
                }
            }
        }

        if (!chasing) {
            const std::array<float, 2>& offset = kRouteOffsets[waypointIndex];
            targetX = anchorX + offset[0];
            targetY = anchorY + offset[1];
        }

        const float directDeltaX = targetX - local.position.x;
        const float directDeltaY = targetY - local.position.y;
        const float directDistance = std::sqrt(
            directDeltaX * directDeltaX + directDeltaY * directDeltaY);

        if (chasing) {
            // Close enough: let the aimbot/triggerbot finish without walking
            // into the target.
            if (directDistance <= 150.0f) {
                stop();
                routePath.clear();
                routePathIndex = 0;
                return;
            }
        } else if (directDistance <= kReachRadius) {
            waypointIndex = (waypointIndex + 1) % kRouteOffsets.size();
            stuckCount = 0;
            routePath.clear();
            routePathIndex = 0;
            return;
        }

        // Follow an A* path over the map's walkable mask. The path is rebuilt
        // when it is missing, the goal moves, or the player leaves the route.
        const std::string& mapId = currentMapIdCached(now);
        const float goalDriftX = targetX - lastGoalX;
        const float goalDriftY = targetY - lastGoalY;
        const bool goalMoved =
            goalDriftX * goalDriftX + goalDriftY * goalDriftY >
            250.0f * 250.0f;
        const bool needsPath = routePath.empty() ||
            routePathIndex >= routePath.size() ||
            goalMoved ||
            mapId != pathMapId ||
            now - lastPathTime >= std::chrono::milliseconds(600);

        if (needsPath) {
            pathMapId = mapId;
            lastPathTime = now;
            lastGoalX = targetX;
            lastGoalY = targetY;
            routePath = navigation::findPath(
                mapId,
                local.position.x,
                local.position.y,
                targetX,
                targetY);
            routePathIndex = 0;
        }

        float aimX = targetX;
        float aimY = targetY;
        if (!routePath.empty()) {
            constexpr float kWaypointReachedRadius = 70.0f;
            while (routePathIndex < routePath.size()) {
                const float dx = routePath[routePathIndex].x - local.position.x;
                const float dy = routePath[routePathIndex].y - local.position.y;
                if (dx * dx + dy * dy <=
                    kWaypointReachedRadius * kWaypointReachedRadius) {
                    ++routePathIndex;
                } else {
                    break;
                }
            }
            if (routePathIndex < routePath.size()) {
                aimX = routePath[routePathIndex].x;
                aimY = routePath[routePathIndex].y;
            } else {
                // End of the grid path: fall back to the raw goal.
                routePath.clear();
                routePathIndex = 0;
            }
        }

        float deltaX = aimX - local.position.x;
        float deltaY = aimY - local.position.y;
        float distance = std::sqrt(deltaX * deltaX + deltaY * deltaY);
        if (distance < 1.0f) {
            deltaX = directDeltaX;
            deltaY = directDeltaY;
            distance = directDistance;
        }

        float movementYaw = local.viewAngle.y;
        if (now < detourUntil) {
            movementYaw += detourOffsetDegrees;
        }

        const float yawRadians = movementYaw * (kPi / 180.0f);
        const float forwardX = std::cos(yawRadians);
        const float forwardY = std::sin(yawRadians);
        const float rightX = forwardY;
        const float rightY = -forwardX;

        const float forwardAmount = deltaX * forwardX + deltaY * forwardY;
        const float rightAmount = deltaX * rightX + deltaY * rightY;

        sendKey(KeyW, forwardAmount > kAxisDeadband);
        sendKey(KeyS, forwardAmount < -kAxisDeadband);
        sendKey(KeyD, rightAmount > kAxisDeadband);
        sendKey(KeyA, rightAmount < -kAxisDeadband);
    }
}
