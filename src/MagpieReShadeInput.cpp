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
// Magpie's renderer is a child window with click-through styles. We locate it
// by class/style and only modify that single renderer window.

static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_inputPassthrough{ false };
static std::atomic<bool> g_busy{ false };
static std::atomic<HWND> g_renderer{ nullptr };
static std::atomic<reshade::api::effect_runtime *> g_runtime{ nullptr };
// -1 = no request, 0 = close overlay, 1 = open overlay
static std::atomic<int> g_overlayRequest{ -1 };

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

struct SearchContext
{
    HWND found = nullptr;
};

static bool IsCurrentProcessWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;

    DWORD pid = 0;
    return GetWindowThreadProcessId(hwnd, &pid) != 0 &&
           pid == GetCurrentProcessId();
}

static bool IsRendererClass(HWND hwnd)
{
    wchar_t className[256]{};
    const int length = GetClassNameW(hwnd, className, ARRAYSIZE(className));
    return length > 0 && wcscmp(className, RENDERER_CLASS) == 0;
}

static bool IsRendererCandidate(HWND hwnd)
{
    if (!IsCurrentProcessWindow(hwnd))
        return false;

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool clickThrough = (exStyle & WS_EX_TRANSPARENT) != 0;
    const bool layered = (exStyle & WS_EX_LAYERED) != 0;

    return IsRendererClass(hwnd) || (clickThrough && layered);
}

static BOOL CALLBACK FindRendererChildProc(HWND hwnd, LPARAM lParam)
{
    auto *ctx = reinterpret_cast<SearchContext *>(lParam);
    if (!ctx)
        return TRUE;

    if (IsRendererCandidate(hwnd))
    {
        ctx->found = hwnd;
        return FALSE;
    }

    return TRUE;
}

static BOOL CALLBACK FindRendererTopProc(HWND hwnd, LPARAM lParam)
{
    auto *ctx = reinterpret_cast<SearchContext *>(lParam);
    if (!ctx)
        return TRUE;

    if (IsRendererCandidate(hwnd))
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
    if (context.found)
        return context.found;

    HWND current = nullptr;
    while ((current = FindWindowExW(HWND_MESSAGE, current, nullptr, nullptr)) != nullptr)
    {
        if (IsRendererCandidate(current))
            return current;
    }

    return nullptr;
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
        originalProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
        if (originalProc == HookProc)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (g_inputPassthrough && msg == WM_MOUSEMOVE)
        ClipCursor(nullptr);

    LRESULT result = CallWindowProcW(originalProc, hwnd, msg, wParam, lParam);

    if (g_inputPassthrough && msg == WM_NCHITTEST && result == HTTRANSPARENT)
        return HTCLIENT;

    return result;
}

static void LogWindowInfo(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    wchar_t className[256]{};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    HWND parent = GetParent(hwnd);
    DWORD pid = 0;
    const DWORD ownerPid = GetWindowThreadProcessId(hwnd, &pid) ? pid : 0;

    char message[512]{};
    sprintf_s(message,
        "Magpie ReShade Input: renderer=%p class=%ls parent=%p pid=%lu style=0x%llX exstyle=0x%llX",
        static_cast<void *>(hwnd), className, static_cast<void *>(parent),
        static_cast<unsigned long>(ownerPid),
        static_cast<unsigned long long>(style),
        static_cast<unsigned long long>(exStyle));

    reshade::log::message(reshade::log::level::info, message);
}

static bool HookRenderer(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsRendererCandidate(hwnd))
        return false;

    WNDPROC currentProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));

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

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previousProc = SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookProc));

    if (previousProc == 0 && GetLastError() != ERROR_SUCCESS)
    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        g_windows.erase(hwnd);
        return false;
    }

    // LSP-ReShade uses this same mechanism. Only the identified Magpie
    // renderer is changed; ordinary Magpie windows are left untouched.
    const LONG_PTR newExStyle = state.originalExStyle &
        ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED);

    if (newExStyle != state.originalExStyle)
    {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, newExStyle);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOACTIVATE | SWP_FRAMECHANGED);

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
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }

        if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(HookProc))
        {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(state.originalProc));
        }
    }

    g_renderer = nullptr;

    std::lock_guard<std::mutex> lock(g_windowsMutex);
    g_windows.clear();
}

