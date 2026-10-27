# TextSearch — 面向 CJK 的本地全文搜索（GUI + CLI + 倒排索引）

**[English](README.md) | 简体中文**

- **纯文本专注**：只索引「记事本可打开」的文本文件（代码 `.py/.c`、配置 `ini/yaml/json`、日志、笔记），不追 PDF/DOCX 等富格式——换取小而快、零依赖。
- **双模式**：即时扫描（多线程 + 64MB 滑窗流式，大文件内存有界）与持久倒排索引（建一次库，之后查询毫秒级；搜索前自动增量更新）随选随用。
- **CJK 一等公民**：中文 bi-gram 切分 + 邻接验证 + 子词补记，中文与连写标识符的内部子串零漏报——不是把 CJK 当字节流。
- **零依赖原生工具**：Win32 原生三栏 GUI + 跨平台命令行 `textsearch-cli`，无第三方库、无捆绑、无遥测；搜索关键词永不落盘。

![TextSearch 主界面](docs/screenshots/main.png)

Windows 平台图形化全文查找工具，只针对"记事本可打开"的文本文件（代码类 `py/c/bas`、配置类 `ini/yaml/json` 等），支持指定路径搜索、双搜索模式（即时扫描 / 倒排索引）、子串/正则/大小写匹配、自动识别 UTF-8/GBK/UTF-16 编码、常用扩展名管理与目录扩展名发现、并发高效扫描、结果内嵌查看器（三栏宽屏布局）。

## 构建方法

### 前置条件

- **CMake** ≥ 3.15（已验证 4.2.3）
- **MinGW-w64 GCC** ≥ 15.2.0（ucrt-posix-seh 构建）
- **Windows 10+**（UCRT 运行时）

### 编译命令

```bash
# 配置（默认 Release 构建，启用 -O2 优化）
cmake -G "MinGW Makefiles" -B build

# 编译
cmake --build build

# 运行单元测试（全绿为准，具体用例数以输出为准）
./build/tests.exe

# 运行性能基准测试（输出 JSON 报告）
./build/bench.exe
```

构建产物：
- `build/textsearch.exe` — 主程序（Win32 GUI）
- `build/textsearch-cli.exe` — 跨平台命令行入口（`search` / `build` / `update`，支持 `--json`）
- `build/tests.exe` — 单元测试
- `build/bench.exe` — 性能基准测试

> **关于链接方式（2026-10 更新）**：当前默认**静态链接**（`-static -static-libgcc -static-libstdc++`），主程序约 1.9MB、零 DLL 依赖。原因：本机 `C:\Windows` 下的 `libstdc++-6.dll` 等运行时是 Tesseract 残留的旧版本，与 GCC 16.x 不兼容——动态链接的 exe 在本机启动即 `0xC0000139`，不可依赖。CMake 侧由 `TEXTSEARCH_STATIC` 开关控制（默认 `ON`）；确要动态构建（约 650KB）时置 `OFF`，并必须把**工具链 `bin/` 目录配套的** 3 个运行时 DLL（`libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`）与 exe 同目录，切勿使用 `C:\Windows` 里的同名文件。详见「技术交底书 → 一、架构决策记录」。

### 无 CMake 时的直接编译（本机当前方式）

开发机若未安装 CMake，可用与 `CMakeLists.txt` 等价的 g++ 命令直接编译（参数与 CMake target 一一对应）：

```bash
TC="<MinGW 工具链>/bin"

# 1) 资源编译（-I gui 让 app.rc 能找到 app.manifest）
"$TC/windres.exe" -I gui -O coff -o build/app_res.o gui/app.rc

# 2) 主程序（默认静态链接：零 DLL 依赖，约 1.9MB）
"$TC/g++.exe" -std=c++17 -O2 -Wall -Wextra -finput-charset=UTF-8 -s \
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -I. \
  -static -static-libgcc -static-libstdc++ \
  -o build/textsearch.exe gui/*.cpp core/*.cpp core/port/*.cpp build/app_res.o \
  -mwindows -municode -lcomctl32 -lcomdlg32 -lriched20 -lole32 -loleaut32 \
  -luuid -lshlwapi -lshell32 -ldwmapi

# 3) 命令行版（跨平台入口：search / build / update）
"$TC/g++.exe" -std=c++17 -O2 -Wall -Wextra -finput-charset=UTF-8 -s \
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -I. \
  -static -static-libgcc -static-libstdc++ \
  -o build/textsearch-cli.exe cli/main.cpp core/*.cpp core/port/*.cpp -municode

# 4) 单元测试（不含 bench_search.cpp，它有独立 main）
"$TC/g++.exe" -std=c++17 -O2 -s \
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -I. \
  -static -static-libgcc -static-libstdc++ \
  -o build/tests.exe core/*.cpp core/port/*.cpp tests/test_*.cpp
```

> **两个易踩点**：
> 1. 去掉 `-static` 改动态链接（约 650KB）时，必须把**工具链 `bin/` 配套的** `libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll` 放到 exe 同目录——`C:\Windows` 下的同名文件是旧版残留，与 GCC 16.x 不兼容（启动即 `0xC0000139`）。
> 2. `DwmSetWindowAttribute`（深色标题栏）需要显式 `-ldwmapi`。老版本 GCC 可能默认带入所以 `CMakeLists.txt` 未写，新工具链（GCC 16.x / binutils 2.4x）下缺它会导致链接失败。
> 3. 新版 binutils 链接时会输出 `.rsrc merge failure: multiple non-default manifests`——这是**警告不是错误**，exit code 为 0，manifest 实际保留（可用 `objdump -s -j .rsrc` 验证），可忽略。

### 可选：UPX 压缩

在 `CMakeLists.txt` 末尾取消注释 UPX 相关行，即可在 Release 构建后自动压缩 `textsearch.exe`。

## 项目结构

代码按「界面层 / 逻辑层 / 测试层」清晰分层，`core/` 与 `gui/` 互不越界：

