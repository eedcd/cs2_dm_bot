#pragma once

// Minimal runtime configuration for DM Bot. The interactive ImGui menu has been
// removed; the program is controlled only by the F4 (enable/disable) and F9
// (exit) hotkeys.

#include <Windows.h>
#include <atomic>
#include <mutex>

#include "core/renderer/sdl_renderer.h"

namespace menu
{
    struct RuntimeConfig
    {
        // ESP
        bool espEnabled = false;
        bool espBox = true;
        bool espHealth = true;
        bool espDistance = true;
        bool espWeapon = true;
        bool espSkeleton = true;
        bool espViewAngle = true;
        bool espFlashIndicator = false;
        bool espWallCheck = true;
        bool grenadeESP = false;
        bool droppedWeaponESP = false;
        bool bombTimer = false;
        bool antiFlash = false;

        // Targeting
        bool targetAllTeams = true;

        // Aimbot
        bool headOffsetEnabled = true;
        float headOffsetAmount = 5.0f;
        float headOffsetAngleMin = 45.0f;
        float headOffsetAngleMax = 135.0f;
        bool aimbotEnabled = false;
        float aimbotFOV = 360.0f;
        float aimbotSmoothing = 5.0f;
        int aimbotBone = 0;
        bool aimbotVisibleOnly = true;
        int aimbotKey = VK_SHIFT;
        bool smartAimEnabled = false;
        int smartAimPriority = 0;
        float mouseSensitivity = 1.0f;
        
        // Triggerbot
        bool triggerbotEnabled = false;
        int triggerbotDelay = 200;
        int triggerbotKey = 0x46;
        bool triggerbotRandomDelay = true;
        int triggerbotDelayMin = 80;
        int triggerbotDelayMax = 150;
        // int triggerbotDelayMin = 0;
        // int triggerbotDelayMax = 0;

        // Automatic movement
        bool autowalkEnabled = false;

        bool inputSuppressed = false;

        // Radar modes were removed; kept for source compatibility.
        [[nodiscard]] bool radarSnapshotEnabled() const noexcept
        {
            return false;
        }
        int radarRefreshRateHz = 20;
    };

    inline std::mutex configMutex;
    inline RuntimeConfig runtimeConfig{};
    inline std::atomic<bool> botEnabled{false};

    // ESP presentation globals consumed by esp::render().
    inline bool espEnabled = true;
    inline bool espBox = true;
    inline bool espHealth = true;
    inline bool espDistance = true;
    inline bool espWeapon = true;
    inline bool espViewAngle = true;
    inline bool espViewAngleText = false;
    inline bool espFlashIndicator = false;
    inline bool espWallCheck = true;
    inline bool espSnaplines = false;
    inline bool espSkeleton = true;
    inline int snaplinesOrigin = 0;

    inline float espSkeletonColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    inline float espBoxColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    inline float espWallColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    inline float espDistanceColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    inline float espWeaponColor[4] = { 0.0f, 1.0f, 1.0f, 1.0f };
    inline float espFlashNormalColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    inline float espFlashColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    inline float espSnaplinesColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    inline float aimbotFOVColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    inline bool aimbotShowFOV = false;
    inline bool aimbotEnabled = false;
    inline float aimbotFOV = 360.0f;
    inline bool bombTimer = false;
    inline bool grenadeESP = false;
    inline bool droppedWeaponESP = false;

    inline int menuToggleKey = VK_F4;
    inline int exitKey = VK_F9;
    inline int viewportMode = 0;

    // Binding UI was removed; these stay false so the renderer hotkey path is
    // always active.
    inline bool isBindingKey = false;
    inline bool suppressHotkeysUntilRelease = false;

    inline bool ConfiguredHotkeysReleased()
    {
        return true;
    }

    inline void setBotEnabled(bool enabled)
    {
        RuntimeConfig config;
        if (enabled) {
            config.espEnabled = true;
            config.espBox = true;
            config.espHealth = true;
            config.espDistance = true;
            config.espWeapon = true;
            config.espSkeleton = true;
            config.espViewAngle = true;
            config.espWallCheck = true;

            config.targetAllTeams = true;

            config.aimbotEnabled = true;
            config.aimbotFOV = 360.0f;
            config.aimbotSmoothing = 5.0f;
            config.aimbotBone = 0;
            config.aimbotVisibleOnly = true;
            config.smartAimEnabled = false;

            config.triggerbotEnabled = true;
            config.triggerbotRandomDelay = true;
            config.triggerbotDelayMin = 100;
            config.triggerbotDelayMax = 300;

            config.headOffsetEnabled = true;
            config.autowalkEnabled = true;
        }

        {
            std::lock_guard<std::mutex> lock(configMutex);
            runtimeConfig = config;
        }
        botEnabled.store(enabled, std::memory_order_relaxed);
    }

    inline void toggleBot()
    {
        setBotEnabled(!botEnabled.load(std::memory_order_relaxed));
    }

    inline RuntimeConfig getRuntimeConfig()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        return runtimeConfig;
    }

    inline void publishRuntimeConfig()
    {
        setBotEnabled(botEnabled.load(std::memory_order_relaxed));
    }
}
