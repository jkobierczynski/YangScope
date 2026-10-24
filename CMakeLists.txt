cmake_minimum_required(VERSION 3.19)

project(YangScope
    VERSION 0.4.0
    DESCRIPTION "Native YANG / NETCONF / gNMI explorer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(YANGSCOPE_BUILD_TESTS "Build the test suite" ON)
set(YANGSCOPE_WITH_GNMI "AUTO" CACHE STRING
    "gNMI support (needs gRPC C++ and protobuf): ON, OFF or AUTO (on when found)")
set_property(CACHE YANGSCOPE_WITH_GNMI PROPERTY STRINGS AUTO ON OFF)

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

# --- gRPC / protobuf (gNMI, optional) ----------------------------------------
set(YANGSCOPE_GNMI OFF)
if(NOT YANGSCOPE_WITH_GNMI STREQUAL "OFF")
    # gRPC's CMake package loads protobuf's own CMake package. Look for it
    # first: CMake's FindProtobuf module defines only some of the protobuf::
    # targets, and protobuf's config (Homebrew, vcpkg) then refuses to load.
    find_package(gRPC CONFIG QUIET)
    if(gRPC_FOUND)
        if(NOT TARGET protobuf::libprotobuf)
            find_package(protobuf CONFIG QUIET)
        endif()
        if(TARGET protobuf::libprotobuf)
            set(Protobuf_FOUND TRUE)
        endif()
    else()
        find_package(Protobuf QUIET)
    endif()
    if(Protobuf_FOUND AND NOT Protobuf_INCLUDE_DIRS AND TARGET protobuf::libprotobuf)
        # config packages don't set it; needed to import the well-known types
        get_target_property(Protobuf_INCLUDE_DIRS protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
    endif()
    if(Protobuf_FOUND AND NOT gRPC_FOUND AND PkgConfig_FOUND)
        # Older distributions ship grpc++ without a CMake package
        pkg_check_modules(GRPCPP IMPORTED_TARGET grpc++)
        find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
        if(GRPCPP_FOUND AND GRPC_CPP_PLUGIN)
            add_library(gRPC::grpc++ INTERFACE IMPORTED)
            target_link_libraries(gRPC::grpc++ INTERFACE PkgConfig::GRPCPP)
            add_executable(gRPC::grpc_cpp_plugin IMPORTED)
            set_target_properties(gRPC::grpc_cpp_plugin PROPERTIES IMPORTED_LOCATION ${GRPC_CPP_PLUGIN})
            set(gRPC_FOUND TRUE)
        endif()
    endif()
    if(Protobuf_FOUND AND gRPC_FOUND AND TARGET gRPC::grpc++ AND TARGET gRPC::grpc_cpp_plugin)
        set(YANGSCOPE_GNMI ON)
    elseif(YANGSCOPE_WITH_GNMI STREQUAL "ON")
        message(FATAL_ERROR "YANGSCOPE_WITH_GNMI=ON but gRPC C++ / protobuf were not found")
    else()
        message(STATUS "gRPC C++ or protobuf not found: building without gNMI support")
    endif()
endif()
message(STATUS "gNMI support: ${YANGSCOPE_GNMI}")

# --- core library (no GUI) --------------------------------------------------
add_library(yangscope_core STATIC
    src/core/YangContext.cpp
    src/core/YangContext.h
    src/core/NodeInfo.cpp
    src/core/NodeInfo.h
    src/core/LyCompat.h
    src/core/EditConfig.cpp
    src/core/EditConfig.h
    src/core/LydUtil.cpp
    src/core/LydUtil.h
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
# Path handling and JSON <-> libyang conversion need no gRPC.
target_sources(yangscope_core PRIVATE
    src/core/GnmiPath.cpp
    src/core/GnmiPath.h
    src/core/GnmiData.cpp
    src/core/GnmiData.h
    src/core/GnmiTypes.h)

if(YANGSCOPE_GNMI)
    # Generated protobuf/gRPC code for the vendored gNMI protos
    set(_gnmi_root ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gnmi)
    set(_gnmi_gen ${CMAKE_CURRENT_BINARY_DIR}/gnmi-gen)
    set(_gnmi_protos proto/gnmi/gnmi.proto proto/gnmi_ext/gnmi_ext.proto)
    set(_gnmi_srcs)
    foreach(p ${_gnmi_protos})
        get_filename_component(_dir ${p} DIRECTORY)
        get_filename_component(_name ${p} NAME_WE)
        list(APPEND _gnmi_srcs
            ${_gnmi_gen}/${_dir}/${_name}.pb.cc ${_gnmi_gen}/${_dir}/${_name}.pb.h
            ${_gnmi_gen}/${_dir}/${_name}.grpc.pb.cc ${_gnmi_gen}/${_dir}/${_name}.grpc.pb.h)
    endforeach()
    get_target_property(_grpc_plugin gRPC::grpc_cpp_plugin IMPORTED_LOCATION_NONE)
    if(NOT _grpc_plugin)
        get_target_property(_grpc_plugin gRPC::grpc_cpp_plugin IMPORTED_LOCATION)
    endif()
    if(NOT _grpc_plugin)
        get_target_property(_grpc_plugin gRPC::grpc_cpp_plugin IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT _grpc_plugin)
        set(_grpc_plugin $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
    endif()
    set(_protoc ${Protobuf_PROTOC_EXECUTABLE})
    if(TARGET protobuf::protoc)
        set(_protoc $<TARGET_FILE:protobuf::protoc>)
    endif()
    set(_proto_includes)
    foreach(d ${Protobuf_INCLUDE_DIRS})
        list(APPEND _proto_includes -I ${d})
    endforeach()
    add_custom_command(
        OUTPUT ${_gnmi_srcs}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gnmi_gen}
        COMMAND ${_protoc} -I ${_gnmi_root} ${_proto_includes}
                --cpp_out=${_gnmi_gen} --grpc_out=${_gnmi_gen}
                --plugin=protoc-gen-grpc=${_grpc_plugin}
                ${_gnmi_protos}
        WORKING_DIRECTORY ${_gnmi_root}
        DEPENDS ${_gnmi_root}/proto/gnmi/gnmi.proto ${_gnmi_root}/proto/gnmi_ext/gnmi_ext.proto
        COMMENT "Generating gNMI protobuf/gRPC code"
        VERBATIM)
    add_library(yangscope_gnmi_proto STATIC ${_gnmi_srcs})
    target_include_directories(yangscope_gnmi_proto PUBLIC ${_gnmi_gen})
    target_link_libraries(yangscope_gnmi_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)
    if(MSVC)
        target_compile_options(yangscope_gnmi_proto PRIVATE /W0)
    else()
        target_compile_options(yangscope_gnmi_proto PRIVATE -w)
    endif()

    target_sources(yangscope_core PRIVATE
        src/core/GnmiClient.cpp
        src/core/GnmiClient.h)
    target_link_libraries(yangscope_core PUBLIC yangscope_gnmi_proto)
    target_compile_definitions(yangscope_core PUBLIC YANGSCOPE_HAVE_GNMI=1)
endif()

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
    src/gui/ChangePreview.cpp
    src/gui/ChangePreview.h
)
target_sources(yangscope_gui PRIVATE src/gui/LiveGraph.cpp src/gui/LiveGraph.h)
if(YANGSCOPE_GNMI)
    target_sources(yangscope_gui PRIVATE
        src/gui/GnmiWorker.cpp
        src/gui/GnmiWorker.h
        src/gui/GnmiConnectDialog.cpp
        src/gui/GnmiConnectDialog.h
        src/gui/GnmiPanel.cpp
        src/gui/GnmiPanel.h)
endif()
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

    add_executable(test_gnmi_data tests/test_gnmi_data.cpp)
    target_link_libraries(test_gnmi_data PRIVATE yangscope_core)
    target_compile_definitions(test_gnmi_data PRIVATE
        YANGSCOPE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
        YANGSCOPE_TEST_NETCONF="${CMAKE_CURRENT_SOURCE_DIR}/tests/netconf")
    add_test(NAME gnmi_data COMMAND test_gnmi_data)

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

    if(YANGSCOPE_GNMI)
        add_executable(test_gnmi tests/test_gnmi.cpp)
        target_link_libraries(test_gnmi PRIVATE yangscope_core)
        target_compile_definitions(test_gnmi PRIVATE
            YANGSCOPE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
            YANGSCOPE_TEST_NETCONF="${CMAKE_CURRENT_SOURCE_DIR}/tests/netconf"
            YANGSCOPE_TEST_GNMI="${CMAKE_CURRENT_SOURCE_DIR}/tests/gnmi"
            YANGSCOPE_PYTHON="${Python3_EXECUTABLE}")
        add_test(NAME gnmi COMMAND test_gnmi)
        set_tests_properties(gnmi PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)

        add_executable(test_gui_gnmi tests/test_gui_gnmi.cpp tests/gui_helpers.h)
        target_link_libraries(test_gui_gnmi PRIVATE yangscope_gui)
        target_compile_definitions(test_gui_gnmi PRIVATE
            YANGSCOPE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
            YANGSCOPE_TEST_NETCONF="${CMAKE_CURRENT_SOURCE_DIR}/tests/netconf"
            YANGSCOPE_TEST_GNMI="${CMAKE_CURRENT_SOURCE_DIR}/tests/gnmi"
            YANGSCOPE_PYTHON="${Python3_EXECUTABLE}")
        add_test(NAME gui_gnmi COMMAND test_gui_gnmi)
        set_tests_properties(gui_gnmi PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

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
