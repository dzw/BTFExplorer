#include "ShellFileOps.h"
#include "ShellUtil.h"
#include "../Util/AppLog.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winioctl.h>
#include <string>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace shell {

namespace {

void ReportError(HWND owner, const wchar_t* title, const std::wstring& path, DWORD error)
{
    WriteAppLog((std::wstring(L"FILEOP ") + title + L" failed: path=" + path +
                 L", win32Error=" + std::to_wstring(error)).c_str());
    std::wstring message = std::wstring(L"无法完成：\n") + path +
        L"\n错误码：" + std::to_wstring(error);
    MessageBoxW(owner, message.c_str(), title, MB_OK | MB_ICONERROR);
}

std::wstring ParentOf(const std::wstring& path)
{
    size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash);
}

std::wstring NameOf(const std::wstring& path)
{
    return path.substr(path.find_last_of(L'\\') + 1);
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& name)
{
    std::wstring out = directory;
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out += name;
    return out;
}

// 目标名已存在时按“名称 (2)”“名称 (3)”递增（原版新建文件夹同款递增）
std::wstring UniquePath(const std::wstring& directory, const std::wstring& baseName,
                        const std::wstring& extension)
{
    for (unsigned int suffix = 1; suffix <= 10000; ++suffix) {
        std::wstring name = baseName;
        if (suffix > 1) name += L" (" + std::to_wstring(suffix) + L")";
        std::wstring candidate = JoinPath(directory, name + extension);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return {};
}

void NotifyCreated(DWORD event, const std::wstring& path)
{
    SHChangeNotify(event, SHCNF_PATHW, path.c_str(), nullptr);
}

bool IsDirectory(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 绝对 PIDL 的最后一个子项辅助函数已随 DragFilesOut 改用 CF_HDROP 而移除：
// SHCreateDataObject 只产生 Shell IDList Array、不产生 CF_HDROP，而资源管理器/桌面
// 等目标是靠 CF_HDROP 读取拖拽文件的，缺它就会静默拒绝拖放（无复制/移动光标、
// effect 落到 NONE），表现为“普通文件列表拖不出去”。改用 CF_HDROP 后不再需要 PIDL。

// Junction 用的 reparse 数据（winioctl.h 的 REPARSE_DATA_BUFFER 在本工程工具集里取不到，
// 这里按公开布局摆一遍：8 字节通用头 + 4 个 USHORT 偏移/长度 + 两条以 NUL 结尾的路径）
#pragma pack(push, 2)
struct MountPointData {
    ULONG reparseTag;
    USHORT reparseDataLength;
    USHORT reserved;
    USHORT substituteOffset;
    USHORT substituteLength;
    USHORT printOffset;
    USHORT printLength;
    WCHAR pathBuffer[1];
};
#pragma pack(pop)
static constexpr USHORT kMountPointHeaderTail = static_cast<USHORT>(4 * sizeof(USHORT));
// 名称区之前的固定字节数：通用头 8 + 4 个 USHORT。必须按这个常量算缓冲区大小，
// 不能用 sizeof(MountPointData)——结构体尾部对齐会把大小撑大，
// 而内核要求 InBufferSize 恰好等于 8 + ReparseDataLength，多一个字节就失败。
static constexpr size_t kMountPointNameOffset = 16;

// 硬链接需要 SeCreateLinkPrivilege，标准用户默认没有；原版正是因此走 runas 提权
// （0x45DE2A: ShellExecuteW(verb=L"runas", L"mklink", 参数, SW_SHOW)）。mklink 是
// cmd 内置命令，必须借 cmd.exe 才能执行。
bool CreateHardLinkElevated(HWND owner, const std::wstring& linkPath, const std::wstring& target)
{
    std::wstring params = L"/c mklink /H \"" + linkPath + L"\" \"" + target + L"\"";
    HINSTANCE r = ShellExecuteW(owner, L"runas", L"cmd.exe", params.c_str(),
                                ParentOf(linkPath).c_str(), SW_SHOWNORMAL);
    WriteAppLog((L"FILEOP hardlink elevated: result=" +
                 std::to_wstring(reinterpret_cast<INT_PTR>(r))).c_str());
    return reinterpret_cast<INT_PTR>(r) > 32;
}

} // namespace

std::wstring CreateNewFolder(HWND owner, const std::wstring& directory)
{
    if (directory.empty()) return {};
    for (unsigned int suffix = 1; suffix <= 10000; ++suffix) {
        std::wstring name = L"新建文件夹";
        if (suffix > 1) name += L" (" + std::to_wstring(suffix) + L")";
        std::wstring path = JoinPath(directory, name);
        if (CreateDirectoryW(path.c_str(), nullptr)) {
            WriteAppLog((L"NEW_FOLDER created: " + path).c_str());
            NotifyCreated(SHCNE_MKDIR, path);
            return path;
        }
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            ReportError(owner, L"新建文件夹", path, error);
            return {};
        }
    }
    MessageBoxW(owner, L"无法找到可用的新文件夹名称。", L"新建文件夹", MB_OK | MB_ICONERROR);
    return {};
}

