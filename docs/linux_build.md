# Linux 构建指南

## 1. 为什么必须使用 Linux

IndustrialAIServiceFramework 的 Phase 2 Reactor、Phase 3 TCP Transport 和 Phase 4
HTTP adapter 直接使用 epoll、eventfd、accept4 和 POSIX Socket；HTTP Core、
Task Runtime、Plugin System 与 `iaisf_task_api` 本身可移植。Phase 7 新增的
`iaisf_service` 和 loopback 集成测试依赖 Linux Reactor/TCP/HTTP adapter；Phase 7
最终 push run 已在 Ubuntu 24.04 真实执行并完成封板，项目源码与测试 warning 均为 0。
timerfd 与 signalfd 仍是后续计划。
Windows MinGW 或 MSVC 可以帮助发现一部分可移植 C++ 问题，但不能替代真实 Linux
构建、测试和运行验证。

推荐环境：

- 原生 Linux；
- WSL2 Ubuntu LTS；
- 受控的 Linux CI 构建机。

本项目不会自动安装 WSL、Linux 发行版或系统依赖。

## 2. 基础工具

必需：

- `g++` 或兼容的 C++17 编译器；
- CMake 3.22 或更高版本；
- Git；
- `ca-certificates`，用于默认 FetchContent 下载。

可选：

- `ninja-build`
- `clang`
- `clang-format`
- `clang-tidy`
- `valgrind`

Ubuntu 上可由用户手工执行：

```bash
sudo apt update
sudo apt install build-essential cmake git ca-certificates
```

Codex 的 Phase 1 执行不会自动运行这些 `sudo` 命令。

## 3. 默认依赖策略

默认配置：

```text
IAISF_BUILD_TESTS=ON
IAISF_USE_SYSTEM_DEPS=OFF
IAISF_BUILD_LINUX_NETWORK=ON  # Linux 默认；其他平台默认 OFF
```

`IAISF_BUILD_LINUX_NETWORK=ON` 创建 Linux-only `iaisf_net` / `iaisf::net`、
`iaisf_tcp` / `iaisf::tcp`、`iaisf_http` / `iaisf::http` 和对应测试。
`iaisf_http_core` / `iaisf::http_core` 不依赖该选项，在 Windows/Linux 都构建。
`iaisf_task` / `iaisf::task` 与 `iaisf_plugin` / `iaisf::plugin` 同样跨平台，
不受该选项控制。
`iaisf_task_api` / `iaisf::task_api` 同样跨平台；`iaisf_service` /
`iaisf::service` 只在 Linux network 目标存在时创建。
该选项在非 Linux 平台显式开启会直接
configure 失败，不提供空实现或静默关闭。

Phase 7 CI 还必须显式构建：

```text
iaisf_task_api
iaisf_task_api_tests
iaisf_service
iaisf_service_tests
```

随后运行完整 CTest。`iaisf_service_tests` 使用 loopback 和 port 0，不依赖固定端口、
数据库、GPU 或外部进程。当前工作区没有本地 Linux/WSL PASS 证据，不能把 Windows
370/370 或 workflow YAML 当成 Linux 结果。

CMake FetchContent 使用固定 tag：

- nlohmann/json `v3.11.3`
- GoogleTest `v1.15.2`

源码和构建产物只进入所选 build 目录。首次配置需要访问 GitHub；网络、证书、代理或 Git 配置错误会使配置失败。

下载失败时：

1. 检查 GitHub 访问、代理、DNS 和 CA 证书；
2. 删除或另选失败的 build 目录后重新配置；
3. 或安装兼容系统包并启用系统依赖模式；
4. 不要手工把第三方源码复制进项目。

## 4. Debug

使用脚本：

```bash
./scripts/build_linux.sh Debug
./scripts/test_linux.sh Debug
```

对应命令：

```bash
cmake -S . -B build/linux-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIAISF_BUILD_TESTS=ON \
  -DIAISF_USE_SYSTEM_DEPS=OFF \
  -DIAISF_BUILD_LINUX_NETWORK=ON
cmake --build build/linux-debug --parallel
ctest --test-dir build/linux-debug --output-on-failure
```

## 5. Release

使用脚本：

```bash
./scripts/build_linux.sh Release
./scripts/test_linux.sh Release
```

对应命令：

