cmake_minimum_required(VERSION 3.16)
project(minivectordb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(minivectordb
  src/server/main.cpp
  src/engine.cpp
  src/sql/lexer.cpp
  src/sql/parser.cpp
  src/common/config.cpp
)

target_include_directories(minivectordb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 静态链接 C++ 运行库，减小运行时镜像依赖。
target_link_libraries(minivectordb -static-libstdc++ -static-libgcc)