std::wstring CreateShortcut(HWND owner, const std::wstring& targetPath,
                             const std::wstring& destination)
{
    if (targetPath.empty() || GetFileAttributesW(targetPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return {};
    std::wstring targetDir = ParentOf(targetPath);
    std::wstring directory = destination.empty() ? targetDir : destination;
    std::wstring base = NameOf(targetPath);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) base.erase(dot);   // 主名不含扩展名
    std::wstring linkPath = UniquePath(directory, base + L" - 快捷方式", L".lnk");
    if (linkPath.empty()) {
        MessageBoxW(owner, L"无法找到可用的快捷方式名称。", L"创建快捷方式", MB_OK | MB_ICONERROR);
        return {};
    }

    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link));   // 0x407F1F 用的同一对 CLSID/IID
    if (SUCCEEDED(hr)) hr = link->SetPath(targetPath.c_str());
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(targetDir.c_str());
    ComPtr<IPersistFile> persist;
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(hr)) hr = persist->Save(linkPath.c_str(), TRUE);
    if (FAILED(hr)) {
        WriteAppLog((L"FILEOP shortcut failed: hr=" +
                     std::to_wstring(static_cast<unsigned long>(hr)) +
                     L", path=" + linkPath).c_str());
        MessageBoxW(owner, L"创建快捷方式失败。", L"创建快捷方式", MB_OK | MB_ICONERROR);
        return {};
    }
    WriteAppLog((L"FILEOP shortcut created: " + linkPath + L" -> " + targetPath).c_str());
    NotifyCreated(SHCNE_CREATE, linkPath);
    return linkPath;
}

std::wstring CreateFileHardLink(HWND owner, const std::wstring& targetFile)
{
    if (targetFile.empty() || IsDirectory(targetFile)) {
        WriteAppLog((L"FILEOP hardlink rejected (not a file): " + targetFile).c_str());
        return {};
    }
    std::wstring directory = ParentOf(targetFile);
    std::wstring linkPath = UniquePath(directory, NameOf(targetFile) + L" 硬链接", L"");
    if (linkPath.empty()) {
        MessageBoxW(owner, L"无法找到可用的硬链接名称。", L"创建硬链接", MB_OK | MB_ICONERROR);
        return {};
    }
    if (::CreateHardLinkW(linkPath.c_str(), targetFile.c_str(), nullptr)) {
        WriteAppLog((L"FILEOP hardlink created: " + linkPath + L" -> " + targetFile).c_str());
        NotifyCreated(SHCNE_CREATE, linkPath);
        return linkPath;
    }
    DWORD error = GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD) {
        // 无建链权限：退到原版的提权 mklink /H
        if (CreateHardLinkElevated(owner, linkPath, targetFile) &&
            GetFileAttributesW(linkPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            NotifyCreated(SHCNE_CREATE, linkPath);
            return linkPath;
        }
        return {};
    }
    ReportError(owner, L"创建硬链接", linkPath, error);
    return {};
}

