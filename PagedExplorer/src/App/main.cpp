#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include "../UI/MainWindow.h"

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
    // 单实例：若 PagedExplorer 已在运行，把参数转发给已有实例并激活后退出
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"PagedExplorer_SingleInstance");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        std::wstring target = lpCmdLine ? lpCmdLine : L"";
        // 去掉引号
        if (!target.empty() && target.front() == L'"') target.erase(0, 1);
        if (!target.empty() && target.back() == L'"') target.pop_back();
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
    if (FAILED(hr)) { if (hMutex) CloseHandle(hMutex); return 1; }

    // 加速键：Alt+Up=上级目录，Alt+Left=后退，Alt+Right=前进
    ACCEL accel[3] = {};
    accel[0].fVirt = FVIRTKEY | FALT; accel[0].key = VK_UP;    accel[0].cmd = IDC_UP;
    accel[1].fVirt = FVIRTKEY | FALT; accel[1].key = VK_LEFT;  accel[1].cmd = IDC_BACK;
    accel[2].fVirt = FVIRTKEY | FALT; accel[2].key = VK_RIGHT; accel[2].cmd = IDC_FORWARD;
    HACCEL hAccel = CreateAcceleratorTableW(accel, 3);

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

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            // 先让加速键处理（Alt+方向键），未命中再正常分发
            if (!TranslateAcceleratorW(w->Hwnd(), hAccel, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        ret = 0;

        if (g_hHook) { UnhookWindowsHookEx(g_hHook); g_hHook = nullptr; }
        g_mainHwnd = nullptr;
    }

    if (hAccel) DestroyAcceleratorTable(hAccel);
    CoUninitialize();
    if (hMutex) CloseHandle(hMutex);
    return ret;
}
