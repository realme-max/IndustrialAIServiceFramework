# Linux 构建指南

## 1. 为什么必须使用 Linux

IndustrialAIServiceFramework 的 Phase 2 网络核心直接使用 epoll、eventfd 和 POSIX Socket；timerfd 与 signalfd 仍是后续计划。Windows MinGW 或 MSVC 可以帮助发现一部分可移植 C++ 问题，但不能替代真实 Linux 构建、测试和运行验证。

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

`IAISF_BUILD_LINUX_NETWORK=ON` 创建 Linux-only `iaisf_net` / `iaisf::net` 和对应测试。该选项在非 Linux 平台显式开启会直接 configure 失败，不提供空实现或静默关闭。

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

Phase 1 程序验证配置后立即退出，不启动 Socket，不进入常驻循环。

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
- 两个 job 都通过构建脚本显式设置 `IAISF_BUILD_LINUX_NETWORK=ON`，并设置 15 分钟 job timeout；
- 两个 job 都使用固定版本 FetchContent，不启用系统依赖模式；
- 不使用 `continue-on-error`，不发布制品或部署。

Phase 1 的历史成功证据是 [GitHub Actions run 30508113122](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122)，它对应 Phase 1 提交，不覆盖当前 Phase 2 改动。workflow 只有在用户提交并 push 当前分支后才能运行；当前尚无 Phase 2 run，不得把 workflow 文件或测试定义本身视为 Linux PASS。

## 11. 环境记录

每次正式 Linux 验收前记录：

```bash
uname -a
cat /etc/os-release
cmake --version
c++ --version
git --version
```

只有当前 Phase 2 commit 的 Debug/Release configure、build、CTest 和 Release smoke 在真实 Linux 中全部成功后，才能把 Phase 2 标记为 completed。MinGW、MSVC、旧提交的 run 或仅执行 CMake configure 都不满足该门槛。

Phase 2B 审计后当前包含 44 个 Linux-only `TEST` 源代码定义：UniqueFd 8、Socket 5、Channel 7、EpollPoller 7、EventLoop 17。连同 Phase 1 的 43 项，若全部被 CMake 发现，Linux CTest 预计接近 87 项。实际 discovery 数量、总数和通过数必须以目标 Linux 构建输出为准；尚未执行时不得预填为成功。

Reactor 测试覆盖 HUP/RDHUP read-side 语义、active Channel 延迟移除、fd 复用、`queue_in_loop` 失败回滚、多线程容量竞争、EventLoop 状态矩阵、异常隔离和 eventfd drain。它们不访问外部网络或固定端口。
