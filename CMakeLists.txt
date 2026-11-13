# SPDX-License-Identifier: CC0-1.0
cmake_minimum_required(VERSION 3.20)
project(ng-fsevents VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
include(GNUInstallDirs)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core DBus)

add_executable(ng-fseventsd
    src/main.cpp
    src/watcher.cpp
    src/newesttree.cpp
    src/service.cpp
)
target_link_libraries(ng-fseventsd PRIVATE Qt6::Core Qt6::DBus)
target_compile_options(ng-fseventsd PRIVATE -Wall -Wextra)

install(TARGETS ng-fseventsd DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
configure_file(data/ng-fsevents.service.in ng-fsevents.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ng-fsevents.service DESTINATION lib/systemd/system)
install(FILES data/org.ng.FsEvents1.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
