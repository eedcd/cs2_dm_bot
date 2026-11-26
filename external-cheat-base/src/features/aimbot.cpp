#ifndef NOMINMAX
#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#endif

#include "aimbot.hpp"
#include "esp.hpp"
#include "menu.hpp"
#include "navigation.hpp"
#include <Windows.h>
#include <iostream>
#include <cmath>
#include <limits>
#include <algorithm>
#include <chrono>
#include <random>
#include <atomic>
#include <array>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{
    using SteadyClock = std::chrono::steady_clock;

    SteadyClock::time_point lastAimUpdate{};
    SteadyClock::time_point triggerTargetAcquired{};
    SteadyClock::time_point lastTriggerShot{};
    std::chrono::milliseconds currentTriggerDelay{200};
    float aimResidualX = 0.0f;
    float aimResidualY = 0.0f;
    int32_t currentAimTargetIndex = -1;
    int32_t currentTriggerTargetIndex = -1;

    // Set when the crosshair is close enough to a target (or the triggerbot is
    // about to shoot) so the movement module can stop the player first.
    std::atomic<bool> aimEngaging{false};
    std::atomic<bool> triggerEngaging{false};

    // Map id used for geometric line-of-sight checks. Reading it touches game
    // memory, so refresh at most once per second.
    const std::string& currentMapIdCached()
    {
        static std::string mapId;
        static SteadyClock::time_point lastQuery{};
        const auto now = SteadyClock::now();
        if (mapId.empty() ||
            now - lastQuery >= std::chrono::seconds(1)) {
            lastQuery = now;
            mapId = esp::currentMapId();
        }
        return mapId;
    }

    // The game's EntitySpottedState_t stores one bit per observer. The bit for
    // the local player is only set when the server-side visibility trace from
    // this player's eyes reached the pawn, so it is a trustworthy positive
    // signal even when a 2D radar mask cannot describe the geometry (props,
    // multi-floor ledges, or thin walls at source resolution).
    bool isLocallySpotted(const EnemyInfo& enemy)
    {
        if (!enemy.spottedByMaskKnown) {
            return false;
        }
        const uint32_t localIndex = esp::localEntityIndex.load(
            std::memory_order_relaxed);
        if (localIndex == 0 || localIndex > 64) {
            return false;
        }
        const uint32_t bit = localIndex - 1;
        return ((enemy.spottedByMask[bit / 32] >> (bit % 32)) & 1u) != 0;
    }

    // Real visibility test. Dormant pawns are outside the PVS and can never be
    // shot. A pawn spotted by the local player is confirmed visible by the game
    // itself; otherwise the segment is traced against the map's full-resolution
    // obstacle mask, sampling head, chest and feet. This keeps a target visible
    // when only its upper body peeks over cover while walls still block every
    // sample. No global "spotted" fallback is used because that flag is
    // team-shared and persists briefly, which caused through-wall locking.
    bool hasClearLineOfSight(
        const std::string& mapId,
        const vec3& eyePosition,
        const EnemyInfo& enemy)
    {
        if (enemy.isDormant) {
            return false;
        }
        if (isLocallySpotted(enemy)) {
            return true;
        }
        if (!navigation::isAvailable(mapId)) {
            // Workshop maps do not ship radar/overview data, so there is no
            // geometry to trace. Fall back to the game's spotted state, but
            // keep it short-range so the bot cannot snap onto enemies through
            // walls across the map.
            return enemy.visibilityKnown && enemy.isSpotted &&
                enemy.distance <= 1200.0f;
        }

        std::array<vec3, 3> samples{};
        int sampleCount = 0;
        if (enemy.hasBones) {
            samples[sampleCount++] =
                enemy.bonePositions[BoneIndex::HEAD];
            samples[sampleCount++] =
                enemy.bonePositions[BoneIndex::SPINE_2];
        } else {
            samples[sampleCount++] = enemy.headPosition;
        }
        samples[sampleCount++] = enemy.position;

        for (int index = 0; index < sampleCount; ++index) {
            const vec3& point = samples[index];
            if (!std::isfinite(point.x) ||
                !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                continue;
            }
            if (navigation::hasLineOfSight3D(
                    mapId,
                    eyePosition.x,
                    eyePosition.y,
                    eyePosition.z,
                    point.x,
                    point.y,
                    point.z)) {
                return true;
            }
        }
        return false;
    }

    int randomTriggerDelayMilliseconds(
        const int minimum,
        const int maximum)
    {
        static thread_local std::mt19937 generator{
            std::random_device{}()};
        const int low = std::max(0, std::min(minimum, maximum));
        const int high = std::max(low, std::max(minimum, maximum));
        std::uniform_int_distribution<int> distribution(low, high);
        return distribution(generator);
    }

    float timeBasedAlpha(
        SteadyClock::time_point& previous,
        float smoothing)
    {
        const auto now = SteadyClock::now();
        float deltaSeconds = 1.0f / 144.0f;
        if (previous.time_since_epoch().count() != 0) {
            deltaSeconds = std::chrono::duration<float>(
                now - previous).count();
        }
        previous = now;
        deltaSeconds = std::clamp(
            deltaSeconds,
            1.0f / 1000.0f,
            0.05f);
        const float safeSmoothing =
            std::max(1.0f, smoothing);
        if (safeSmoothing <= 1.01f) {
            return 1.0f;
        }
        const float responsePerSecond =
            60.0f / safeSmoothing;
        return 1.0f -
            std::exp(-responsePerSecond * deltaSeconds);
    }

    LONG consumeMouseDelta(float value, float& residual)
    {
        const float accumulated = value + residual;
        const LONG whole = static_cast<LONG>(
            std::trunc(accumulated));
        residual = accumulated - static_cast<float>(whole);
        return whole;
    }

    void resetAimState()
    {
        lastAimUpdate = {};
        aimResidualX = 0.0f;
        aimResidualY = 0.0f;
        currentAimTargetIndex = -1;
        aimEngaging.store(false, std::memory_order_relaxed);
    }

    void resetTriggerState()
    {
        triggerTargetAcquired = {};
        currentTriggerTargetIndex = -1;
        triggerEngaging.store(false, std::memory_order_relaxed);
    }
}

