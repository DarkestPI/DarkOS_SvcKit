cmake_minimum_required(VERSION 3.20)
project(DarkOS VERSION 0.1.0 LANGUAGES C CXX)

# 设置C、C++标准与现代编译选项
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/oem/bin)
# set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/oem/lib)
# set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/oem/lib)

# 分层接线：hardware（HAL）→ SvcKit（流媒体套件）→ project（组装，唯一 ELF）；
# add_subdirectory(hardware)
# add_subdirectory(third_party)
# add_subdirectory(SvcKit)
# add_subdirectory(project)
add_subdirectory(applications)      # 可执行程序，依赖所有库