std::wstring CreateJunction(HWND owner, const std::wstring& targetDir)
{
    if (targetDir.empty() || !IsDirectory(targetDir)) {
        WriteAppLog((L"FILEOP junction rejected (not a directory): " + targetDir).c_str());
        return {};
    }
    std::wstring directory = ParentOf(targetDir);
    std::wstring linkPath = UniquePath(directory, NameOf(targetDir) + L" Junction", L"");
    if (linkPath.empty()) {
        MessageBoxW(owner, L"无法找到可用的 Junction 名称。", L"创建 Junction", MB_OK | MB_ICONERROR);
        return {};
    }
    if (!CreateDirectoryW(linkPath.c_str(), nullptr)) {
        ReportError(owner, L"创建 Junction", linkPath, GetLastError());
        return {};
    }

    // IO_REPARSE_TAG_MOUNT_POINT 数据（等价 mklink /J，但不需要管理员权限）。
    // SubstituteName 要用 NT 设备路径形式 "\??\<盘>:\..."。
    std::wstring print = targetDir;
    while (print.size() > 1 && print.back() == L'\\') print.pop_back();
    std::wstring substitute = L"\\??\\" + print;

    const USHORT subBytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    const USHORT printBytes = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    // 两条路径各带一个结束 NUL，结束符也计入 ReparseDataLength（少算/多算都报
    // ERROR_INVALID_REPARSE_DATA 4392）
    const USHORT tailNulls = static_cast<USHORT>(2 * sizeof(wchar_t));
    const USHORT dataLen =
        static_cast<USHORT>(kMountPointHeaderTail + subBytes + printBytes + tailNulls);
    const size_t totalBytes = kMountPointNameOffset + subBytes + tailNulls + printBytes;
    std::vector<BYTE> raw(totalBytes, 0);
    auto* buffer = reinterpret_cast<MountPointData*>(raw.data());
    buffer->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer->reparseDataLength = dataLen;
    buffer->substituteLength = subBytes;
    buffer->printOffset = static_cast<USHORT>(subBytes + sizeof(wchar_t));
    buffer->printLength = printBytes;
    memcpy(buffer->pathBuffer, substitute.data(), subBytes);
    memcpy(reinterpret_cast<BYTE*>(buffer->pathBuffer) + buffer->printOffset,
           print.data(), printBytes);

    HANDLE link = CreateFileW(linkPath.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr);
    bool ok = false;
    if (link != INVALID_HANDLE_VALUE) {
        DWORD returned = 0;
        ok = DeviceIoControl(link, FSCTL_SET_REPARSE_POINT, buffer,
                             static_cast<DWORD>(raw.size()), nullptr, 0, &returned, nullptr)
             != FALSE;
        CloseHandle(link);
    }
    if (!ok) {
        DWORD error = GetLastError();
        RemoveDirectoryW(linkPath.c_str());   // 链接目录没成，别留个空壳
        WriteAppLog((L"FILEOP junction failed: path=" + linkPath +
                     L", win32Error=" + std::to_wstring(error)).c_str());
        std::wstring message = L"无法创建 Junction（目标需为本机 NTFS 路径）：\n" + linkPath +
            L"\n错误码：" + std::to_wstring(error);
        MessageBoxW(owner, message.c_str(), L"创建 Junction", MB_OK | MB_ICONERROR);
        return {};
    }
    WriteAppLog((L"FILEOP junction created: " + linkPath + L" -> " + print).c_str());
    NotifyCreated(SHCNE_MKDIR, linkPath);
    return linkPath;
}

bool ShowProperties(HWND owner, const std::wstring& path)
{
    if (path.empty()) return false;
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.hwnd = owner;
    info.lpVerb = L"properties";
    info.nShow = SW_SHOWNORMAL;
    UniquePIDL pidl = PIDLFromPath(path);
    if (pidl) {
        info.fMask = SEE_MASK_INVOKEIDLIST;
        info.lpIDList = pidl.get();
    } else {
        info.lpFile = path.c_str();   // 虚拟位置取不到 PIDL，按路径交给 shell
    }
    if (!ShellExecuteExW(&info)) {
        WriteAppLog((L"FILEOP properties failed: path=" + path +
                     L", win32Error=" + std::to_wstring(GetLastError())).c_str());
        MessageBoxW(owner, L"无法打开属性对话框。", L"属性", MB_OK | MB_ICONERROR);
        return false;
    }
    WriteAppLog((L"FILEOP properties opened: " + path).c_str());
    return true;
}