```bash
cmake -S . -B build/linux-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DIAISF_BUILD_TESTS=ON \
  -DIAISF_USE_SYSTEM_DEPS=OFF \
  -DIAISF_BUILD_LINUX_NETWORK=ON
cmake --build build/linux-release --parallel
ctest --test-dir build/linux-release --output-on-failure
```

## 6. Smoke

Release 构建和测试完成后：

```bash
./scripts/smoke_linux.sh
```

脚本真实执行：

```bash
build/linux-release/iaisf_server --version
build/linux-release/iaisf_server \
  --config config/iaisf.example.json
```

当前 `iaisf_server` 仍只验证配置后立即退出，不创建 EventLoop/TcpServer，不进入
常驻循环。TCP 能力当前由独立 library/component tests 使用。

## 7. 系统依赖模式

环境已经提供兼容版本时：

```bash
cmake -S . -B build/linux-system-deps \
  -DCMAKE_BUILD_TYPE=Release \
  -DIAISF_BUILD_TESTS=ON \
  -DIAISF_USE_SYSTEM_DEPS=ON \
  -DIAISF_BUILD_LINUX_NETWORK=ON
cmake --build build/linux-system-deps --parallel
ctest --test-dir build/linux-system-deps --output-on-failure
```

系统模式通过 `find_package` 查找：

- nlohmann_json 3.11.3 或兼容版本；
- GTest 1.15.2 或兼容版本。

缺少包时 CMake 会明确失败，不会偷偷切回下载模式。

## 8. 关闭测试

作为其他项目子目录使用或只构建 CLI 时可以显式关闭：

```bash
cmake -S . -B build/linux-no-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DIAISF_BUILD_TESTS=OFF \
  -DIAISF_USE_SYSTEM_DEPS=OFF \
  -DIAISF_BUILD_LINUX_NETWORK=ON
cmake --build build/linux-no-tests --parallel
```

关闭测试后不会获取 GoogleTest；nlohmann/json 仍是配置模块依赖。

## 9. 清理

构建目录可独立删除，不要删除源码目录：

```bash
rm -rf -- build/linux-debug
rm -rf -- build/linux-release
rm -rf -- build/linux-system-deps
```

执行前应确认当前路径确实位于项目根目录。构建产物不会写入 `include/`、`src/`、`tests/` 或 `config/`。

## 10. GitHub Actions

`.github/workflows/linux-ci.yml` 使用固定 `ubuntu-24.04` runner、GCC 默认工具链和最小 `contents: read` 权限：

- `linux-debug`：记录环境，执行 Debug configure/build/CTest；
- `linux-release`：执行 Release configure/build/CTest 和 CLI smoke；
- workflow 在两个 job 的普通 build 后显式构建全部 HTTP、Task 与 Plugin targets，
  包括 `iaisf_plugin` 和 `iaisf_plugin_tests`，使跨平台插件层参与情况可见；
- 两个 job 都通过构建脚本显式设置 `IAISF_BUILD_LINUX_NETWORK=ON`，并设置 15 分钟 job timeout；
- 两个 job 都使用固定版本 FetchContent，不启用系统依赖模式；
- 不使用 `continue-on-error`，不发布制品或部署。

首次功能 [GitHub Actions run 30514521602](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30514521602)
对应 Reactor 实现提交 `f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f`。Debug/Release
构建、CTest 和 smoke 通过，但 Release 测试构建存在 2 条 `-Wunused-result`
warning，因此该 run 只作为功能通过历史记录。

Phase 2 的最终零 warning 证据是
[GitHub Actions run 30516007475](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475)，
对应 warning 修复提交 `4db8708a5121f8477d835addd0b16170a3e2054f`、分支
`phase/2-reactor-core`、push event、attempt 1。head SHA 和两个 job 的 checkout SHA
均为该提交。workflow status/conclusion 为 `completed/success`，两个 job 和全部步骤
均为 `success`，没有 failed、cancelled、skipped、neutral 或
`continue-on-error`。

## 11. 环境记录

每次正式 Linux 验收前记录：

```bash
uname -a
cat /etc/os-release
cmake --version
c++ --version
git --version
```

Phase 2 已在 GitHub-hosted `ubuntu-24.04` runner 上完成验证：

