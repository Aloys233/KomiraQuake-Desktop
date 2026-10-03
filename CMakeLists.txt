cmake_minimum_required(VERSION 3.21)

project(KomiraQuake VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core Gui Widgets Quick QuickControls2 Qml Network WebSockets Sql Multimedia TextToSpeech Svg
)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(komiraquake
    src/main.cpp

    # model / core
    src/core/quake_calculator.cpp
    src/core/intensity_calculator.cpp
    src/core/coordinate_transform.cpp
    src/core/ip_geo_lookup.cpp
    src/core/travel_table.cpp
    src/core/travel_time_service.cpp
    src/core/event_gate.cpp
    src/core/time_sync.cpp

    # source
    src/source/eew_parser.cpp
    src/source/wolfx_source.cpp

    # store / prefs
    src/store/history_store.cpp
    src/prefs/settings_store.cpp

    # services
    src/service/location_service.cpp
    src/service/ntp_clock.cpp
    src/service/alert_sound_service.cpp
    src/service/speech_service.cpp
    src/service/alert_announcer.cpp
    src/service/tray_controller.cpp

    # app
    src/app/app_controller.cpp
)

target_include_directories(komiraquake PRIVATE src)

target_link_libraries(komiraquake PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
    Qt6::Multimedia
    Qt6::TextToSpeech
    Qt6::Svg
)

set(KOMIRA_QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/WarningOverlay.qml
    qml/WarningHud.qml
    qml/HudCard.qml
    qml/IntensityBadge.qml
    qml/EventTile.qml
    qml/EventListSidebar.qml
    qml/MapView.qml
    qml/SettingsPage.qml
    qml/GlassCard.qml
    qml/GlassButton.qml
    qml/GlassSwitch.qml
    qml/GlassSlider.qml
    qml/GlassComboBox.qml
    qml/GlassTextField.qml
    qml/Toast.qml
    qml/AppIcon.qml
    qml/BackdropBlur.qml
    qml/SettingsSection.qml
)

# 让 QML 落在模块根目录（qrc:/qt/qml/KomiraQuake/Xxx.qml）。
# 否则子目录路径会导致 "qmlRegisterType requires absolute URLs" 以及 QTP0004 警告。
foreach(qml_file ${KOMIRA_QML_FILES})
    get_filename_component(qml_name ${qml_file} NAME)
    set_source_files_properties(${qml_file} PROPERTIES QT_RESOURCE_ALIAS ${qml_name})
endforeach()

qt_add_qml_module(komiraquake
    URI KomiraQuake
    VERSION 1.0
    QML_FILES ${KOMIRA_QML_FILES}
)

file(GLOB KOMIRA_ICON_FILES CONFIGURE_DEPENDS qml/icons/*.svg qml/icons/LICENSE.txt)
qt_add_resources(komiraquake lucide_icons PREFIX "/icons" BASE "qml/icons" FILES ${KOMIRA_ICON_FILES})

# 资源（travel_times.json / sounds）拷贝到可执行文件旁的 assets/
add_custom_command(TARGET komiraquake POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:komiraquake>/assets
    COMMENT "Copying KomiraQuake assets next to the binary"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/ DESTINATION share/komiraquake/assets)

install(TARGETS komiraquake
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

# ---------------------------------------------------------------------------
# Windows：install/CPack 阶段收集 Qt 运行时依赖（windeployqt + QML import 扫描），
# 把 Qt DLL、插件与 QML 运行时模块收进安装目录。Linux 分发包（deb / AppImage）
# 由 CI 用 linuxdeploy 打包，不走这里。
# ---------------------------------------------------------------------------
if(WIN32)
    qt_generate_deploy_qml_app_script(
        TARGET komiraquake
        OUTPUT_SCRIPT komiraquake_deploy_script
        NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR
        DEPLOY_USER_QML_MODULES_ON_UNSUPPORTED_PLATFORM
    )
    install(SCRIPT ${komiraquake_deploy_script})
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(core_engine_test tests/core_engine_test.cpp
        src/core/coordinate_transform.cpp src/core/event_gate.cpp
        src/core/intensity_calculator.cpp src/core/quake_calculator.cpp
        src/core/time_sync.cpp src/core/travel_table.cpp)
    target_include_directories(core_engine_test PRIVATE src)
    add_test(NAME core_engine COMMAND core_engine_test)
    qt_add_executable(qt_business_test tests/qt_business_test.cpp
        src/core/coordinate_transform.cpp src/core/event_gate.cpp
        src/core/intensity_calculator.cpp src/core/quake_calculator.cpp
        src/core/ip_geo_lookup.cpp src/service/location_service.cpp
        src/core/travel_table.cpp src/core/travel_time_service.cpp
        src/source/eew_parser.cpp src/source/wolfx_source.cpp
        src/store/history_store.cpp src/prefs/settings_store.cpp)
    target_include_directories(qt_business_test PRIVATE src)
    target_link_libraries(qt_business_test PRIVATE Qt6::Test Qt6::Network Qt6::WebSockets Qt6::Sql)
    add_test(NAME qt_business COMMAND qt_business_test)
    file(GLOB_RECURSE GUI_TEST_SOURCES CONFIGURE_DEPENDS src/*.cpp)
    list(FILTER GUI_TEST_SOURCES EXCLUDE REGEX "/main.cpp$")
    qt_add_executable(gui_test tests/gui_test.cpp ${GUI_TEST_SOURCES})
    target_include_directories(gui_test PRIVATE src)
    target_compile_definitions(gui_test PRIVATE KOMIRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(gui_test PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Widgets
        Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::WebSockets Qt6::Sql
        Qt6::Multimedia Qt6::TextToSpeech Qt6::Svg)
    qt_add_resources(gui_test test_lucide_icons PREFIX "/icons" BASE "qml/icons" FILES ${KOMIRA_ICON_FILES})
    qt_add_resources(gui_test test_qml PREFIX "/qt/qml/KomiraQuake" FILES ${KOMIRA_QML_FILES})
    add_test(NAME qt_gui COMMAND gui_test)
    set_tests_properties(qt_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()

# 桌面集成：.desktop 与图标，deb / AppImage / NSIS 均复用这套安装规则
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/komiraquake.desktop
    DESTINATION share/applications)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/komiraquake.svg
    DESTINATION share/icons/hicolor/scalable/apps
    RENAME komiraquake.svg)

# ---------------------------------------------------------------------------
# 打包（CPack）
# 版本号默认取 project() 的版本；CI 用 -DCPACK_PACKAGE_VERSION=<tag 去掉 v> 覆盖。
# 目前只用于 Windows 的 NSIS 安装包；Linux 的 deb / AppImage 由 CI 脚本产出。
# ---------------------------------------------------------------------------
if(NOT CPACK_PACKAGE_VERSION)
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
endif()

set(CPACK_PACKAGE_NAME "komiraquake")
set(CPACK_PACKAGE_VENDOR "KomiraQuake")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Earthquake early warning and monitoring client")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Aloys233/KomiraQuake-Desktop")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "KomiraQuake")
set(CPACK_PACKAGE_EXECUTABLES "komiraquake" "KomiraQuake")

# NSIS：开始菜单/桌面快捷方式、覆盖安装前先卸旧版
set(CPACK_NSIS_DISPLAY_NAME "KomiraQuake")
set(CPACK_NSIS_PACKAGE_NAME "KomiraQuake")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_CREATE_DESKTOP_LINKS "komiraquake")

include(CPack)
