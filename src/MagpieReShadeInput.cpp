#include <windows.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include <cwchar>
#include <cstdio>
#include "reshade.hpp"

// Magpie/ReShade input passthrough.
// Locate the exact Magpie renderer window, including child windows, and hook
// only that window. Avoid modifying unrelated Magpie windows.

static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_inputPassthrough{ false };
static std::atomic<bool> g_busy{ false };
static std::atomic<HWND> g_renderer{ nullptr };

static constexpr int TOGGLE_KEY = VK_F10;
static constexpr wchar_t RENDERER_CLASS[] = L"Magpie_Renderer";

struct WindowState
{
    WNDPROC originalProc = nullptr;
    LONG_PTR originalStyle = 0;
    LONG_PTR originalExStyle = 0;
    bool stylesModified = false;
};

static std::map<HWND, WindowState> g_windows;
static std::mutex g_windowsMutex;

static bool IsOurProcessWindow(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

struct SearchContext
{
    HWND found = nullptr;
};

static BOOL CALLBACK FindRendererChildProc(HWND hwnd, LPARAM lParam)
{
    auto *ctx = reinterpret_cast<SearchContext *>(lParam);
    if (!ctx || !IsOurProcessWindow(hwnd))
        return TRUE;

    wchar_t className[256]{};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) > 0 &&
        wcscmp(className, RENDERER_CLASS) == 0)
    {
        ctx->found = hwnd;
        return FALSE;
    }

    return TRUE;
}

static BOOL CALLBACK FindRendererTopProc(HWND hwnd, LPARAM lParam)
{
    auto *ctx = reinterpret_cast<SearchContext *>(lParam);
    if (!ctx || !IsOurProcessWindow(hwnd))
        return TRUE;

    wchar_t className[256]{};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) > 0 &&
        wcscmp(className, RENDERER_CLASS) == 0)
    {
        ctx->found = hwnd;
        return FALSE;
    }

    EnumChildWindows(hwnd, FindRendererChildProc, lParam);
    return ctx->found ? FALSE : TRUE;
}

static HWND FindRendererRecursive()
{
    SearchContext context{};
    EnumWindows(FindRendererTopProc, reinterpret_cast<LPARAM>(&context));
    return context.found;
}

static LRESULT CALLBACK HookProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
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

    // Do not force a Windows cursor here. ReShade draws its own ImGui cursor
    // when the overlay is open; forcing IDC_ARROW caused cursor conflicts.
    if (g_inputPassthrough && msg == WM_MOUSEMOVE)
        ClipCursor(nullptr);

    LRESULT result = CallWindowProcW(
        originalProc,
        hwnd,
        msg,
        wParam,
        lParam);

    // Convert a deliberate transparent hit-test into a client hit-test while
    // passthrough is active so mouse interaction can reach ReShade.
    if (g_inputPassthrough &&
        msg == WM_NCHITTEST &&
        result == HTTRANSPARENT)
    {
        return HTCLIENT;
    }

    return result;
}

static void LogWindowInfo(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    HWND parent = GetParent(hwnd);

    char message[256]{};
    sprintf_s(
        message,
        "Magpie ReShade Input: renderer=%p parent=%p style=0x%llX exstyle=0x%llX",
        static_cast<void *>(hwnd),
        static_cast<void *>(parent),
        static_cast<unsigned long long>(style),
        static_cast<unsigned long long>(exStyle));

    reshade::log::message(reshade::log::level::info, message);
}

static bool HookRenderer(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsOurProcessWindow(hwnd))
        return false;

    WNDPROC currentProc = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(hwnd, GWLP_WNDPROC));

    if (currentProc == HookProc)
    {
        g_renderer = hwnd;
        return true;
    }

    WindowState state;
    state.originalProc = currentProc;
    state.originalStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    state.originalExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        g_windows[hwnd] = state;
    }

    if (!IsWindow(hwnd))
        return false;

    SetWindowLongPtrW(
        hwnd,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(HookProc));

    // Only remove click-through/non-activating bits if present. Never touch
    // WS_EX_LAYERED, since Magpie's compositor may depend on it.
    LONG_PTR newExStyle = state.originalExStyle &
        ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);

    if (newExStyle != state.originalExStyle)
    {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, newExStyle);
        SetWindowPos(
            hwnd,
            nullptr,
            0, 0, 0, 0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_NOZORDER |
            SWP_NOACTIVATE |
            SWP_FRAMECHANGED);

        std::lock_guard<std::mutex> lock(g_windowsMutex);
        auto it = g_windows.find(hwnd);
        if (it != g_windows.end())
            it->second.stylesModified = true;
    }

    g_renderer = hwnd;
    return true;
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

    g_renderer = nullptr;

    std::lock_guard<std::mutex> lock(g_windowsMutex);
    g_windows.clear();
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

static void FocusRenderer(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root)
        root = hwnd;

    BringWindowToTop(root);
    SetForegroundWindow(root);

    // SetFocus only works directly when the caller and target share a GUI
    // thread. Do not use AttachThreadInput here; the renderer can be owned by
    // another thread and forcing input attachment can destabilize Magpie.
    if (GetWindowThreadProcessId(root, nullptr) == GetCurrentThreadId())
        SetFocus(hwnd);
}

static void EnablePassthrough()
{
    if (g_busy.exchange(true))
        return;

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: locating Magpie_Renderer..."
    );

    HWND renderer = FindRendererRecursive();

    if (!renderer)
    {
        reshade::log::message(
            reshade::log::level::warning,
            "Magpie ReShade Input: Magpie_Renderer not found."
        );
        g_busy = false;
        return;
    }

    LogWindowInfo(renderer);

    if (!HookRenderer(renderer))
    {
        reshade::log::message(
            reshade::log::level::warning,
            "Magpie ReShade Input: failed to hook Magpie_Renderer."
        );
        g_busy = false;
        return;
    }

    g_inputPassthrough = true;

    // Focus the renderer before sending Home so ReShade gets its overlay
    // hotkey rather than the source application.
    FocusRenderer(renderer);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    SendHome();

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: passthrough ON (F10), renderer focused and Home sent."
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