| 项目 | Debug | Release |
|---|---|---|
| Ubuntu | 24.04.4 LTS | runner label 为 `ubuntu-24.04` |
| GCC | 13.3.0 | configure 识别 GNU 13.3.0 |
| CMake | 3.31.6 | 同一 workflow 工具环境 |
| configure/build | pass / pass | pass / pass |
| CTest | 87/87，0 failed | 87/87，0 failed |
| Reactor 测试 | 44/44 | 44/44 |
| 项目源码 warning | 0 | 0 |
| 项目测试 warning | 0 | 0 |
| 独立 smoke | 不适用 | version/config 均 pass |

Phase 2B 的 44 个 Linux-only Reactor 测试分布为：UniqueFd 8、Socket 5、
Channel 7、EpollPoller 7、EventLoop 17。构建日志确认 `iaisf_net` 和
`iaisf_net_tests` 在 Debug/Release 中均实际构建，不是仅由源码数量推算。

Release smoke 实际输出：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T05:14:04.588Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

最终 Debug/Release 编译日志中，项目源码 warning 为 0、项目测试 warning 为 0；
首次功能 run 的两处 `-Wunused-result` 已消失。日志中含 “warning” 的文本仅来自
checkout 阶段的 `git init` 默认分支提示，不是编译 warning。

当前 Windows 宿主仍没有可运行的 Linux/WSL 环境；以上 Linux 结果全部来自可追溯的
GitHub Actions run，不代表本机 Linux 编译。

Reactor 测试覆盖 HUP/RDHUP read-side 语义、active Channel 延迟移除、fd 复用、`queue_in_loop` 失败回滚、多线程容量竞争、EventLoop 状态矩阵、异常隔离和 eventfd drain。它们不访问外部网络或固定端口。

## 12. Phase 3 最终验证矩阵

Phase 3 提交新增 `iaisf_tcp` 和 `iaisf_tcp_tests`，包含 50 项 TCP 测试：

```text
Buffer 13
Ipv4Endpoint 4
Acceptor/Socket server operations 7
TcpConnection 6
TcpServer integration 20
```

Phase 3B 还为 EventLoop intrusive internal cleanup lane 新增 1 项 Reactor 测试，
因此最终矩阵为基础 43 + Reactor 45 + TCP 50 = 138。内部 cleanup lane
不使用普通有界 `queue_in_loop`，调度时不分配，并由 Acceptor/TcpServer 内嵌节点
提供容量满载下的生命周期清理保证。

测试只使用 loopback port 0 或 socketpair，无外部服务、固定 sleep 或 detached
thread，单项 CTest timeout 为 20 秒。最终证据为
[Linux CI run 30524686201](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30524686201)，
对应提交 `0a45658d0e450dd9dfde052808a27ae92ad08881`、分支
`phase/3-tcp-transport`、push event、attempt 1。run head SHA、两个 job 的 checkout
SHA、本地 HEAD 和 upstream 完全一致。

| 项目 | Debug | Release |
|---|---|---|
| runner / OS | `ubuntu-24.04` / Ubuntu 24.04.4 LTS | `ubuntu-24.04` |
| GCC / CMake | 13.3.0 / 3.31.6 | 同一 workflow 工具环境 |
| configure/build | pass / pass | pass / pass |
| CTest | 138/138，0 failed | 138/138，0 failed |
| Foundation / Reactor / TCP | 43/43；45/45；50/50 | 43/43；45/45；50/50 |
| TCP targets | `iaisf_tcp`、`iaisf_tcp_tests` built | 两个 target built |
| 项目源码/测试 warning | 0 / 0 | 0 / 0 |
| 独立 smoke | 不适用 | version/config pass |

Release smoke 实际输出：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T07:58:10.003Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

workflow conclusion 为 `success`，两个 job 和全部 Actions steps 均为 success，没有
failed、cancelled、skipped、neutral、timeout 或 `continue-on-error`。日志中仅
checkout 阶段的 `git init` 默认分支提示包含单词 “warning”；项目源码和测试编译
warning 均为 0。当前 Windows 宿主仍没有可运行的 Linux/WSL；上述结论来自 CI，
不代表本机执行过 Linux build。

## 13. Phase 4 Linux 最终验证

Phase 4 保持相同命令和 `IAISF_BUILD_LINUX_NETWORK=ON`。成功 configure 后应存在：

