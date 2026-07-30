# 阶段状态

## 当前结论

```text
PHASE_1_FOUNDATION_IMPLEMENTED_LINUX_VALIDATION_BLOCKED
```

- 当前阶段：Phase 1B 封板审计
- 实现状态：基础工程、公共类型、配置、同步日志、CLI、测试、脚本和 Linux CI workflow 已实现
- Linux 验证：blocked，workflow 尚未 push、运行或取得成功证据
- 补充验证：Windows Visual Studio 2022 x64 Debug/Release 回归结果记录在下文
- 日期：2026-07-30（Asia/Shanghai）
- 下一阶段：Phase 2，未开始

只有真实 Linux Debug、Release、CTest 和 smoke 全部通过后，才能把标记改为 `PHASE_1_FOUNDATION_COMPLETED`。

## Git 状态

| 项目 | 结果 |
|---|---|
| 当前分支 | `phase/1-foundation` |
| Phase 0 提交 | `5fbcec0 docs: complete phase 0 architecture design` |
| origin | `https://github.com/realme-max/IndustrialAIServiceFramework.git` |
| 开始时工作区 | clean |
| commit | 未执行 |
| push | 未执行 |
| PR | 未创建 |

Phase 0 提交没有 amend、reset、rebase 或重写。

## Phase 总览

| Phase | 名称 | 状态 | 验证结论 |
|---:|---|---|---|
| 0 | 只读调查与架构设计 | completed | 已提交为 `5fbcec0` |
| 1 | C++17 基础工程与公共基础设施 | implemented and audited / Linux blocked | Linux CI workflow created；run pending |
| 2 | Socket、epoll 与 EventLoop | planned | 未开始 |
| 3 | HTTP 协议与路由 | planned | 未开始 |
| 4 | 线程池与任务系统 | planned | 未开始 |
| 5 | 插件系统 | planned | 未开始 |
| 6 | 定时器与任务超时 | planned | 未开始 |
| 7 | 异步日志与配置扩展 | planned | 未开始 |
| 8 | 压力测试与工程完善 | planned | 未开始 |
| 9 | 真实工业视觉插件预留 | planned | 未开始，需用户明确批准 |

## Phase 1 已实现

### 构建

- CMake minimum 3.22，项目版本 0.1.0。
- C++17 且关闭 GNU extensions。
- target 级 include、link、compile features 和 warning。
- `configure_file` 生成 build-tree 版本头，不含时间、用户、机器、路径或 Git hash。
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON`，生成文件由 `.gitignore` 排除。
- 只声明 build-tree include 路径；没有尚未实现的 install/export 接口。

Targets：

```text
iaisf_core (STATIC)
  -> nlohmann_json::nlohmann_json

iaisf_server
  -> iaisf_core

iaisf_tests
  -> iaisf_core
  -> GTest::gtest
  -> GTest::gtest_main
