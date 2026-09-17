include_guard(GLOBAL)

# ActivityKit owns presentation independently of the host's execution grants.
function(iiSocietyContainer_add_ios_live_activity target)
    cmake_parse_arguments(PARSE_ARGV 1 activity "" "TEAM;BRIDGE_TARGET" "")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR NOT CMAKE_GENERATOR STREQUAL "Xcode"
        OR NOT CMAKE_Swift_COMPILER_LOADED)
        message(FATAL_ERROR "Live Activities require an iOS Xcode build with Swift enabled")
    endif()
    get_target_property(identifier ${target} MACOSX_BUNDLE_GUI_IDENTIFIER)
    get_target_property(plist ${target} MACOSX_BUNDLE_INFO_PLIST)
    if(NOT identifier OR NOT plist)
        message(FATAL_ERROR "Configure the containing app's identifier and Info.plist first")
    endif()
    set(native "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/ios-live-activity/${target}")
    file(MAKE_DIRECTORY "${generated}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${plist}")
    file(READ "${plist}" info)
    string(REGEX REPLACE "</dict>[ \t\r\n]*</plist>"
        "<key>NSSupportsLiveActivities</key><true/>\n</dict></plist>" info "${info}")
    file(WRITE "${generated}/App.Info.plist.in" "${info}")
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${generated}/App.Info.plist.in"
        XCODE_ATTRIBUTE_ALWAYS_EMBED_SWIFT_STANDARD_LIBRARIES YES)

    set(common "${native}/TaskActivityState.swift" "${native}/TaskActivityAttributes.swift")
    set(bridge "${target}ActivityBridge")
    add_library(${bridge} STATIC ${common} "${native}/TaskActivityBridge.swift")
    target_compile_options(${bridge} PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-parse-as-library>")
    target_link_libraries(${bridge} PRIVATE "-framework ActivityKit" "-framework Foundation" "-framework UIKit")
    set_target_properties(${bridge} PROPERTIES Swift_LANGUAGE_VERSION 5 AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    if(NOT activity_BRIDGE_TARGET)
        set(activity_BRIDGE_TARGET "${target}")
    endif()
    target_include_directories(${activity_BRIDGE_TARGET} PRIVATE "${native}")
    target_link_libraries(${activity_BRIDGE_TARGET} PRIVATE ${bridge})

    set(widget "${target}LiveActivity")
    set(activity_identifier "${identifier}.liveactivity")
    configure_file("${native}/Widget.Info.plist.in" "${generated}/Widget.Info.plist" @ONLY)
    add_executable(${widget} MACOSX_BUNDLE ${common} "${native}/TaskActivityWidget.swift")
    target_compile_options(${widget} PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-parse-as-library;-application-extension>")
    target_link_libraries(${widget} PRIVATE "-framework ActivityKit" "-framework WidgetKit" "-framework SwiftUI")
    set_target_properties(${widget} PROPERTIES
        Swift_LANGUAGE_VERSION 5 AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
        BUNDLE_EXTENSION appex MACOSX_BUNDLE_GUI_IDENTIFIER "${activity_identifier}"
        MACOSX_BUNDLE_INFO_PLIST "${generated}/Widget.Info.plist"
        XCODE_PRODUCT_TYPE "com.apple.product-type.app-extension"
        XCODE_EXPLICIT_FILE_TYPE "wrapper.app-extension"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${activity_identifier}"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 16.2
        XCODE_ATTRIBUTE_APPLICATION_EXTENSION_API_ONLY YES
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2" XCODE_ATTRIBUTE_SKIP_INSTALL YES
        XCODE_ATTRIBUTE_ALWAYS_EMBED_SWIFT_STANDARD_LIBRARIES NO)
    if(activity_TEAM)
        set_target_properties(${widget} PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${activity_TEAM}")
    endif()
    set_property(TARGET ${target} APPEND PROPERTY XCODE_EMBED_APP_EXTENSIONS ${widget})
    set_target_properties(${target} PROPERTIES XCODE_EMBED_APP_EXTENSIONS_CODE_SIGN_ON_COPY YES)
    add_dependencies(${target} ${widget})
endfunction()