```text
iaisf_http_core
iaisf_http_core_tests
iaisf_http
iaisf_http_tests
```

最终测试矩阵：

```text
Foundation 43
Reactor 45
TCP 51
HTTP Core 84
HTTP integration 16
CTest total 239
```

Phase 3 的最终 CI 历史结果仍是 TCP 50/50、总计 138/138；第 51 项 TCP 测试验证
HTTP 所需的 `close_after_write()` 传输契约，已在 Phase 4 最终矩阵中通过，不回写
Phase 3 历史结果。

当前本机没有可运行 WSL/Linux，因此：

- Windows Debug/Release 已验证 `iaisf_http_core` 和 84 项 core tests，均为 127/127；
- 本机没有编译 `iaisf_http`，没有运行 loopback integration；
- Phase 3 run `30524686201` 不能作为 Phase 4 结果；
- Phase 4 Linux 结论来自下述可追溯的最终 push run。

### 13.1 失败与修复历史

实现提交 `9b87fdb8804ee37a8cf3b87a7b9193a3130b85d3` 对应的首次
[run 30537924856](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30537924856)
在 Debug/Release 各执行 238 项，均为 237/238。唯一失败测试
`HttpServerTest.RejectsFramingAmbiguitiesAndConfiguredLimits` 使用 32 字节响应
Header 单行上限，小于固定错误 `Content-Type` 行含 CRLF 的 41 字节。Parser 已将
缺失 Host 映射为 400，但错误响应序列化预检失败，Session 按既有策略 fail-closed。

修复提交 `0818ebf4f71366cc3cd2fe4e36e95fe667b687a5` 把 fixture 调整为精确
41 字节，仍由 44 字节请求 Header 验证 431，并新增 portable exact-limit 测试。

### 13.2 最终 CI 证据

| 项目 | 结果 |
|---|---|
| workflow | Linux CI |
| run | [30539245789](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30539245789) |
| attempt / event / conclusion | 1 / push / success |
| branch | `phase/4-http-protocol` |
| head / checkout / local / upstream | `0818ebf4f71366cc3cd2fe4e36e95fe667b687a5` |
| runner / OS | `ubuntu-24.04` / Ubuntu 24.04.4 LTS |
| GCC / CMake | 13.3.0 / 3.31.6 |
| Debug | configure/build pass；239/239，0 failed |
| Release | configure/build pass；239/239，0 failed |
| HTTP | Core 84/84；Integration 16/16；原失败测试通过 |
| HTTP targets | 四个 target 在 Debug/Release 均实际构建 |
| warning | 项目源码 0；项目测试 0 |
| 非成功或掩盖 | failed/cancelled/skipped/neutral/timeout/continue-on-error 均为 0 |

Release smoke：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T11:38:17.579Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

同 SHA 的 PR run checkout 了 GitHub 临时 merge commit，因此最终封板选择 checkout
SHA 与本地 HEAD/upstream 完全一致的 push run。Phase 4 状态为
`PHASE_4_HTTP_PROTOCOL_COMPLETED`。

## 14. Phase 5 Linux 最终验证

Phase 5 新增跨平台 targets：

```text
iaisf_task
iaisf_task_tests
```

它们不受 `IAISF_BUILD_LINUX_NETWORK` 控制；Linux CI 仍使用
`IAISF_BUILD_LINUX_NETWORK=ON`，从而在同一矩阵构建 Foundation、Reactor、TCP、
HTTP 和 Task Runtime。workflow 的 Debug/Release 显式验证命令均保留已有 HTTP
targets，并追加：

```bash
cmake --build build/linux-debug --target iaisf_task iaisf_task_tests --parallel
cmake --build build/linux-release --target iaisf_task iaisf_task_tests --parallel
```

实际 workflow 将它们与 HTTP targets 放在同一个 `cmake --build --target ...` 调用中；
上面的拆分仅用于说明 task 入口。完整 CTest 和 Release CLI smoke 未删除，没有
`continue-on-error`、artifact upload 或部署。

最终 push [Linux CI run 30547126540](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30547126540)
对提交 `79d3d4e89feb71595dc67d820f9a5398dcc814d4` 完成真实验证：

