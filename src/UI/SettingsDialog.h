#pragma once
#include <windows.h>
#include <functional>

// 文件列表 / 分页栏 / 设置界面共用的“每页项数”档位；顺序即下拉框顺序
inline constexpr int kPageSizes[4] = { 50, 100, 200, 500 };
inline constexpr int kPageSizeCount = 4;

// “每页项数” -> 下拉框下标；非法值回落到 100（也就是下标 1）
inline int PageSizeToIndex(int size)
{
    for (int i = 0; i < kPageSizeCount; ++i)
        if (kPageSizes[i] == size) return i;
    return 1;
}

// 设置界面的数据：进对话框时带初值，出来时回写结果
struct SettingsData {
    bool gridLines  = true;   // 文件列表网格线
    bool pagination = true;   // 分页开关：关闭后一页显示全部条目（每页项数用主界面分页栏改）
    int  paneCount  = 1;      // 窗格数量 1~4
    int  triLayout  = 1;      // 三窗格排列：0=品字形(1上2下) 1=倒品字形(2上1下)
    bool autoStart  = false;  // 开机自启动
    bool navSound   = true;   // 窗格换目录时播放导航音
};

namespace settings {

// 开机自启动：HKCU\Software\Microsoft\Windows\CurrentVersion\Run\PagedExplorer
bool GetAutoStart(bool& enabled);
void SetAutoStart(bool enabled);

// 模态显示设置对话框。owner 为父窗口，font 为界面字体（可为 nullptr）。
// onApply 在“确定”或“应用”被点击时调用（data 为当前对话框中的设置），用于即时生效；
// “取消”不会调用它。返回 true  = 用户按了“确定”；false = 取消（或窗口创建失败）。
bool Show(HWND owner, HFONT font, SettingsData& data,
         std::function<void(const SettingsData&)> onApply);

} // namespace settings
