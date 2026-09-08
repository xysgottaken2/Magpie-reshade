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
// The previous implementation assumed that Magpie_Renderer was a top-level
// window. In practice Magpie registers that class, but the actual renderer
// window may be a child/owned window. This implementation follows the same
// general approach as LSP-ReShade: enumerate every window belonging to the
// current Magpie process, including child windows, subclass them, and remove
// the styles/WM_NCHITTEST behavior that causes click-through.

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

    // Magpie/overlay windows can deliberately return HTTRANSPARENT to make
    // mouse input pass through to the application underneath. ReShade needs
    // the overlay window to receive that input instead.
    if (g_inputPassthrough &&
        msg == WM_NCHITTEST &&
        result == HTTRANSPARENT)
    {
        return HTCLIENT;
    }

    return result;
}

static DWORD GetCurrentProcessIdValue()
{
    return GetCurrentProcessId();
}

static bool IsOurProcessWindow(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessIdValue();
}

static void ProcessWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsOurProcessWindow(hwnd))
        return;

    WNDPROC currentProc = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(hwnd, GWLP_WNDPROC));

    if (currentProc != HookProc)
    {
        WindowState state;
        state.originalProc = currentProc;
        state.originalStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        state.originalExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

        {
            std::lock_guard<std::mutex> lock(g_windowsMutex);

            // Do not overwrite the original WndProc if we already hooked it.
            auto it = g_windows.find(hwnd);
            if (it == g_windows.end())
                g_windows.emplace(hwnd, state);
        }

        SetWindowLongPtrW(
            hwnd,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(HookProc));

        // The window could have been destroyed between enumeration and the
        // SetWindowLongPtr call. That is harmless; IsWindow is checked again
        // when restoring.
    }

    if (!g_inputPassthrough)
        return;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    bool firstActivation = false;
    LONG_PTR originalExStyle = 0;

    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);

        auto it = g_windows.find(hwnd);
        if (it == g_windows.end())
            return;

        WindowState &state = it->second;

        if (!state.stylesModified)
        {
            state.originalStyle = style;
            state.originalExStyle = exStyle;
            state.stylesModified = true;
            firstActivation = true;
        }

        originalExStyle = state.originalExStyle;
    }

    LONG_PTR newExStyle = exStyle &
        ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED);

    LONG_PTR newStyle = style & ~WS_DISABLED;

    bool changed =
        (newExStyle != exStyle) ||
        (newStyle != style);

    if (changed)
    {
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

    // Only force an overlay-style window to the foreground. This avoids
    // constantly stealing focus from the game while still making the actual
    // transparent/overlay window interactive.
    bool isOverlay =
        (originalExStyle &
            (WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE)) != 0;

    if ((changed || firstActivation) && isOverlay)
    {
        SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_FRAMECHANGED |
            SWP_NOACTIVATE);
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

    // EnumChildWindows enumerates all descendants of this window.
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
            SetWindowLongPtrW(
                hwnd,
                GWL_EXSTYLE,
                state.originalExStyle);

            SetWindowLongPtrW(
                hwnd,
                GWL_STYLE,
                state.originalStyle);

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

static void ActivateForegroundWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    DWORD currentThread = GetCurrentThreadId();
    DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);
    HWND foreground = GetForegroundWindow();
    DWORD foregroundThread =
        foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;

    if (foregroundThread && foregroundThread != currentThread)
        AttachThreadInput(currentThread, foregroundThread, TRUE);

    if (targetThread && targetThread != currentThread)
        AttachThreadInput(currentThread, targetThread, TRUE);

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (targetThread && targetThread != currentThread)
        AttachThreadInput(currentThread, targetThread, FALSE);

    if (foregroundThread && foregroundThread != currentThread)
        AttachThreadInput(currentThread, foregroundThread, FALSE);
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
        "Magpie ReShade Input: scanning Magpie windows..."
    );

    // Hook all current Magpie windows, including children. This is the key
    // difference from the old FindWindowW("Magpie_Renderer", ...) approach.
    ProcessAllMagpieWindows();

    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        count = g_windows.size();
    }

    if (count == 0)
    {
        reshade::log::message(
            reshade::log::level::warning,
            "Magpie ReShade Input: no Magpie windows found."
        );

        g_busy = false;
        return;
    }

    g_inputPassthrough = true;

    // Apply passthrough styles immediately to the windows we just found.
    ProcessAllMagpieWindows();

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: passthrough ON (F10)."
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
        bool keyState =
            (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;

        if (keyState && !previousKeyState)
        {
            if (g_inputPassthrough)
                DisablePassthrough();
            else
                EnablePassthrough();
        }

        previousKeyState = keyState;

        // Magpie can create/recreate renderer/child windows while scaling is
        // running. Periodically catch those new windows while passthrough is
        // active.
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

        std::this_thread::sleep_for(
            std::chrono::milliseconds(25));
    }
}

BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD reason,
    LPVOID)
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
