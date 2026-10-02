#include "ShellContextMenu.h"
#include "ShellUtil.h"
#include <shlobj.h>
#include <cwctype>

namespace shell {

static constexpr UINT CMD_FIRST = 1;
static constexpr UINT CMD_LAST  = 0x7FFF;
static constexpr UINT CMD_CUSTOM = CMD_LAST + 1;

static bool IsHiddenProviderItem(const std::wstring& text)
{
    // static const wchar_t* const hiddenNames[] = {
    //     L"snagit", L"techsmith", L"tortoisesvn",
    //     L"百度云", L"百度网盘", L"baidu",
    //     L"腾讯云", L"腾讯微云", L"微云", L"tencent", L"weiyun"
    // };
    // std::wstring normalized = text;
    // for (wchar_t& ch : normalized)
    //     ch = static_cast<wchar_t>(std::towlower(ch));
    // for (const wchar_t* name : hiddenNames) {
    //     if (normalized.find(name) != std::wstring::npos)
    //         return true;
    // }
    return false;
}

static bool FilterProviderItems(HMENU menu)
{
    bool removed = false;
    for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i) {
        wchar_t text[512]{};
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_STRING | MIIM_SUBMENU;
        item.dwTypeData = text;
        item.cch = static_cast<UINT>(std::size(text));
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item))
            continue;

        if (IsHiddenProviderItem(text)) {
            DeleteMenu(menu, static_cast<UINT>(i), MF_BYPOSITION);
            removed = true;
        } else if (item.hSubMenu && FilterProviderItems(item.hSubMenu)) {
            if (GetMenuItemCount(item.hSubMenu) == 0)
                DeleteMenu(menu, static_cast<UINT>(i), MF_BYPOSITION);
            removed = true;
        }
    }
    return removed;
}

bool ShowContextMenu(HWND hwnd, const std::wstring& path, const std::wstring& menuDir,
                     POINT ptScreen, const std::wstring& customItem,
                     bool& customItemSelected)
{
    customItemSelected = false;
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
        FilterProviderItems(menu);
        if (!customItem.empty()) {
            if (GetMenuItemCount(menu) > 0)
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, CMD_CUSTOM, customItem.c_str());
        }
        UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    ptScreen.x, ptScreen.y, hwnd, nullptr);
        if (cmd == CMD_CUSTOM) {
            customItemSelected = true;
        } else if (cmd >= CMD_FIRST && cmd <= CMD_LAST) {
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
