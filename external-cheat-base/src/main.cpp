#include "features/esp.hpp"
#include "features/aimbot.hpp"
#include "features/movement.hpp"
#include "features/menu.hpp"
#include "core/renderer/sdl_renderer.h"
#include "core/diagnostics.hpp"
#include "core/performance_metrics.hpp"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>
#include <Windows.h>
#include "imgui.h"

uint32_t WIDTH = 0;
uint32_t HEIGHT = 0;
uint32_t WINDOW_W = 0;
uint32_t WINDOW_H = 0;
int32_t VIEWPORT_X = 0;
int32_t VIEWPORT_Y = 0;
uint32_t VIEWPORT_W = 0;
uint32_t VIEWPORT_H = 0;

namespace
{
    class ScopedHandle
    {
    public:
        explicit ScopedHandle(HANDLE handle = nullptr)
            : handle_(handle)
        {
        }

        ~ScopedHandle()
        {
            if (handle_) {
                CloseHandle(handle_);
            }
        }

        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        HANDLE get() const
        {
            return handle_;
        }

    private:
        HANDLE handle_ = nullptr;
    };

    bool hasArgument(int argc, char* argv[], const char* expected)
    {
        for (int index = 1; index < argc; ++index) {
            if (_stricmp(argv[index], expected) == 0) {
                return true;
            }
        }
        return false;
    }

    void showFatalError(const wchar_t* message)
    {
        diagnostics::log(message);
        MessageBoxW(
            nullptr,
            message,
            L"DM Bot",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }

    // Draws the compact, always-on-top status badge in the top-right corner.
    // Yellow means CS2 is not attached yet, blue means the bot is attached and
    // shows whether it is currently enabled.
    void renderStatusBadge(bool cs2Found)
    {
        // ESP-only stages are intentionally stealthy: no badge at all.
        if (menu::espOnlyStage.load(std::memory_order_relaxed) != 0) {
            return;
        }
        if (WIDTH == 0 || HEIGHT == 0) {
            return;
        }

        const bool enabled =
            menu::botEnabled.load(std::memory_order_relaxed);
        const char* text = !cs2Found
            ? "DM Bot waiting for CS2 process"
            : (enabled ? "DM Bot enable" : "DM Bot disable");

        const float scale = std::max(1.0f, sdl_renderer::getDpiScale());
        const float fontSize = 20.0f * scale;
        ImFont* font = ImGui::GetFont();
        if (!font) {
            return;
        }

        const ImVec2 textSize = font->CalcTextSizeA(
            fontSize,
            FLT_MAX,
            0.0f,
            text);
        const float paddingX = 14.0f * scale;
        const float paddingY = 10.0f * scale;
        const float margin = 16.0f * scale;
        const float boxWidth = textSize.x + paddingX * 2.0f;
        const float boxHeight = textSize.y + paddingY * 2.0f;

        const ImVec2 minimum(
            static_cast<float>(WIDTH) - margin - boxWidth,
            margin);
        const ImVec2 maximum(
            static_cast<float>(WIDTH) - margin,
            margin + boxHeight);

        const ImU32 background = cs2Found
            ? IM_COL32(30, 110, 220, 235)
            : IM_COL32(255, 214, 0, 245);
        const ImU32 foreground = cs2Found
            ? IM_COL32(255, 255, 255, 255)
            : IM_COL32(0, 0, 0, 255);

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        drawList->AddRectFilled(
            minimum,
            maximum,
            background,
            6.0f * scale);
        drawList->AddText(
            font,
            fontSize,
            ImVec2(minimum.x + paddingX, minimum.y + paddingY),
            foreground,
            text);
    }

    // F9 is handled globally so it also works on the waiting screen, where the
    // overlay window is not guaranteed to own the foreground.
    void pollExitHotkey()
    {
        static bool latch = false;
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !latch) {
            latch = true;
            sdl_renderer::running = false;
        } else if (!down) {
            latch = false;
        }
    }
}

