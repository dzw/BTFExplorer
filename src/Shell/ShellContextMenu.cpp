#include "ShellContextMenu.h"
#include "ShellUtil.h"
#include <shlobj.h>

namespace shell {

static constexpr UINT CMD_FIRST = 1;
static constexpr UINT CMD_LAST  = 0x7FFF;

bool ShowContextMenu(HWND hwnd, const std::wstring& path, const std::wstring& menuDir,
                     POINT ptScreen)
{
    UniquePIDL pidl;
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;

    if (!path.empty()) {
        pidl = PIDLFromPath(path);
        if (!pidl) return false;
        IShellFolder* pParent = nullptr;
        if (FAILED(SHBindToParent(reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl.get()), IID_IShellFolder,
                                  reinterpret_cast<void**>(&pParent), &child)))
            return false;
        parent.Attach(pParent);
    } else {
        std::wstring dir = menuDir.empty() ? L"C:\\" : menuDir;
        pidl = PIDLFromPath(dir);
        if (!pidl) return false;
        IShellFolder* pFolder = nullptr;
        if (FAILED(SHBindToObject(nullptr, reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl.get()), nullptr, IID_IShellFolder,
                                  reinterpret_cast<void**>(&pFolder))))
            return false;
        parent.Attach(pFolder);
    }

    ComPtr<IContextMenu> cm;
    HRESULT hr;
    if (child) {
        PCUITEMID_CHILD_ARRAY one = &child;
        hr = parent->GetUIObjectOf(hwnd, 1, one, IID_IContextMenu, nullptr,
                                   reinterpret_cast<void**>(cm.operator&()));
    } else {
        hr = parent->CreateViewObject(hwnd, IID_IContextMenu,
                                      reinterpret_cast<void**>(cm.operator&()));
    }
    if (FAILED(hr) || !cm) return false;

    HMENU menu = CreatePopupMenu();
    if (!menu) return false;
    bool invoked = false;
    if (SUCCEEDED(cm->QueryContextMenu(menu, 0, CMD_FIRST, CMD_LAST, CMF_NORMAL))) {
        UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    ptScreen.x, ptScreen.y, hwnd, nullptr);
        if (cmd >= CMD_FIRST && cmd <= CMD_LAST) {
            CMINVOKECOMMANDINFOEX info = { sizeof(info) };
            info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            info.hwnd = hwnd;
            info.lpVerb  = reinterpret_cast<LPCSTR>(MAKEINTRESOURCEA(cmd - CMD_FIRST));
            info.lpVerbW = reinterpret_cast<LPCWSTR>(MAKEINTRESOURCEW(cmd - CMD_FIRST));
            info.nShow = SW_SHOWNORMAL;
            info.ptInvoke = ptScreen;
            invoked = SUCCEEDED(cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)));
        }
    }
    DestroyMenu(menu);
    return invoked;
}

} // namespace shell
