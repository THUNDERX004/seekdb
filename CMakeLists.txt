cmake_minimum_required(VERSION 3.20)
project(minivectordb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(minivectordb src/minivectordb.cpp)
find_package(Threads REQUIRED)
target_link_libraries(minivectordb PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(minivectordb PRIVATE /O2 /W4)
else()
  target_compile_options(minivectordb PRIVATE -O3 -DNDEBUG -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
  target_link_options(minivectordb PRIVATE -static-libgcc -static-libstdc++)
endif()

install(TARGETS minivectordb RUNTIME DESTINATION /app)