```

CTest 还注册 `iaisf_server_version` 和 `iaisf_server_example_config`。

### 依赖

- nlohmann/json `v3.11.3`
- GoogleTest `v1.15.2`
- 默认固定 tag FetchContent
- `IAISF_USE_SYSTEM_DEPS=ON` 时使用 `find_package`
- `IAISF_BUILD_TESTS=OFF` 时不获取 GoogleTest
- 第三方源码仅存在忽略的 build 目录，没有加入 Git

### Error/Result

- `ErrorCode`：InvalidArgument、ConfigError、IoError、InternalError。
- 稳定小写字符串及未知值 fallback。
- Error 保留公开可变字段，非空不是类型级永久不变量。
- 项目生产路径使用 `make_error`；构造、工厂和 Result failure 边界归一化空 message。
- `Result<T>` 使用 `std::variant`，支持 move-only 和非默认构造类型。
- `Result<void>` 表达无返回值操作。
- API 误用抛 `std::logic_error`；普通失败不使用异常控制流。

### AppConfig

结构：

```text
service.name
runtime.worker_threads
runtime.task_queue_capacity
logging.level
```

默认值：

- service name：IndustrialAIServiceFramework
- worker threads：硬件并发度夹在 1—256
- task queue capacity：1024
- log level：info

规则：

- service name 最大 128 bytes，非空、非全空白、无控制字符；
- worker threads 1—256；
- task queue capacity 1—1,000,000；
- log level 严格小写 `trace/debug/info/warn/error`；
- 缺失分组/字段使用默认值；
- 所有未知顶层和组内字段返回 ConfigError；
- JSON 语法、类型和值错误返回 ConfigError；
- 文件不存在、非普通文件、无法打开/读取返回 IoError；
- 错误不输出完整配置或不必要的绝对路径。

### 同步日志

- `ILogger` 最小虚接口。
- `ConsoleLogger` 注入 `std::ostream&`，不拥有流。
- 支持五级阈值和运行时更新。
- mutex 保护阈值和完整行写出。
- UTC 毫秒时间、level、component、message。
- 换行和控制字符被清洗。
- 没有后台线程、队列、文件、轮转或压缩。

### Application

- `--version`：固定输出并返回 0，不加载配置。
- `--help`：输出 usage 并返回 0。
- `--config <path>`：加载/验证配置、初始化同步 logger、输出验证日志并退出。
- 无参数：输出版本和 Phase 1 提示并退出。
- 未知、冲突、缺失/多余参数：输出错误和 usage，返回 2。
- 配置失败：返回 1，不抛出预期异常。
- `main.cpp` 只收集参数、构造 Application、返回退出码并处理不可预期异常。

### Phase 1B 审计结论

- Error：公开字段允许构造后被清空；改为准确记录“工厂和 Result 边界归一化”，并增加清空后进入 Result 的测试。
- Result：通过编译期断言固定 value/error 的 `&`、`const&`、`&&` 返回类别；move-only 和 rvalue move 测试通过。
- 版本头：只生成到 build tree，源码树没有 `version.hpp`；应用和测试均 include `iaisf/version.hpp`。
- CMake：移除未实现的 `INSTALL_INTERFACE`；未发现全局 include/link/definitions、硬编码 toolchain 或本机路径。
- AppConfig：补充 float/string/null/bool、超大整数和 UTF-8 byte 上限测试；JSON 库异常在配置加载边界转换为 ConfigError。
- Logger：Linux 使用 `gmtime_r`、Windows 使用 `gmtime_s`；注入 stream 必须比 logger 生命周期长，且不能在 logger 外并发直写。
- Application：help/version 不读配置，config 错误返回 1，参数错误返回 2，不启动网络或线程。
- CTest：仍为 41 项 unit 和 2 项 smoke；路径不依赖 CTest 工作目录。审计发现原进程内序号在并行 CTest 进程间可能重名，现改为原子创建唯一系统临时目录并由 RAII 删除；Windows Debug `ctest -j 8` 复验为 43/43。
- Shell：三个脚本通过 `bash -n`；本机未安装 shellcheck，未自动安装。

## 环境调查

### Windows

| 项目 | 结果 |
|---|---|
| OS | Windows 10 家庭中文版 10.0.19045 x64 |
| CPU | Intel Core i5-12400，12 logical processors |
| RAM | 约 15.75 GiB |
| Git | 2.53.0.windows.1 |
| CMake/CTest | 4.1.0 |
| Visual Studio C++ | MSVC 19.38.33130.0，VS 2022 |
| PATH g++ | MinGW-w64 GCC 5.3.0 i686，未用于验收 |

### Linux/WSL

- `wsl.exe --status`：exit 50。
- `wsl.exe --list --verbose`：exit 1，只返回安装/用法信息。
- 无可运行发行版，无法取得 uname、os-release、Linux compiler 或 Linux CMake。
- Docker、Podman 均未发现。
- 未自动安装 WSL、发行版、Docker 或系统包。

## 真实执行结果

### Linux Debug

| 项目 | 结果 |
|---|---|
| configure | not run |
| build | not run |
| CTest | not run |
| 测试总数/通过/失败 | unknown / unknown / unknown |
| warning | unknown |

### Linux Release

| 项目 | 结果 |
|---|---|
| configure | not run |
| build | not run |
| CTest | not run |
| 测试总数/通过/失败 | unknown / unknown / unknown |
| warning | unknown |

### Linux smoke

| 项目 | 结果 |
|---|---|
| `--version` | not run |
| example config | not run |
| 实际退出码 | not available |

### Windows/MSVC 补充结果

| 配置 | Configure | Build | CTest |
|---|---|---|---|
| Debug | pass | pass | 43/43 pass，0 failed |
| Release | pass（同一 multi-config tree） | pass | 43/43 pass，0 failed |

上述结果为 2026-07-30 Phase 1B 干净回归。最终项目自身 C++ 编译 warning 为 0。Visual Studio 的本机 vcpkg applocal 集成在 executable target 后尝试调用缺失的 `pwsh.exe`，产生非致命诊断；退出码、产物和 CTest 均成功。这不是项目 CMake 发起的依赖模式，也不是 Linux 结果。

Windows 首次配置使用默认 FetchContent 模式并访问 GitHub，固定版本的 nlohmann/json 与 GoogleTest 获取成功；没有启用系统依赖模式。第三方源码和产物只位于被忽略的 `build/windows-vs2022/_deps`。

### GitHub Actions Linux CI

- workflow：`.github/workflows/linux-ci.yml`
- runner：固定 `ubuntu-24.04`
- jobs：`linux-debug`、`linux-release`
- Debug：环境记录、configure、build、CTest
- Release：configure、build、CTest、`--version` 和示例配置 smoke
- 权限：`contents: read`
- 状态：workflow 已创建，但未 commit、push 或运行；没有 Linux PASS 证据

手工 Windows Release smoke：

| 命令 | 退出码 | 输出摘要 |
|---|---:|---|
| `iaisf_server.exe --version` | 0 | `IndustrialAIServiceFramework 0.1.0` |
| `iaisf_server.exe --config config/iaisf.example.json` | 0 | INFO 日志包含 `configuration validated` |
| `iaisf_server.exe` | 0 | 版本和“不启动网络”提示 |
| `iaisf_server.exe --unknown` | 2 | argument error 和 usage |

## 实际命令

主要命令：

```powershell
git status --short
git branch --show-current
git log -1 --oneline
git remote -v
git switch -c phase/1-foundation
wsl.exe --status
wsl.exe --list --verbose

