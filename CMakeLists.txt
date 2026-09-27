cmake_minimum_required(VERSION 3.16)
project(agssetup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# agssetup.cpp declares Q_OBJECT and ends with #include "agssetup.moc",
# so moc has to run on it.
set(CMAKE_AUTOMOC ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)

# Target
add_executable(agssetup agssetup.cpp)

target_link_libraries(agssetup PRIVATE Qt6::Widgets Qt6::OpenGLWidgets)

# Install (optional)
# install(TARGETS agssetup DESTINATION bin)