| 目录 / 文件 | 角色 | 说明 |
|-------------|------|------|
| `gui/` | **界面层** | Win32 原生 GUI，共 9 个源文件（另有 `app.rc`/`app.manifest` 资源，见下） |
| `cli/` | **命令行入口** | `textsearch-cli`（search / build / update，`--json` 输出），只依赖 core，随 core 跨平台 |
| `core/` | **逻辑层** | 纯 C++17，无窗口代码：扫描 / 匹配 / 编码识别 / 索引 / 线程池等；平台差异收口在 `core/port/`（Windows 实现之外另附可移植 stub，见 ADR A6） |
| `tests/` | **测试层** | 单元测试 + 性能基准，仅依赖 `core/`，脱离 GUI 独立运行（GBK 用例以 Win32 API 为预言机，暂为 Windows 专属） |

**界面层（`gui/` 下全部文件即界面文件）：**

| 文件 | 职责 |
|------|------|
| `main.cpp` | 程序入口 `wWinMain`，初始化 COM、创建主窗口、消息循环 |
| `main_window.h` / `main_window.cpp` | 主窗口：三栏布局、菜单、搜索控件、模式切换、事件处理 |
| `viewer.h` / `viewer.cpp` | 结果内嵌查看器：右栏 RichEdit，载入文件、高亮命中、行号跳转 |
| `dlg_extensions.h` / `dlg_extensions.cpp` | 扩展名管理对话框：常用扩展名编辑、目录扩展名发现、抽样验证 UI |
| `dlg_whitelist.h` / `dlg_whitelist.cpp` | 「编辑常用扩展名」对话框：多行编辑 + 校验 + 重置默认，写入 ini `[Extensions] common` |

> 界面层依赖 `core/` 提供的功能，但 `core/` 不反向依赖 `gui/`；因此 `tests` 能在无窗口环境下直接编译运行 `core`。
> 布局 / 颜色 / 字体目前硬编码在 `main_window.cpp` / `viewer.cpp`，未使用 `.rc` 资源文件。

### UI 改进指引

当前界面属于「先求能用、后求好用」阶段的产物：`gui/` 下的控件布局、间距、Tab 顺序、对齐方式多为随手摆放，未系统考量合理性；颜色 / 字体 / 尺寸硬编码在 `main_window.cpp` 与 `viewer.cpp` 中，没有 `.rc` 资源文件。后续若接手打磨界面，请遵循以下原则：

- **只动 `gui/`**：布局、外观、交互的改动都应局限在界面层，不要为 UI 需要去改 `core/`。如确需 core 暴露新数据，优先通过接口 / 回调，而非把逻辑耦合进窗口代码。
- **从布局合理性入手**：先按真实使用流梳理控件分组与 Tab 顺序（路径 → 关键词 → 选项 → 操作 → 结果），再谈美化。当前三栏为固定布局，**优先做可拖拽分隔条**（让用户自调三栏宽度）；DPI 缩放已有 `scale_dpi` 处理。窗口位置 / 尺寸 / 最大化状态**已实现记忆**（存 settings.json），此项无需再做。界面元素说明与交互设计决策见 `docs/ui-notes.md`。
- **保持 Win32 原生**：维持原生控件可保证 `core/` 零改动与零额外 DLL 依赖；只有追求大幅现代化外观时，才评估换 GUI 框架——那将重写 `gui/` 整层，但 `core/` 仍可复用。
- **回归保护**：每次 UI 改动后跑一遍 `./build/tests.exe`，确认没有误伤逻辑层（tests 不覆盖 UI，但能防住边界改动）。

## 已知缺陷与待改进（移交清单）

以下功能缺口由使用方在移交前逐一指出，供接手者有的放矢地改进。除已修复项外，均位于界面层 `gui/`，不触碰 `core/` 逻辑。

### 1. 界面布局随意（已写入上方「UI 改进指引」）

- **现象**：控件随手摆放，未系统考量分组 / Tab 顺序 / 对齐 / 间距；颜色字体硬编码，无 `.rc` 资源文件。
- **改进方向**：按「路径 → 关键词 → 选项 → 操作 → 结果」梳理流程，优先做可拖拽分隔条、窗口记忆、DPI 适配。

### 2. 索引状态与当前搜索模式不可见（已修复 2026-08-28）

- **现象**：状态栏仅显示「正在扫描… / 已完成（命中 X 条）」，既不提示当前目录是否已建索引，也不说明本次搜索走的是「索引模式」还是「实时扫描」。
- **影响**：用户无法判断搜索来源；扩展名过滤 bug 之所以长期难察觉，正是因为程序悄悄走了索引分支而界面无任何提示。
- **修复**：状态栏新增独立「索引」pane（part2），在启动恢复路径、浏览选目录、路径框失焦、搜索开始、索引建完等时机检测索引状态，显示「索引：已建立 / 未建立 / 路径框内是无效路径」；搜索模式（索引/实时）**在 worker 线程判定**后 `PostMessage(WM_SEARCH_MODE)` 回传 UI（判定要读索引文件算 checksum，绝不能在 UI 线程同步跑），开始/扫描中/结束的状态文本统一追加「（索引模式）/（实时扫描）」。仅改 `gui/main_window.cpp`，core 零改动。
- **补充说明**：有效的「已建立 / 未建立」检测走 `index_exists()`，该函数开销较大（读索引文件校验），因此**不在每次按键时触发**，仅在路径选定/失焦等节点 + 异步后台线程执行。

### 3. 建索引时未提示 / 确认扩展名，且静默回退常用扩展名（已修复 2026-08-28）

- **现象**：`on_build_index()` 用 `resolve_extensions()`（左栏勾选）决定索引哪些类型，但若左栏未勾选任何项，会**静默回退到整个常用扩展名（21 种）**全量建索引；且建之前无摘要 / 确认。
- **影响**：用户本想只索引少量类型，却可能被静默全量建索引。
- **修复**：`on_build_index()` 建前弹摘要确认框（目录 + 扩展名清单 + 数量，MB_OKCANCEL）；左栏未勾选时明确警告「将按常用扩展名（N 种）全量建立索引」并以警告图标区分，取消则中止。常用扩展名只读自 `load_whitelist()`，core 零改动。

### 4. 扩展名列缺少手动添加入口（已修复 2026-08-28）

- **现象**：主窗口扩展名列与「收集扩展名」对话框都没有"输入扩展名并添加"的入口，目录中不存在的扩展名（如 `.xyz`）无从添加。
- **修复**：「收集扩展名」对话框新增「手动添加：输入框 + 添加按钮」行：输入 `.xyz` 或 `xyz` 自动规范化（小写 + 前导点）并做字符校验，加入列表即默认勾选；输入框内 Enter 直接触发添加（子类化拦截，不会误触「确定」）；重复添加自动合并。勾选收集改为直接读列表项（文本 + 复选态），与 `collected` 集合顺序解耦。手动添加为会话内临时生效（进入左栏勾选列表），不写常用扩展名；如需持久化可后续接 `save_whitelist()`。涉及 `gui/dlg_extensions.cpp`。