// Calculate head offset for side-facing enemies
// When enemy is facing sideways (angle 45-135 degrees to player),
// the head extends forward in the direction they are facing
vec3 calculateHeadOffset(
    const vec3& headPos,
    float enemyViewYaw,
    float angleToPlayer,
    const menu::RuntimeConfig& config)
{
    // Check if head offset is enabled
    if (!config.headOffsetEnabled) {
        return headPos;
    }

    // Check if enemy is within the angle range for offset
    if (angleToPlayer < config.headOffsetAngleMin ||
        angleToPlayer > config.headOffsetAngleMax) {
        return headPos;
    }

    // Calculate offset strength based on angle
    // Maximum offset at 90 degrees (perfectly sideways), less at edges
    float centerAngle =
        (config.headOffsetAngleMin + config.headOffsetAngleMax) / 2.0f;
    float angleRange =
        (config.headOffsetAngleMax - config.headOffsetAngleMin) / 2.0f;
    float angleFactor = angleRange > 0.0001f
        ? 1.0f - std::abs(angleToPlayer - centerAngle) / angleRange
        : 1.0f;
    angleFactor = std::max(0.0f, std::min(1.0f, angleFactor));  // Clamp to 0-1

    // Calculate offset direction (perpendicular to enemy's facing direction)
    // Enemy viewYaw: 0 = facing +X, 90 = facing +Y, etc.
    float yawRadians = enemyViewYaw * static_cast<float>(M_PI) / 180.0f;

    // Head extends in the direction enemy is facing
    float offsetX = std::cos(yawRadians) * config.headOffsetAmount * angleFactor;
    float offsetY = std::sin(yawRadians) * config.headOffsetAmount * angleFactor;

    vec3 adjustedHead = headPos;
    adjustedHead.x += offsetX;
    adjustedHead.y += offsetY;
    // Z offset is typically not needed as head height stays same

    return adjustedHead;
}

namespace
{
    vec3 targetPosition(
        const EnemyInfo& enemy,
        int selectedBone,
        const menu::RuntimeConfig& config)
    {
        vec3 position{};
        switch (selectedBone) {
            case 1:
                position = enemy.hasBones
                    ? enemy.bonePositions[BoneIndex::NECK]
                    : vec3{
                        enemy.headPosition.x,
                        enemy.headPosition.y,
                        enemy.headPosition.z - 5.0f
                    };
                break;
            case 2:
                if (enemy.hasBones) {
                    position =
                        enemy.bonePositions[BoneIndex::SPINE_2];
                } else {
                    position = {
                        (enemy.headPosition.x + enemy.position.x) /
                            2.0f,
                        (enemy.headPosition.y + enemy.position.y) /
                            2.0f,
                        (enemy.headPosition.z + enemy.position.z) /
                            2.0f
                    };
                }
                return position;
            case 0:
            default:
                position = enemy.hasBones
                    ? enemy.bonePositions[BoneIndex::HEAD]
                    : enemy.headPosition;
                break;
        }

        return enemy.viewAngleKnown
            ? calculateHeadOffset(
                position,
                enemy.viewYaw,
                enemy.angleToPlayer,
                config)
            : position;
    }
}

