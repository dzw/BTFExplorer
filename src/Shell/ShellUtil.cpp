#include "ShellUtil.h"
#include "../FileModel/FileEntry.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <objidl.h>     // IDataObject / FORMATETC / STGMEDIUM
#include <ole2.h>       // OleGetClipboard / ReleaseStgMedium
#include <mutex>
#include <unordered_map>
#include <cwctype>

namespace shell {

UniquePIDL PIDLFromPath(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF flags = 0;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, &flags)) && pidl)
        return UniquePIDL(pidl);
    return {};
}

std::wstring PathFromPIDL(PCIDLIST_ABSOLUTE pidl) {
    wchar_t buf[MAX_PATH * 2] = {};
    if (pidl && SHGetPathFromIDListW(pidl, buf))
        return buf;
    return {};
}

std::wstring DisplayNameFromPIDL(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return {};
    STRRET sr{};
    ComPtr<IShellFolder> desktop;
    if (FAILED(SHGetDesktopFolder(&desktop))) return {};
    if (SUCCEEDED(desktop->GetDisplayNameOf(pidl, SHGDN_INFOLDER, &sr))) {
        wchar_t buf[MAX_PATH * 2] = {};
        if (SUCCEEDED(StrRetToBufW(&sr, pidl, buf, MAX_PATH * 2)))
            return buf;
    }
    return {};
}

HICON GetIconForEntry(const std::wstring& path, bool isFolder) {
    SHFILEINFOW fi{};
    DWORD flags = SHGFI_ICON | SHGFI_SMALLICON;
    if (path.empty() || SHGetFileInfoW(path.c_str(), isFolder ? FILE_ATTRIBUTE_DIRECTORY : 0, &fi, sizeof(fi), flags))
        return fi.hIcon;
    return nullptr;
}

std::wstring TypeNameForEntry(const std::wstring& path, bool isFolder) {
    SHFILEINFOW fi{};
    DWORD flags = SHGFI_TYPENAME;
    if (path.empty() || SHGetFileInfoW(path.c_str(), isFolder ? FILE_ATTRIBUTE_DIRECTORY : 0, &fi, sizeof(fi), flags))
        return fi.szTypeName;
    return {};
}

int SysIconIndexForEntry(const std::wstring& path, bool isFolder) {
    SHFILEINFOW fi{};
    if (!path.empty() && SHGetFileInfoW(path.c_str(), isFolder ? FILE_ATTRIBUTE_DIRECTORY : 0,
                                         &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
        return fi.iIcon;
    return -1;
}

namespace {

// 取文件名部分的扩展名（含点、小写）；无扩展名返回空串。
// 不用 PathFindExtensionW：它对"目录名带点"的路径会取错位置。
std::wstring ExtensionKeyOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return {};
    std::wstring key = path.substr(dot);
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
    return key;
}

struct DisplayNameCache {
    std::mutex mtx;
    std::unordered_map<std::wstring, std::wstring> typeByExt;  // 扩展名 -> 类型名
    std::unordered_map<std::wstring, int> iconByExt;           // 扩展名 -> 图标索引
    std::wstring folderTypeName;                               // 文件夹类型名，只查一次
};
DisplayNameCache& Cache() {
    static DisplayNameCache c;
    return c;
}

// 这些类型的图标来自文件内容/目标（每文件不同），按扩展名缓存会张冠李戴
bool IconIsPerFile(const std::wstring& ext) {
    static const wchar_t* kPerFileExts[] = {
        L".exe", L".lnk", L".ico", L".cur", L".ani", L".cpl", L".scr", L".msi",
    };
    for (const wchar_t* e : kPerFileExts)
        if (ext == e) return true;
    return false;
}

} // namespace

std::wstring CachedTypeNameForEntry(const std::wstring& path, bool isFolder) {
    if (path.empty()) return {};
    DisplayNameCache& c = Cache();
    if (isFolder) {
        std::lock_guard<std::mutex> lk(c.mtx);
        if (c.folderTypeName.empty())
            c.folderTypeName = TypeNameForEntry(path, true);
        return c.folderTypeName;
    }
    std::wstring key = ExtensionKeyOf(path);
    {
        std::lock_guard<std::mutex> lk(c.mtx);
        auto it = c.typeByExt.find(key);
        if (it != c.typeByExt.end()) return it->second;
    }
    std::wstring name = TypeNameForEntry(path, false);
    if (name.empty()) return {}; // 查不到的（文件刚被删等）不入缓存，避免空结果被永久记住
    std::lock_guard<std::mutex> lk(c.mtx);
    // 并发下另一线程可能已先插入同 key，emplace 返回已存在的那份即可
    auto it = c.typeByExt.emplace(std::move(key), std::move(name)).first;
    return it->second;
}

// ---------------------------------------------------------------------------
// 剪贴板：文件（CF_HDROP）
// ---------------------------------------------------------------------------

namespace {

// 从 CF_HDROP 的 HGLOBAL 里读出路径列表
std::vector<std::wstring> PathsFromDrop(HGLOBAL h)
{
    std::vector<std::wstring> out;
    auto* drop = static_cast<HDROP>(GlobalLock(h));
    if (!drop) return out;
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);   // 不含结尾 '\0'
        std::wstring p(len, L'\0');
        if (DragQueryFileW(drop, i, p.data(), len + 1) > 0)
            out.push_back(std::move(p));
    }
    GlobalUnlock(h);
    return out;
}

} // namespace

