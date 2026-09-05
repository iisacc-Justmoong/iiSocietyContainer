cmake_minimum_required(VERSION 3.24)
project(iiSocietyContainerConsumer LANGUAGES CXX)

find_package(iiSocietyContainer 0.1.0 CONFIG REQUIRED)
add_executable(iiSocietyContainerConsumer ../hello_world.cpp)
target_link_libraries(iiSocietyContainerConsumer PRIVATE iiSocietyContainer::iiSocietyContainer)
set_target_properties(iiSocietyContainerConsumer PROPERTIES CXX_EXTENSIONS OFF)
enable_testing()
add_test(NAME iiSocietyContainer.installed_consumer COMMAND iiSocietyContainerConsumer)
