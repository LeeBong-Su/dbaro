cmake_minimum_required(VERSION 3.21)

project(dbaro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 6.8 이상 (개발 PC는 6.10.3). LGPLv3로 쓰려면 동적 링크를 유지한다.
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network Svg)
# .xlsx(ZIP) 읽기에 Qt의 QZipReader를 쓴다 (Qt 비공개 API: Qt를 올릴 때 확인한다)
# 배포본은 빌드한 Qt를 함께 넣으므로 비공개 모듈 경고는 끈다.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS CorePrivate)
qt_standard_project_setup()

# 앱 코드 전체. 실행 파일과 이후의 테스트가 함께 링크한다.
qt_add_library(dbaro_lib STATIC
    src/cli/CliApp.cpp
    src/cli/CliApp.h
    src/cli/CliConsole.cpp
    src/cli/CliConsole.h
    src/cli/McpServer.cpp
    src/cli/McpServer.h
    src/core/ConnectSpec.cpp
    src/core/ConnectSpec.h
    src/core/ConnectionProfile.cpp
    src/core/ConnectionProfile.h
    src/core/ConnectionStore.cpp
    src/core/ConnectionStore.h
    src/core/DataImport.cpp
    src/core/DataImport.h
    src/core/QueryHistory.cpp
    src/core/QueryHistory.h
    src/core/ResultEdits.cpp
    src/core/ResultEdits.h
    src/core/ResultExport.cpp
    src/core/ResultExport.h
    src/core/ScriptFolders.cpp
    src/core/ScriptFolders.h
    src/core/SecretStore.cpp
    src/core/SecretStore.h
    src/core/SqlBinds.cpp
    src/core/SqlBinds.h
    src/core/SqlCompletion.cpp
    src/core/SqlCompletion.h
    src/core/SqlDialect.h
    src/core/SqlFormatter.cpp
    src/core/SqlFormatter.h
    src/core/SqlIdentifiers.cpp
    src/core/SqlIdentifiers.h
    src/core/SqlRename.cpp
    src/core/SqlRename.h
    src/core/SqlSafety.cpp
    src/core/SqlSafety.h
    src/core/SqlSplitter.cpp
    src/core/SqlSplitter.h
    src/core/TextFile.cpp
    src/core/TextFile.h
    src/core/TextTable.cpp
    src/core/TextTable.h
    src/core/WorkspaceStore.cpp
    src/core/WorkspaceStore.h
    src/ipc/EngineClient.cpp
    src/ipc/EngineClient.h
    src/ai/AiAgent.cpp
    src/ai/AiAgent.h
    src/ai/AiSettings.cpp
    src/ai/AiSettings.h
    src/ai/ClaudeClient.cpp
    src/ai/ClaudeClient.h
    src/ui/AiDbTools.cpp
    src/ui/AiDbTools.h
    src/ui/AiPanel.cpp
    src/ui/AiPanel.h
    src/ui/AiSettingsDialog.cpp
    src/ui/AiSettingsDialog.h
    src/ui/AppController.cpp
    src/ui/AppController.h
    src/ui/BindDialog.cpp
    src/ui/BlockSelectTextView.cpp
    src/ui/BlockSelectTextView.h
    src/ui/ClaudeCodeDialog.cpp
    src/ui/ClaudeCodeDialog.h
    src/ui/ClaudeCodeLauncher.cpp
    src/ui/ClaudeCodeLauncher.h
    src/ui/BindDialog.h
    src/ui/BusyIndicator.cpp
    src/ui/BusyIndicator.h
    src/ui/ConnectionBar.cpp
    src/ui/ConnectionBar.h
    src/ui/ConnectionDialog.cpp
    src/ui/ConsoleTab.cpp
    src/ui/ConsoleTab.h
    src/ui/ConnectionDialog.h
    src/ui/EditorTabs.cpp
    src/ui/EditorTabs.h
    src/ui/ExportDialog.cpp
    src/ui/ExportDialog.h
    src/ui/FindBar.cpp
    src/ui/FindBar.h
    src/ui/FontZoom.cpp
    src/ui/FontZoom.h
    src/ui/HistoryDialog.cpp
    src/ui/HistoryDialog.h
    src/ui/ImportDialog.cpp
    src/ui/ImportDialog.h
    src/ui/LauncherWindow.cpp
    src/ui/LauncherWindow.h
    src/ui/Icons.cpp
    src/ui/Icons.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/MetadataService.cpp
    src/ui/MetadataService.h
    src/ui/MonitorTab.cpp
    src/ui/MonitorTab.h
    src/ui/ErdView.cpp
    src/ui/ErdView.h
    src/ui/RenameDialog.cpp
    src/ui/RenameDialog.h
    src/ui/ReverseDialog.cpp
    src/ui/ReverseDialog.h
    src/ui/ReverseTab.cpp
    src/ui/ReverseTab.h
    src/ui/ObjectExplorer.cpp
    src/ui/ObjectExplorer.h
    src/ui/PlanView.cpp
    src/ui/PlanView.h
    src/ui/QuerySession.cpp
    src/ui/QuerySession.h
    src/ui/QueryTab.cpp
    src/ui/QueryTab.h
    src/ui/ResultModel.cpp
    src/ui/ResultModel.h
    src/ui/ResultPanel.cpp
    src/ui/ResultPanel.h
    src/ui/ScrollHelpers.cpp
    src/ui/ScrollHelpers.h
    src/ui/PlanObjectWindow.cpp
    src/ui/PlanObjectWindow.h
    src/ui/ScriptFoldersDialog.cpp
    src/ui/ScriptFoldersDialog.h
    src/ui/SingleInstance.cpp
    src/ui/SingleInstance.h
    src/ui/SqlCompleter.cpp
    src/ui/SqlCompleter.h
    src/ui/SqlEditor.cpp
    src/ui/SqlEditor.h
    src/ui/SqlHighlighter.cpp
    src/ui/SqlHighlighter.h
    src/ui/TableDetailTab.cpp
    src/ui/TableDetailTab.h
    src/ui/Theme.cpp
    src/ui/Theme.h
    src/ui/UiHelpers.h
    src/ui/WindowsIntegration.cpp
    src/ui/WindowsIntegration.h
)