// 一律走 OLE：OleGetClipboard 会把“裸剪贴板格式”也包成数据对象，所以本程序
// SetFileClipboard 放的 CF_HDROP 和资源管理器复制的都能读到；反过来只用
// IsClipboardFormatAvailable(CF_HDROP) 的话，OleSetClipboard 放的 IDataObject
// （shell 视图自己的复制走这条）会被判成“剪贴板里没有文件”。
bool ClipboardHasFiles()
{
    ComPtr<IDataObject> dto;
    if (FAILED(OleGetClipboard(&dto)) || !dto)
        return IsClipboardFormatAvailable(CF_HDROP) != FALSE;   // OLE 不可用时退回裸格式
    FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    return dto->QueryGetData(&fe) == S_OK;
}

bool GetClipboardFiles(std::vector<std::wstring>& paths, bool& move)
{
    paths.clear();
    move = false;
    ComPtr<IDataObject> dto;
    if (SUCCEEDED(OleGetClipboard(&dto)) && dto) {
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg{};
        if (SUCCEEDED(dto->GetData(&fe, &stg)) && stg.hGlobal) {
            paths = PathsFromDrop(stg.hGlobal);
            ReleaseStgMedium(&stg);
        }
        const CLIPFORMAT cf =
            static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        FORMATETC fe2{ cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg2{};
        if (SUCCEEDED(dto->GetData(&fe2, &stg2)) && stg2.hGlobal) {
            if (auto* effect = static_cast<DWORD*>(GlobalLock(stg2.hGlobal))) {
                if (*effect & DROPEFFECT_MOVE) move = true;
                GlobalUnlock(stg2.hGlobal);
            }
            ReleaseStgMedium(&stg2);
        }
        if (!paths.empty()) return true;
    }
    // 兜底：OLE 没拿到就按原来的裸剪贴板读（本程序 SetFileClipboard 放的就是裸格式）
    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    if (!OpenClipboard(nullptr)) return false;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        paths = PathsFromDrop(static_cast<HGLOBAL>(h));
        const UINT pe = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        if (HANDLE he = pe ? GetClipboardData(pe) : nullptr) {
            if (auto* effect = static_cast<DWORD*>(GlobalLock(he))) {
                if (*effect & DROPEFFECT_MOVE) move = true;
                GlobalUnlock(he);
            }
        }
    }
    CloseClipboard();
    return !paths.empty();
}

int CachedSysIconIndexForEntry(const std::wstring& path, bool isFolder) {
    if (path.empty()) return -1;
    // 文件夹图标可能被 desktop.ini 逐目录定制，不按扩展名缓存
    if (isFolder) return SysIconIndexForEntry(path, true);
    std::wstring key = ExtensionKeyOf(path);
    if (IconIsPerFile(key)) return SysIconIndexForEntry(path, false);
    DisplayNameCache& c = Cache();
    {
        std::lock_guard<std::mutex> lk(c.mtx);
        auto it = c.iconByExt.find(key);
        if (it != c.iconByExt.end()) return it->second;
    }
    int idx = SysIconIndexForEntry(path, false);
    if (idx < 0) return -1; // 同上：失败不缓存
    std::lock_guard<std::mutex> lk(c.mtx);
    c.iconByExt.emplace(std::move(key), idx);
    return idx;
}

} // namespace shell