```text
workflow / event / attempt: Linux CI / push / 1
conclusion: success
branch: phase/5-task-runtime
head / Debug checkout / Release checkout:
79d3d4e89feb71595dc67d820f9a5398dcc814d4
runner: ubuntu-24.04
OS: Ubuntu 24.04.4 LTS
kernel: 6.17.0-1020-azure
compiler: GCC 13.3.0
CMake: 3.31.6

Foundation          43
Reactor             45
TCP                 51
HTTP Core           84
HTTP Integration    16
Task Runtime        85
total              324
```

Debug 与 Release configure/build 均成功，CTest 均为 324/324、0 failed；日志实际
列出 Task Runtime 85/85。两个配置均实际构建 `iaisf_task` 和
`iaisf_task_tests`。Release smoke 输出：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T13:31:28.474Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

两个 job 和全部可见步骤均为 success；没有 failed、cancelled、skipped、neutral、
timeout 或 `continue-on-error`。Debug/Release 项目源码和测试编译 warning 均为 0。
当前宿主仍没有 `bash`、WSL 或 Linux；该结论来自可追溯的 GitHub Actions run，
不宣称本机执行了 Linux configure/build/CTest。

当前状态：

```text
PHASE_5_TASK_RUNTIME_COMPLETED
```

## 15. Phase 6 Linux 验证入口

Phase 6 新增跨平台 targets：

```text
iaisf_plugin
iaisf_plugin_tests
```

Linux CI 的 Debug/Release 显式 target 列表已经加入二者，并继续构建 HTTP、Task
及 Linux network targets，运行完整 CTest；Release 继续执行 version/config smoke。
预期模块定义数来自当前源码：

```text
Foundation          43
Reactor             45
TCP                 51
HTTP Core           84
HTTP Integration    16
Task Runtime        97
Plugin System       92
Total              428
```

Phase 6 功能提交 `66a606bb53bf8ed80b8efd6faf7c6529b5cd22d1` 已由首次
[Linux CI run 30602538268](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30602538268)
实际验证。workflow `Linux CI`，push event，attempt 1；head、Debug checkout、
Release checkout、本地 HEAD 和 upstream 一致。runner 为 `ubuntu-24.04`，环境是
Ubuntu 24.04.4 LTS、kernel `6.17.0-1020-azure`、GCC 13.3.0、CMake 3.31.6。

| Job | Configure/build | CTest | Task Runtime | Plugin System | Smoke |
|---|---|---:|---:|---:|---|
| Linux Debug | success | 428/428 | 97/97 | 92/92 | N/A |
| Linux Release | success | 428/428 | 97/97 | 92/92 | version/config success |

实际模块数为 Foundation 43、Reactor 45、TCP 51、HTTP Core 84、HTTP Integration
16、Task Runtime 97、Plugin System 92。`iaisf_plugin` 与 `iaisf_plugin_tests`
在两个配置都实际构建，专项测试均实际执行。

Phase 6B/6C 增加并审计的 `validate_json_value` 是跨平台 core source；Task Runtime
与 Plugin targets 均实际链接它。JSON byte 校验用 counting stream 精确匹配 nlohmann
compact dump 的 UTF-8 输出（包含引号、反斜杠、控制字符和 key 转义），首次超限即
中止，不保存额外整份 dump 文本。Adapter 生成的 validator/handler closure 只持有
只读 PluginManager，不持有 Adapter 或 TaskManager；释放 runtime closure 后 Manager
与 Plugin 可析构，不存在 shared ownership cycle。Linux CI 必须实际执行 Task
Runtime 97 和 Plugin System 92。

本次 run 的功能、CTest 与 smoke 全部成功，但 Debug 和 Release 各出现：

- 项目源码 warning 3 条：`plugin_metadata.cpp` 两条与
  `mock_vision_plugin.cpp` 一条 `-Wsign-conversion`；
- 项目测试 warning 3 条：PluginMetadata 聚合初始化的
  `-Wmissing-field-initializers`。

所有 workflow step 均 success，没有 failed、cancelled、skipped、neutral、timeout
或 `continue-on-error`。由于项目 warning 不为 0，这次功能成功不是 Phase 6 最终
零 warning 封板证据。