static void FocusRenderer(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root)
        root = hwnd;

    HWND foreground = GetForegroundWindow();
    DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    DWORD currentThread = GetCurrentThreadId();

    if (foregroundThread && foregroundThread != currentThread)
        AttachThreadInput(foregroundThread, currentThread, TRUE);

    BringWindowToTop(root);
    SetForegroundWindow(root);
    SetFocus(hwnd);

    if (foregroundThread && foregroundThread != currentThread)
        AttachThreadInput(foregroundThread, currentThread, FALSE);
}

static void RequestOverlay(bool open)
{
    g_overlayRequest.store(open ? 1 : 0, std::memory_order_release);
}

// This runs on ReShade's render thread, so calling effect_runtime::open_overlay
// here is safe and, unlike SendInput(Home), directly invokes ReShade's own
// overlay state transition. ReShade documents this API as the supported way
// to open/close the overlay and reports the request through the
// reshade_open_overlay add-on event.
static void OnReShadePresent(reshade::api::effect_runtime *runtime)
{
    g_runtime.store(runtime, std::memory_order_release);

    const int request = g_overlayRequest.exchange(-1, std::memory_order_acq_rel);
    if (request == -1)
        return;

    const bool open = request != 0;
    const bool changed = runtime->open_overlay(
        open, reshade::api::input_source::keyboard);

    reshade::log::message(
        reshade::log::level::info,
        open
            ? (changed ? "Magpie ReShade Input: ReShade overlay opened via API."
                       : "Magpie ReShade Input: ReShade overlay open request was ignored.")
            : (changed ? "Magpie ReShade Input: ReShade overlay closed via API."
                       : "Magpie ReShade Input: ReShade overlay close request was ignored."));
}

static void OnInitEffectRuntime(reshade::api::effect_runtime *runtime)
{
    g_runtime.store(runtime, std::memory_order_release);
}

static void OnDestroyEffectRuntime(reshade::api::effect_runtime *runtime)
{
    reshade::api::effect_runtime *expected = runtime;
    g_runtime.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

static void EnablePassthrough()
{
    if (g_busy.exchange(true))
        return;

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: locating Magpie renderer by click-through styles...");

    HWND renderer = FindRendererRecursive();

    if (!renderer)
    {
        reshade::log::message(
            reshade::log::level::warning,
            "Magpie ReShade Input: no layered+transparent Magpie renderer found.");
        g_busy = false;
        return;
    }

    LogWindowInfo(renderer);

    if (!HookRenderer(renderer))
    {
        reshade::log::message(
            reshade::log::level::warning,
            "Magpie ReShade Input: failed to hook renderer.");
        g_busy = false;
        return;
    }

    g_inputPassthrough = true;
    FocusRenderer(renderer);

    // Do not synthesize Home anymore. Ask ReShade itself to open its overlay.
    RequestOverlay(true);

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: passthrough ON (F10), renderer focused and overlay open requested.");

    g_busy = false;
}

static void DisablePassthrough()
{
    if (g_busy.exchange(true))
        return;

    // Close ReShade through its API before restoring Magpie's input capture.
    RequestOverlay(false);
    g_inputPassthrough = false;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    RestoreAllWindows();

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: passthrough OFF (F10).");

    g_busy = false;
}

static void Worker()
{
    bool previousKeyState = false;

    while (g_running)
    {
        const bool keyState = (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;

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

        if (!reshade::register_addon(hModule))
            return FALSE;

        reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::register_event<reshade::addon_event::reshade_present>(OnReShadePresent);

        std::thread(Worker).detach();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_running = false;
        g_inputPassthrough = false;

        reshade::unregister_event<reshade::addon_event::reshade_present>(OnReShadePresent);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        RestoreAllWindows();
        reshade::unregister_addon(hModule);
    }

    return TRUE;
}

extern "C" __declspec(dllexport)
const char *NAME = "Magpie ReShade Input Passthrough";

extern "C" __declspec(dllexport)
const char *DESCRIPTION =
    "Allows ReShade overlay input passthrough to Magpie.";
