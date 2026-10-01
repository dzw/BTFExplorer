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
- 大目录采用后台枚举，并按页缓存

## 编译

Visual Studio 2022/2026，打开 `PagedExplorer.sln`，选择 `x64`，直接生成。

说明：这是第一版原型，Shell 原生右键菜单、图标和文件操作已经接入；后续可继续加入缩略图、搜索、标签页、拖放、真正的 Shell Namespace 扩展等。
