#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <cstdio>
#include <vector>
#include "../UI/MainWindow.h"
#include "../Util/AppLog.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// 与 MainWindow 中定义保持一致：Win+E 拦截后激活主窗口
static constexpr UINT WM_APP_WIN_E = WM_APP + 2;

// ---------------------------------------------------------------------------
// 全局低层键盘钩子：拦截 Win+E，替换为激活 PagedExplorer
// （仅首个实例安装；返回 1 表示吞掉该按键，阻止系统打开资源管理器）
// ---------------------------------------------------------------------------
static HHOOK g_hHook = nullptr;
static HWND  g_mainHwnd = nullptr;

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* ks = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        static bool lwin = false, rwin = false;
        if (ks->vkCode == VK_LWIN) lwin = (wParam != WM_KEYUP && wParam != WM_SYSKEYUP);
        if (ks->vkCode == VK_RWIN) rwin = (wParam != WM_KEYUP && wParam != WM_SYSKEYUP);
        bool winDown = lwin || rwin;
        bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                       !(ks->flags & LLKHF_UP);
        if (keyDown && winDown && ks->vkCode == 'E') {
            if (g_mainHwnd) PostMessageW(g_mainHwnd, WM_APP_WIN_E, 0, 0);
            return 1; // 抑制系统默认的 Win+E
        }
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int)
{
    std::vector<wchar_t> exePath(32768);
    DWORD exeLength = GetModuleFileNameW(nullptr, exePath.data(),
                                        static_cast<DWORD>(exePath.size()));
    WriteAppLog(exeLength > 0 && exeLength < exePath.size()
        ? (L"START exe=" + std::wstring(exePath.data(), exeLength)).c_str()
        : L"START exe path unavailable");

    // 解析命令行：取第一个参数（自己处理引号/空白，比手工去引号可靠——
    // 诸如 "D:\dir" 后跟空格的命令行，靠 back()==L'"' 判断会漏删引号）
    std::wstring target;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            if (argc > 1 && argv[1]) target = argv[1];
            LocalFree(argv);
        }
    }

    // 单实例：若 PagedExplorer 已在运行，把参数转发给已有实例并激活后退出
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"PagedExplorer_SingleInstance");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        WriteAppLog(L"SECOND_INSTANCE forwarding or activating the existing instance");
        for (int i = 0; i < 20; ++i) {            // 等待首个实例创建好窗口
            HWND h = FindWindowW(L"PagedExplorerMain", nullptr);
            if (h) {
                if (!target.empty()) {            // 转发路径，让已有实例打开
                    COPYDATASTRUCT cds{};
                    cds.dwData = 1;
                    cds.cbData = static_cast<DWORD>((target.size() + 1) * sizeof(wchar_t));
                    cds.lpData = target.data();
                    SendMessageW(h, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
                } else {
                    PostMessageW(h, WM_APP_WIN_E, 0, 0); // 仅激活
                }
                break;
            }
            Sleep(50);
        }
        CloseHandle(hMutex);
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        WriteAppLog(L"CoInitializeEx failed");
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // 加速键：焦点在列表/地址栏等子控件上时也能生效（Alt+消息不会自动转到主窗口）
    //   Alt+方向键导航，Alt+1~4 切窗格数量，Alt+小键盘8/2 切品字形态
    // 小键盘 NumLock 关闭时 8/2 会被上报成 VK_UP/VK_DOWN：
    // 8 与 Alt+Up（上级目录）冲突，保持导航；2 无冲突，额外绑 Alt+Down 兜底
    ACCEL accel[10] = {};
    accel[0].fVirt = FVIRTKEY | FALT; accel[0].key = VK_UP;      accel[0].cmd = IDC_UP;
    accel[1].fVirt = FVIRTKEY | FALT; accel[1].key = VK_LEFT;    accel[1].cmd = IDC_BACK;
    accel[2].fVirt = FVIRTKEY | FALT; accel[2].key = VK_RIGHT;   accel[2].cmd = IDC_FORWARD;
    accel[3].fVirt = FVIRTKEY | FALT; accel[3].key = '1';        accel[3].cmd = IDC_LAYOUT1;
    accel[4].fVirt = FVIRTKEY | FALT; accel[4].key = '2';        accel[4].cmd = IDC_LAYOUT2;
    accel[5].fVirt = FVIRTKEY | FALT; accel[5].key = '3';        accel[5].cmd = IDC_LAYOUT3;
    accel[6].fVirt = FVIRTKEY | FALT; accel[6].key = '4';        accel[6].cmd = IDC_LAYOUT4;
    accel[7].fVirt = FVIRTKEY | FALT; accel[7].key = VK_NUMPAD8; accel[7].cmd = IDC_TRI_PINTOP;
    accel[8].fVirt = FVIRTKEY | FALT; accel[8].key = VK_NUMPAD2; accel[8].cmd = IDC_TRI_PINDOWN;
    accel[9].fVirt = FVIRTKEY | FALT; accel[9].key = VK_DOWN;    accel[9].cmd = IDC_TRI_PINDOWN;
    HACCEL hAccel = CreateAcceleratorTableW(accel, 10);

    int ret = 1;
    if (MainWindow* w = MainWindow::Create(hInst)) {
        g_mainHwnd = w->Hwnd();

        // 首次启动带参数：打开传入的目录或文件
        if (lpCmdLine && *lpCmdLine) {
            std::wstring target = lpCmdLine;
            if (!target.empty() && target.front() == L'"') target.erase(0, 1);
            if (!target.empty() && target.back() == L'"') target.pop_back();
            if (!target.empty()) w->OpenTarget(target);
        }

        // 安装全局低层键盘钩子，拦截 Win+E 替换系统资源管理器
        g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
        WriteAppLog(g_hHook ? L"KEYBOARD_HOOK installed" : L"KEYBOARD_HOOK installation failed");

        MSG msg;
        BOOL messageResult = 0;
        while ((messageResult = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            // 先让加速键处理（Alt+方向键），未命中再正常分发
            if (!TranslateAcceleratorW(w->Hwnd(), hAccel, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (messageResult == 0) {
            WriteAppLog(L"MESSAGE_LOOP received WM_QUIT");
        } else {
            WriteAppLog(L"MESSAGE_LOOP GetMessage failed");
        }
        ret = 0;

        if (g_hHook) { UnhookWindowsHookEx(g_hHook); g_hHook = nullptr; }
        g_mainHwnd = nullptr;
    } else {
        WriteAppLog(L"STARTUP_FAILED MainWindow::Create returned null");
    }

    if (hAccel) DestroyAcceleratorTable(hAccel);
    CoUninitialize();
    if (hMutex) CloseHandle(hMutex);
    return ret;
}