int main(int argc, char* argv[])
{
    SetLastError(ERROR_SUCCESS);
    ScopedHandle instanceMutex(CreateMutexW(
        nullptr,
        FALSE,
        L"Local\\DM-Bot-76F5E6C2-235A-4AA0"));
    const DWORD mutexError = GetLastError();
    if (!instanceMutex.get()) {
        showFatalError(L"Unable to create the single-instance guard.");
        return -1;
    }
    if (mutexError == ERROR_ALREADY_EXISTS) {
        MessageBoxW(
            nullptr,
            L"DM Bot is already running.",
            L"DM Bot",
            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }

    memory::SetWritesAllowed(
        hasArgument(argc, argv, "--allow-memory-writes"));

#ifndef SHOW_CONSOLE
    FreeConsole();
#endif

    menu::setBotEnabled(false);

    while (sdl_renderer::running)
    {
        // The overlay is always click-through; there is no interactive GUI.
        sdl_renderer::menuVisible = false;
        if (!sdl_renderer::initWaiting()) {
            showFatalError(
                L"Unable to initialize the overlay. Windows 10/11 with "
                L"per-monitor DPI awareness and a working SDL2.dll are required.");
            memory::Close();
            return -1;
        }
        if (!sdl_renderer::initImGui()) {
            showFatalError(L"Unable to initialize ImGui.");
            sdl_renderer::destroy();
            memory::Close();
            return -1;
        }

        DWORD lastCheckTime = 0;
        bool gameFound = false;
        while (sdl_renderer::running && !gameFound)
        {
            sdl_renderer::pollEvents();
            pollExitHotkey();

            const DWORD currentTime = GetTickCount();
            if (currentTime - lastCheckTime >= 3000 ||
                lastCheckTime == 0) {
                lastCheckTime = currentTime;
                if (esp::init()) {
                    gameFound = true;
                    break;
                }
            }

            if (!sdl_renderer::beginFrame()) {
                break;
            }
            sdl_renderer::newFrameImGui();
            renderStatusBadge(false);
            sdl_renderer::renderImGui();
            sdl_renderer::endFrame();
            Sleep(16);
        }

        sdl_renderer::shutdownImGui();
        sdl_renderer::destroy();
        if (!sdl_renderer::running) {
            break;
        }
        if (!gameFound ||
            !sdl_renderer::init(
                L"Counter-Strike 2",
                static_cast<DWORD>(esp::pID)) ||
            !sdl_renderer::initImGui() ||
            !aimbot::init()) {
            diagnostics::log(
                L"Game attach failed or raced with shutdown; retrying.");
            sdl_renderer::shutdownImGui();
            sdl_renderer::destroy();
            esp::clearRuntimeState();
            memory::Close();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500));
            continue;
        }

        std::atomic<bool> dataRunning{true};
        std::thread dataThread([&dataRunning]() {
            bool stateClearedWhileInactive = false;
            while (dataRunning.load(std::memory_order_relaxed)) {
                const menu::RuntimeConfig config =
                    menu::getRuntimeConfig();
                const bool gameForeground =
                    sdl_renderer::isGameForeground();

                if (!gameForeground) {
                    movement::stop();
                    if (!stateClearedWhileInactive) {
                        esp::clearRuntimeState();
                        stateClearedWhileInactive = true;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(25));
                    continue;
                }
                stateClearedWhileInactive = false;

                esp::updateEntities(config);
                aimbot::update(config);
                aimbot::updateTriggerbot(config);
                movement::update(config.autowalkEnabled);

                const bool latencySensitive =
                    config.aimbotEnabled || config.triggerbotEnabled;
                const auto interval = std::chrono::microseconds(
                    latencySensitive ? 4000 : 8000);
                std::this_thread::sleep_for(interval);
            }
            movement::stop();
        });

        auto nextRenderTime = std::chrono::steady_clock::now();
        while (sdl_renderer::running &&
               !sdl_renderer::isGameDisconnected())
        {
            sdl_renderer::pollEvents();
            pollExitHotkey();
            sdl_renderer::updateWindowPosition();
            if (!sdl_renderer::isGameVisible()) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(25));
                nextRenderTime = std::chrono::steady_clock::now();
                continue;
            }

            if (!sdl_renderer::beginFrame()) {
                break;
            }
            sdl_renderer::newFrameImGui();

            const bool enabled =
                menu::botEnabled.load(std::memory_order_relaxed);
            const bool espOnly =
                menu::espOnlyStage.load(std::memory_order_relaxed) != 0;
            if (enabled || espOnly) {
                esp::render();
            }
            renderStatusBadge(true);

            sdl_renderer::renderImGui();
            sdl_renderer::endFrame();

            const int refreshRate = std::max(
                60,
                sdl_renderer::getTargetRefreshRate());
            nextRenderTime += std::chrono::nanoseconds(
                1000000000LL / refreshRate);
            const auto now = std::chrono::steady_clock::now();
            if (nextRenderTime > now) {
                std::this_thread::sleep_until(nextRenderTime);
            } else {
                nextRenderTime = now;
            }
        }

        dataRunning.store(false, std::memory_order_relaxed);
        dataThread.join();
        movement::stop();
        sdl_renderer::shutdownImGui();
        sdl_renderer::destroy();
        esp::clearRuntimeState();
        memory::Close();

        if (sdl_renderer::running) {
            diagnostics::log(
                L"CS2 disconnected; returning to the waiting screen.");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500));
        }
    }

    movement::stop();
    memory::Close();
    return 0;
}