# 자체 아이콘 (:/icons/<이름>.svg)
qt_add_resources(dbaro_lib "icons"
    PREFIX "/"
    BASE "resources"
    FILES
        resources/icons/add.svg
        resources/icons/ai.svg
        resources/icons/close.svg
        resources/icons/column.svg
        resources/icons/commit.svg
        resources/icons/connection-add.svg
        resources/icons/connections.svg
        resources/icons/console.svg
        resources/icons/database.svg
        resources/icons/dbaro.svg
        resources/icons/explain.svg
        resources/icons/explain-actual.svg
        resources/icons/folder.svg
        resources/icons/hide.svg
        resources/icons/key.svg
        resources/icons/monitor.svg
        resources/icons/object.svg
        resources/icons/rollback.svg
        resources/icons/routine.svg
        resources/icons/run-script.svg
        resources/icons/run.svg
        resources/icons/schema.svg
        resources/icons/stop.svg
        resources/icons/table.svg
        resources/icons/view.svg
)

target_include_directories(dbaro_lib PUBLIC src)

target_link_libraries(dbaro_lib PUBLIC
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
)
target_link_libraries(dbaro_lib PRIVATE Qt6::CorePrivate)

if(WIN32)
    # SecretStore: Windows 자격 증명 관리자 (CredWriteW 등)
    # WindowsIntegration: 작업 표시줄 점프 목록(COM), 창 테두리 색(DWM)
    target_link_libraries(dbaro_lib PRIVATE Advapi32 Ole32 Shell32 Propsys Shlwapi Dwmapi User32)
elseif(APPLE)
    # SecretStore: 로그인 키체인 (SecItemAdd 등)
    target_link_libraries(dbaro_lib PRIVATE "-framework Security" "-framework CoreFoundation")
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

if(APPLE)
    # 앱 번들(DBaro.app) 아이콘과 Info.plist 값
    set(DBARO_ICNS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/dbaro.icns")
    set_source_files_properties("${DBARO_ICNS_PATH}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(dbaro PRIVATE "${DBARO_ICNS_PATH}")
    set_target_properties(dbaro PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "DBaro"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.leebong-su.dbaro"
        MACOSX_BUNDLE_ICON_FILE "dbaro.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
endif()

set_target_properties(dbaro PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# 명령줄 도구 (콘솔 프로그램). GUI와 같은 폴더에 두고 같은 연결 목록·비밀번호·실행 기록을 쓴다.
qt_add_executable(dbaro-cli
    src/cli/main.cpp
)
target_link_libraries(dbaro-cli PRIVATE dbaro_lib)
if(WIN32)
    target_sources(dbaro-cli PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/dbaro.rc")
endif()
set_target_properties(dbaro-cli PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
)

# 앱 단위 테스트 (ctest). 배포 스크립트는 실행 파일만 빌드하므로 배포본에는 들어가지 않는다.
option(DBARO_BUILD_TESTS "Build the app unit tests" ON)
if(DBARO_BUILD_TESTS)
    find_package(Qt6 6.8 COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        qt_add_executable(dbaro_tests
            tests/SqlRenameTest.cpp
        )
        target_link_libraries(dbaro_tests PRIVATE dbaro_lib Qt6::Test)
        set_target_properties(dbaro_tests PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
        add_test(NAME dbaro_tests COMMAND dbaro_tests)

        # AI 도우미: 가짜 Claude API 서버로 스트리밍·도구 루프 확인 (네트워크 밖으로 나가지 않는다)
        qt_add_executable(dbaro_ai_tests
            tests/AiAgentTest.cpp
        )
        target_link_libraries(dbaro_ai_tests PRIVATE dbaro_lib Qt6::Test Qt6::Network)
        set_target_properties(dbaro_ai_tests PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
        add_test(NAME dbaro_ai_tests COMMAND dbaro_ai_tests)
    endif()
endif()
