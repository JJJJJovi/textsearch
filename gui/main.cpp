// gui/main.cpp — WinMain 入口 + 消息循环（T6 主流程整合）
// 主线程仅处理 Win32 消息，不跑搜索；worker 线程跑 Scanner，通过 PostMessage 回传。
#include "gui/main_window.h"
#include "core/logger.h"
#include "core/config.h"
#include "core/i18n.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>  // CoInitializeEx / CoUninitialize

// 启用 DPI 感知（R12）：优先 Per-Monitor V2，回退 System DPI Aware。
// 全部用动态加载，避免 MinGW 头文件缺声明。
static void enable_dpi_awareness() {
    // 动态尝试 SetProcessDpiAwarenessContext（Win10 1703+）
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetCtxProc = BOOL(WINAPI*)(HANDLE);
        auto pSetCtx = reinterpret_cast<SetCtxProc>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (pSetCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((DPI_CONTEXT_HANDLE)-4)
            HANDLE v2 = reinterpret_cast<HANDLE>(-4);
            if (pSetCtx(v2)) return;
            // 回退 PER_MONITOR_AWARE = ((DPI_CONTEXT_HANDLE)-3)
            HANDLE v1 = reinterpret_cast<HANDLE>(-3);
            if (pSetCtx(v1)) return;
        }
    }
    // 回退 SetProcessDpiAwareness（Win8.1+，shcore.dll）
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using SetDpiProc = HRESULT(WINAPI*)(int);
        auto pSetDpi = reinterpret_cast<SetDpiProc>(
            reinterpret_cast<void*>(GetProcAddress(shcore, "SetProcessDpiAwareness")));
        if (pSetDpi) {
            // PROCESS_PER_MONITOR_DPI_AWARE = 2
            if (SUCCEEDED(pSetDpi(2))) {
                return;
            }
        }
    }
    // 最终回退 SetProcessDPIAware（Vista+，user32）
    SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    // R12：DPI 感知（必须在创建任何窗口前调用）
    enable_dpi_awareness();

    // i18n：语言在配置里（跟随系统/zh/en），先于任何界面创建
    i18n::set_language_from(load_config().ui_lang);

    // COM 初始化（IFileDialog 浏览目录需要）
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 初始化日志（F.2.8）
    logger::init();

    // 创建主窗口
    MainWindow win;
    HWND hwnd = win.create(hInstance, nCmdShow);
    if (!hwnd) {
        MessageBoxW(nullptr, i18n::tr("msg.create_fail"), i18n::tr("err"),
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    // 消息循环（含加速键 F.1.3）
    // 不使用 IsDialogMessage：避免其吞掉 Esc/Enter，与快捷键及列表 Enter 冲突。
    HACCEL hAccel = win.accel_table();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (hAccel && TranslateAcceleratorW(hwnd, hAccel, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
