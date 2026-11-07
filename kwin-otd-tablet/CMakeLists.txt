cmake_minimum_required(VERSION 3.20)
project(kwin-otd-tablet CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui DBus Widgets)

# SteamOS ships KWin's headers but not extra-cmake-modules, which KWinConfig.cmake needs,
# so point at KWin directly instead of find_package(KWin).
find_library(KWIN_LIBRARY NAMES kwin REQUIRED)

add_library(otdtablet MODULE otdtablet.cpp)
target_include_directories(otdtablet PRIVATE
    /usr/include/kwin
    /usr/include/KF6/KConfig
    /usr/include/KF6/KConfigCore
    /usr/include/KF6/KCoreAddons
    /usr/include/KF6/KWindowSystem
    /usr/include/libdrm
)
target_link_libraries(otdtablet PRIVATE ${KWIN_LIBRARY} Qt6::Core Qt6::Gui Qt6::DBus Qt6::Widgets)
set_target_properties(otdtablet PROPERTIES PREFIX "")

# Installs to <prefix>/kwin/plugins/otdtablet.so; KWin finds it through QT_PLUGIN_PATH=<prefix>
install(TARGETS otdtablet LIBRARY DESTINATION kwin/plugins)