> **移交备忘（已修复，非缺口）**：扩展名过滤在索引模式下失效（勾选 / 取消勾选不生效）已于 2026-08-28 修复并写入单测（`tests/test_extension_manager.cpp`、`tests/test_indexer.cpp`），二进制 `build/textsearch.exe` 时间戳 08:38:31 起已包含修复。本条不计入上述待改进项，仅避免接手者重复报告。

## 测试

### 单元测试（tests）

基于自研的 header-only 轻量框架 `tests/simpletest.h`（宏 `TEST_CASE` / `REQUIRE` / `REQUIRE_EQ`），框架注释预留了未来迁移到 Catch2 的接口。所有用例通过 `tests/test_main.cpp` 的 `run_all()` 一次性注册并运行。

**运行方式（二选一）：**

```bash
# 方式一：直接跑编译出的控制台程序
./build/tests.exe

# 方式二：通过 CTest（已在 CMakeLists.txt 注册为 textsearch_tests）
ctest --test-dir build --output-on-failure
```

**输出示例：**

```
[PASS] filesystem_basic
[PASS] encoding_utf8_gbk
...
<N> passed, 0 failed        # N = 当前用例数，不随文档写死
```

返回码：全部通过为 `0`，有失败为 `1`，可直接用于 CI 门禁。

**覆盖模块（见 `tests/test_*.cpp`）：**

| 测试文件 | 被测模块 |
|----------|----------|
| `test_filesystem.cpp` | `core/filesystem`（路径 / 目录遍历） |
| `test_text_detect.cpp` | `core/text_detect`（文本类型探测） |
| `test_encoding.cpp` | `core/encoding`（编码识别 / 转换） |
| `test_extension_manager.cpp` | `core/extension_manager`（扩展名管理） |
| `test_matcher.cpp` | `core/matcher`（匹配逻辑） |
| `test_threadsystem.cpp` | `core/threadpool`（线程池） |
| `test_scanner.cpp` | `core/scanner`（扫描器） |
| `test_indexer.cpp` | `core/indexer` + `index_store` + `mmap_index`（索引 / 存储 / mmap 读路径） |

> 注：当前框架不支持按名称筛选单个用例，改动某模块后请整体跑一遍回归。

> **易踩坑：实时 / 索引两条扫描路径都须做扩展名过滤。**
> 搜索有两条路径（`core/scanner.cpp` 路由）：① **实时模式**走 `filesystem::enumerate_files`，在列举文件阶段就按 `ext_set` 过滤；② **索引模式**走 `Indexer::search`，旧实现未接收 `ext_set`，会搜出索引内全部文件，导致「取消勾选的类型仍出现」。任何涉及扩展名过滤的改动，**必须同时改 `indexer.cpp` 的 `search()` 和 `scanner.cpp` 的实时分支**，并补两条路径各自的回归用例（见 `test_extension_manager.cpp` 与 `test_indexer.cpp`），否则极易修了这一边、漏了那一边。

### 性能基准（bench）

`tests/bench_search.cpp` 有独立 `main()`，在 CMake 中单独编译为 `bench` target（不计入 `tests`）。它链接 `psapi` 测量内存峰值，输出 JSON 报告到 stdout：

```bash
./build/bench.exe
```

用于量化搜索 / 索引的真实性能，对照「性能基线」一节验证是否回归。

## 使用说明

### 基本流程

1. **选目录**：在路径输入框中输入或点击「浏览」选择搜索根目录
2. **输入关键词**：在关键词输入框中输入搜索内容
3. **搜索**：点击「搜索」按钮或按 Enter 开始搜索
4. **查看结果**：中栏显示搜索结果列表（文件路径 + 行号 + 命中行文本）
5. **双击跳转**：双击中栏结果行，右栏查看器载入该文件——左侧行号按文件真实行显示，全文命中标黄（黄底黑字，上限 1000 处），并**自动定位到该行的第一个标黄处**；标题显示「文件名 [编码] · N 处命中」（N 与该文件在结果列表中的行数同口径，正则模式带 5 秒计数限时）
6. **自动换行**：右栏标题右端的「自动换行」复选框控制预览折行（默认开，退出记忆）。**折行仅是显示效果，不改变文件真实行数与行号**——折行产生的续行不显示行号，行号始终对应文件真实行
7. **导出结果**：点击「导出结果」，支持 txt 和 csv 两种格式

### 搜索选项

| 选项 | 说明 |
|------|------|
| ☑ 递归 | 递归搜索子目录 |
| ☑ 正则 | 使用正则表达式搜索（ECMAScript 语法） |
| ☑ 大小写 | 区分大小写（不勾选则忽略大小写） |
| ☑ 全词 | 全词匹配（词边界检查） |
| ☐ 强制即时扫描 | 跳过索引走即时扫描（不动索引本身）。用于两种模式结果比对，验证索引准确性 |
| 语言（组合框） | 界面语言：跟随系统 / 简体中文 / English，切换后**重启生效**（文案表在 `core/i18n.cpp`，加语言 = 加一列） |
| 阈值 MB | 大文件流式阈值（默认 50MB）。单文件 < 阈值：整文件读入内存匹配（快路径）；≥ 阈值：mmap 64MB 滑窗流式扫描（内存有界）。该阈值管的是**单文件**内存峰值，与索引大小无关。调高（如 100MB）在大内存机器上能让更多大文件走快路径，但 threadpool 并发下峰值 = 阈值 × 并发线程数，需量力；调低（如 20MB）峰值更可控，适合小内存或目录中大量中等体积文件。 |
| 通配排除 | 通配排除模式（如 `*.log*`，排除含 .log 的扩展名） |
| 黑名单 | 黑名单扩展名（精确匹配，如 `*.tmp`） |

## 命令行用法（textsearch-cli）

无 GUI 环境或需要管道/脚本集成时用 CLI（与 GUI 共用同一套 core，结果口径一致）：

