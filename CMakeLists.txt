cmake_minimum_required(VERSION 3.16)
project(Workspace)

set(CMAKE_CXX_STANDARD 17)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
find_package(GDAL REQUIRED)

# QTermWidget6 - exact paths
find_path(QTERMWIDGET_INCLUDE_DIR 
    NAMES qtermwidget.h 
    PATHS /usr/include/qtermwidget6
)

find_library(QTERMWIDGET_LIBRARY 
    NAMES qtermwidget6 
    PATHS /usr/lib/x86_64-linux-gnu
)

# CREATE TARGET FIRST
add_executable(workspace main.cpp mainwindow.cpp)

# THEN configure it
target_link_libraries(workspace
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    GDAL::GDAL          # Modern GDAL target
    ${QTERMWIDGET_LIBRARY}
    X11
    xkbcommon
    xkbfile
)

target_include_directories(workspace PRIVATE 
    ${QTERMWIDGET_INCLUDE_DIR}
    ${GDAL_INCLUDE_DIRS}
)

# Qt6 MOC
set_target_properties(workspace PROPERTIES 
    AUTOMOC ON
    AUTORCC ON
)

