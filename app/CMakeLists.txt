cmake_minimum_required(VERSION 3.21)

project(dbaro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 6.8 이상 (개발 PC는 6.10.3). LGPLv3로 쓰려면 동적 링크를 유지한다.
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

# 앱 코드 전체. 실행 파일과 이후의 테스트가 함께 링크한다.
qt_add_library(dbaro_lib STATIC
    src/core/ConnectionProfile.cpp
    src/core/ConnectionProfile.h
    src/core/ConnectionStore.cpp
    src/core/ConnectionStore.h
    src/core/SecretStore.cpp
    src/core/SecretStore.h
    src/core/SqlDialect.h
    src/core/SqlSplitter.cpp
    src/core/SqlSplitter.h
    src/ui/ConnectionDialog.cpp
    src/ui/ConnectionDialog.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/ObjectExplorer.cpp
    src/ui/ObjectExplorer.h
    src/ui/QueryTab.cpp
    src/ui/QueryTab.h
    src/ui/ResultPanel.cpp
    src/ui/ResultPanel.h
    src/ui/SqlEditor.cpp
    src/ui/SqlEditor.h
    src/ui/SqlHighlighter.cpp
    src/ui/SqlHighlighter.h
    src/ui/UiHelpers.h
)

target_include_directories(dbaro_lib PUBLIC src)

target_link_libraries(dbaro_lib PUBLIC
    Qt6::Widgets
    Qt6::Network
)

if(WIN32)
    # SecretStore: Windows 자격 증명 관리자 (CredWriteW 등)
    target_link_libraries(dbaro_lib PRIVATE Advapi32)
endif()

qt_add_executable(dbaro
    src/main.cpp
)

target_link_libraries(dbaro PRIVATE dbaro_lib)

set_target_properties(dbaro PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
