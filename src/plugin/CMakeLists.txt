# JUCE is fetched rather than vendored so the repository stays small and the
# version is a single line to bump. Pinned to a release tag: an unpinned
# dependency on a plugin framework is how a build that worked last month stops
# working today.
include(FetchContent)

set(FBK_JUCE_TAG "8.0.15" CACHE STRING "JUCE release tag to build against")

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ${FBK_JUCE_TAG}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(JUCE)

# Formats: VST3 everywhere, AU on macOS only, plus a standalone build. The
# standalone is not an afterthought - it talks to ASIO/CoreAudio directly and so
# gives the lowest round trip available, with no host buffering in the way.
set(FBK_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND FBK_FORMATS AU)
endif()

juce_add_plugin(FBKSuppressor
    PRODUCT_NAME                "FBKSuppressor"
    COMPANY_NAME                "FBKSuppressor"
    BUNDLE_ID                   "org.canops.fbksuppressor"
    PLUGIN_MANUFACTURER_CODE    Fbks
    PLUGIN_CODE                 Fbk1
    FORMATS                     ${FBK_FORMATS}
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             "Fx" "Restoration"
    AU_MAIN_TYPE                "kAudioUnitType_Effect"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT  "FBKSuppressor needs microphone access to process live audio input."
)

juce_generate_juce_header(FBKSuppressor)

target_sources(FBKSuppressor PRIVATE
    PluginProcessor.cpp
    PluginEditor.cpp
)

target_compile_definitions(FBKSuppressor PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=0

    # Low-latency driver backends for the standalone build.
    #
    # These are off by default in JUCE, and without them the standalone silently
    # offers only the high-latency system paths - which makes it useless for the one
    # job it is best at. The standalone exists precisely because it talks to the
    # driver directly and so gives the lowest achievable round trip; a plugin build
    # inherits whatever the host is using and does not care.
    #
    # JUCE_ASIO needs no external SDK from 8.0 onwards: the required headers
    # (iasiodrv.h, asio.h, asiosys.h) are bundled in
    # juce_audio_devices/native/asio. Enabling it does mean the resulting binary is
    # subject to the Steinberg ASIO SDK licence terms.
    #
    # WASAPI is already on by default and offers exclusive and low-latency shared
    # modes at runtime, so it needs no flag - it appears as a separate device type in
    # the standalone's audio settings and is the fallback when no ASIO driver exists.
    $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>

    # JACK is the Linux equivalent, and the same argument applies. JUCE loads
    # libjack at runtime, so a machine without it simply does not list the device
    # type rather than failing to start.
    $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>
)

target_link_libraries(FBKSuppressor PRIVATE
    fbk_dsp
    juce::juce_audio_utils
    juce::juce_dsp
PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