bool aimbot::init()
{
    // Reuse the already validated read-only process handle and module base.
    // Reopening here could invalidate a working ESP handle on a transient
    // OpenProcess failure.
    pID = esp::pID;
    modBase = esp::modBase;
    return pID != 0 && modBase != 0;
}

float aimbot::normalizeAngle(float angle)
{
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

vec2 aimbot::calcAngle(const vec3& src, const vec3& dst)
{
    // Delta = TargetPos - LocalEyePos
    vec3 delta = { dst.x - src.x, dst.y - src.y, dst.z - src.z };

    float hyp = std::sqrt(delta.x * delta.x + delta.y * delta.y);

    vec2 angle;
    // Correct CS2 formula:
    // Yaw = atan2(Δy, Δx) * 180/π
    angle.y = std::atan2(delta.y, delta.x) * (180.0f / static_cast<float>(M_PI));
    // Pitch = -atan2(Δz, sqrt(Δx² + Δy²)) * 180/π
    angle.x = -std::atan2(delta.z, hyp) * (180.0f / static_cast<float>(M_PI));

    return angle;
}



float aimbot::getFOV(const vec2& viewAngle, const vec2& aimAngle)
{
    float pitchDiff = normalizeAngle(aimAngle.x - viewAngle.x);
    float yawDiff = normalizeAngle(aimAngle.y - viewAngle.y);

    return std::sqrt(pitchDiff * pitchDiff + yawDiff * yawDiff);
}


void aimbot::update(const menu::RuntimeConfig& config)
{
    if (!config.aimbotEnabled ||
        config.inputSuppressed ||
        !sdl_renderer::isInputAllowed()) {
        resetAimState();
        return;
    }

    // Aimbot runs for as long as DM Bot is enabled; no extra key is required.

    // Use cached local player data (updated once per frame in esp::updateEntities)
    if (!esp::localPlayer.isValid) {
        resetAimState();
        return;
    }

    const vec2& currentViewAngle = esp::localPlayer.viewAngle;
    const vec3& eyePos = esp::localPlayer.eyePosition;

    const esp::EnemySnapshot enemies =
        esp::getEnemySnapshot();
    if (!enemies) {
        resetAimState();
        return;
    }

    struct Candidate
    {
        bool valid = false;
        int32_t entityIndex = -1;
        vec2 angle{};
        float metric = std::numeric_limits<float>::infinity();
    };
    Candidate best{};
    Candidate retained{};
    const std::string& mapId = currentMapIdCached();

    for (const EnemyInfo& enemy : *enemies) {
        const vec2 candidateAngle = calcAngle(
            eyePos,
            targetPosition(
                enemy,
                config.aimbotBone,
                config));
        const float fov = getFOV(
            currentViewAngle,
            candidateAngle);
        if (!std::isfinite(fov)) {
            continue;
        }
        if (!config.smartAimEnabled && fov >= config.aimbotFOV) {
            continue;
        }

        // Only engage targets that are actually visible. Using the game's
        // "spotted" flag here was wrong: it is a radar marker, so enemies
        // behind walls were locked while visible ones were ignored.
        if ((config.aimbotVisibleOnly || config.smartAimEnabled) &&
            !hasClearLineOfSight(mapId, eyePos, enemy)) {
            continue;
        }

        float metric = fov;
        if (config.smartAimEnabled) {
            metric = config.smartAimPriority == 0
                ? enemy.distance
                : static_cast<float>(enemy.health) +
                    enemy.distance * 0.001f;
        }

        const Candidate candidate{
            true,
            static_cast<int32_t>(enemy.entityIndex),
            candidateAngle,
            metric
        };
        if (!best.valid || metric < best.metric) {
            best = candidate;
        }
        if (candidate.entityIndex == currentAimTargetIndex) {
            retained = candidate;
        }
    }

    if (!best.valid) {
        resetAimState();
        return;
    }

    // Keep a still-valid target unless a replacement is materially better.
    // This prevents frame-to-frame oscillation between nearly equal players.
    Candidate selected = best;
    if (retained.valid) {
        // Keep a valid target briefly, but do not stay on a much worse angle.
        // With a 360-degree search a large hysteresis would keep the aim on a
        // distant/blocked-looking enemy while a visible one is right ahead.
        const float hysteresis = config.smartAimEnabled
            ? std::max(1.0f, std::abs(best.metric) * 0.10f)
            : 2.0f;
        if (retained.metric <= best.metric + hysteresis) {
            selected = retained;
        }
    }
    currentAimTargetIndex = selected.entityIndex;
    const float aimError = getFOV(currentViewAngle, selected.angle);
    aimEngaging.store(
        std::isfinite(aimError) && aimError <= 6.0f,
        std::memory_order_relaxed);

    // Exponential time-based smoothing is stable across worker rates.
    const float alpha =
        timeBasedAlpha(lastAimUpdate, config.aimbotSmoothing);
    float deltaPitch =
        normalizeAngle(
            selected.angle.x - currentViewAngle.x) * alpha;
    float deltaYaw =
        normalizeAngle(
            selected.angle.y - currentViewAngle.y) * alpha;

    // Convert angle delta to mouse movement
    const float mouseSensitivityFactor =
        std::max(0.01f, config.mouseSensitivity) * 0.022f;

    // In CS2: Moving mouse RIGHT decreases Yaw, DOWN increases Pitch
    float moveX = -deltaYaw / mouseSensitivityFactor;
    float moveY = deltaPitch / mouseSensitivityFactor;

    // Move mouse if delta is significant
    const LONG moveWholeX =
        consumeMouseDelta(moveX, aimResidualX);
    const LONG moveWholeY =
        consumeMouseDelta(moveY, aimResidualY);
    if (moveWholeX != 0 || moveWholeY != 0)
    {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = moveWholeX;
        input.mi.dy = moveWholeY;
        if (SendInput(1, &input, sizeof(INPUT)) != 1) {
            aimResidualX = 0.0f;
            aimResidualY = 0.0f;
        }
    }
}

bool aimbot::isEngaging()
{
    return aimEngaging.load(std::memory_order_relaxed) ||
        triggerEngaging.load(std::memory_order_relaxed);
}

void aimbot::updateTriggerbot(const menu::RuntimeConfig& config)
{
    if (!config.triggerbotEnabled ||
        config.inputSuppressed ||
        !sdl_renderer::isInputAllowed()) {
        resetTriggerState();
        return;
    }

    // Triggerbot runs for as long as DM Bot is enabled; no extra key is required.

    if (!esp::localPlayer.isValid) {
        resetTriggerState();
        return;
    }

    const int32_t crosshairEntityIndex =
        esp::localPlayer.crosshairEntityIndex;
    if (crosshairEntityIndex <= 0) {
        resetTriggerState();
        return;
    }

    const esp::EnemySnapshot enemies =
        esp::getEnemySnapshot();
    if (!enemies) {
        resetTriggerState();
        return;
    }

    bool liveEnemyUnderCrosshair = false;
    for (const EnemyInfo& enemy : *enemies) {
        if (static_cast<int32_t>(enemy.entityIndex) ==
            crosshairEntityIndex) {
            liveEnemyUnderCrosshair = true;
            break;
        }
    }
    if (!liveEnemyUnderCrosshair) {
        resetTriggerState();
        return;
    }

    // The crosshair is on a target: stop moving while the shot is prepared.
    triggerEngaging.store(true, std::memory_order_relaxed);

    const auto now = SteadyClock::now();
    if (currentTriggerTargetIndex != crosshairEntityIndex) {
        currentTriggerTargetIndex = crosshairEntityIndex;
        triggerTargetAcquired = now;
        currentTriggerDelay = std::chrono::milliseconds(
            config.triggerbotRandomDelay
                ? randomTriggerDelayMilliseconds(
                    config.triggerbotDelayMin,
                    config.triggerbotDelayMax)
                : std::max(0, config.triggerbotDelay));
    }

    if (now - triggerTargetAcquired < currentTriggerDelay) {
        return;
    }

    // The configured delay controls initial reaction time. A separate minimum
    // shot interval prevents a zero-delay setting from injecting hundreds of
    // clicks per second while the crosshair remains on one entity.
    constexpr auto MINIMUM_SHOT_INTERVAL =
        std::chrono::milliseconds(50);
    if (lastTriggerShot.time_since_epoch().count() != 0 &&
        now - lastTriggerShot < MINIMUM_SHOT_INTERVAL) {
        return;
    }

    if (!sdl_renderer::isInputAllowed()) {
        resetTriggerState();
        return;
    }

    INPUT clicks[2]{};
    clicks[0].type = INPUT_MOUSE;
    clicks[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    clicks[1].type = INPUT_MOUSE;
    clicks[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (SendInput(2, clicks, sizeof(INPUT)) == 2) {
        lastTriggerShot = now;
    } else {
        resetTriggerState();
    }
}
