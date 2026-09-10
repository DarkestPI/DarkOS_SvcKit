# 启用测试和打包
include(CTest)
enable_testing()
include(CPack)

# 查找并管理第三方依赖
find_package(spdlog CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

# 添加子目录，定义模块间的依赖关系
add_subdirectory(foundation)        # 基础库，被所有上层模块依赖
add_subdirectory(subsystems)        # 子系统，依赖foundation
add_subdirectory(src/core)          # 引擎核心，依赖foundation和subsystems
add_subdirectory(tests)             # 测试，链接被测试的库