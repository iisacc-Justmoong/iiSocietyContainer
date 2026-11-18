if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BUILD_DIR OR NOT DEFINED DATA_DIR)
    message(FATAL_ERROR "SOURCE_DIR, BUILD_DIR and DATA_DIR are required")
endif()
set(_prefix "${BUILD_DIR}/documentation-install-test")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${_prefix}" --component Documentation
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Documentation installation failed: ${_output}\n${_error}")
endif()
set(_pairs
    "FileLoading.md|docs/FileLoading.md"
    "LICENSE|LICENSE"
    "NOTICE|NOTICE"
    "docs/License.md|docs/License.md")
foreach(_pair IN LISTS _pairs)
    string(REPLACE "|" ";" _parts "${_pair}")
    list(GET _parts 0 _relative)
    list(GET _parts 1 _source)
    set(_document "${_prefix}/${DATA_DIR}/iiSocietyContainer/${_relative}")
    if(NOT EXISTS "${_document}")
        message(FATAL_ERROR "The installed ${_relative} is missing")
    endif()
    file(SHA256 "${SOURCE_DIR}/${_source}" _expected)
    file(SHA256 "${_document}" _actual)
    if(NOT _actual STREQUAL _expected)
        message(FATAL_ERROR "The installed ${_relative} differs from the maintained source")
    endif()
endforeach()
message(STATUS "Installed FileLoading guide, LICENSE, NOTICE, and Korean license statement match the maintained files")
