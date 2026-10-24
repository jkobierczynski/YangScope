cmake_minimum_required(VERSION 3.19)

project(YangScope
    VERSION 0.3.0
    DESCRIPTION "Native YANG / NETCONF / gNMI explorer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(YANGSCOPE_BUILD_TESTS "Build the test suite" ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)

# --- libssh (NETCONF over SSH) ----------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBSSH IMPORTED_TARGET libssh>=0.9)
endif()
add_library(yangscope_libssh INTERFACE)
if(LIBSSH_FOUND)
    target_link_libraries(yangscope_libssh INTERFACE PkgConfig::LIBSSH)
else()
    # vcpkg and some distributions ship a CMake package instead
    find_package(libssh CONFIG REQUIRED)
    if(TARGET ssh::ssh)
        target_link_libraries(yangscope_libssh INTERFACE ssh::ssh)
    else()
        target_link_libraries(yangscope_libssh INTERFACE ssh)
    endif()
endif()

# --- libyang (CESNET) -------------------------------------------------------
# Supported: libyang 2.1 and newer (tested with 2.1.30 and 6.4.x).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBYANG IMPORTED_TARGET libyang>=2.1)
endif()
if(LIBYANG_FOUND)
    add_library(yangscope_libyang INTERFACE)
    target_link_libraries(yangscope_libyang INTERFACE PkgConfig::LIBYANG)
else()
    # Fallback for platforms without pkg-config (e.g. vcpkg on Windows)
    find_path(LIBYANG_INCLUDE_DIR libyang/libyang.h REQUIRED)
    find_library(LIBYANG_LIBRARY NAMES yang libyang REQUIRED)
    add_library(yangscope_libyang INTERFACE)
    target_include_directories(yangscope_libyang INTERFACE ${LIBYANG_INCLUDE_DIR})
    target_link_libraries(yangscope_libyang INTERFACE ${LIBYANG_LIBRARY})
endif()

# The libyang log callback signature changed between major versions;
# detect which one we have instead of hard-coding version numbers.
include(CheckCXXSourceCompiles)
if(LIBYANG_FOUND)
    set(CMAKE_REQUIRED_INCLUDES ${LIBYANG_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${LIBYANG_LINK_LIBRARIES})
else()
    set(CMAKE_REQUIRED_INCLUDES ${LIBYANG_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES ${LIBYANG_LIBRARY})
endif()
check_cxx_source_compiles("
#include <libyang/libyang.h>
static void cb(LY_LOG_LEVEL, const char *, const char *, const char *, uint64_t) {}
int main() { ly_set_log_clb(cb); return 0; }
" YANGSCOPE_LY_LOG_CLB_V3)
# Newer releases expose the directory with libyang's own internal modules.
check_cxx_source_compiles("
#include <libyang/libyang.h>
int main() { return ly_yang_module_dir() != nullptr ? 0 : 1; }
" YANGSCOPE_HAVE_LY_MODULE_DIR)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
unset(CMAKE_REQUIRED_LIBRARIES)

# --- core library (no GUI) --------------------------------------------------
add_library(yangscope_core STATIC
    src/core/YangContext.cpp
    src/core/YangContext.h
    src/core/NodeInfo.cpp
    src/core/NodeInfo.h
    src/core/LyCompat.h
    src/core/EditConfig.cpp
    src/core/EditConfig.h
    src/core/DataTree.cpp
    src/core/DataTree.h
    src/core/NetconfFraming.cpp
    src/core/NetconfFraming.h
    src/core/NetconfXml.cpp
    src/core/NetconfXml.h
    src/core/NetconfSession.cpp
    src/core/NetconfSession.h
    src/core/SchemaSync.cpp
    src/core/SchemaSync.h
)
target_include_directories(yangscope_core PUBLIC src)
target_link_libraries(yangscope_core PUBLIC Qt6::Core yangscope_libyang yangscope_libssh)
if(YANGSCOPE_LY_LOG_CLB_V3)
    target_compile_definitions(yangscope_core PRIVATE YANGSCOPE_LY_LOG_CLB_V3=1)
endif()
if(YANGSCOPE_HAVE_LY_MODULE_DIR)
    target_compile_definitions(yangscope_core PRIVATE YANGSCOPE_HAVE_LY_MODULE_DIR=1)
endif()
target_compile_definitions(yangscope_core PUBLIC
    YANGSCOPE_VERSION="${PROJECT_VERSION}")

# --- GUI application --------------------------------------------------------
add_library(yangscope_gui STATIC
    src/gui/MainWindow.cpp
    src/gui/MainWindow.h
    src/gui/SchemaModel.cpp
    src/gui/SchemaModel.h
    src/gui/YangHighlighter.cpp
    src/gui/YangHighlighter.h
    src/gui/SearchPanel.cpp
    src/gui/SearchPanel.h
    src/gui/NetconfWorker.cpp
    src/gui/NetconfWorker.h
    src/gui/ConnectDialog.cpp
    src/gui/ConnectDialog.h
    src/gui/NetconfPanel.cpp
    src/gui/NetconfPanel.h
    src/gui/DataTreeWidget.cpp
    src/gui/DataTreeWidget.h
    src/gui/XmlHighlighter.cpp
    src/gui/XmlHighlighter.h
)
target_link_libraries(yangscope_gui PUBLIC yangscope_core Qt6::Widgets)

qt_add_executable(yangscope
    src/main.cpp
)
target_link_libraries(yangscope PRIVATE yangscope_gui)
qt_add_resources(yangscope "icons"
    PREFIX "/"
    BASE packaging
    FILES packaging/yangscope.png)
set_target_properties(yangscope PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.jkobierczynski.yangscope
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

if(MSVC)
    target_compile_options(yangscope_core PRIVATE /W4)
else()
    target_compile_options(yangscope_core PRIVATE -Wall -Wextra)
endif()

include(GNUInstallDirs)
install(TARGETS yangscope
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    install(FILES packaging/yangscope.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/yangscope.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

# --- tests ------------------------------------------------------------------
if(YANGSCOPE_BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE yangscope_core)
    target_compile_definitions(test_core PRIVATE
        YANGSCOPE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME core COMMAND test_core)

    add_executable(test_edit tests/test_edit.cpp)
    target_link_libraries(test_edit PRIVATE yangscope_core)
    target_compile_definitions(test_edit PRIVATE
        YANGSCOPE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
        YANGSCOPE_TEST_NETCONF="${CMAKE_CURRENT_SOURCE_DIR}/tests/netconf")
    add_test(NAME edit COMMAND test_edit)

    # NETCONF: framing/XML unit tests always run; the session tests need
    # Python 3 with paramiko for the bundled test server and are skipped
    # (exit code 77) otherwise.
    find_package(Python3 COMPONENTS Interpreter)
    add_executable(test_netconf tests/test_netconf.cpp)
    target_link_libraries(test_netconf PRIVATE yangscope_core)
    target_compile_definitions(test_netconf PRIVATE
        YANGSCOPE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
        YANGSCOPE_TEST_NETCONF="${CMAKE_CURRENT_SOURCE_DIR}/tests/netconf"
        YANGSCOPE_PYTHON="${Python3_EXECUTABLE}")
    add_test(NAME netconf COMMAND test_netconf)
    set_tests_properties(netconf PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

    # End-to-end GUI test against the test server (offscreen platform).
    add_executable(test_gui tests/test_gui.cpp)
    target_link_libraries(test_gui PRIVATE yangscope_gui)
    target_compile_definitions(test_gui PRIVATE
        YANGSCOPE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
        YANGSCOPE_TEST_NETCONF="${CMAKE_CURRENT_SOURCE_DIR}/tests/netconf"
        YANGSCOPE_PYTHON="${Python3_EXECUTABLE}")
    add_test(NAME gui COMMAND test_gui)
    set_tests_properties(gui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
