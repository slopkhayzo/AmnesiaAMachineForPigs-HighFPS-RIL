cmake_minimum_required(VERSION 3.24)

project(
    AmnesiaAmfpFpsFix
    VERSION 1.0.0
    DESCRIPTION "High-frame-rate presentation fix for Amnesia: A Machine for Pigs"
    LANGUAGES C CXX
)

include(CTest)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)
set_property(TARGET minhook PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "AmnesiaAMFP.FPSFix currently supports Windows/MSVC only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure an x86 build environment; aamfp.exe is a 32-bit process")
endif()

function(amfp_apply_common_settings target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    target_compile_options(${target} PRIVATE
        /W4
        /WX
        /permissive-
        /utf-8
        /Zc:preprocessor
    )
    set_property(TARGET ${target} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endfunction()

add_library(AmnesiaAmfpFpsFix MODULE
    src/bootstrap.cpp
    src/camera_audit.cpp
    src/camera_interpolation.cpp
    src/camera_runtime.cpp
    src/config.cpp
    src/dllmain.cpp
    src/enemy_pose_audit.cpp
    src/enemy_pose_runtime.cpp
    src/frame_cadence.cpp
    src/hands_audit.cpp
    src/hands_interpolation.cpp
    src/hands_runtime.cpp
    src/host_validation.cpp
    src/image_trail_timing.cpp
    src/logger.cpp
    src/mouse_look.cpp
    src/mouse_preview_math.cpp
    src/node_animation_runtime.cpp
    src/particle_runtime.cpp
    src/presentation_clock.cpp
    src/pose_audit.cpp
    src/pose_interpolation.cpp
    src/pose_runtime.cpp
    src/ragdoll_pose_audit.cpp
    src/ragdoll_pose_runtime.cpp
    src/ragdoll_skinning_runtime.cpp
    src/render_gate_patch.cpp
    src/sha256.cpp
    src/signature_scan.cpp
    src/target_mapping.cpp
    src/telemetry.cpp
    src/world_ownership_audit.cpp
    src/world_runtime.cpp
)

amfp_apply_common_settings(AmnesiaAmfpFpsFix)
target_link_libraries(AmnesiaAmfpFpsFix PRIVATE bcrypt minhook)
target_link_options(AmnesiaAmfpFpsFix PRIVATE
    /DYNAMICBASE
    /NXCOMPAT
    /GUARD:CF
    /INCREMENTAL:NO
)
set_target_properties(AmnesiaAmfpFpsFix PROPERTIES
    OUTPUT_NAME "AmnesiaAMFP.FPSFix"
    PREFIX ""
    SUFFIX ".asi"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET AmnesiaAmfpFpsFix POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/AmnesiaAMFP.FPSFix.ini"
            "$<TARGET_FILE_DIR:AmnesiaAmfpFpsFix>/AmnesiaAMFP.FPSFix.ini"
    VERBATIM
)

install(TARGETS AmnesiaAmfpFpsFix LIBRARY DESTINATION . RUNTIME DESTINATION .)
install(FILES config/AmnesiaAMFP.FPSFix.ini DESTINATION .)

set(AMFP_PACKAGE_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/package.cmake")
set(AMFP_PACKAGE_NAME "AmnesiaAMFP.FPSFix-${PROJECT_VERSION}")
add_custom_target(package_plugin_only
    COMMAND "${CMAKE_COMMAND}"
            -DMODE=plugin-only
            -DPLUGIN_FILE=$<TARGET_FILE:AmnesiaAmfpFpsFix>
            -DINI_FILE=${CMAKE_SOURCE_DIR}/config/AmnesiaAMFP.FPSFix.ini
            -DREADME_FILE=${CMAKE_SOURCE_DIR}/packaging/README.txt
            -DMINHOOK_LICENSE_FILE=${CMAKE_SOURCE_DIR}/third_party/minhook/LICENSE.txt
            -DOUTPUT_DIR=${CMAKE_SOURCE_DIR}/dist/release/${AMFP_PACKAGE_NAME}-plugin-only
            -DARCHIVE_FILE=${CMAKE_SOURCE_DIR}/dist/${AMFP_PACKAGE_NAME}-plugin-only.zip
            -P "${AMFP_PACKAGE_SCRIPT}"
    DEPENDS AmnesiaAmfpFpsFix
    VERBATIM
)

# The loader binary is kept with the ignored research material; the bundle
# target exists only where it is available.
set(AMFP_UAL_X86 "${CMAKE_SOURCE_DIR}/research/tools/ultimate-asi-loader/dinput8.dll")
if(EXISTS "${AMFP_UAL_X86}")
    add_custom_target(package_ultimate_asi_loader
        COMMAND "${CMAKE_COMMAND}"
                -DMODE=ultimate-asi-loader
                -DPLUGIN_FILE=$<TARGET_FILE:AmnesiaAmfpFpsFix>
                -DINI_FILE=${CMAKE_SOURCE_DIR}/config/AmnesiaAMFP.FPSFix.ini
                -DREADME_FILE=${CMAKE_SOURCE_DIR}/packaging/README.txt
                -DMINHOOK_LICENSE_FILE=${CMAKE_SOURCE_DIR}/third_party/minhook/LICENSE.txt
                -DLOADER_FILE=${AMFP_UAL_X86}
                -DNOTICE_FILE=${CMAKE_SOURCE_DIR}/packaging/THIRD-PARTY-NOTICES.txt
                -DOUTPUT_DIR=${CMAKE_SOURCE_DIR}/dist/release/${AMFP_PACKAGE_NAME}-with-Ultimate-ASI-Loader
                -DARCHIVE_FILE=${CMAKE_SOURCE_DIR}/dist/${AMFP_PACKAGE_NAME}-with-Ultimate-ASI-Loader.zip
                -P "${AMFP_PACKAGE_SCRIPT}"
        DEPENDS AmnesiaAmfpFpsFix
        VERBATIM
    )
endif()

if(BUILD_TESTING)
    add_executable(frame_cadence_tests
        tests/frame_cadence_tests.cpp
        src/frame_cadence.cpp
    )
    target_include_directories(frame_cadence_tests PRIVATE src)
    amfp_apply_common_settings(frame_cadence_tests)
    add_test(NAME frame_cadence COMMAND frame_cadence_tests)

    add_executable(presentation_clock_tests
        tests/presentation_clock_tests.cpp
        src/presentation_clock.cpp
    )
    target_include_directories(presentation_clock_tests PRIVATE src)
    amfp_apply_common_settings(presentation_clock_tests)
    add_test(NAME presentation_clock COMMAND presentation_clock_tests)

    add_executable(host_validation_tests
        tests/host_validation_tests.cpp
        src/host_validation.cpp
    )
    target_include_directories(host_validation_tests PRIVATE src)
    amfp_apply_common_settings(host_validation_tests)
    add_test(NAME host_validation COMMAND host_validation_tests)

    add_executable(signature_scan_tests
        tests/signature_scan_tests.cpp
        src/signature_scan.cpp
    )
    target_include_directories(signature_scan_tests PRIVATE src)
    amfp_apply_common_settings(signature_scan_tests)
    add_test(NAME signature_scan COMMAND signature_scan_tests)

    add_executable(render_gate_patch_tests
        tests/render_gate_patch_tests.cpp
        src/render_gate_patch.cpp
    )
    target_include_directories(render_gate_patch_tests PRIVATE src)
    amfp_apply_common_settings(render_gate_patch_tests)
    add_test(NAME render_gate_patch COMMAND render_gate_patch_tests)

    add_executable(camera_audit_tests
        tests/camera_audit_tests.cpp
        src/camera_audit.cpp
    )
    target_include_directories(camera_audit_tests PRIVATE src)
    amfp_apply_common_settings(camera_audit_tests)
    add_test(NAME camera_audit COMMAND camera_audit_tests)

    add_executable(camera_interpolation_tests
        tests/camera_interpolation_tests.cpp
        src/camera_interpolation.cpp
    )
    target_include_directories(camera_interpolation_tests PRIVATE src)
    amfp_apply_common_settings(camera_interpolation_tests)
    add_test(NAME camera_interpolation COMMAND camera_interpolation_tests)

    add_executable(hands_audit_tests
        tests/hands_audit_tests.cpp
        src/hands_audit.cpp
    )
    target_include_directories(hands_audit_tests PRIVATE src)
    amfp_apply_common_settings(hands_audit_tests)
    add_test(NAME hands_audit COMMAND hands_audit_tests)

    add_executable(hands_interpolation_tests
        tests/hands_interpolation_tests.cpp
        src/hands_interpolation.cpp
    )
    target_include_directories(hands_interpolation_tests PRIVATE src)
    amfp_apply_common_settings(hands_interpolation_tests)
    add_test(NAME hands_interpolation COMMAND hands_interpolation_tests)

    add_executable(hands_runtime_tests
        tests/hands_runtime_tests.cpp
        src/hands_interpolation.cpp
        src/hands_runtime.cpp
    )
    target_include_directories(hands_runtime_tests PRIVATE src)
    amfp_apply_common_settings(hands_runtime_tests)
    add_test(NAME hands_runtime COMMAND hands_runtime_tests)

    add_executable(pose_audit_tests
        tests/pose_audit_tests.cpp
        src/pose_audit.cpp
    )
    target_include_directories(pose_audit_tests PRIVATE src)
    amfp_apply_common_settings(pose_audit_tests)
    add_test(NAME pose_audit COMMAND pose_audit_tests)

    add_executable(pose_runtime_tests
        tests/pose_runtime_tests.cpp
        src/hands_interpolation.cpp
        src/pose_interpolation.cpp
        src/pose_runtime.cpp
    )
    target_include_directories(pose_runtime_tests PRIVATE src)
    amfp_apply_common_settings(pose_runtime_tests)
    add_test(NAME pose_runtime COMMAND pose_runtime_tests)

    add_executable(enemy_pose_audit_tests
        tests/enemy_pose_audit_tests.cpp
        src/enemy_pose_audit.cpp
        src/pose_audit.cpp
    )
    target_include_directories(enemy_pose_audit_tests PRIVATE src)
    amfp_apply_common_settings(enemy_pose_audit_tests)
    add_test(NAME enemy_pose_audit COMMAND enemy_pose_audit_tests)

    add_executable(enemy_pose_runtime_tests
        tests/enemy_pose_runtime_tests.cpp
        src/enemy_pose_runtime.cpp
        src/hands_interpolation.cpp
        src/pose_interpolation.cpp
    )
    target_include_directories(enemy_pose_runtime_tests PRIVATE src)
    amfp_apply_common_settings(enemy_pose_runtime_tests)
    add_test(NAME enemy_pose_runtime COMMAND enemy_pose_runtime_tests)

    add_executable(ragdoll_pose_audit_tests
        tests/ragdoll_pose_audit_tests.cpp
        src/pose_audit.cpp
        src/ragdoll_pose_audit.cpp
    )
    target_include_directories(ragdoll_pose_audit_tests PRIVATE src)
    amfp_apply_common_settings(ragdoll_pose_audit_tests)
    add_test(NAME ragdoll_pose_audit COMMAND ragdoll_pose_audit_tests)

    add_executable(ragdoll_pose_runtime_tests
        tests/ragdoll_pose_runtime_tests.cpp
        src/hands_interpolation.cpp
        src/pose_interpolation.cpp
        src/ragdoll_pose_runtime.cpp
    )
    target_include_directories(ragdoll_pose_runtime_tests PRIVATE src)
    amfp_apply_common_settings(ragdoll_pose_runtime_tests)
    add_test(NAME ragdoll_pose_runtime COMMAND ragdoll_pose_runtime_tests)

    add_executable(ragdoll_skinning_runtime_tests
        tests/ragdoll_skinning_runtime_tests.cpp
        src/ragdoll_skinning_runtime.cpp
    )
    target_include_directories(ragdoll_skinning_runtime_tests PRIVATE src)
    amfp_apply_common_settings(ragdoll_skinning_runtime_tests)
    add_test(NAME ragdoll_skinning_runtime COMMAND ragdoll_skinning_runtime_tests)

    add_executable(image_trail_timing_tests
        tests/image_trail_timing_tests.cpp
        src/image_trail_timing.cpp
    )
    target_include_directories(image_trail_timing_tests PRIVATE src)
    amfp_apply_common_settings(image_trail_timing_tests)
    add_test(NAME image_trail_timing COMMAND image_trail_timing_tests)

    add_executable(mouse_preview_math_tests
        tests/mouse_preview_math_tests.cpp
        src/mouse_preview_math.cpp
    )
    target_include_directories(mouse_preview_math_tests PRIVATE src)
    amfp_apply_common_settings(mouse_preview_math_tests)
    add_test(NAME mouse_preview_math COMMAND mouse_preview_math_tests)

    add_executable(node_animation_runtime_tests
        tests/node_animation_runtime_tests.cpp
        src/hands_interpolation.cpp
        src/node_animation_runtime.cpp
    )
    target_include_directories(node_animation_runtime_tests PRIVATE src)
    amfp_apply_common_settings(node_animation_runtime_tests)
    add_test(NAME node_animation_runtime COMMAND node_animation_runtime_tests)

    add_executable(particle_runtime_tests
        tests/particle_runtime_tests.cpp
        src/particle_runtime.cpp
    )
    target_include_directories(particle_runtime_tests PRIVATE src)
    amfp_apply_common_settings(particle_runtime_tests)
    add_test(NAME particle_runtime COMMAND particle_runtime_tests)

    add_executable(world_audit_tests
        tests/world_audit_tests.cpp
        src/world_audit.cpp
    )
    target_include_directories(world_audit_tests PRIVATE src)
    amfp_apply_common_settings(world_audit_tests)
    add_test(NAME world_audit COMMAND world_audit_tests)

    add_executable(world_ownership_audit_tests
        tests/world_ownership_audit_tests.cpp
        src/world_ownership_audit.cpp
    )
    target_include_directories(world_ownership_audit_tests PRIVATE src)
    amfp_apply_common_settings(world_ownership_audit_tests)
    add_test(NAME world_ownership_audit COMMAND world_ownership_audit_tests)

    add_executable(world_runtime_tests
        tests/world_runtime_tests.cpp
        src/hands_interpolation.cpp
        src/world_runtime.cpp
    )
    target_include_directories(world_runtime_tests PRIVATE src)
    amfp_apply_common_settings(world_runtime_tests)
    add_test(NAME world_runtime COMMAND world_runtime_tests)

    add_executable(camera_runtime_tests
        tests/camera_runtime_tests.cpp
        src/camera_interpolation.cpp
        src/camera_runtime.cpp
    )
    target_include_directories(camera_runtime_tests PRIVATE src)
    amfp_apply_common_settings(camera_runtime_tests)
    add_test(NAME camera_runtime COMMAND camera_runtime_tests)

    add_executable(asi_bootstrap_host tests/asi_bootstrap_host.cpp)
    amfp_apply_common_settings(asi_bootstrap_host)
    set_target_properties(asi_bootstrap_host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(
        NAME asi_bootstrap_rejects_incompatible_host
        COMMAND asi_bootstrap_host "$<TARGET_FILE:AmnesiaAmfpFpsFix>"
    )

    add_executable(target_mapping_probe
        tests/target_mapping_probe.cpp
        src/signature_scan.cpp
        src/target_mapping.cpp
    )
    target_include_directories(target_mapping_probe PRIVATE src)
    amfp_apply_common_settings(target_mapping_probe)
endif()