```bash
# 即时搜索（目录里所有 .cpp/.h；缺省 --ext = 常用扩展名全集）
textsearch-cli search D:\projects\myapp "getUserName" --ext=.cpp,.h

# 正则 + 区分大小写 + JSON 输出（管道友好）
textsearch-cli search D:\logs "2026-08-29.*ERROR" --regex --case --json

# 为目录建倒排索引（之后 search 自动走索引模式，毫秒级）
textsearch-cli build D:\projects\myapp --ext=.cpp,.h

# 手动增量更新（GUI/搜索路径本会自动做，CLI 供脚本使用）
textsearch-cli update D:\projects\myapp
```

- 输出格式默认 `路径:行:列:\t命中行`，`--json` 输出对象数组；摘要与进度走 stderr，重定向安全。
- 输出为 UTF-8 字节；Windows 控制台显示中文请先 `chcp 65001` 或使用 Windows Terminal。
- 退出码：`0` 正常执行（含 0 命中），`1` 用法/路径/正则错误。
- `--realtime` 强制实时扫描跳过索引（与 GUI 的「强制即时扫描」一致，用于两模式结果比对）。
- CLI 默认不写日志文件（关键词本就永不落盘）。

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+L` | 聚焦路径输入框 |
| `Ctrl+F` | 聚焦关键词输入框 |
| `Enter`（关键词框内） | 开始搜索 |
| `Esc` | 取消搜索 |
| `↑`/`↓` | 结果列表上下移动 |
| `Enter`（结果列表内） | 右栏打开文件并跳转命中行 |
| `Ctrl+C`（结果列表内） | 复制选中行信息 |
| `Ctrl+E` | 导出结果 |
| `Alt+1`/`2`/`3` | 焦点切换左/中/右栏 |

## 双搜索模式

### 即时扫描模式（临时 / 一次性任务）

- **适用场景**：临时搜索不常查的目录
- **工作方式**：多线程文件级并行扫描，逐文件读取+匹配
- **特点**：无需预建索引，搜索即开始；支持大文件流式内存映射（>50MB 自动切换）

### 索引模式（庞大目录常搜）

- **适用场景**：需要反复搜索的大型代码库 / 文档库
- **工作方式**：先「建立索引」构建倒排索引，后续搜索走索引查询
- **特点**：建库一次，查询极快（<200ms）
- **增量更新（2026-08-30 已接入）**：走索引模式搜索前会自动调用 `Indexer::update_incremental()`，对比 mtime/size 自动处理**新增 / 修改 / 删除**的文件，保证结果不因索引陈旧而漏报或误报。有变更时状态栏会追加提示「索引已同步 N 个变更文件」。
  - **快速预检**：索引通常以 mmap 只读打开（零分配），而写回必须切到 `IndexStore`（全量 load + 全量 save，大索引代价很高）。因此更新前先用 mmap 读索引中的文件清单与当前目录比对，**无变化则直接返回**，不做任何 load/save——常规搜索几乎无额外开销。
  - **开关**：`AppConfig.auto_update_index`（默认 true）。若某超大目录的预检开销不可接受，可置为 false（此时需手动重建索引以防结果陈旧）。
  - **单测**：`indexer_update_incremental`、`indexer_incremental_delete`、`indexer_incremental_fastpath_and_counts`。
- **建立索引**：点击「建立索引」按钮，在当前路径下构建索引
- **自动路由**：搜索时自动检测是否有可用索引，有则走索引模式，无则走即时扫描
- **强制即时**：勾选选项区的「强制即时扫描」复选框，即可跳过索引走即时扫描（**不影响索引本身，索引仍保留**）。勾选后状态栏显示「（实时扫描）」，取消勾选后自动恢复索引模式。该设置会记住。
  - **主要用途：验证索引模式的准确性**——同一关键词分别用「勾选」和「不勾选」各搜一遍，两边结果集应当完全一致；若不一致即说明索引模式有漏报/误报（见「技术交底书 → 五、正确性验证待办」）。
- **扩展名过滤**：索引模式下，搜索时按当前勾选的扩展名集合过滤（`Indexer::search` 接收 `ext_set`）。但索引**建库时**只收录了当时纳入的类型——若某个类型当时没建进索引，勾了也搜不到，需点「建立索引」重建。
  - ⚠️ **索引外类型有告警**：勾选了建索引时未纳入的类型时，状态栏会明确告警（这些文件搜不到；如需包含请重建索引）。
- **重建索引**：当扩展名纳入范围有重大变化，或怀疑索引损坏时，重新「建立索引」（文件增删改已由增量更新自动覆盖，无需因此重建）。

### 何时用哪个

| 场景 | 推荐模式 |
|------|----------|
| 搜一次就完 | 即时扫描 |
| 反复搜同一目录 | 先建索引，后续秒回 |
| 目录很小（<100文件） | 即时扫描即可 |
| 目录庞大（数千文件） | 建索引 |
| 文件经常变更 | 建索引即可（搜索前自动增量更新） |

## 扩展名管理

- **预置常用扩展名**：内置常见文本扩展名（`.py .c .cpp .h .hpp .cc .bas .ini .yaml .yml .json .toml .txt .md .xml .html .css .js .ts .log .conf .cfg`）——**仅作兜底**，实际生效值来自配置文件，可自行修改
- **编辑常用扩展名**：点「收集扩展名」→「编辑常用扩展名…」，每行一个扩展名（也支持逗号/空格分隔），含「重置为默认」。也可直接用记事本编辑 `textsearch.ini` 的 `[Extensions] common`
- **收集扩展名**：点击「收集扩展名」按钮，递归扫描目录所有扩展名，预勾选已知文本类，确认后加入待搜对象
- **常用扩展名命中项排在列表最前**：收集完成后，**命中常用扩展名（即默认已勾选）的扩展名排在最前面**，组内按字母序，无需在长列表中逐个查找哪些被勾选了
- **抽样验证**：添加新扩展名时自动抽样 1~3 个文件，逐个验证是否为文本（NUL 字节探测）
- **常用扩展名位置**：`%LOCALAPPDATA%/TextSearch/textsearch.ini` 的 `[Extensions] common`（见「配置（INI）」一节）
  ```ini
  [Extensions]
  whitelist = .py, .c, .cpp, .txt, .md
  ```

## 编码自动识别

工具自动识别以下编码（无需手动指定）：

1. UTF-8 BOM（`EF BB BF`）→ UTF-8
2. UTF-16 LE BOM（`FF FE`）→ UTF-16 LE
3. UTF-16 BE BOM（`FE FF`）→ UTF-16 BE
4. 严格 UTF-8 解码 → UTF-8
5. GBK（CP 936）解码 → GBK
6. 无 BOM 但"0x00 间隔出现" → 疑似 UTF-16 LE
7. 都失败 → 跳过

二进制文件通过 NUL 字节探测自动跳过（前 8KB 含 NUL 即判定为二进制）。

## 索引技术

### 倒排索引设计

- **存储格式**：简易文件式 KV 存储（`<root>/.textsearch/index.lmdb`），5 个命名子库：
  - `meta`：版本/根路径/构建时间/文件数/词项数/扩展名集合
  - `paths`：路径 → file_id 反查
  - `files`：file_id → FileRecord（路径 + mtime + size + 编码）
  - `postings`：词项 → 命中列表 (file_id, line, col)
  - `trigrams`：3-gram → file_id 集合（正则加速用）
- **完整性校验**：FNV-1a 32 位校验和，损坏自动检测并删除降级
- **mmap 零拷贝读**：搜索时索引文件经 `CreateFileMapping + MapViewOfFile` 映射进进程地址空间（`core/mmap_index.cpp`），只建轻量查找表（key 拷贝、value 不拷贝），`get()` 返回指向 mmap 内存的裸指针。相比传统「整文件读进 `unordered_map`」（每条 value 一次堆分配，50MB 索引 = 数百万次 new），open 阶段零 value 分配，首次搜索与冷启动显著加速；OS page cache 让重启后二次 open 近零成本。
- **posting list 压缩**：varint + zigzag-delta 编码（`[count:varint][Δfid:zigvarint][Δline:zigvarint][Δcol:zigvarint]`），相比裸 uint32 三元组（12B/项）体积省 60-75%，连带减小索引文件、load 解析时间与常驻内存。

### 词项切分

- **ASCII 标识符**：`[A-Za-z0-9_]+` 作为词项
- **中文 bi-gram**：连续 CJK 字符两两组合（如"中文文本" → 中文/文文/文本）
- **大小写归一**：postings 词项统一小写存储，查询时也小写匹配

### 正则 trigram 加速

- 索引期：每文件每行提取字符级 3-gram，文件级去重
- 查询期：从正则提取长度≥3 的字面量段，取 3-gram 查交集得候选文件集，逐文件精筛
- 无字面段（如 `\d+`）→ 回退全文件扫描

## 配置（INI）

所有设置集中在一处：**`%LOCALAPPDATA%/TextSearch/textsearch.ini`**

```ini
[Search]
threshold_mb = 50
last_root = 
recursive = 1
use_regex = 0
ignore_case = 0
whole_word = 0
auto_update_index = 1
max_results = 100000
search_timeout_sec = 120
watchdog_sec = 60
history_max = 10

