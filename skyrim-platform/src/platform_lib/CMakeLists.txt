if(MSVC)
    include(${CMAKE_SOURCE_DIR}/cmake/apply_default_settings.cmake)

    file(GLOB_RECURSE sources "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
    add_library(platform_lib STATIC ${sources})
    target_link_libraries(platform_lib PUBLIC viet)
    target_include_directories(platform_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(platform_lib PUBLIC "${SKYRIM_PLATFORM_SOURCE_DIR}/src/third_party")

    find_package(unofficial-node-addon-api REQUIRED)
    target_link_libraries(platform_lib PUBLIC unofficial::node-addon-api::node-addon-api)

    apply_default_settings(TARGETS platform_lib)

    # Exercise the mixed language standards of platform_lib and CommonLib's
    # consumers. Keep the standalone regression out of normal client builds.
    add_library(platform_thread_pool_smoke_20 OBJECT EXCLUDE_FROM_ALL
        ${CMAKE_SOURCE_DIR}/misc/boyhouse/thread_pool_smoke/cpp20.cpp)
    target_link_libraries(platform_thread_pool_smoke_20 PRIVATE platform_lib)
    set_target_properties(platform_thread_pool_smoke_20 PROPERTIES CXX_STANDARD 20)
    add_executable(platform_thread_pool_smoke EXCLUDE_FROM_ALL
        ${CMAKE_SOURCE_DIR}/misc/boyhouse/thread_pool_smoke/main.cpp
        $<TARGET_OBJECTS:platform_thread_pool_smoke_20>)
    target_link_libraries(platform_thread_pool_smoke PRIVATE platform_lib)
    set_target_properties(platform_thread_pool_smoke PROPERTIES CXX_STANDARD 23)
    apply_default_settings(TARGETS platform_thread_pool_smoke_20 platform_thread_pool_smoke)
    if(BUILD_UNIT_TESTS)
        set_target_properties(platform_thread_pool_smoke PROPERTIES EXCLUDE_FROM_ALL FALSE)
        add_test(NAME platform_thread_pool_smoke COMMAND platform_thread_pool_smoke)
    endif()
endif()
