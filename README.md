# PagedExplorer

原生 Windows C++20 / Win32 / Shell API 的分页资源管理器第一版。

## 已实现

- Win32 原生窗口
- 地址栏
- 返回 / 前进 / 上一级 / 刷新
- 左侧目录树
- Windows Shell 原生图标
- 详细信息列表
- 虚拟 ListView
- 50/100/200/500 每页
- 首页/上一页/下一页/末页
- 名称/修改时间/类型/大小排序
- 双击进入文件夹/打开文件
- F2 重命名
- Delete 删除
- Ctrl+C / Ctrl+X / Ctrl+V
- Explorer Shell 右键菜单
- 目录树右上角的“定位”按钮可展开并选中当前标签页对应目录
- 目录树控件、懒展开和路径同步逻辑独立封装在 `src/UI/DirectoryTree.cpp`
- 工具栏“菜单过滤词”可编辑右键菜单扩展屏蔽关键词（每行一个，配置保存在程序目录的 `context-menu-filters.txt`）
- 最小化窗口仍保留在任务栏；关闭窗口时隐藏到系统托盘，托盘右键菜单可打开应用、设置或退出
- 系统托盘图标与右键菜单封装在独立的 `src/UI/TrayIcon.cpp` 类中
- 托盘“设置”中可启用/关闭“Windows 启动时运行本应用”（仅当前用户，无需管理员权限）
- 运行日志写入 `%LOCALAPPDATA%\\PagedExplorer\\PagedExplorer.log`，用于诊断启动、窗口关闭和托盘行为
- 大目录采用后台枚举，并按页缓存
- 多窗格布局：Alt+1~4 = 1~4 个窗格（2 个左右 / 3 个品字或倒品 / 4 个田字）
- Alt+小键盘8 = 品字形（1 上 2 下），Alt+小键盘2 或 Alt+↓ = 倒品字形（2 上 1 下，默认）
  小键盘需 NumLock 开启；没有记录时三窗格用倒品字形
- 窗格数量与品字形态存在 favorites.txt（panes=/tri=），下次启动沿用
- Ctrl+点击 “+” 新增窗格（最多 4 个）；窗格间分隔条可拖宽，标题条可拖到其它窗格
- 鼠标中键点击分页标题 = 关闭该分页，锁定分页也可以用中键关（最后一个分页不会被关掉；窗格空了会自动合并）
- 分页标题右键菜单：关闭 / 关闭其他 / 关闭右边 / 锁定
  “锁定”的分页不会被“关闭其他、关闭右边”关掉（中键是明确操作，可以关掉锁定的分页）；标题前显示 `[锁]`。
  锁定只禁止关闭与地址变化：激活锁定分页时在地址栏输入新地址，会在同一窗格新开分页打开；锁定的分页仍可拖动到其它窗格

## 编译

### CMake

```powershell
cmake -S . -B cmake-build -G "Visual Studio 17 2022" -A x64
cmake --build cmake-build --config Release
```

生成文件位于 `cmake-build/bin/Release/PagedExplorer.exe`。

### Visual Studio

Visual Studio 2022/2026，打开 `PagedExplorer.sln`，选择 `x64`，直接生成。
也可以运行 `build.bat`，通过 MSBuild 构建解决方案。

说明：这是第一版原型，Shell 原生右键菜单、图标和文件操作已经接入；后续可继续加入缩略图、搜索、标签页、拖放、真正的 Shell Namespace 扩展等。

右键菜单扩展过滤依据扩展菜单项显示的文字，不会修改注册表或卸载插件。关键词按不区分大小写的子串匹配；默认包含 TortoiseSVN、YunShell 和百度网盘相关词。删除某行可放行对应菜单，清空配置文件可关闭全部过滤。
