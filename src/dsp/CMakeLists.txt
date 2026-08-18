# Core DSP library. Deliberately free of any dependency on JUCE or any plugin
# SDK so that it compiles and unit-tests on any platform, including CI runners
# with no audio stack at all.
add_library(fbk_dsp STATIC
    Fft.cpp
    ErbBands.cpp
    Analyser.cpp
    NoiseTracker.cpp
    HowlDetector.cpp
    TonalCanceller.cpp
    MaskFilter.cpp
    FeedbackSuppressor.cpp
)

target_include_directories(fbk_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(fbk_dsp PUBLIC cxx_std_20)

set_target_properties(fbk_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(fbk_dsp PRIVATE /W4 /fp:fast)
else()
    target_compile_options(fbk_dsp PRIVATE
        -Wall -Wextra -Wpedantic
        -ffast-math -fno-finite-math-only)
endif()

# Flush denormals to zero on x86. Denormals in the recursive state of the
# oscillators and biquads are a classic source of sudden CPU spikes mid-show.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86|AMD64|amd64)")
    if(NOT MSVC)
        target_compile_options(fbk_dsp PRIVATE -msse2 -mfpmath=sse)
    endif()
endif()
