
#include <windows.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include "reshade.hpp"

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_enabled{false};
static std::atomic<bool> g_busy{false};

static HWND g_magpie = nullptr;
static LONG_PTR g_oldExStyle = 0;
static LONG_PTR g_oldStyle = 0;
static HWND g_previousForeground = nullptr;

// Change this if you want another toggle key.
// F10 is deliberately different from ReShade's Home key.
static constexpr int TOGGLE_KEY = VK_F10;

static HWND FindMagpieRenderer()
{
    HWND result = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        char cls[256]{};
        GetClassNameA(hwnd, cls, sizeof(cls));

        if (strcmp(cls, "Magpie_Renderer") == 0 && IsWindowVisible(hwnd))
        {
            *reinterpret_cast<HWND *>(lp) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

static HWND FindWindowBehind(HWND renderer)
{
    if (!renderer)
        return nullptr;

    // Magpie's renderer is normally above the source window in Z-order.
    HWND h = GetWindow(renderer, GW_HWNDNEXT);
    while (h)
    {
        if (IsWindowVisible(h) && IsWindowEnabled(h))
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);

            // Prefer a window belonging to another process (the game/source).
            if (pid != GetCurrentProcessId())
            {
                char cls[256]{};
                GetClassNameA(h, cls, sizeof(cls));

                if (strcmp(cls, "Progman") != 0 &&
                    strcmp(cls, "WorkerW") != 0 &&
                    strcmp(cls, "Shell_TrayWnd") != 0)
                    return h;
            }
        }
        h = GetWindow(h, GW_HWNDNEXT);
    }

    return nullptr;
}

static void ActivateWindow(HWND hwnd)
{
    if (!hwnd)
        return;

    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myThread = GetCurrentThreadId();

    if (fgThread && fgThread != myThread)
        AttachThreadInput(fgThread, myThread, TRUE);

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (fgThread && fgThread != myThread)
        AttachThreadInput(fgThread, myThread, FALSE);
}

static void SendHome()
{
    INPUT in[2]{};

    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_HOME;

    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_HOME;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, in, sizeof(INPUT));
}

static void EnablePassthrough()
{
    if (g_busy.exchange(true))
        return;

    HWND hwnd = FindMagpieRenderer();
    if (!hwnd)
    {
        reshade::log::message(
            reshade::log::level::warning,
            "Magpie ReShade Input: Magpie_Renderer not found.");
        g_busy = false;
        return;
    }

    g_magpie = hwnd;
    g_previousForeground = GetForegroundWindow();

    g_oldExStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    g_oldStyle = GetWindowLongPtr(hwnd, GWL_STYLE);

    // Magpie deliberately uses NOACTIVATE. ReShade needs the renderer
    // window to become the active input window for its overlay.
    LONG_PTR ex = g_oldExStyle;
    ex &= ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
    ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);

    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    ActivateWindow(hwnd);

    // Give Windows/ReShade a moment to observe the new foreground window.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    SendHome();

    g_enabled = true;
    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: passthrough ON (F10).");

    g_busy = false;
}

static void DisablePassthrough()
{
    if (g_busy.exchange(true))
        return;

    HWND hwnd = g_magpie;
    if (hwnd && IsWindow(hwnd))
    {
        // Close ReShade overlay first.
        SendHome();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        SetWindowLongPtr(hwnd, GWL_EXSTYLE, g_oldExStyle);
        SetWindowLongPtr(hwnd, GWL_STYLE, g_oldStyle);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    HWND target = FindWindowBehind(hwnd);
    if (target)
        ActivateWindow(target);
    else if (g_previousForeground && IsWindow(g_previousForeground))
        ActivateWindow(g_previousForeground);

    g_magpie = nullptr;
    g_enabled = false;

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: passthrough OFF (F10).");

    g_busy = false;
}

static void Worker()
{
    bool last = false;

    while (g_running)
    {
        const bool down = (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;

        if (down && !last && !g_busy)
        {
            if (g_enabled)
                DisablePassthrough();
            else
                EnablePassthrough();
        }

        last = down;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

extern "C" __declspec(dllexport) const char *NAME =
    "Magpie ReShade Input Passthrough";

extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Makes the Magpie renderer focusable so ReShade's overlay can receive keyboard and mouse input.";

static HMODULE g_module = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = hModule;

        if (!reshade::register_addon(hModule))
            return FALSE;

        DisableThreadLibraryCalls(hModule);

        std::thread(Worker).detach();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_running = false;

        if (g_enabled)
        {
            // Best effort cleanup. Avoid waiting here.
            HWND hwnd = g_magpie;
            if (hwnd && IsWindow(hwnd))
            {
                SetWindowLongPtr(hwnd, GWL_EXSTYLE, g_oldExStyle);
                SetWindowLongPtr(hwnd, GWL_STYLE, g_oldStyle);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        }

        reshade::unregister_addon(hModule);
    }

    return TRUE;
}
