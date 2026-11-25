cmake_minimum_required(VERSION 3.21)
project(WorldForge VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(WORLDFORGE_BUILD_EDITOR "Build the Qt desktop editor" ON)
option(WORLDFORGE_BUILD_EDITOR_AUDIT "Build the isolated editor UI audit" OFF)

include(FetchContent)
set(OVERRIDE_CXX_FLAGS OFF)
set(INTERPROCEDURAL_OPTIMIZATION OFF)
set(ENABLE_ALL_WARNINGS OFF)
set(ENABLE_INSTALL OFF)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "Use the same runtime library as WorldForge")
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
set(ENABLE_OBJECT_STREAM OFF)
set(USE_SSE4_1 OFF)
set(USE_SSE4_2 OFF)
set(USE_AVX OFF)
set(USE_AVX2 OFF)
set(USE_LZCNT OFF)
set(USE_TZCNT OFF)
set(USE_F16C OFF)
set(USE_FMADD OFF)
FetchContent_Declare(Jolt
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG 0373ec0dd762e4bc2f6acdb08371ee84fa23c6db
    SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(Jolt)
install(FILES "${jolt_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME Jolt-LICENSE.txt)

set(manifold_directory_option)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(manifold_directory_option EXCLUDE_FROM_ALL)
endif()
FetchContent_Declare(manifold
    GIT_REPOSITORY https://github.com/elalish/manifold.git
    GIT_TAG 798d83c8d7fabcddd23c1617097b95ba40f2597c
    ${manifold_directory_option})
function(worldforge_add_manifold)
    set(BUILD_SHARED_LIBS OFF)
    set(MANIFOLD_CROSS_SECTION OFF)
    set(MANIFOLD_DOWNLOADS OFF)
    set(MANIFOLD_TEST OFF)
    set(MANIFOLD_STRICT OFF)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        FetchContent_MakeAvailable(manifold)
    else()
        FetchContent_Populate(manifold)
        add_subdirectory("${manifold_SOURCE_DIR}" "${manifold_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
    install(FILES "${manifold_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME Manifold-LICENSE.txt)
endfunction()
worldforge_add_manifold()

function(worldforge_compile_options target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(WorldForgeCore STATIC
    Engine/Core/Guid.cpp Engine/Core/Guid.h
    Engine/Scene/SceneDocument.cpp Engine/Scene/SceneDocument.h
    Engine/Geometry/Mesh.cpp Engine/Geometry/Mesh.h
    Engine/Geometry/MeshBoolean.cpp Engine/Geometry/MeshBoolean.h
    Engine/Geometry/MeshRemesh.cpp Engine/Geometry/MeshRemesh.h
    Engine/Geometry/SculptBrush.cpp Engine/Geometry/SculptBrush.h
    Engine/Geometry/MeshTopology.cpp Engine/Geometry/MeshTopology.h
    Engine/Geometry/MeshModifiers.cpp Engine/Geometry/MeshModifiers.h
    Engine/Geometry/MeshUv.cpp Engine/Geometry/MeshUv.h
    Engine/World/WorldSimulation.cpp Engine/World/WorldSimulation.h
    Engine/World/PlayableScene.cpp Engine/World/PlayableScene.h
    Engine/World/Gameplay.cpp Engine/World/Gameplay.h
    Engine/World/NativeGameplay.cpp Engine/World/NativeGameplay.h Engine/World/WorldForgeNative.h
    Engine/Render/ColorPipeline.h Engine/Render/Flip.cpp Engine/Render/Flip.h Engine/Render/AgxCurve.inc Engine/Render/AgxMatrices.inc
    Engine/Render/ProbeGeometry.cpp Engine/Render/ProbeGeometry.h
)
target_include_directories(WorldForgeCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(WorldForgeCore PRIVATE Jolt manifold)
worldforge_compile_options(WorldForgeCore)

# Vulkan is optional at build time. Without the headers and glslang, the Player renders with
# Direct3D 11 only and the editor has no live viewport.
find_path(Vulkan_INCLUDE_DIR NAMES vulkan/vulkan.h
    HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{LOCALAPPDATA}/WorldForge/Vulkan/Vulkan-Headers/include")
find_program(WORLDFORGE_GLSLANG NAMES glslang glslangValidator
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{LOCALAPPDATA}/WorldForge/Vulkan/glslang-16.6.0/bin")
if(Vulkan_INCLUDE_DIR AND WORLDFORGE_GLSLANG)
    add_library(WorldForgeVulkan STATIC
        Engine/Render/Vulkan/VulkanApi.cpp Engine/Render/Vulkan/VulkanApi.h
        Engine/Render/Vulkan/HdrFrame.cpp Engine/Render/Vulkan/HdrFrame.h)
    target_include_directories(WorldForgeVulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(WorldForgeVulkan SYSTEM PUBLIC "${Vulkan_INCLUDE_DIR}")
    worldforge_compile_options(WorldForgeVulkan)
    set(worldforge_shader_includes Engine/Render/Shaders/Lighting.glsl Engine/Render/Shaders/ColorPipeline.glsl
        Engine/Render/Shaders/PlayView.glsl Engine/Render/Shaders/SceneMotion.glsl Engine/Render/Shaders/Shading.glsl Engine/Render/AgxCurve.inc Engine/Render/AgxMatrices.inc)
    # Compiles GLSL to SPIR-V. With a variable name, the output is a C++ header that holds the words.
    function(worldforge_spirv source output variable)
        set(arguments -V "-I${CMAKE_CURRENT_SOURCE_DIR}" ${ARGN})
        if(variable)
            list(APPEND arguments --vn ${variable})
        endif()
        get_filename_component(directory "${output}" DIRECTORY)
        add_custom_command(OUTPUT "${output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}"
            COMMAND "${WORLDFORGE_GLSLANG}" ${arguments} "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${output}"
            DEPENDS "${source}" ${worldforge_shader_includes} VERBATIM)
    endfunction()
    set(probe_shader "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shaders/ProbeTrace_comp.h")
    worldforge_spirv(Engine/Render/Shaders/ProbeTrace.comp "${probe_shader}" spirv_ProbeTrace_comp)
    set(cull_shader "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shaders/CullDraws_comp.h")
    worldforge_spirv(Engine/Render/Shaders/CullDraws.comp "${cull_shader}" spirv_CullDraws_comp)
    set(reduce_depth_shader "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shaders/ReduceDepth_comp.h")
    worldforge_spirv(Engine/Render/Shaders/ReduceDepth.comp "${reduce_depth_shader}" spirv_ReduceDepth_comp)
    set(fog_volume_shader "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shaders/FogVolume_comp.h")
    worldforge_spirv(Engine/Render/Shaders/FogVolume.comp "${fog_volume_shader}" spirv_FogVolume_comp)
    target_sources(WorldForgeVulkan PRIVATE
        Engine/Render/Vulkan/ProbeTrace.cpp Engine/Render/Vulkan/ProbeTrace.h "${probe_shader}"
        Engine/Render/Vulkan/DrawCulling.cpp Engine/Render/Vulkan/DrawCulling.h "${cull_shader}"
        Engine/Render/Vulkan/OcclusionDepth.cpp Engine/Render/Vulkan/OcclusionDepth.h "${reduce_depth_shader}"
        Engine/Render/Vulkan/FogVolume.cpp Engine/Render/Vulkan/FogVolume.h "${fog_volume_shader}")
    target_include_directories(WorldForgeVulkan PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shaders")
    target_link_libraries(WorldForgeVulkan PRIVATE ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    add_executable(WorldForgePlayer WIN32 Player/main.cpp Player/PlayerRender.h Player/ReferenceScene.cpp Player/ReferenceScene.h)
    target_link_libraries(WorldForgePlayer PRIVATE WorldForgeCore d3d11 dxgi d3dcompiler shell32 comdlg32
        windowscodecs shlwapi ole32 psapi gdi32)
    target_compile_definitions(WorldForgePlayer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    worldforge_compile_options(WorldForgePlayer)
    if(TARGET WorldForgeVulkan)
        foreach(shader Sky.vert Sky.frag Shadow.vert Display.vert Display.frag Temporal.frag Ambient.frag BloomDown.frag BloomUp.frag DepthOfField.frag MotionBlur.frag
                       PlayScene.vert PlayScene.frag)
            string(REPLACE "." "_" variable "${shader}")
            set(output "${CMAKE_CURRENT_BINARY_DIR}/player-shaders/${variable}.h")
            worldforge_spirv("Engine/Render/Shaders/${shader}" "${output}" "spirv_${variable}")
            list(APPEND player_shaders "${output}")
        endforeach()
        set(meshlet_shader "${CMAKE_CURRENT_BINARY_DIR}/player-shaders/PlayMeshlets_mesh.h")
        worldforge_spirv(Engine/Render/Shaders/PlayMeshlets.mesh "${meshlet_shader}" spirv_PlayMeshlets_mesh --target-env vulkan1.2)
        list(APPEND player_shaders "${meshlet_shader}")
        target_sources(WorldForgePlayer PRIVATE Player/VulkanRenderer.cpp Player/VulkanRenderer.h ${player_shaders})
        target_include_directories(WorldForgePlayer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/player-shaders")
        target_link_libraries(WorldForgePlayer PRIVATE WorldForgeVulkan)
        target_compile_definitions(WorldForgePlayer PRIVATE WORLDFORGE_PLAYER_VULKAN)
    endif()
    install(TARGETS WorldForgePlayer RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
    add_executable(WorldForgeCoreTests Tests/SceneDocumentTests.cpp)
    target_link_libraries(WorldForgeCoreTests PRIVATE WorldForgeCore)
    worldforge_compile_options(WorldForgeCoreTests)
    add_test(NAME WorldForge.Core COMMAND WorldForgeCoreTests)
    if(TARGET WorldForgeVulkan)
        add_executable(WorldForgeProbeTests Tests/ProbeTraceTests.cpp)
        target_link_libraries(WorldForgeProbeTests PRIVATE WorldForgeCore WorldForgeVulkan)
        worldforge_compile_options(WorldForgeProbeTests)
        add_test(NAME WorldForge.ProbeTrace COMMAND WorldForgeProbeTests)
        set_tests_properties(WorldForge.ProbeTrace PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    endif()
    if(TARGET WorldForgePlayer)
        set(standalone_fixture "${CMAKE_CURRENT_BINARY_DIR}/standalone-smoke.wfplay")
        add_test(NAME WorldForge.StandaloneFixture COMMAND WorldForgeCoreTests "${standalone_fixture}")
        set_tests_properties(WorldForge.StandaloneFixture PROPERTIES FIXTURES_SETUP StandaloneFixture)
        add_test(NAME WorldForge.PlayerWithoutEditor COMMAND WorldForgePlayer --smoke-test-warp "${standalone_fixture}")
        set_tests_properties(WorldForge.PlayerWithoutEditor PROPERTIES FIXTURES_REQUIRED StandaloneFixture TIMEOUT 45)
    endif()
endif()

if(NOT WORLDFORGE_BUILD_EDITOR)
    return()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg Network)
qt_standard_project_setup()

set(editor_sources
    Components/AssetHub.h Components/AssetHub.cpp
    Components/ProjectSurfaceDetails.h Components/ProjectSurfaceDetails.cpp
    Components/GameplayAuthoring.h Components/GameplayAuthoring.cpp
    Components/BehaviorEditor.h Components/BehaviorEditor.cpp
    Components/BehaviorCode.h Components/BehaviorCode.cpp
    Components/NativeGameplayEditor.h Components/NativeGameplayEditor.cpp
    Components/LocalMultiplayer.h Components/LocalMultiplayer.cpp
    Components/WorldLightingVulkan.cpp Components/WorldLightingVulkan.h
    Components/ViewportFrameVulkan.cpp Components/ViewportFrameVulkan.h
    App/EditorLog.cpp App/EditorLog.h
    Components/CommandTerminal.cpp Components/CommandTerminal.h
    Components/ProjectLauncher.cpp Components/ProjectLauncher.h
    Components/WindowCaption.cpp Components/WindowCaption.h
    App/MainWindow.h App/MainWindow.cpp
    App/SystemMetrics.h App/SystemMetrics.cpp
    Components/EditorPanels.h Components/EditorPanels.cpp
    Components/Viewport.h Components/Viewport.cpp
    Components/LiveViewport.h Components/LiveViewport.cpp
    DesignSystem/Icons.h DesignSystem/Icons.cpp
    State/ProjectStore.h State/ProjectStore.cpp
    State/GlbImport.h State/GlbImport.cpp
    State/StudioPublishReview.h State/StudioPublishReview.cpp
    State/StudioClip.h State/StudioClip.cpp
    State/ForgeAi.h State/ForgeAi.cpp
    State/PlayableStreet.h State/PlayableStreet.cpp State/CityDistrict.h State/CityDistrict.cpp
    Workspaces/ForgeAiJob.h Workspaces/ForgeAiJob.cpp
    Components/ForgeAiPanel.h Components/ForgeAiPanel.cpp
    State/WorldScene.h State/WorldScene.cpp
    State/WorldRuntime.h State/WorldRuntime.cpp
    State/NativeComponents.h State/NativeComponents.cpp
    Workspaces/WorkspaceCatalog.h Workspaces/WorkspaceCatalog.cpp
    Workspaces/StudioWorkspace.h Workspaces/StudioWorkspace.cpp
    Workspaces/StudioViews.h Workspaces/StudioViews.cpp
    Workspaces/EmbeddedStudio.h Workspaces/EmbeddedStudio.cpp
)
set(concept_art
    Docs/file_000000000a4481f5b152fe135f0875fb.png
    Docs/file_000000001b8c8211a2910938bbbedb44.png
    Docs/file_000000002aac82099c4b6a6130892725.png
    Docs/file_00000000389081f9930a2924e08d6fe0.png
    Docs/file_000000003e18820d85b6a524ad596f27.png
    Docs/file_00000000418c8208a492dcb2b14aaa90.png
    Docs/file_00000000437082119cebcfae73fcd61e.png
    Docs/file_000000004db482119a8a3c703124c125.png
    Docs/file_00000000569c82128e96b4d620ab1fa7.png
    Docs/file_0000000064b4820b8061f40d48c49509.png
    Docs/file_00000000694c820ead12cda391661601.png
    Docs/file_000000007e9c821183f934c945a7f4da.png
    Docs/file_000000009a5c8211a4f785ae1dbbb4d3.png
    Docs/file_00000000cae881f583b6c2525193d90e.png
    Docs/file_00000000ffdc81faa622de2e167904bf.png
)

if(TARGET WorldForgeVulkan)
    foreach(shader Components/Shaders/Scene.vert Components/Shaders/Scene.frag
                   Components/Shaders/SelectionOutline.vert Components/Shaders/SelectionOutline.frag
                   Engine/Render/Shaders/Sky.vert Engine/Render/Shaders/Sky.frag Engine/Render/Shaders/Shadow.vert
                   Engine/Render/Shaders/Display.vert Engine/Render/Shaders/Display.frag Engine/Render/Shaders/Temporal.frag
                   Engine/Render/Shaders/Ambient.frag Engine/Render/Shaders/BloomDown.frag Engine/Render/Shaders/BloomUp.frag
                   Engine/Render/Shaders/DepthOfField.frag Engine/Render/Shaders/MotionBlur.frag)
        get_filename_component(name "${shader}" NAME)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv")
        worldforge_spirv("${shader}" "${output}" "")
        list(APPEND scene_shaders "${output}")
    endforeach()
    add_custom_target(WorldForgeSceneShaders DEPENDS ${scene_shaders})
else()
    message(STATUS "Live Vulkan viewport disabled. Install the Vulkan headers and glslang.")
endif()

function(configure_editor target)
    qt_add_resources(${target} "${target}_behavior_code" PREFIX "/"
        FILES Resources/BehaviorCode/Behavior.cpp Resources/BehaviorCode/WorldForgeBehavior.h
              Resources/BehaviorCode/WorldForgeGameplay.h Resources/BehaviorCode/Behavior.vcxproj
              Resources/BehaviorCode/NativeGameplay.cpp Resources/BehaviorCode/NativeGameplay.vcxproj
              Engine/World/WorldForgeNative.h Engine/World/WorldForgeNativeComponents.h)
    qt_add_resources(${target} "${target}_studio_publish" PREFIX "/"
        FILES Workspaces/StudioPublish.py Workspaces/StudioNavigation.py Workspaces/ForgeJob.py)
    worldforge_compile_options(${target})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE WorldForgeCore Qt6::Widgets Qt6::Svg Qt6::Network)
    if(WIN32)
        target_link_libraries(${target} PRIVATE pdh dxgi)
    endif()

    qt_add_resources(${target} "${target}_theme"
        PREFIX "/"
        FILES DesignSystem/WorldForgeTheme.qss DesignSystem/chevron.svg DesignSystem/check.svg Data/demo.json Data/studios.json
    )
    qt_add_resources(${target} "${target}_concept_art"
        BIG_RESOURCES PREFIX "/" OPTIONS --no-compress FILES ${concept_art}
    )
    qt_add_resources(${target} "${target}_launcher_art"
        BIG_RESOURCES PREFIX "/" OPTIONS --no-compress
        FILES Data/Launcher/open-world-city.jpg Data/Launcher/fantasy-rpg.jpg Data/Launcher/action-adventure.jpg
              Data/Launcher/racing-experience.jpg Data/Launcher/shooter-arena.jpg
              Data/Launcher/survival-sandbox.jpg Data/Launcher/simulation-city.jpg Data/Launcher/skyline-banner.png
    )
    if(TARGET WorldForgeSceneShaders)
        add_dependencies(${target} WorldForgeSceneShaders)
        target_link_libraries(${target} PRIVATE WorldForgeVulkan)
        target_compile_definitions(${target} PRIVATE WORLDFORGE_VULKAN)
        target_include_directories(${target} SYSTEM PRIVATE "${Vulkan_INCLUDE_DIR}")
        qt_add_resources(${target} "${target}_scene_shaders" PREFIX "/"
            BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${scene_shaders})
    endif()
endfunction()

qt_add_executable(WorldForgeEditor WIN32 MACOSX_BUNDLE App/main.cpp ${editor_sources})
configure_editor(WorldForgeEditor)

if(BUILD_TESTING AND WORLDFORGE_BUILD_EDITOR_AUDIT)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(WorldForgeEditorAudit Tests/EditorAudit.cpp ${editor_sources}
        Workspaces/ModelingWorkspace.h Workspaces/ModelingWorkspace.cpp
        Workspaces/SculptingWorkspace.h Workspaces/SculptingWorkspace.cpp
        Workspaces/SurfaceWorkspace.h Workspaces/SurfaceWorkspace.cpp)
    configure_editor(WorldForgeEditorAudit)
    target_link_libraries(WorldForgeEditorAudit PRIVATE Qt6::Test)
    add_test(NAME WorldForge.EditorAudit
        COMMAND WorldForgeEditorAudit "${CMAKE_CURRENT_BINARY_DIR}/qa" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(WorldForge.EditorAudit PROPERTIES TIMEOUT 360)
    if(TARGET WorldForgeSceneShaders)
        add_test(NAME WorldForge.Authoring
            COMMAND WorldForgeEditorAudit "${CMAKE_CURRENT_BINARY_DIR}/authoring-qa" "${CMAKE_CURRENT_SOURCE_DIR}" --authoring)
        set_tests_properties(WorldForge.Authoring PROPERTIES TIMEOUT 180)
        add_test(NAME WorldForge.Rendering
            COMMAND WorldForgeEditorAudit "${CMAKE_CURRENT_BINARY_DIR}/rendering-qa" "${CMAKE_CURRENT_SOURCE_DIR}" --rendering)
        # About 140 s on the GTX 1650 with the night, cloud-transition and fog-volume checks (2026-11-25).
        set_tests_properties(WorldForge.Rendering PROPERTIES TIMEOUT 300)
        if(TARGET WorldForgePlayer)
            add_test(NAME WorldForge.EngineSession COMMAND WorldForgeEditorAudit
                "${CMAKE_CURRENT_BINARY_DIR}/engine-session-qa" "${CMAKE_CURRENT_SOURCE_DIR}" --engine-session)
            set_tests_properties(WorldForge.EngineSession PROPERTIES TIMEOUT 120)
            add_test(NAME WorldForge.SurfaceLibrary COMMAND WorldForgeEditorAudit
                "${CMAKE_CURRENT_BINARY_DIR}/surface-library-qa" "${CMAKE_CURRENT_SOURCE_DIR}" --surface-library)
            set_tests_properties(WorldForge.SurfaceLibrary PROPERTIES TIMEOUT 120)
            set_tests_properties(WorldForge.Authoring PROPERTIES FIXTURES_SETUP PlayableScene)
            add_test(NAME WorldForge.Player COMMAND WorldForgePlayer --smoke-test
                "${CMAKE_CURRENT_BINARY_DIR}/authoring-qa/outdoor.wfplay")
            set_tests_properties(WorldForge.Player PROPERTIES FIXTURES_REQUIRED PlayableScene TIMEOUT 30)
        endif()
    endif()
endif()

install(TARGETS WorldForgeEditor RUNTIME DESTINATION bin BUNDLE DESTINATION .)
if(WIN32)
    set(WORLDFORGE_STUDIO_RUNTIME "" CACHE PATH "Installed WorldForge Studio runtime to bundle with the editor")
    if(WORLDFORGE_STUDIO_RUNTIME)
        if(NOT EXISTS "${WORLDFORGE_STUDIO_RUNTIME}/WorldForgeStudio.exe")
            message(FATAL_ERROR "WORLDFORGE_STUDIO_RUNTIME must contain WorldForgeStudio.exe and its runtime files.")
        endif()
        install(DIRECTORY "${WORLDFORGE_STUDIO_RUNTIME}/" DESTINATION bin/Studio COMPONENT Studio)
    endif()
    qt_generate_deploy_app_script(
        TARGET WorldForgeEditor
        OUTPUT_SCRIPT deploy_script
        NO_TRANSLATIONS
    )
    install(SCRIPT "${deploy_script}")
endif()
