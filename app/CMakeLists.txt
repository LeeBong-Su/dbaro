cmake_minimum_required(VERSION 3.21)

project(dbaro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 6.8 이상 (개발 PC는 6.10.3). LGPLv3로 쓰려면 동적 링크를 유지한다.
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network Svg)
qt_standard_project_setup()

# 앱 코드 전체. 실행 파일과 이후의 테스트가 함께 링크한다.
qt_add_library(dbaro_lib STATIC
    src/core/ConnectSpec.cpp
    src/core/ConnectSpec.h
    src/core/ConnectionProfile.cpp
    src/core/ConnectionProfile.h
    src/core/ConnectionStore.cpp
    src/core/ConnectionStore.h
    src/core/SecretStore.cpp
    src/core/SecretStore.h
    src/core/SqlDialect.h
    src/core/SqlSafety.cpp
    src/core/SqlSafety.h
    src/core/SqlSplitter.cpp
    src/core/SqlSplitter.h
    src/ipc/EngineClient.cpp
    src/ipc/EngineClient.h
    src/ui/ConnectionDialog.cpp
    src/ui/ConnectionDialog.h
    src/ui/EditorTabs.cpp
    src/ui/EditorTabs.h
    src/ui/Icons.cpp
    src/ui/Icons.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/ObjectExplorer.cpp
    src/ui/ObjectExplorer.h
    src/ui/QuerySession.cpp
    src/ui/QuerySession.h
    src/ui/QueryTab.cpp
    src/ui/QueryTab.h
    src/ui/ResultModel.cpp
    src/ui/ResultModel.h
    src/ui/ResultPanel.cpp
    src/ui/ResultPanel.h
    src/ui/SqlEditor.cpp
    src/ui/SqlEditor.h
    src/ui/SqlHighlighter.cpp
    src/ui/SqlHighlighter.h
    src/ui/Theme.cpp
    src/ui/Theme.h
    src/ui/UiHelpers.h
)

# 자체 아이콘 (:/icons/<이름>.svg)
qt_add_resources(dbaro_lib "icons"
    PREFIX "/"
    BASE "resources"
    FILES
        resources/icons/add.svg
        resources/icons/close.svg
        resources/icons/commit.svg
        resources/icons/database-add.svg
        resources/icons/database.svg
        resources/icons/dbaro.svg
        resources/icons/hide.svg
        resources/icons/rollback.svg
        resources/icons/run-script.svg
        resources/icons/run.svg
        resources/icons/stop.svg
)

target_include_directories(dbaro_lib PUBLIC src)

target_link_libraries(dbaro_lib PUBLIC
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
)

if(WIN32)
    # SecretStore: Windows 자격 증명 관리자 (CredWriteW 등)
    target_link_libraries(dbaro_lib PRIVATE Advapi32)
endif()

qt_add_executable(dbaro
    src/main.cpp
)

target_link_libraries(dbaro PRIVATE dbaro_lib)

if(WIN32)
    # 실행 파일 아이콘과 버전 정보 (탐색기, 작업 표시줄, 설치 프로그램에 보인다)
    set(DBARO_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/dbaro.ico")
    configure_file(resources/dbaro.rc.in "${CMAKE_CURRENT_BINARY_DIR}/dbaro.rc" @ONLY)
    target_sources(dbaro PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/dbaro.rc")
endif()

set_target_properties(dbaro PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
