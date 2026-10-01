#pragma once
#include "ShellUtil.h"
#include "../FileModel/FileEntry.h"
#include <vector>
#include <functional>

namespace shell {

// 用 FindFirstFile 风格的轻量枚举（Win32 层，够快），返回一页。
// offset 从 0 开始；hintDir 可为空。
// 返回抓到的条目数；total 可为 nullptr。
size_t EnumeratePage(const std::wstring& dir,
                     size_t offset,
                     size_t count,
                     std::vector<FileEntry>& out,
                     unsigned long long* total);

} // namespace shell