cmake -S . -B build/windows-vs2022 `
  -G "Visual Studio 17 2022" -A x64 `
  -DIAISF_BUILD_TESTS=ON `
  -DIAISF_USE_SYSTEM_DEPS=OFF
cmake --build build/windows-vs2022 --config Debug --clean-first --parallel
ctest --test-dir build/windows-vs2022 -C Debug --output-on-failure
cmake --build build/windows-vs2022 --config Release --clean-first --parallel
ctest --test-dir build/windows-vs2022 -C Release --output-on-failure

.\build\windows-vs2022\Release\iaisf_server.exe --version
.\build\windows-vs2022\Release\iaisf_server.exe `
  --config .\config\iaisf.example.json

D:\Git\Git\bin\bash.exe -n `
  scripts/build_linux.sh `
  scripts/test_linux.sh `
  scripts/smoke_linux.sh
```

正式 Linux 命令尚未执行。

## 参考工程保护

任务开始基线：

| 文件数 | 字节数 | 聚合 SHA-256 |
|---:|---:|---|
| 62 | 59,240,225 | `83AE7E469DEA30C860DEFD4D26CB313B7B3C87EFCD9387414741E152EE46CF27` |

任务结束复核仍为 62 个文件、59,240,225 字节和相同聚合 SHA-256。参考工程未被构建、格式化、添加到 Git 或修改。

## 未实现

- Network/Reactor、UniqueFd、Socket、Channel、Poller、epoll、eventfd
- HTTP request/response/parser/router
- ThreadPool 和运行时任务队列
- TaskManager、TaskRepository、TaskExecutor
- PluginManager、EchoPlugin、MockVisionPlugin
- timerfd/signalfd 实现和超时
- 异步日志、文件日志和轮转
- 数据库、用户、HTML、TLS、Docker、部署和 Release 发布
- benchmark、性能数据、真实 AI

`worker_threads` 和 `task_queue_capacity` 当前只是经过校验的后续配置基线，不会创建 worker 或队列。

## 当前阻塞与风险

- 唯一阶段阻塞：GitHub Actions 尚未实际运行，缺少 Ubuntu 24.04 Debug/Release/CTest/smoke 成功证据。
- 默认 FetchContent 首次 Linux 配置需要 GitHub 网络和有效 CA 证书。
- 系统依赖模式已设计但未在已安装 Linux 包环境验证。
- Windows/NTFS 工作区不能可靠表达新 shell 脚本的 POSIX executable bit；workflow 会在运行时执行 `chmod +x scripts/*.sh`，人工 Linux 使用前仍应确认权限。
- Visual Studio 本机 vcpkg applocal 集成的非致命 `pwsh.exe` 诊断不影响当前测试，但应与 Linux 结果分开记录。

## Phase 2 建议

GitHub Actions Linux 验收真实通过并补齐证据后，再开始：

1. `UniqueFd`
2. Socket 基础封装
3. Channel
4. EpollPoller
5. EventLoop
6. eventfd 跨线程唤醒
7. 对应基础单元测试

不要在 Phase 2 同时塞入 HTTP、任务和插件。

## 建议 commit

未执行 commit。建议：

```text
ci: validate phase 1 foundation on Linux
```
