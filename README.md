

# 快速开始

## 1. 创建项目

python3 SvcKit/scripts/setup_project.py my_project --output ./applications

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