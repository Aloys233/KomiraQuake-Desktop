cmake_minimum_required(VERSION 3.21)

project(KomiraQuake VERSION 1.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 链接期优化能力探测（是否启用见下方 komiraquake 目标）。
# 注意**不要**用全局的 CMAKE_INTERPROCEDURAL_OPTIMIZATION：它会把 sentry/crashpad
# 这些第三方静态库一起拉进 LTO，构建时间翻倍且极易踩到它们的编译选项问题。
# 只在 app 目标上开启，热点正是自己的几何/LOD 代码。
include(CheckIPOSupported)
check_ipo_supported(RESULT KOMIRA_IPO_SUPPORTED OUTPUT KOMIRA_IPO_ERROR LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core Gui Widgets Quick QuickControls2 Qml Network WebSockets Sql Multimedia Svg Concurrent OpenGL
)

# 场景图的 QSG* 头会经 qsgtexture.h -> qsgtexture_platform.h 引入 <GL/gl.h>。
# Nix 把 libglvnd 拆成多个 output，Qt6::OpenGL 只带库、不带开发头，于是编译期
# 找不到 GL/gl.h（自建环境里 GL 头来自全局 include 路径，NixOS 上没有）。
# 这里显式补一个 include 目录：优先用 pkg-config，退化到在 Nix store 里找。
find_package(PkgConfig QUIET)
set(KOMIRA_GL_INCLUDE_DIR "")
if(PkgConfig_FOUND)
    pkg_check_modules(KOMIRA_GL QUIET gl)
    if(KOMIRA_GL_INCLUDE_DIRS)
        set(KOMIRA_GL_INCLUDE_DIR ${KOMIRA_GL_INCLUDE_DIRS})
    endif()
endif()
if(NOT KOMIRA_GL_INCLUDE_DIR)
    file(GLOB _komira_glvnd_dirs "/nix/store/*-libglvnd-*-dev")
    foreach(_dir ${_komira_glvnd_dirs})
        if(EXISTS "${_dir}/include/GL/gl.h")
            set(KOMIRA_GL_INCLUDE_DIR "${_dir}/include")
            break()
        endif()
    endforeach()
endif()
if(KOMIRA_GL_INCLUDE_DIR)
    message(STATUS "GL headers: ${KOMIRA_GL_INCLUDE_DIR}")
else()
    message(WARNING "找不到 GL/gl.h；场景图头文件可能编译失败（非 Nix 系统通常由 mesa/glvnd 提供）")
endif()

qt_standard_project_setup(REQUIRES 6.5)

# Official Sentry Native SDK with its out-of-process Crashpad backend. The
# handler executable is installed beside KomiraQuake below; the runtime points
# to that absolute path instead of relying on PATH/current working directory.
include(FetchContent)
set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "Build sentry-native statically" FORCE)
set(SENTRY_BACKEND crashpad CACHE STRING "Sentry crash backend" FORCE)
set(SENTRY_BUILD_TESTS OFF CACHE BOOL "Do not build sentry-native tests" FORCE)
set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "Do not build sentry-native examples" FORCE)
set(SENTRY_ENABLE_INSTALL OFF CACHE BOOL "Install sentry-native as a separate package" FORCE)
if(WIN32)
    set(SENTRY_TRANSPORT winhttp CACHE STRING "Sentry transport" FORCE)
else()
    set(SENTRY_TRANSPORT curl CACHE STRING "Sentry transport" FORCE)
