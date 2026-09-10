
# 项目模板

cpp_project_template/
├── CMakeLists.txt                 # 根CMake配置
├── .clang-format                  # 代码格式化配置
├── .clang-tidy                    # 静态分析配置
├── .gitignore
├── README.md
├── cmake/                         # 自定义CMake模块
│   ├── FindDependencies.cmake
│   └── CodeCoverage.cmake
├── include/                       # 公共头文件
│   └── rv1126b_ipc/
├── src/                           # 源代码
│   ├── main.cpp
│   └── lib/
├── tests/                         # 单元测试
│   ├── CMakeLists.txt
│   └── test_basic.cpp
├── third_party/                   # 第三方依赖
├── scripts/                       # 工具脚本
│   └── setup_project.py
├── .github/workflows/             # CI/CD流水线
│   └── ci.yml
└── docs/                          # 项目文档
    └── design.md