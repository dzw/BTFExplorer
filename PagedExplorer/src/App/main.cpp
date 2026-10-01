#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include "../UI/MainWindow.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    int ret = 1;
    if (MainWindow* w = MainWindow::Create(hInst)) {
        (void)w;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ret = 0;
    }
    CoUninitialize();
    return ret;
}
