# PagedExplorer 项目备忘

## 项目位置（2026-10-02 用户重组过）
- 工程在 `d:\APrj\BinDir` **根目录**（曾长期在 `BinDir\PagedExplorer\` 子目录，用户已挪出并新增 `src\FileModel\` 等）。
  构建脚本 `build.bat`（编译 `src\App\main.cpp src\Shell\*.cpp src\Pagination\*.cpp src\UI\*.cpp`，输出 `build\PagedExplorer.exe`）。
  `src\main.cpp` 是旧版单窗口遗留备份，不参与编译。
- 构建注意：用户常开着 VS 调试器，会锁 `build\PagedExplorer.pdb` 报 C1041；等其释放或稍后重试。

## 功能快捷键（已实现）
- Alt+1~4：窗格数量（2 左右 / 3 品字或倒品 / 4 田字）；Alt+小键盘8=品字形，Alt+小键盘2（或 Alt+↓）=倒品字形（默认）。
- 布局持久化在 favorites.txt（sort=/panes=/tri=），启动沿用；没有记录时三窗格用倒品字形。
- 缩减窗格时，被缩窗格分页若与目标窗格目录重复则直接丢弃（PaneHasDir 判重）。
- 中键点分页标题=关闭；分页标题右键菜单=关闭/关闭其他/关闭右边/锁定（锁定的不可关、不可拖，标题带 [锁] 前缀）。
- 分页可拖到其它窗格；Ctrl+点"+"加窗格（最多 4 个）。

## 破坏性操作一律延后到主窗口消息循环
关闭分页/移动分页都可能连带 RemovePane→DestroyWindow(tab)，绝不能在 tab 自己的窗口过程中直接做：
用 `pendingCloseTab_/pendingMoveTab_ + PostMessage(WM_APP_CLOSE_TAB / WM_APP_MOVE_TAB)` 模式。

## 跨进程调试（PowerShell 测试脚本）经验
- 跨进程 SendMessage 绝不能传本进程指针（TCM_GETITEMRECT/TCM_GETITEMTEXT 等），目标进程会崩(0xC0000409)。
  要取目标进程内数据用 OpenProcess+VirtualAllocEx+ReadProcessMemory（VM_OPERATION|READ|WRITE, 0x1F0FFF）。
- 安全可用：TCM_GETITEMCOUNT(0x1304)、LVM_GETITEMCOUNT(0x1004)、PostMessage+坐标、WM_SYSKEYDOWN 模拟按键、
  ClientToScreen/ScreenToClient/GetWindowRect（win32u 系统调用，写的是调用方内存）。
- **GetWindowTextW 跨进程读不到其它进程控件文本**（只能拿标题），地址栏/状态栏读出来恒为空，别用它做断言。
- lParam 打包坐标要 `-band 0xFFFF` 再移位，负数会污染高 16 位。
- 弹出菜单（#32768）投递 WM_KEYDOWN 选不中菜单项；要驱动菜单命令可临时加 WM_APP 钩子直调，验完删除。
- .ps1 脚本里写中文易乱码导致解析失败，脚本输出一律用 ASCII。
- 每个命令是独立 PowerShell 会话：Add-Type 的 P/Invoke 类型必须与调用在同一次命令里。

## 命令行参数打开目录（2026-10-02 修）
外部启动器传 `"D:\dir" `（引号+尾随空格）时旧代码手工去引号漏删 → GetFileAttributes 失败 → 不导航。
已改用 `CommandLineToArgvW` 取 argv[1]；WM_COPYDATA 接收端改按 C 字符串构造（原 cbData 含结尾 '\0' 会被带进
std::wstring，嵌入 '\0' 让枚举 pattern 变成目录本身，列表只剩目录一项）；NormalizePath 顺带截断嵌入 '\0'
并去首尾引号/空白。