endif()
FetchContent_Declare(sentry_native
    GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
    GIT_TAG 0.12.1
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(sentry_native)

qt_add_executable(komiraquake WIN32
    src/main.cpp

    # model / core
    src/model/event_list_model.h
    src/model/event_list_model.cpp
    src/model/station_snapshot.h
    src/model/station_snapshot.cpp
    src/core/quake_calculator.cpp
    src/core/intensity_calculator.cpp
    src/core/coordinate_transform.cpp
    src/core/ip_geo_lookup.cpp
    src/core/travel_table.cpp
    src/core/travel_time_service.cpp
    src/core/event_gate.cpp
    src/core/geo_geometry.cpp
    src/core/time_sync.cpp
    src/core/vector_map_data.cpp
    src/core/source_estimator.cpp
    src/core/region_names.cpp

    # source
    src/source/earthquake_source.h
    src/source/eew_parser.cpp
    src/source/wolfx_source.cpp
    src/source/pancakes_parser.cpp
    src/source/pancakes_source.cpp
    src/source/jian_protocol.cpp
    src/source/jian_parser.cpp
    src/source/jian_source.cpp
    src/source/kmoni_station_parser.cpp
    src/source/kmoni_station_source.cpp
    src/source/snet_station_parser.cpp
    src/source/snet_station_source.cpp
    src/source/kma_station_parser.cpp
    src/source/kma_station_source.cpp
    src/source/pancakes_seis_parser.cpp
    src/source/pancakes_seis_source.cpp
    src/source/whews_protocol.cpp
    src/source/whews_parser.cpp
    src/source/whews_source.cpp
    src/source/wolfx_seis_parser.cpp
    src/source/wolfx_seis_source.cpp
    src/source/simulated_protocol.cpp
    src/source/simulated_parser.cpp
    src/source/simulated_source.cpp

    # store / prefs
    src/store/history_store.cpp
    src/prefs/settings_store.cpp

    # services
    src/service/location_service.cpp
    src/service/ntp_clock.cpp
    src/service/alert_sound_service.cpp
    src/service/alert_announcer.cpp
    src/service/tray_controller.cpp
    src/service/autostart.cpp
    src/service/update_service.cpp
    src/service/sentry_service.cpp
    src/service/source_estimation_service.cpp
        src/service/travel_time_arrivals.h

    # app
    src/app/app_controller.cpp

    # map
    src/map/vector_map_item.cpp
    src/map/vector_mesh.cpp

    # core（三角化：纯 C++，无 Qt）
    src/core/polygon_tessellator.cpp
)

target_include_directories(komiraquake PRIVATE src)
if(KOMIRA_GL_INCLUDE_DIR)
    target_include_directories(komiraquake PRIVATE ${KOMIRA_GL_INCLUDE_DIR})
endif()
# 生成的 qmltyperegistrations.cpp 以 `#include <vector_map_item.h>` 引入 SOURCES 里的头
# （无路径），故 src/map 必须也在头文件搜索路径中，否则注册代码找不到类型。
target_include_directories(komiraquake PRIVATE src/map)

# 供 C++ 与 QML 展示的版本号；正式构建由 CI 通过 KOMIRA_BUILD_VERSION 对齐 release tag。
set(KOMIRA_VERSION_VALUE "${PROJECT_VERSION}")
if(DEFINED KOMIRA_BUILD_VERSION AND NOT KOMIRA_BUILD_VERSION STREQUAL "")
    set(KOMIRA_VERSION_VALUE "${KOMIRA_BUILD_VERSION}")
endif()
target_compile_definitions(komiraquake PRIVATE KOMIRA_VERSION="${KOMIRA_VERSION_VALUE}")

if(KOMIRA_IPO_SUPPORTED)
    set_property(TARGET komiraquake PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "IPO 不可用（${KOMIRA_IPO_ERROR}），跳过链接期优化")
endif()

target_link_libraries(komiraquake PRIVATE
    sentry
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
    Qt6::Multimedia
    Qt6::Svg
    Qt6::Concurrent
    # 场景图的 QSG* 头经由 qsgtexture_platform.h 间接需要 GL 头；Qt6::OpenGL 提供
    # 它的 include 路径（Nix 下 GL 头不在默认搜索路径）。
    Qt6::OpenGL
)

set(KOMIRA_QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/WarningHud.qml
    qml/HudCard.qml
    qml/IntensityBadge.qml
    qml/EventTile.qml
    qml/EventListSidebar.qml
    qml/MapView.qml
    qml/StationIntensityCard.qml
    qml/SourceEstimateCard.qml
    qml/EstimateMarker.qml
    qml/StationTooltip.qml
    qml/SettingsPage.qml
    qml/settings/AppearanceSection.qml
    qml/settings/MapSection.qml
    qml/settings/LocationSection.qml
    qml/settings/WarningSection.qml
    qml/settings/AudioSection.qml
    qml/settings/SourceSection.qml
    qml/settings/ClockSection.qml
    qml/settings/AboutSection.qml
    qml/settings/SourceEstimationSection.qml
    qml/SettingSlider.qml
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

# qmlcachegen 必须能找到 Qt 自带的 QML 类型（QtQuick / QtQuick.Controls …），否则
# 「编译绑定」大面积退化成运行时解释：实测缺导入路径时 1855 条绑定只有 72 条能编译
# （3.9%），补上后绝大多数恢复。qt_add_qml_module 在跨模块引用时才自动加路径，
# 这里显式声明依赖，保证主题、图层、设置页这些跨文件引用的类型可静态解析。
# Qt 的 qml 目录布局因发行方式而异（Nix：<prefix>/lib/qt-6/qml；官方安装器：
# <prefix>/qml；部分发行版：<prefix>/lib64/qt6/qml）。这里按候选逐个探测，
# 认「里面确实有 QtQuick 目录」的那个，避免某个平台静默退回「没有导入路径」。
# QT6_INSTALL_PREFIX 指向 qtbase，而 QtQuick 的 QML 类型在 qtdeclarative —— 两者
# 在 Nix 里是不同的 store 路径，所以还要从 Qt6::Qml 的库位置反推一次前缀。
get_target_property(_komira_qml_lib Qt6::Qml IMPORTED_LOCATION)
if(NOT _komira_qml_lib)
    get_target_property(_komira_qml_lib Qt6::Qml LOCATION)
endif()
if(_komira_qml_lib)
    get_filename_component(_komira_qml_libdir "${_komira_qml_lib}" DIRECTORY)
    get_filename_component(_komira_qml_prefix "${_komira_qml_libdir}" DIRECTORY)
endif()

set(KOMIRA_QML_IMPORT_PATH "")
foreach(_komira_qml_candidate IN ITEMS
        "${_komira_qml_prefix}/lib/qt-6/qml"
        "${_komira_qml_prefix}/lib64/qt6/qml"
        "${_komira_qml_prefix}/qml"
        "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}"
        "${QT6_INSTALL_PREFIX}/lib/qt-6/qml"
        "${QT6_INSTALL_PREFIX}/lib64/qt6/qml"
        "${QT6_INSTALL_PREFIX}/qml")
    if(IS_DIRECTORY "${_komira_qml_candidate}/QtQuick")
        set(KOMIRA_QML_IMPORT_PATH "${_komira_qml_candidate}")
        break()
    endif()
endforeach()
if(KOMIRA_QML_IMPORT_PATH STREQUAL "")
    message(WARNING "找不到 Qt 的 QML 导入目录；编译绑定将退化为运行时解释（功能不受影响，"
                    "但 QML 绑定会慢数倍）。")
endif()

qt_add_qml_module(komiraquake
    URI KomiraQuake
    VERSION 1.0
    QML_FILES ${KOMIRA_QML_FILES}
    IMPORT_PATH ${KOMIRA_QML_IMPORT_PATH}
    # 矢量地图渲染项：用 QML_ELEMENT 注册（由 qt_add_qml_module 生成注册代码，
    # 无需也不应再手写 qmlRegisterType，否则与模块系统冲突）。
    SOURCES
        src/map/vector_map_item.h
        src/map/vector_map_item.cpp
        src/map/station_map_item.h
        src/map/station_map_item.cpp
)

file(GLOB KOMIRA_ICON_FILES CONFIGURE_DEPENDS qml/icons/*.svg qml/icons/LICENSE.txt)
qt_add_resources(komiraquake lucide_icons PREFIX "/icons" BASE "qml/icons" FILES ${KOMIRA_ICON_FILES})


# Google Sans（OFL）静态拉丁子集，见 tools/import_google_sans.py；打包进二进制避免依赖系统字体。
file(GLOB KOMIRA_FONT_FILES CONFIGURE_DEPENDS fonts/*.ttf fonts/OFL.txt)
qt_add_resources(komiraquake komira_fonts PREFIX "/fonts" BASE "fonts" FILES ${KOMIRA_FONT_FILES})

# 资源（travel_times.json / sounds）拷贝到可执行文件旁的 assets/
add_custom_command(TARGET komiraquake POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:komiraquake>/assets
    COMMENT "Copying KomiraQuake assets next to the binary"
)

# Crashpad is an out-of-process handler, so it must be shipped with every
# executable that can initialise Sentry (the app and the integration tests).
function(komira_copy_crashpad_handler target_name)
    if(TARGET crashpad_handler)
        add_dependencies(${target_name} crashpad_handler)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:crashpad_handler>
                    $<TARGET_FILE_DIR:${target_name}>
            COMMENT "Copying Crashpad handler next to ${target_name}"
        )
    endif()
endfunction()
komira_copy_crashpad_handler(komiraquake)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/ DESTINATION share/komiraquake/assets)

install(TARGETS komiraquake
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(TARGET crashpad_handler)
    install(TARGETS crashpad_handler
        RUNTIME DESTINATION bin
    )
endif()

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
        src/core/geo_geometry.cpp src/core/vector_map_data.cpp
        src/core/intensity_calculator.cpp src/core/quake_calculator.cpp
        src/core/time_sync.cpp src/core/travel_table.cpp
        src/core/source_estimator.cpp src/core/region_names.cpp
        src/core/polygon_tessellator.cpp)
    target_include_directories(core_engine_test PRIVATE src)
    add_test(NAME core_engine COMMAND core_engine_test)
    qt_add_executable(qt_business_test tests/qt_business_test.cpp
        src/core/coordinate_transform.cpp src/core/event_gate.cpp
        src/core/geo_geometry.cpp src/core/vector_map_data.cpp
        src/core/intensity_calculator.cpp src/core/quake_calculator.cpp
        src/core/ip_geo_lookup.cpp src/service/location_service.cpp
        src/core/travel_table.cpp src/core/travel_time_service.cpp
        src/service/autostart.cpp
        src/source/eew_parser.cpp src/source/wolfx_source.cpp
        src/source/earthquake_source.h
        src/source/pancakes_parser.cpp src/source/pancakes_source.cpp
        src/source/jian_protocol.cpp src/source/jian_parser.cpp src/source/jian_source.cpp
        src/source/kmoni_station_parser.cpp src/source/kmoni_station_source.cpp
        src/source/snet_station_parser.cpp src/source/snet_station_source.cpp
        src/source/kma_station_parser.cpp src/source/kma_station_source.cpp
        src/source/pancakes_seis_parser.cpp src/source/pancakes_seis_source.cpp
        src/source/whews_protocol.cpp src/source/whews_parser.cpp src/source/whews_source.cpp
        src/source/wolfx_seis_parser.cpp src/source/wolfx_seis_source.cpp
        src/source/simulated_protocol.cpp src/source/simulated_parser.cpp
        src/source/simulated_source.cpp
        src/service/sentry_service.cpp
        src/service/source_estimation_service.cpp
        src/service/travel_time_arrivals.h
        src/core/source_estimator.cpp src/core/region_names.cpp
        src/store/history_store.cpp src/prefs/settings_store.cpp)
    target_include_directories(qt_business_test PRIVATE src)
    target_link_libraries(qt_business_test PRIVATE Qt6::Test Qt6::Network Qt6::WebSockets Qt6::Sql sentry)
    komira_copy_crashpad_handler(qt_business_test)
    add_test(NAME qt_business COMMAND qt_business_test)
    file(GLOB_RECURSE GUI_TEST_SOURCES CONFIGURE_DEPENDS src/*.cpp)
    list(FILTER GUI_TEST_SOURCES EXCLUDE REGEX "/main.cpp$")
    # GLOB 只收 .cpp；含 Q_OBJECT 的接口头需显式列出，AUTOMOC 才会处理其 moc。
    qt_add_executable(gui_test tests/gui_test.cpp ${GUI_TEST_SOURCES} src/source/earthquake_source.h
        src/source/simulated_source.h src/model/event_list_model.h)
    target_include_directories(gui_test PRIVATE src)
    if(KOMIRA_GL_INCLUDE_DIR)
        target_include_directories(gui_test PRIVATE ${KOMIRA_GL_INCLUDE_DIR})
    endif()
    # gui_test 用 qt_add_resources 打包 QML 而非 qt_add_qml_module，故拿不到 QML_ELEMENT
    # 自动生成的注册代码；这里显式注册 VectorMapItem，否则 MapView.qml 加载失败。
    target_include_directories(gui_test PRIVATE src/map)
    target_compile_definitions(gui_test PRIVATE KOMIRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        KOMIRA_VERSION="${PROJECT_VERSION}")
    target_link_libraries(gui_test PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Widgets
        Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::WebSockets Qt6::Sql
        Qt6::Multimedia Qt6::Svg Qt6::Concurrent Qt6::OpenGL sentry)
    komira_copy_crashpad_handler(gui_test)
    qt_add_resources(gui_test test_lucide_icons PREFIX "/icons" BASE "qml/icons" FILES ${KOMIRA_ICON_FILES})
    qt_add_resources(gui_test test_komira_fonts PREFIX "/fonts" BASE "fonts" FILES ${KOMIRA_FONT_FILES})
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
