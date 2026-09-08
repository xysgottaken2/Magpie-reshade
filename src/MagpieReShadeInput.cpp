#include <windows.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include "reshade.hpp"

static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_enabled{ false };
static std::atomic<bool> g_busy{ false };

static HWND g_magpie = nullptr;
static HWND g_previous_foreground = nullptr;

static LONG_PTR g_saved_exstyle = 0;
static LONG_PTR g_saved_style = 0;

static constexpr int TOGGLE_KEY = VK_F10;

// Find Magpie renderer window directly by its window class.
static HWND FindMagpieRenderer()
{
    HWND hwnd = FindWindowW(L"Magpie_Renderer", nullptr);

    if (hwnd && IsWindow(hwnd))
        return hwnd;

    return nullptr;
}

// Find the window behind Magpie in the Z-order.
static HWND FindWindowBehind(HWND renderer)
{
    if (!renderer)
        return nullptr;

    HWND hwnd = GetWindow(renderer, GW_HWNDNEXT);

    while (hwnd)
    {
        if (IsWindow(hwnd) &&
            IsWindowVisible(hwnd) &&
            IsWindowEnabled(hwnd))
        {
            wchar_t class_name[256]{};
            GetClassNameW(hwnd, class_name, 256);

            if (wcscmp(class_name, L"Progman") != 0 &&
                wcscmp(class_name, L"WorkerW") != 0 &&
                wcscmp(class_name, L"Shell_TrayWnd") != 0)
            {
                DWORD renderer_pid = 0;
                DWORD window_pid = 0;

                GetWindowThreadProcessId(renderer, &renderer_pid);
                GetWindowThreadProcessId(hwnd, &window_pid);

                if (window_pid != renderer_pid)
                    return hwnd;
            }
        }

        hwnd = GetWindow(hwnd, GW_HWNDNEXT);
    }

    return nullptr;
}

static void ActivateWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    DWORD current_thread = GetCurrentThreadId();
    DWORD target_thread = GetWindowThreadProcessId(hwnd, nullptr);
    DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);

    if (foreground_thread != current_thread)
        AttachThreadInput(current_thread, foreground_thread, TRUE);

    if (target_thread != current_thread)
        AttachThreadInput(current_thread, target_thread, TRUE);

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (target_thread != current_thread)
        AttachThreadInput(current_thread, target_thread, FALSE);

    if (foreground_thread != current_thread)
        AttachThreadInput(current_thread, foreground_thread, FALSE);
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

    HWND renderer = FindMagpieRenderer();

    if (!renderer)
    {
        reshade::log::message(
            reshade::log::level::warning,
            "Magpie ReShade Input: Magpie_Renderer not found."
        );

        g_busy = false;
        return;
    }

    g_magpie = renderer;
    g_previous_foreground = GetForegroundWindow();

    g_saved_exstyle = GetWindowLongPtrW(renderer, GWL_EXSTYLE);
    g_saved_style = GetWindowLongPtrW(renderer, GWL_STYLE);

    LONG_PTR exstyle = g_saved_exstyle;

    exstyle &= ~WS_EX_NOACTIVATE;
    exstyle &= ~WS_EX_TRANSPARENT;

    SetWindowLongPtrW(renderer, GWL_EXSTYLE, exstyle);

    SetWindowPos(
        renderer,
        HWND_TOP,
        0, 0, 0, 0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_NOOWNERZORDER |
        SWP_FRAMECHANGED
    );

    ActivateWindow(renderer);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    SendHome();

    g_enabled = true;

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

    if (!g_magpie || !IsWindow(g_magpie))
    {
        g_enabled = false;
        g_busy = false;
        return;
    }

    SendHome();

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    SetWindowLongPtrW(
        g_magpie,
        GWL_EXSTYLE,
        g_saved_exstyle
    );

    SetWindowLongPtrW(
        g_magpie,
        GWL_STYLE,
        g_saved_style
    );

    SetWindowPos(
        g_magpie,
        nullptr,
        0, 0, 0, 0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_NOZORDER |
        SWP_NOOWNERZORDER |
        SWP_FRAMECHANGED
    );

    HWND behind = FindWindowBehind(g_magpie);

    if (behind)
        ActivateWindow(behind);
    else if (g_previous_foreground &&
             IsWindow(g_previous_foreground))
        ActivateWindow(g_previous_foreground);

    g_enabled = false;

    reshade::log::message(
        reshade::log::level::info,
        "Magpie ReShade Input: passthrough OFF (F10)."
    );

    g_magpie = nullptr;
    g_previous_foreground = nullptr;

    g_busy = false;
}

static void Worker()
{
    bool previous_key_state = false;

    while (g_running)
    {
        bool key_state =
            (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;

        if (key_state && !previous_key_state)
        {
            if (g_enabled)
                DisablePassthrough();
            else
                EnablePassthrough();
        }

        previous_key_state = key_state;

        std::this_thread::sleep_for(
            std::chrono::milliseconds(10)
        );
    }
}

BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD reason,
    LPVOID
)
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
    }

    return TRUE;
}

extern "C" __declspec(dllexport)
const char *NAME = "Magpie ReShade Input Passthrough";

extern "C" __declspec(dllexport)
const char *DESCRIPTION =
    "Allows ReShade overlay input passthrough to Magpie.";