DWORD DragFilesOut(HWND owner, const std::vector<std::wstring>& paths)
{
    if (paths.empty()) return DROPEFFECT_NONE;

    // 拖出用 CF_HDROP：资源管理器/桌面等目标正是从 CF_HDROP 读取拖拽文件的。
    // 之前用 SHCreateDataObject 只产生 Shell IDList Array、没有 CF_HDROP，目标拿
    // 不到路径就静默拒绝（无复制/移动光标、effect=NONE），这就是“普通列表拖不出去”。
    // 这里手工实现 IDataObject，只放 CF_HDROP（DROPFILES 头 + 双 '\0' 结尾的宽字符
    // 路径串），与 SetFileClipboard 用的一模一样——那份已被证明能跟 Explorer 互通。
    WriteAppLog((L"FILEOP drag out begin: items=" + std::to_wstring(paths.size()) +
                 L", first=" + paths.front()).c_str());
    SIZE_T chars = 0;
    for (const auto& p : paths) chars += p.size() + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                            sizeof(DROPFILES) + (chars + 1) * sizeof(wchar_t));
    if (!h) {
        WriteAppLog(L"FILEOP drag out: GlobalAlloc for CF_HDROP failed");
        return DROPEFFECT_NONE;
    }
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    if (!df) {
        GlobalFree(h);
        WriteAppLog(L"FILEOP drag out: GlobalLock for CF_HDROP failed");
        return DROPEFFECT_NONE;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    auto* w = reinterpret_cast<wchar_t*>(df + 1);
    for (const auto& p : paths) {
        size_t n = p.size() + 1;
        wmemcpy(w, p.c_str(), n);
        w += n;
    }
    *w = L'\0';                       // 列表以双 '\0' 结束
    GlobalUnlock(h);

    class DropData : public IDataObject {
    public:
        explicit DropData(HGLOBAL hd) : hd_(hd) {}
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
            if (riid == IID_IDataObject || riid == IID_IUnknown) {
                *ppv = static_cast<IDataObject*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return 1; }
        STDMETHODIMP_(ULONG) Release() override { return 1; }
        STDMETHODIMP GetData(FORMATETC* fe, STGMEDIUM* stg) override {
            if (fe->cfFormat != CF_HDROP || !(fe->tymed & TYMED_HGLOBAL) ||
                fe->dwAspect != DVASPECT_CONTENT)
                return DV_E_FORMATETC;
            stg->tymed = TYMED_HGLOBAL;
            stg->pUnkForRelease = nullptr;
            // 交出独立副本：SHDoDragDrop 结束时才会释放，避免源 h 提前被我们 GlobalFree
            HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, ::GlobalSize(hd_));
            if (!copy) return STG_E_MEDIUMFULL;
            void* cs = GlobalLock(copy);
            void* ss = GlobalLock(hd_);
            if (cs && ss) memcpy(cs, ss, ::GlobalSize(hd_));
            if (cs) GlobalUnlock(copy);
            if (ss) GlobalUnlock(hd_);
            stg->hGlobal = copy;
            return S_OK;
        }
        STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
        STDMETHODIMP QueryGetData(FORMATETC* fe) override {
            if (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL) &&
                fe->dwAspect == DVASPECT_CONTENT)
                return S_OK;
            return DV_E_FORMATETC;
        }
        STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
            if (out) { *out = FORMATETC{}; out->ptd = nullptr; }
            return DATA_S_SAMEFORMATETC;   // 只有一种等价表示
        }
        STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
        STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppEnum) override {
            if (!ppEnum) return E_POINTER;
            if (dwDirection != DATADIR_GET) { *ppEnum = nullptr; return E_NOTIMPL; }
            // 用系统标准枚举器真正枚举出 CF_HDROP。之前返回 E_NOTIMPL：有些目标会
            // “先枚举格式再决定收不收”，枚举失败就当成“没有可用格式”直接拒绝，
            // 表现为拖拽完全没有反应。这里补上，让 QueryGetData/EnumFormatEtc 两条
            // 探测路径都能看到 CF_HDROP。
            FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            return SHCreateStdEnumFmtEtc(1, &fe, ppEnum);
        }
        STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return E_NOTIMPL; }
        STDMETHODIMP DUnadvise(DWORD) override { return E_NOTIMPL; }
        STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return E_NOTIMPL; }
    private:
        HGLOBAL hd_;
    };

    DropData data(h);
    DWORD effect = DROPEFFECT_NONE;
    // grfEffects=7：COPY|MOVE|LINK 全交给目标决定。默认无修饰键为复制，
    // 按住 Shift=移动，Ctrl+Shift=创建快捷方式（由 Explorer 自己按修饰键解释）。
    HRESULT hr = SHDoDragDrop(owner, &data, nullptr,
                      DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    WriteAppLog((L"FILEOP drag out: hr=" + std::to_wstring(static_cast<unsigned long>(hr)) +
                 L", effect=" + std::to_wstring(effect) +
                 L", items=" + std::to_wstring(paths.size())).c_str());
    GlobalFree(h);
    if (FAILED(hr)) return DROPEFFECT_NONE;
    return effect;
}

} // namespace shell
