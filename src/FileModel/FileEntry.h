#pragma once
#include <windows.h>
#include <string>

// 一个文件/文件夹条目。枚举时只填 name / isFolder / path（轻量），
// 其余字段（大小、日期、类型名、图标）在 UI 线程显示时惰性填充，
// 这样枚举 230 万项时每项只付出最小的代价。
struct FileEntry {
    std::wstring name;        // 显示名
    std::wstring path;        // 文件系统路径（虚拟位置为空）
    bool isFolder = false;
    bool filled = false;      // 惰性字段是否已填充
    unsigned long long size = 0;
    FILETIME writeTime{};
    std::wstring typeName;
    int iconIndex = -1;       // 系统图像列表中的图标索引，-1 = 未取
};

inline unsigned long long FileTimeToUInt64(const FILETIME& ft) {
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
