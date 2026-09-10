

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

构建中间文件位于 `applications/my_project/build-rv1126b/`，最终产物位于
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
- `DARKOS_BUILD_HAL_IMPLEMENTATIONS`：构建 host_x86 或 Rockchip HAL，默认 `OFF`；
- `DARKOS_BUILD_PROTOCOL_IMPLEMENTATIONS`：构建协议实现，默认 `OFF`。

骨架阶段缺少源码的组件使用 `INTERFACE` 目标占位。应用统一链接
`DarkOS::SvcKit`，也可以按需链接 `DarkOS::Media`、`DarkOS::Networking`、
`DarkOS::RTSP` 等细粒度目标。

# 目录结构

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
