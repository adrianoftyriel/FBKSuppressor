add_executable(fbk_tests TestMain.cpp)

target_link_libraries(fbk_tests PRIVATE fbk_dsp)
target_include_directories(fbk_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(fbk_tests PRIVATE /W4)
else()
    target_compile_options(fbk_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME dsp COMMAND fbk_tests)
