

# 快速开始

## 1. 创建项目

```bash
python3 SvcKit/scripts/setup_project.py my_project --output ./applications
```

## 2. 构建应用

每个 `applications/<project>` 都是独立的顶层 CMake 工程，配置应用时会通过
`cmake/DarkOS.cmake` 自动引入 `platform` 和 `SvcKit`：

```bash
cd applications/my_project
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./../../output/bin/my_project
```

默认发布目录位于 SDK 根目录的 `output/`：

```text
output/
├── bin/    # Application 可执行文件
├── lib/    # 动态库和静态库
└── etc/    # Application 运行配置
```

可以在配置时修改发布根目录：

```bash
cmake -S . -B build -DDARKOS_OUTPUT_ROOT=/path/to/rootfs
```

执行 `cmake --install build` 也会按照相同的 `bin/lib/etc` 结构安装。

### RV1126B 交叉编译

RV1126B 的工具链、构建类型和发布目录已经保存在 Application 自己的
`CMakePresets.json` 中：

```bash
cd applications/my_project
cmake --preset rv1126b
cmake --build --preset rv1126b -j
```

构建中间文件位于 `applications/rv1126b_ipc/build/`，最终产物位于
SDK 根目录的 `output/rv1126b/`。

应用位于 SDK 仓库之外时，显式指定 SDK 根目录：

```bash
cmake -S . -B build \
    -DDARKOS_SDK_PATH=/path/to/DarkOS_SvcKit \
    -DCMAKE_BUILD_TYPE=Release
```

可用的 SDK 构建开关：

- `DARKOS_BUILD_TESTS`：构建测试，默认 `OFF`；
- `DARKOS_BUILD_EXAMPLES`：构建 SDK 示例，默认 `OFF`；
- `DARKOS_BUILD_HAL_IMPLEMENTATIONS`：构建选中平台的 HAL，默认 `OFF`；
- `DARKOS_PLATFORM`：选择 Ubuntu 等参考 HAL 平台，默认 `auto`；
- `DARKOS_VENDOR`、`DARKOS_SOC`：选择真实芯片厂商和 SoC，例如
  `rockchip`、`rv1126b`；RV1126B 工具链可以自动识别；
- `DARKOS_BUILD_SECURITY_COMPONENTS`：构建 `svc_crypto` 和 `svc_keystore`，
  默认 `OFF`；启用后需要 OpenSSL `libcrypto`；
- `DARKOS_BUILD_PROTOCOL_IMPLEMENTATIONS`：构建协议实现，默认 `OFF`。

需要显式选择平台时，例如：

```bash
cmake -S . -B build \
    -DDARKOS_VENDOR=rockchip \
    -DDARKOS_SOC=rv1126b \
    -DDARKOS_BUILD_HAL_IMPLEMENTATIONS=ON
```

`allwinner`、`artosyn`、`ingenic`、`novatek`、`sunplus` 已登记为 vendor，但只有
在 `platform/vendors/<vendor>` 提供构建入口和 SoC 实现后才能启用。未启用 HAL
实现时，这些未完成平台不会影响公共接口和 SvcKit 的构建。

骨架阶段缺少源码的组件使用 `INTERFACE` 目标占位。应用统一链接
`DarkOS::SvcKit`，也可以按需链接 `DarkOS::Media`、`DarkOS::Networking`、
`DarkOS::RTSP` 等细粒度目标。

## IDE 跳转（clangd）

clangd 按就近原则查找编译数据库：从当前文件逐级向上找 `compile_commands.json`
或 `<目录>/build/compile_commands.json`。`applications/<app>/build/` 下的库只
对应用自己的源码生效，直接编辑 `platform/`、`SvcKit/` 下的文件会找不到库、
无法跳转。根目录的 `build.sh` 提供了一键修复：

```bash
./build.sh ide
```

它做两件事：

1. 在 SDK 根目录配置一份 host 工程（`build/`，已被 gitignore），其
   `compile_commands.json` 覆盖 platform host 实现、shared 后端、libhardware
   和 SvcKit；
2. 把 `platform/vendors/rockchip/socs/rv1126b/build/compile_commands.json`
   链接到 `applications/rv1126b_ipc` 的交叉编译数据库——厂商代码需要交叉
   工具链 + Rockit SDK 头文件，host 库覆盖不到；该链接随 rv1126b_ipc 重建
   自动更新（尚未构建过时先执行 `./build.sh app rv1126b_ipc`）。

配置变更后如跳转未更新，执行一次"clangd: Restart language server"。

`build.sh` 还有其它子命令：`host` 构建根目录 host 工程的测试聚合目标、
`test` 构建并运行全部 host 测试（`ctest`）、`app <name>` 按应用自己的
preset 配置并构建 `applications/<name>`、`clean [<name>]` 删除应用构建
目录（省略名字则清理全部应用）。

# 目录结构

```bash
SvcKit/
├── CMakeLists.txt              # 根CMake文件
├── README.md
├── LICENSE
├── .gitignore
├── .github/                    # CI/CD工作流
│   └── workflows/
│       ├── ci.yml
│       └── cd.yml
├── docs/                       # 项目文档
├── external/                   # 第三方依赖管理 (vcpkg或git submodule)
├── src/
│   ├── foundation/            # 基础库
│   │   ├── core/              # 智能指针、日志、配置
│   │   ├── math/              # 向量、矩阵、四元数
│   │   ├── platform/          # 平台抽象层
│   │   └── utils/             # 通用工具
│   ├── subsystems/            # 引擎子系统
│   │   ├── graphics/          # 渲染抽象 (Vulkan/OpenGL)
│   │   ├── analytics/         # 智能适配分析
│   │   ├── audio/             # 音频系统
│   │   └── networking/        # 网络通信
│   ├── core/                  # 引擎核心
│   │   ├── ecs/               # 实体组件系统
│   │   ├── scene/             # 场景管理
│   │   └── resource/          # 资源管理器
│   └── applications/          # 可执行程序
│       ├── game/              # 示例游戏
│       └── editor/            # 引擎编辑器
├── tests/                     # 测试目录
│   ├── unit/                  # 单元测试 (Google Test)
│   └── integration/           # 集成测试
├── scripts/                   # 实用脚本
└── build/                     # 构建输出目录 (git忽略)
```