#include <windows.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include "reshade.hpp"

// Magpie/ReShade input passthrough.
//
// Magpie registers several window classes. The previous implementation was
// intentionally broad, but changing styles on every window in Magpie.exe can
// interfere with Magpie's own UI/renderer and crash the process. This version
// is conservative: it only modifies windows that actually advertise the
// click-through/no-activate styles, and it never removes WS_EX_LAYERED.

static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_inputPassthrough{ false };
static std::atomic<bool> g_busy{ false };

static constexpr int TOGGLE_KEY = VK_F10;

struct WindowState
{
    WNDPROC originalProc = nullptr;
    LONG_PTR originalStyle = 0;
    LONG_PTR originalExStyle = 0;
    bool stylesModified = false;
};

static std::map<HWND, WindowState> g_windows;
static std::mutex g_windowsMutex;

static LRESULT CALLBACK HookProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_inputPassthrough)
    {
        if (msg == WM_SETCURSOR || msg == WM_MOUSEMOVE)
        {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            ClipCursor(nullptr);

            if (msg == WM_SETCURSOR)
                return TRUE;
        }
    }

    WNDPROC originalProc = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        auto it = g_windows.find(hwnd);
        if (it != g_windows.end())
            originalProc = it->second.originalProc;
    }

    if (!originalProc)
    {
        originalProc = reinterpret_cast<WNDPROC>(
            GetWindowLongPtrW(hwnd, GWLP_WNDPROC));

        if (originalProc == HookProc)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT result = CallWindowProcW(
        originalProc,
        hwnd,
        msg,
        wParam,
        lParam);

    // The important part: Magpie/overlay windows may report HTTRANSPARENT,
    // which makes mouse input fall through. Convert that result to a normal
    // client hit so ReShade can receive the mouse interaction.
    if (g_inputPassthrough &&
        msg == WM_NCHITTEST &&
        result == HTTRANSPARENT)
    {
        return HTCLIENT;
    }

    return result;
}

static bool IsOurProcessWindow(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

static void ProcessWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsOurProcessWindow(hwnd))
        return;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    // Do not touch ordinary Magpie windows. In particular, do not alter
    // Magpie's main window or renderer merely because it belongs to the same
    // process. Only windows that are actually configured as click-through or
    // non-activating are candidates for passthrough.
    const bool candidate =
        (exStyle & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) != 0;

    if (!candidate)
        return;

    WNDPROC currentProc = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(hwnd, GWLP_WNDPROC));

    if (currentProc != HookProc)
    {
        WindowState state;
        state.originalProc = currentProc;
        state.originalStyle = style;
        state.originalExStyle = exStyle;

        {
            std::lock_guard<std::mutex> lock(g_windowsMutex);
            if (g_windows.find(hwnd) == g_windows.end())
                g_windows.emplace(hwnd, state);
        }

        // Re-check that the handle still exists before changing its WndProc.
        if (!IsWindow(hwnd))
            return;

        SetWindowLongPtrW(
            hwnd,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(HookProc));
    }

    if (!g_inputPassthrough)
        return;

    LONG_PTR newExStyle = exStyle &
        ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);

    // Keep WS_EX_LAYERED intact. Magpie can rely on layered composition and
    // removing it was a likely cause of the immediate crash seen with F10.
    LONG_PTR newStyle = style & ~WS_DISABLED;

    const bool changed =
        (newExStyle != exStyle) ||
        (newStyle != style);

    if (changed)
    {
        if (!IsWindow(hwnd))
            return;

        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, newExStyle);
        SetWindowLongPtrW(hwnd, GWL_STYLE, newStyle);

        SetWindowPos(
            hwnd,
            nullptr,
            0, 0, 0, 0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_NOZORDER |
            SWP_NOACTIVATE |
            SWP_FRAMECHANGED);
    }
}

static BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM)
{
    ProcessWindow(hwnd);
    return TRUE;
}

static BOOL CALLBACK EnumTopLevelProc(HWND hwnd, LPARAM)
{
    ProcessWindow(hwnd);
    EnumChildWindows(hwnd, EnumChildProc, 0);
    return TRUE;
}

static void ProcessAllMagpieWindows()
{
    EnumWindows(EnumTopLevelProc, 0);
}

static void RestoreAllWindows()
{
    std::vector<std::pair<HWND, WindowState>> snapshot;

    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        snapshot.assign(g_windows.begin(), g_windows.end());
    }

    for (const auto &entry : snapshot)
    {
        HWND hwnd = entry.first;
        const WindowState &state = entry.second;

        if (!IsWindow(hwnd))
            continue;

        if (state.stylesModified)
        {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, state.originalExStyle);
            SetWindowLongPtrW(hwnd, GWL_STYLE, state.originalStyle);
            SetWindowPos(
                hwnd,
                nullptr,
                0, 0, 0, 0,
                SWP_NOMOVE |
                SWP_NOSIZE |
                SWP_NOZORDER |
                SWP_NOACTIVATE |
                SWP_FRAMECHANGED);
        }

        LONG_PTR currentProc = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
        if (currentProc == reinterpret_cast<LONG_PTR>(HookProc))
        {
            SetWindowLongPtrW(
                hwnd,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(state.originalProc));
        }
    }

    std::lock_guard<std::mutex> lock(g_windowsMutex);
    g_windows.clear();
}

static void CleanupDeadWindows()
{
    std::lock_guard<std::mutex> lock(g_windowsMutex);

    for (auto it = g_windows.begin(); it != g_windows.end();)
    {
        if (!IsWindow(it->first))
            it = g_windows.erase(it);
        else
            ++it;
    }
}

static void SendHome()
{
    INPUT inputs[2]{};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_HOME;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_HOME;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
}

static void EnablePassthrough()
{
    if (g_busy.exchange(true))
        return;

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: scanning click-through windows..."
    );

    // Hook only actual click-through/non-activating windows. This avoids
    // modifying Magpie's ordinary application windows.
    ProcessAllMagpieWindows();

    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        count = g_windows.size();
    }

    g_inputPassthrough = true;

    // Apply the minimal style change and WM_NCHITTEST hook.
    ProcessAllMagpieWindows();

    reshade::log::message(
        reshade::log::level::info,
        count == 0
            ? "Magpie ReShade Input: no click-through windows found; WM_NCHITTEST hook active."
            : "Magpie ReShade Input: passthrough ON (F10)."
    );

    g_busy = false;
}

static void DisablePassthrough()
{
    if (g_busy.exchange(true))
        return;

    g_inputPassthrough = false;

    SendHome();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RestoreAllWindows();

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: passthrough OFF (F10)."
    );

    g_busy = false;
}

static void Worker()
{
    bool previousKeyState = false;
    int cleanupCounter = 0;

    while (g_running)
    {
        const bool keyState =
            (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;

        if (keyState && !previousKeyState)
        {
            if (g_inputPassthrough)
                DisablePassthrough();
            else
                EnablePassthrough();
        }

        previousKeyState = keyState;

        if (g_inputPassthrough && !g_busy)
        {
            ProcessAllMagpieWindows();

            ++cleanupCounter;
            if (cleanupCounter >= 20)
            {
                CleanupDeadWindows();
                cleanupCounter = 0;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        reshade::register_addon(hModule);
        std::thread(Worker).detach();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_running = false;
        g_inputPassthrough = false;
    }

    return TRUE;
}

extern "C" __declspec(dllexport)
const char *NAME = "Magpie ReShade Input Passthrough";

extern "C" __declspec(dllexport)
const char *DESCRIPTION =
    "Allows ReShade overlay input passthrough to Magpie.";
