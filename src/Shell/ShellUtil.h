#pragma once
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <memory>

struct PIDLDeleter { void operator()(void* p) const { if (p) CoTaskMemFree(p); } };
using UniquePIDL = std::unique_ptr<void, PIDLDeleter>;

struct EnumRelease { void operator()(IEnumIDList* p) const { if (p) p->Release(); } };

// COM 智能指针（不引 tlogon/comdef，手写最小版）
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p(o.p) { o.p = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept { if (this != &o) { Release(); p = o.p; o.p = nullptr; } return *this; }
    T** operator&() { Release(); return &p; }
    T* operator->() const { return p; }
    T* Get() const { return p; }
    T* get() const { return p; }
    void Attach(T* t) { Release(); p = t; }
    T* Detach() { T* t = p; p = nullptr; return t; }
    void Release() { if (p) { p->Release(); p = nullptr; } }
    explicit operator bool() const { return p != nullptr; }
private:
    T* p = nullptr;
};

namespace shell {

// 从路径创建绝对 PIDL（失败返回 nullptr）
UniquePIDL PIDLFromPath(const std::wstring& path);
// 从 PIDL 取文件系统路径（虚拟位置返回空）
std::wstring PathFromPIDL(PCIDLIST_ABSOLUTE pidl);
// 显示名
std::wstring DisplayNameFromPIDL(PCIDLIST_ABSOLUTE pidl);

// 取小图标 HICON（调用方负责 DestroyIcon）
HICON GetIconForEntry(const std::wstring& path, bool isFolder);

// 取类型名（如 "PNG 文件"、"文件文件夹"）
std::wstring TypeNameForEntry(const std::wstring& path, bool isFolder);

// 取系统图像列表 (SHGFI_SYSICONINDEX) 中的小图标索引，失败返回 -1
int SysIconIndexForEntry(const std::wstring& path, bool isFolder);

// 带进程内缓存的类型名查询：文件按扩展名只查一次 SHGetFileInfoW，文件夹查一次。
// 枚举线程和 UI 线程都会调用（线程安全）。虚拟列表每帧每行都取类型列文本，
// 不缓存的话滚动时每帧要打进 shell/注册表几十次，这是滚动卡顿的主因。
std::wstring CachedTypeNameForEntry(const std::wstring& path, bool isFolder);

// 带进程内缓存的图标索引查询：文件按扩展名缓存。图标逐文件而异的类型
// （exe/lnk/ico 等）与文件夹不做扩展名缓存，仍逐路径查询。
int CachedSysIconIndexForEntry(const std::wstring& path, bool isFolder);

} // namespace shell