warning 修复提交 `853ccccca80cdc042b3d51eae52fe45566aa2b22` 的最终
[Linux CI run 30604428624](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30604428624)
为 workflow `Linux CI`、push event、attempt 1。head、Debug checkout、Release
checkout、本地 HEAD 与 upstream 一致；runner 为 `ubuntu-24.04`，Ubuntu 24.04.4
LTS、kernel `6.17.0-1020-azure`、GCC 13.3.0、CMake 3.31.6。

| Job | Configure/build | CTest | Foundation | Reactor | TCP | HTTP Core | HTTP Integration | Task | Plugin | Smoke |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Linux Debug | success | 428/428 | 43 | 45 | 51 | 84 | 16 | 97 | 92 | N/A |
| Linux Release | success | 428/428 | 43 | 45 | 51 | 84 | 16 | 97 | 92 | version/config success |

两个配置均实际构建 `iaisf_plugin` 和 `iaisf_plugin_tests`，完整日志中项目源码
warning 0、项目测试 warning 0。Release smoke 输出版本
`IndustrialAIServiceFramework 0.1.0`，示例配置验证成功。workflow、两个 job 和
所有步骤均 success，无 failed、cancelled、skipped、neutral、timeout、
`continue-on-error` 或被隐藏的失败。

当前 Phase 6 状态：

```text
PHASE_6_PLUGIN_SYSTEM_COMPLETED
```

## 16. Phase 7B Linux 验证入口

Phase 7B 当前只完成 Windows 与静态审计，没有本机 Linux/WSL/bash 结果。Windows
Debug/Release 均为 370/370，不能替代 Linux。当前 Linux build 必须显式构建：

```text
iaisf_task_api
iaisf_task_api_tests
iaisf_service
iaisf_service_tests
```

当前源代码注册的跨平台 CTest 为 370；Linux 还增加 Reactor 45、TCP 51、HTTP
Integration 16 和 Service 15，故预计完整 CTest 为 497。该数字只是 CI 前清单，不是
PASS：最终必须以 exact commit 对应的 Ubuntu 24.04 Debug/Release configure、build、
CTest 和 Release smoke 原始日志为准。

Service 15 个 CTest 定义中的参数化错误矩阵实际展开 37 个 GoogleTest case；应重点
确认 active HTTP Channel 内触发 stop、blocking plugin、Stopping POST 503、
Session/connection 清空、worker join、重复 stop、occupied port/duplicate plugin/start
failure 回滚全部实际执行。workflow 不使用 `continue-on-error`、warning suppression、
部署或制品上传；项目源码和测试 warning 必须为 0 才能封板。

## Phase 7E 最终 Linux CI 记录

本机没有以 WSL/Linux 构建结果冒充验证；最终证据来自 [Linux CI run 30779555703](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30779555703)，对应提交 `1cc332b9d9e02ae78ec9e43455d36ffe939f73e2`。runner 为 `ubuntu-24.04`、Ubuntu 24.04.4 LTS，kernel `6.17.0-1020-azure`，GCC 13.3.0、CMake 3.31.6。Debug 和 Release configure/build 成功，CTest 均 `497/497`，Release version/config smoke 成功；ActiveHttpStop 测试两种配置均通过。

历史 run 的 workflow conclusion 为 success，但项目测试 warning 当时非零：`tests/service/test_industrial_ai_service.cpp:1041:51` 的 `-Wshadow`（每个 job 3 次）。该 run 不是最终封板证据。workflow 无 failed/cancelled/skipped/neutral/timeout，也未使用 `continue-on-error`。尚未执行 `ctest --repeat until-fail:50`。

## Phase 7G 最终 Linux 封板

最终 push [Linux CI run 30781932731](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30781932731) 对提交 `a44b1272bf603a17724fa17c66d60ee0e18bb918` 完成 Ubuntu 24.04.4 LTS、kernel 6.17.0-1020-azure、GCC 13.3.0、CMake 3.31.6 的 Debug/Release 验证。两套配置 configure/build 成功，CTest 均 `497/497`，Release version/config smoke 成功，项目源码与测试 warning 均为 0。

ActiveHttpStop 测试在 Debug/Release 均通过。workflow 无失败或取消步骤，未使用 `continue-on-error`。尚未执行 `ctest --repeat until-fail:50`。Phase 7 状态为 `PHASE_7_SERVICE_INTEGRATION_COMPLETED`；Phase 8 尚未开始。
