# Linux 构建指南

## 1. 为什么必须使用 Linux

IndustrialAIServiceFramework 的 Phase 2 Reactor 和 Phase 3 TCP Transport 直接使用
epoll、eventfd、accept4 和 POSIX Socket；timerfd 与 signalfd 仍是后续计划。
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
`iaisf_tcp` / `iaisf::tcp` 和对应测试。该选项在非 Linux 平台显式开启会直接
configure 失败，不提供空实现或静默关闭。

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
- Phase 3 revision 在两个 job 的普通 build 后显式构建 `iaisf_tcp` 和
  `iaisf_tcp_tests`，使 target 参与情况在日志中可见；
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

## 12. Phase 3 待验证矩阵

当前工作区新增 `iaisf_tcp` 和 `iaisf_tcp_tests`，源码定义 50 项 TCP 测试：

```text
Buffer 13
Ipv4Endpoint 4
Acceptor/Socket server operations 7
TcpConnection 6
TcpServer integration 20
```

Phase 3B 还为 EventLoop intrusive internal cleanup lane 新增 1 项 Reactor 测试，
因此当前源码静态定义为基础 43 + Reactor 45 + TCP 50 = 138。内部 cleanup lane
不使用普通有界 `queue_in_loop`，调度时不分配，并由 Acceptor/TcpServer 内嵌节点
提供容量满载下的生命周期清理保证。该算术只用于检查 discovery 完整性，不是 PASS。

测试只使用 loopback port 0 或 socketpair，无外部服务、固定 sleep 或 detached
thread，单项 CTest timeout 为 20 秒。当前没有 Phase 3 commit/run URL，故下列
结果都保持 unknown，而不是由源码定义数推算：

| 项目 | 当前状态 |
|---|---|
| Ubuntu runner / GCC / CMake | 待新 run 记录 |
| Debug configure/build | 未运行 |
| Debug CTest 总数/通过/失败 | 未运行 |
| Release configure/build | 未运行 |
| Release CTest 总数/通过/失败 | 未运行 |
| TCP 50 项实际执行 | 未确认 |
| Reactor 当前 45 项实际执行 | 未确认；Phase 2 历史 run 只执行当时 44 项 |
| Release smoke | 未运行 |
| 项目源码/测试 warning | 未确认 |

Phase 2 run `30516007475` 只证明 `iaisf_net` 基线，不包含当前 TCP revision。用户
完成 commit/push 后，应使用同一 head SHA 的完整 Debug/Release jobs，确认两个 TCP
target 构建、全部 TCP 测试执行、smoke 成功且无被隐藏的失败，再更新阶段状态。
