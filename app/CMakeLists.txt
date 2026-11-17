cmake_minimum_required(VERSION 3.21)

project(dbaro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 6.8 이상 (개발 PC는 6.10.3). LGPLv3로 쓰려면 동적 링크를 유지한다.
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

qt_add_executable(dbaro
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/ObjectExplorer.cpp
    src/ui/ObjectExplorer.h
    src/ui/QueryTab.cpp
    src/ui/QueryTab.h
    src/ui/ResultPanel.cpp
    src/ui/ResultPanel.h
)

target_include_directories(dbaro PRIVATE src)

target_link_libraries(dbaro PRIVATE
    Qt6::Widgets
    Qt6::Network
)

set_target_properties(dbaro PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
