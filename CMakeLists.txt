cmake_minimum_required(VERSION 3.24)
project(iiSocietyContainer VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(DEFINED ENV{HOME})
        set(_user_home "$ENV{HOME}")
    elseif(DEFINED ENV{USERPROFILE})
        set(_user_home "$ENV{USERPROFILE}")
    else()
        message(FATAL_ERROR "Set CMAKE_INSTALL_PREFIX explicitly: no user home is available")
    endif()
    set(CMAKE_INSTALL_PREFIX "${_user_home}/.local/SDK/iiSocietyContainer" CACHE PATH "Install prefix" FORCE)
endif()

find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

add_library(iiSocietyContainer SHARED iiSocietyContainer.h iiSocietyContainer.cpp)
add_library(iiSocietyContainer::iiSocietyContainer ALIAS iiSocietyContainer)
target_compile_features(iiSocietyContainer PUBLIC cxx_std_20)
target_compile_definitions(iiSocietyContainer PRIVATE IISOCIETYCONTAINER_BUILDING_LIBRARY)
target_link_libraries(iiSocietyContainer PUBLIC Qt6::Core)
target_include_directories(iiSocietyContainer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(iiSocietyContainer PROPERTIES
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

if(BUILD_TESTING)
    add_executable(iiSocietyContainerHelloWorld tests/hello_world.cpp)
    target_link_libraries(iiSocietyContainerHelloWorld PRIVATE iiSocietyContainer::iiSocietyContainer)
    set_target_properties(iiSocietyContainerHelloWorld PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiSocietyContainer.hello_world COMMAND iiSocietyContainerHelloWorld)
endif()

install(TARGETS iiSocietyContainer EXPORT iiSocietyContainerTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES iiSocietyContainer.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer)
install(EXPORT iiSocietyContainerTargets
    FILE iiSocietyContainerTargets.cmake
    NAMESPACE iiSocietyContainer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer
)
configure_package_config_file(cmake/iiSocietyContainerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer
)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer
)
