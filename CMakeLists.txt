cmake_minimum_required(VERSION 3.16)
project(agssetup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Target
add_executable(agssetup agssetup.cpp)

target_link_libraries(agssetup PRIVATE Qt6::Widgets)

# Install (optional)
# install(TARGETS agssetup DESTINATION bin)
