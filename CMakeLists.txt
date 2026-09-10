cmake_minimum_required(VERSION 3.20)
project(DarkOS VERSION 0.1.0 LANGUAGES C CXX)

# SDK 自身的开发入口。业务应用不需要从这里构建，而是在自己的
# CMakeLists.txt 中 include(cmake/DarkOS.cmake)。
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DarkOS.cmake")
