#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>

// 列序契约：0=名称 1=类型 2=大小 3=修改日期（FileList::Create 按此建列，
// MainWindow 的排序 / GETDISPINFO 按此取数，两边必须一致）
namespace filelist {
enum Col { ColName = 0, ColType, ColSize, ColMTime };

// 显示辅助（GETDISPINFO 与别处共用）
std::wstring FormatSize(unsigned long long sz);
std::wstring FormatTime(const FILETIME& ft);
} // namespace filelist

// 窗格文件列表的交互回调：控件侧（本文件）负责显示、鼠标键盘、命中测试；
// 业务决策（导航/菜单/重命名/数据）交回 MainWindow。shell 视图实现
// （ShellFolderView）上线后，两种实现共用同一套业务入口。
class FileListDelegate {
public:
    virtual ~FileListDelegate() = default;
    // 用户交互到非激活窗格：延后激活（宿主判断是否需要）
    virtual void ActivatePaneDeferred(HWND list) = 0;
    // 慢双击重命名（单击 -> 停顿 -> 再单击同一项）
    virtual void SlowRename(HWND list, int item) = 0;
    // 就地重命名完成：newName 是编辑框里的新文件名（空 = 取消或未改）。
    // 宿主负责真正执行改名并刷新；列表本身不保留编辑框里的文字。
    virtual void CommitRename(HWND list, int item, const std::wstring& newName) = 0;
    // 按住左键拖出超过系统拖拽阈值：请求发起拖拽（item = 按下时命中的行）。
    // 宿主必须延后执行：拖拽循环自己泵消息，不能在列表的消息链里直接进。
    virtual void StartDrag(HWND list, int item) = 0;
    // 键盘命令（Cmd*）
    virtual void ListKeyCommand(HWND list, int cmd) = 0;
    // 右键菜单（screenPt 屏幕坐标；keyboard=键盘触发的默认位置）
    virtual void ListContextMenu(HWND list, const POINT& screenPt, bool keyboard) = 0;
    // 列头点击排序
    virtual void ListColumnClicked(HWND list, int col) = 0;
    // 双击打开
    virtual void ListItemActivated(HWND list, int item) = 0;
    // 取行数据（GETDISPINFO）；返回 false = 该格无内容
    virtual bool GetItemText(HWND list, int item, int subItem, std::wstring& out) = 0;
    virtual int  GetItemIcon(HWND list, int item) = 0;
};

// 自绘虚拟文件列表（窗格的默认实现）：LVS_OWNERDATA 报表视图，
// 图标挂系统图像列表（跨窗格共享一份），类型名/图标索引带扩展名级缓存。
class FileList {
public:
    enum KeyCmd { CmdCopy, CmdCut, CmdPaste, CmdDelete, CmdDeleteNoRecycle, CmdRename,
                  CmdEscape, CmdUp };

    bool Create(HWND parent, FileListDelegate* delegate, HFONT font, int controlId);
    ~FileList();
    HWND Handle() const { return list_; }

    // “显示网格线”设置开关
    void SetGridLines(bool show);

    // 就地重命名（资源管理器风格）：在指定行上弹出编辑框，选中主名（不含扩展名）。
    // 编辑结束由 LVN_ENDLABELEDIT -> CommitRename 交回宿主执行。
    // 返回是否成功进入编辑状态。
    bool BeginRename(int item);

    // 主窗口收到本列表的 WM_NOTIFY 后调进来；返回 true = 已处理
    bool HandleNotify(NMHDR* nm);

private:
    static LRESULT CALLBACK ProcStatic(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT Proc(HWND, UINT, WPARAM, LPARAM);

    HWND list_ = nullptr;
    WNDPROC origProc_ = nullptr;
    FileListDelegate* delegate_ = nullptr;
    bool gridLines_ = true;
    // 慢双击重命名状态（本列表自己的单击记录）
    int lastClickItem_ = -1;
    bool renaming_ = false;   // 就地重命名编辑框存活期间（屏蔽慢双击判定）
    DWORD lastClickTime_ = 0;
    // 拖出状态：按下时的点 + 行，移动超过阈值即请求拖拽
    POINT dragStartPt_{};
    int dragStartItem_ = -1;
    bool dragPending_ = false;
};
