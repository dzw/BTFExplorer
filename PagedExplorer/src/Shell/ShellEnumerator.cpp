#include "ShellEnumerator.h"
#include <algorithm>

namespace shell {

// 思路：Windows 目录枚举本身是顺序的（NTFS 的 B+ 树也是按名称序返回），
// 要拿到第 N 页就顺序跳过前 offset 项。FindFirstFileEx 使用
// FindExInfoBasic + FIND_FIRST_EX_LARGE_FETCH 可显著加速大目录。
size_t EnumeratePage(const std::wstring& dir,
                     size_t offset,
                     size_t count,
                     std::vector<FileEntry>& out,
                     unsigned long long* total)
{
    out.clear();
    if (dir.empty()) return 0;

    std::wstring pattern = dir;
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd{};
    // LARGE_FETCH：NTFS 枚举一次取一大块，百万文件目录快数倍
    DWORD flags = FIND_FIRST_EX_LARGE_FETCH;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, flags);
    if (h == INVALID_HANDLE_VALUE) {
        if (total) *total = 0;
        return 0;
    }

    size_t skipped = 0;
    size_t taken = 0;
    auto accept = [&fd]() {
        return !(fd.cFileName[0] == L'.' &&
                 (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)));
    };

    // 先跳过 offset 项（不构造 string，只数数）
    bool ok = true;
    while (skipped < offset) {
        if (accept()) ++skipped;
        if (!FindNextFileW(h, &fd)) { ok = false; break; }
    }
    if (total) {
        // 调用方要总数：需要走完全目录，代价大；分页 UI 用“还有更多”策略，
        // 这里只在第一页时统计（后台线程调用）。
        // 注意：必须用独立的 fd2，不能复用 fd —— 第二次枚举会覆盖 fd，
        // 导致主枚举句柄之后读到错乱数据、列表出现重复条目。
        if (offset == 0) {
            unsigned long long n = 0;
            WIN32_FIND_DATAW fd2{};
            HANDLE h2 = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd2,
                                         FindExSearchNameMatch, nullptr, flags);
            if (h2 != INVALID_HANDLE_VALUE) {
                for (;;) {
                    if (!(fd2.cFileName[0] == L'.' &&
                          (fd2.cFileName[1] == 0 || (fd2.cFileName[1] == L'.' && fd2.cFileName[2] == 0))))
                        ++n;
                    if (!FindNextFileW(h2, &fd2)) break;
                }
                FindClose(h2);
            }
            *total = n;
        }
    }

    if (ok) {
        out.reserve(count);
        while (taken < count) {
            if (accept()) {
                FileEntry e;
                e.name = fd.cFileName;
                e.path = dir + (dir.back() == L'\\' ? L"" : L"\\") + fd.cFileName;
                e.isFolder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                e.writeTime = fd.ftLastWriteTime;
                e.iconIndex = SysIconIndexForEntry(e.path, e.isFolder);
                e.filled = true; // 枚举时这些字段本来就拿到了
                out.push_back(std::move(e));
                ++taken;
            }
            if (!FindNextFileW(h, &fd)) break;
        }
    }
    FindClose(h);
    return out.size();
}

} // namespace shell
