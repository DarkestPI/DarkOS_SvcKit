
# RV1126B IPC Application

本项目在启动时读取两类 JSON 配置：

- `boards/<board>.json`：板级硬件资源清单；
- `etc/app.json`：业务服务到硬件资源的绑定。

默认构建打包 `rv1126b_ipc_v1`：

```bash
cmake --preset rv1126b_ipc -DDARKOS_BOARD=rv1126b_ipc_v1
cmake --build --preset rv1126b_ipc -j
```

构建结果中的配置为 `output/rv1126b_ipc/etc/board.json` 和
`output/rv1126b_ipc/etc/app.json`。安装到设备后程序默认从 `/etc` 读取；宿主机
调试可以显式指定源码配置：

```bash
rv1126b_ipc \
  --board-config applications/rv1126b_ipc/boards/rv1126b_ipc_v1.json \
  --app-config applications/rv1126b_ipc/etc/app.json
```

当前阶段程序完成配置解析、Schema 校验、串口资源存在性/波特率/独占冲突校验，
尚未打开真实串口或启动 PTZ、控制协议服务。

## 原始项目模板说明

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