[Window]
x = 0
y = 0
width = 1200
height = 800
maximized = 0
left_col_w = 0
right_col_w = 0
dark = 0
ui_lang = system
preview_word_wrap = 1

[Privacy]
log_enabled = 1
log_redact_paths = 1

; 常用扩展名（常见文本文件类型）。收集扩展名时命中此列表的项会默认勾选并排在列表最前面。
; 可在此直接编辑，或在「收集扩展名 → 编辑常用扩展名…」中修改。
common = .py, .c, .cpp, .h, .hpp, .cc, .bas, .ini, .yaml, .yml, .json, .toml, .txt, .md, .xml, .html, .css, .js, .ts, .log, .conf, .cfg
```

> **术语说明**：界面与配置文件一律称「常用扩展名」（"白名单"易被误解为屏蔽名单/安全白名单）。
> 代码内部标识符沿用 `whitelist`（`load_whitelist` / `save_whitelist` / `default_whitelist`），
> 英文 allow-list 语义无歧义，二者指向同一份数据。配置 key 为 `common`；
> 读取时若 `common` 为空会回退读旧 key `whitelist`。

**为什么用 INI 而不是 JSON**：配置文件的读写性能不是考量（KB 级、启动读一次），**人手可编辑性**才是——INI 每行 `key = value`、支持 `;` 注释，写错一行只影响一项；JSON 少个引号整份就解析失败。文件为 **UTF-8 带 BOM**，可直接用记事本双击编辑中文不乱码。

**常用扩展名不再写死在程序里**：内置默认值仅在配置文件缺失时作为兜底。实际生效值来自 ini 的 `[Extensions] common`，可在「收集扩展名」对话框点**「编辑常用扩展名…」**修改（每行一个扩展名，支持逗号/空格分隔，含「重置为默认」），也可直接编辑 ini 文件。

> **旧配置自动迁移**：从早期版本（使用 `settings.json` 与 `whitelist.json`）升级时，首次启动会自动把旧配置迁移进 `textsearch.ini`，旧 json 文件保留不动。

> **注意**：`textsearch.ini` 由程序在设置变更/退出时整体重写，**ini 内手写注释与自定义排版不会保留**。需要备注请写在外部笔记。

## 文件位置

| 项目 | 路径 |
|------|------|
| **配置（含常用扩展名）** | `%LOCALAPPDATA%/TextSearch/textsearch.ini` ← 所有设置集中于此 |
| 日志 | `%LOCALAPPDATA%/TextSearch/textsearch.log`（5MB × 3 文件轮转） |
| 索引 | `<搜索目录>/.textsearch/index.lmdb` |
| 常用扩展名（旧版，已合并进 ini） | `%LOCALAPPDATA%/TextSearch/whitelist.json` |

## 隐私与日志

提 issue 贴日志是常见操作，所以日志写什么必须说清楚。当前行为：

| 数据 | 是否落盘 | 说明 |
|------|----------|------|
| **搜索关键词 / 正则** | **从不** | 可能是密码、API Key、身份证号。只保留在内存里供本次会话的高亮与导出，**没有开关可开**——这条是硬保证 |
| 配置文件里的关键词 | **从不** | `textsearch.ini` 不再有 `last_pattern` 项；旧版本已经写进 ini 的那条会在下次保存时自动清除 |
| 文件路径 | **脱敏后** | 折叠成 `...\末级目录\文件名`，用户名、上层项目目录被抹掉。例：`C:\Users\alice\Documents\secret\notes.txt` → `...\secret\notes.txt` |
| 索引文件 | 完整路径 | 索引必须按真实路径工作，无法脱敏。**索引位于你自己的搜索目录内**（`<搜索目录>/.textsearch/`），不要随日志一起外发 |

需要完整路径排查问题时，改 ini 的 `[Privacy]` 节（改完重启生效）：

```ini
[Privacy]
log_enabled = 1          ; 0 = 完全停止写日志（一个字节都不落盘）
log_redact_paths = 1     ; 0 = 日志里保留完整路径（贴日志前记得先清空）
```

> 两个开关默认都是 `1`（安全默认）。**贴日志前建议先确认 `log_redact_paths = 1`。**

## 正则限制声明

- **引擎**：`std::wregex`（ECMAScript 语法）
- **单行模式**：`^`/`$` 匹配行首行尾，`.` 不匹配换行符，不支持跨行匹配
- **预览不高亮**：正则模式下右栏预览只滚动定位到命中行，不做字面量高亮（正则命中串无法从 pattern 直接还原）
- **不支持特性**：
  - Possessive 量词 `++`
  - Atomic 组 `(?>...)`
  - Lookbehind `(?<=...)`
- **ReDoS 防护**：自动检测嵌套量词（如 `(x+)+`）并警告性能风险
- **超时**：单文件正则匹配超时（默认 120 秒，`SearchOptions::search_timeout_sec`）——到时停止该文件剩余部分，已找到的结果照常返回

## 技术栈

| 项 | 决定 |
|----|------|
| 语言 | C++17 |
| GUI | Win32 原生 API（零依赖） |
| 构建 | CMake 4.2.3 + MinGW Makefiles（无 CMake 时可直接 g++，见「构建方法」） |
| 编译器 | MinGW-w64 GCC 15.2.0（ucrt-posix-seh，任务书验证版本）；当前开发机实际用 WinLibs GCC 16.2.0 |
| 正则 | `std::wregex`（宽字符层） |
| 并发 | 线程池，文件级并行（`std::thread`） |
| 大文件 | 内存映射 MMF（64MB 窗口滑动） |
| 编码 | UTF-8 / GBK / UTF-16 自动识别 |
| 单测 | 轻量级 header-only 框架 |
| 运行目标 | Windows 10+（UCRT 运行时） |

## 性能基线

基于参考目录（约 100 文件 / 混合编码）：

| 指标 | 基线 | 备注 |
|------|------|------|
| 即时模式全量完成 | < 5s | 子串模式，默认并发 |
| 索引模式建库 | < 10s | |
| 索引模式单次查询 | < 200ms | |
| 大文件(>50MB)流式内存峰值 | < 100MB | |
| 即时模式峰值内存 | < 512MB | |
| UI 帧率（搜索中） | ≥ 30fps | 不卡顿 |

运行 `./build/bench.exe` 获取实际测量数据（JSON 格式输出到 stdout）。

## 技术交底书

> 本节是**内部技术档案**，记录设计决策的理由、已落地的性能优化、已知约束与未来升级方向。
> 目的：防止时间久远后遗忘「为什么这么做」和「下一步该做什么」。
> 最后更新：2026-08-29（性能优化 v1→v5 完成，mmap 零拷贝落地后）

### 一、架构决策记录（ADR 摘要）

| # | 决策 | 理由与权衡 |
|---|------|-----------|
| A1 | **索引存储自研 `IndexStore`，不用 LMDB** | 任务书原设计用 LMDB，但实测未引入。自研简易文件式 KV（`[magic][version][checksum][num_dbs]...`）零依赖、格式可控、便于加压缩与 mmap。代价：无 MVCC / 无并发写，写路径需独占。 |
| A2 | **读路径双轨：`MmapIndexReader`（只读/零拷贝） + `IndexStore`（可读写）** | `IndexStore::open()` 把整个索引读进内存 map（每条 value 一次堆分配）。搜索只需读 → 用 mmap 零分配；建库/增量需写 → 用 `IndexStore`。`Indexer::open()` 优先 mmap，失败回退 `IndexStore`；`update_incremental()` 开头把 reader 切回 store（要写回）。`get_blob()` 统一封装两条路径。 |
| A3 | **建库 `file_id` 用 `atomic<uint32_t>` 分配，允许空洞** | 并行分片下无法预知哪些文件是二进制（判二进制要读文件头，在 worker 内）。二进制文件也消费一个 id → 产生空洞。但 `file_id` 仅作不透明主键，`get_all_file_ids` / 增量更新只遍历**实际存储**的 id 集合，空洞无害；`meta.nfiles` 记实际入库数而非最大 id。 |
| A4 | **索引文件格式版本化（v1 → v2）** | v2 引入 varint+delta 压缩后，旧格式若被新反序列化器读取会解出垃圾结果（静默错误，比崩溃更危险）。故 `IndexStore` 文件头 version 升到 2，`load()` 遇到 `version != 2` 直接拒绝 → `index_exists` 返回 false → 自动逼用户重建。**改序列化格式时必须同步升版本。** |
| A5 | **默认静态链接（2026-10 起）；动态仅作可选项** | 原判断「动态 650KB 更优」被本机现实推翻：`C:\Windows` 残留的旧版 `libstdc++-6.dll` 与 GCC 16.x 不兼容，动态 exe 启动即 `0xC0000139`。现默认 `-static`（约 1.9MB，零依赖），CMake 由 `TEXTSEARCH_STATIC` 控制（默认 ON）；确要动态必须用工具链配套 DLL。详见「构建方法」。 |
| A6 | **core/ 无窗口；平台差异收口在 core/port/（2026-10-26 起）** | core 不含任何窗口/消息代码。原先 6 处直接调用 Win32 的点（MMF/GBK 转换/长路径/隐藏属性等）已全部收口到 `core/port/`：`port_win.cpp` 为 Windows 实现，`port_stub.cpp` 为可移植降级实现（内置 UTF-8 编解码 + fstream 窗口预读，GBK 暂不支持）——`-DTEXTSEARCH_PORT_STUB` 全量编译通过即接口完备性证明。CLI（`cli/`）随 core 具备跨平台编译面，POSIX 真机验证待做。 |
| A7 | **UI 需求不改 core；确需新数据走接口/回调** | 项目硬性约定（见「UI 改进指引」）。本次三项界面缺陷修复全部只在 `gui/` + 调用既有 core 接口完成，core 零改动。后续性能改造则相反：性能问题在 core，UI 只做呈现。 |

### 二、性能优化演进（2026-08-28 ~ 08-29）

从「4.36MB 静态包 + 搜索启动卡死」到「650KB + 启动秒起 + 查找飞快」，共 10 项改动。**记录根因以免重复排查**：

| 轮次 | 问题现象 | 根因 | 解法 | 收益 |
|------|---------|------|------|------|
| v1 | 路径框失焦后 UI 卡住（标题栏「未响应」） | `update_index_status()` 在 **UI 线程**同步调 `index_exists()`，该函数要读索引文件算 checksum | 异步后台线程 + `PostMessage` 回包；原子代际号 `index_check_gen_` 防止旧结果覆盖新结果 | 消除 UI 卡顿 |
| v2 | **有索引时点搜索像卡死** | `on_search()` 在 **UI 线程**同步调 `index_exists()` 预判模式 | 模式判定移到 worker 线程，回传 `WM_SEARCH_MODE`；UI 先显示「正在扫描…」，回包后补模式 | 搜索启动不再卡顿 |
| v2 | exe 4.36MB、冷启动慢 | 静态链接 libstdc++ | 改动态链接 + `-s` strip | **4.36MB → 637KB** |
| v2 | `index_exists` 重复校验开销大 | 每次调用都全文件 checksum | core 层加**进程内 mtime+size 缓存**（mutex + unordered_map），文件未变复用上次结果 | 重复调用近零开销 |
| v3 | 每次搜索都重新 load 整个索引 | `scanner.cpp` 每次 `Indexer idx; idx.open()` | `Indexer::open_cached()` 按 (root, mtime) 缓存已打开的 Indexer 实例 | **重复搜索省掉全文件 load** |
| v3 | 每 term 取 posting list 都拷贝整条 vector | `IndexStore::get()` 是 `out = kit->second`（值拷贝） | 新增指针重载 `const vector<uint8_t>* get(db,key)`，热路径调用点全改用指针 | 免每 term 拷贝 |
| v3 | 建库慢 | `build()` 串行 `for path : filtered` | ThreadPool 文件级并行，每 worker 产出 `BuildFile`（不碰 store_ 避免并发写），主线程串行合并 | 建库 **2-4×** |
| v4 | 索引文件大、load 解析慢 | postings 存裸 uint32 三元组（12B/项） | varint + zigzag-delta 压缩 | 体积 **-60~75%** |
| v4 | 建库每文件开两次文件 | `read_head_bytes`(8KB) + `read_text`(全文) | `read_file_bytes` 单次读全文，head 取前 8KB 复用 | 每文件省一次 open+read |
| v5 | 首次搜索 / 冷启动 open 慢 | `IndexStore::open` 把每条 value 读进 vector = 数百万次 `new`，时间被**分配器**吃掉而非 I/O | 新增 `MmapIndexReader`：mmap + 轻量查找表（key 拷贝、value 只存 offset+len），`get()` 返回 mmap 内裸指针 | **open 零 value 分配** |

### 三、已知约束与易踩的坑

1. **`index_exists()` 是双重读**：内部 `store.open()`（已完整 load）后 `verify()` 又 load 进临时对象算 checksum——**同一文件读两遍**。已用 mtime 缓存缓解（第二次起跳过），但首次调用仍双读。若要彻底优化：把 checksum 校验合并进 `load()`（load 时顺带算 FNV 存成员），`verify()` 直接读成员值。
2. **zigzag 编码的负数掩码**：`zig_enc` 必须写成 `(u << 1) ^ (-(u >> 31))`。若误写成 `^(u >> 31)`，负数 delta 只会翻转最低位而非全部位 → 解码错乱。**该 bug 的隐蔽之处**：全正 delta（英文场景）能蒙混过关，只有中文 bi-gram 跨行 col 递减（负 delta）才暴露，靠 `indexer_search_chinese_substring` 单测抓出。改压缩代码务必跑全量中文用例。
3. **改序列化格式必须升 `IndexStore` 文件版本**（见 A4），否则旧索引被静默误读。
4. **build 期间必须释放 mmap**：`build()` 开头 `reader_.reset()`，否则 mmap 句柄占用导致删除索引文件失败（Windows 上无法删除被映射的文件）。
5. **动态链接依赖 3 个 DLL**：`libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll`，必须与 exe 同目录。
6. **`DwmSetWindowAttribute` 需显式 `-ldwmapi`**（新工具链下必需，见「构建方法」）。
7. **扩展名过滤有两条路径**：改动必须同时覆盖 `indexer.cpp` 的 `search()` 与 `scanner.cpp` 的实时分支，并补两侧回归用例（见「测试」节警告）。
8. **⚠️ mtime 必须用 `file_mtime_unix()` 计算（core/util.h）**。绝不要写
   `ftime - file_time_type::clock::now() + system_clock::now()` —— 该式**每次调用采样两次 `now()`**，
   微秒级间隔会让秒级截断在秒边界抖动 ±1 秒。后果：索引刚建完就被误判「文件已修改」，
   于是**每次增量更新都触发一次无谓的全量 load + 重写**（大索引上代价极高），且误判方向随机、
   极难复现（本坑在 2026-08-30 接入增量更新时由新增单测暴露，已统一修复）。
   索引侧（build / `index_single_file` / `update_incremental`）与扫描侧（`mtime_filter`）
   **必须共用同一函数**，否则两侧秒值可能差 1，导致「刚重建的文件仍被判为已修改」。
9. **升级 `file_mtime_unix` 后旧索引可能一次性全量重建**：旧索引里存的 mtime 由旧公式算出，
   与新值可能差 1 秒 → 首次增量更新会把全部文件判为「已修改」并重建一遍。属一次性、结果正确，
   大索引上耗时会明显，属预期行为。

### 四、未来升级方向（按 ROI 排序）

| 优先级 | 方向 | 状态 | 收益 | 代价 / 说明 |
|--------|------|------|------|------------|
| ★★★ | **Roaring bitmap 替代 `std::set<uint32_t>` 做 trigram 候选交集** | 未做 | 正则候选筛选快一个数量级（分段位图 + SIMD 求交） | 中等。手写简化版 Roaring 可保持零依赖。当前 `set_intersection` 是 O(n) 红黑树操作 |
| ★★★ | **查询结果 LRU 缓存**（key = pattern+opts+root+ext_set+索引 mtime） | 未做 | 完全相同查询秒回；适合「搜一次切走又回来」 | 中等。要处理失效（索引 mtime 变、ext_set 变） |
| ★★ | **`index_exists` 单次 load**（checksum 合并进 load） | 未做（已用 mtime 缓存缓解） | 首次检测省一半读 | 小。见「已知约束 1」 |
| ★★ | **`enumerate_files` → `FindFirstFileExW` + `FIND_FIRST_EX_LARGE_FETCH`** | 未做 | 大目录（上万文件）枚举快 3-5×，减少系统调用 | 小-中。Win10+ 可用；影响实时扫描与建库枚举 |
| ★★ | **实时模式 I/O 并发**：I/O 阶段 oversubscribe（线程数 2× 核数），匹配阶段收回；或 `IOCP`/overlapped 真异步 | 未做 | 冷读目录的实时扫描更快 | 中。当前 threadpool 小文件阶段是 I/O bound、CPU 空转 |
| ★ | **LSM / append-only 段式存储** | 未做 | 增量更新只追加不重写整个索引文件 | 大。当前 `update_incremental` 提交时 `save()` 重写全文件；只在「常搜 + 常改」场景才值 |
| ★ | **SIMD tokenize / 文本扫描** | 未做 | 词边界扫描 16/32 字节一次 | 中。边际收益，但技术观感强 |
| — | **正则引擎换 RE2** | 不推荐 | 线性时间 + 真 ReDoS 免疫 | **违背零依赖气质**。当前 `std::wregex` + trigram 预筛已够用，「无字面段」回退全扫是唯一慢路径。除非正则成为高频路径否则不动 |
| — | **term Bloom filter / `unordered_map` 换 `std::map`** | 不推荐 | 省 map 查找 | 整个索引已在内存，bloom 省的是 O(log n) 红黑树查找，收益有限；换 `unordered_map` 反而更直接 |

**判断原则**：上表 ★★★ 两项做完，这个工具的性能就基本到头了，剩下都是边际收益。**不建议继续堆性能**——应转向正确性验证（见下）与可移植性。

### 五、正确性验证待办（当前最高优先级）

用户体感「启动秒起、建索引快、查找快」，但**查找结果是否「尽全」（零漏报）尚未长期验证**——这决定工具可不可信，优先级高于任何性能优化。

**已有保障**：`indexer_vs_realtime_consistency` / `indexer_vs_realtime_chinese_consistency` 两个单测已在做「索引模式 vs 实时模式结果比对」。

**建议的补充验证矩阵**（用已知语料如 `core/` 目录本身，交叉核对索引模式与实时模式结果集是否一致）：

| 关键词类型 | 例子 | 关注点 |
|-----------|------|--------|
| 纯 ASCII 标识符 | `Indexer`、`deserialize_postings` | 词项切分边界 |
| 中文子串 | `张三丰`、`中文` | bi-gram 切分 + 邻接验证 |
| 中英混排 | `索引index` | 跨字符集切分 |
| 正则（有字面段） | `foo.*bar` | trigram 候选筛选是否漏 |
| 正则（无字面段） | `\d+` | 回退全扫路径 |
| 大小写 / 全词 | `Index` vs `index` | 归一与词边界 |

**若发现索引模式漏报，最可能的两处**：① 中文 bi-gram 的**邻接验证**（`search_substring` 多 token 分支）；② 正则的 **trigram 候选筛选**。这两处是「零漏报」设计里最容易出偏差的地方。发现问题应立即补成单测固化。

### 六、开源定位与跨平台路径（讨论结论）

**Linux 生态现状（调研结论）**：
- **实时一次性 CLI 搜索**：`ripgrep` 是绝对标杆（SIMD + mmap + 高度调优正则）。**手搓实时扫描想赢 rg 不现实**，那是数年的 SIMD 调优成果。但 rg 无持久索引、无 GUI、英文优先（CJK 当字节流，不做 bi-gram 词边界）。
- **GUI + 持久索引 + 内容搜索**：`Recoll` 一家独大但臃肿（解析 PDF/DOCX/邮件、依赖 Xapian）——正是本项目反感的「功能多格式全」路线。`Tracker`/`Baloo` 是与桌面环境绑定的重量级索引器。
- **CJK 优先 + 索引 + GUI**：**基本空白**。

**结论**：Linux 有更强的实时工具（rg），但「**持久索引 + 纯粹不臃肿 + CJK 优先 + 有 GUI**」这个生态位**两个平台都没人占**。这恰恰是开源的理由。

**差异化卖点（应写进开源 README 的开头）**：
- 纯文本专注（不追求解析 PDF/DOCX，不追格式全）
- 双模式：实时（临时搜）+ 持久倒排索引（反复搜同一目录，第二次起远超 rg 的全量扫描）
- CJK 一等公民：bi-gram 切分 + 邻接验证，不是简单的字节流匹配
- 零依赖原生 GUI，不收费、无捆绑

**若追求受众面，优先级高于继续压性能的三件事**：
1. **跨平台**：`core/` 已平台无关，只有 `gui/` 是 Win32。加一个**命令行入口**（`textsearch-cli`）或跨平台 GUI 层，受众立刻翻倍，且命令行版可直接与 rg 对比。
2. **文档语言**：当前全中文注释与 README，国际受众看不懂。核心文件加英文头注释 + 英文 README。
3. **README 定位改写**：现在是「移交清单」口吻（面向接手者），开源要改成「为什么这个不一样」——把上述差异化卖点打头。

## 语言支持范围

- **界面**：目前简体中文 / English（选项条切换，重启生效）；文案表在
  `core/i18n.cpp`，加语言 = 加一列（架构位已备）。
- **实时扫描**：解码后的逐字符子串匹配，**任何语言**的文本都能搜。
- **索引模式**：当前 tokenizer 只切分 **ASCII 单词与 CJK 表意文字**——
  带音标的拉丁文（café/ü/ñ）、西里尔、希腊、假名、谚文等字符会被当作
  分隔符丢弃，含这些字符的词暂不可索引（实时模式不受影响）。
  Unicode 感知 tokenizer 是规划中的架构位（`tokenize_line` 单点改造）。

## 许可

MIT License
