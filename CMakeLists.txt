project(score_addon_videoio LANGUAGES CXX)

score_common_setup()

if(NOT TARGET score_plugin_gfx)
  message(STATUS "score-addon-videoio: requires score_plugin_gfx, skipping")
  return()
endif()

find_package(${QT_VERSION} REQUIRED COMPONENTS Core Gui Widgets)

# ---------------------------------------------------------------------------
# Vendor SDK detection. Each vendor is independently optional and gated by a
# SCORE_HAS_<VENDOR> compile definition; the unified Direct Video I/O device
# hardcodes the compiled-in set (no registry).
# ---------------------------------------------------------------------------
set(VIDEOIO_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_videoio.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoIOSettings.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoOutput.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoInput.cpp"
)
set(VIDEOIO_HDRS
  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_videoio.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoIOSettings.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoIOCaps.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoOutput.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoInput.hpp"
  # NOTE: the shared, vendor-neutral NVIDIA-DVP shim templates now live in
  # score-plugin-gfx (Gfx/Graph/interop/Dvp{Capture,Output}{Gl,D3D11}.hpp +
  # DmaLockPolicy.hpp); AJA/DeckLink consume them from there.
)
set(VIDEOIO_GEN_SRCS "")
set(VIDEOIO_DEFS "")
set(VIDEOIO_INCS "")

# --- AJA -------------------------------------------------------------------
# Vendored libajantv2 SDK (git submodule, built as a static lib). Merged in
# from the now-deprecated score-addon-aja; the device/GPU code is relocated
# verbatim. Independently optional: gated on the submodule being checked out.
set(VIDEOIO_AJA_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/libajantv2"
    CACHE PATH "AJA SDK root directory")
if(EXISTS "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes/ntv2card.h")
  # Build the vendored AJA SDK as a static lib named `ajantv2`. Guarded so it
  # coexists with a still-present (deprecated) score-addon-aja in the same tree.
  if(NOT TARGET ajantv2)
    set(AJANTV2_BUILD_SHARED        OFF CACHE BOOL "" FORCE)
    set(AJANTV2_DISABLE_DEMOS       ON  CACHE BOOL "" FORCE)
    set(AJANTV2_DISABLE_TOOLS       ON  CACHE BOOL "" FORCE)
    set(AJANTV2_DISABLE_PLUGIN_LOAD ON  CACHE BOOL "" FORCE)
    set(AJANTV2_DISABLE_TESTS       ON  CACHE BOOL "" FORCE)
    add_subdirectory("${VIDEOIO_AJA_SDK_ROOT}"
                     "${CMAKE_CURRENT_BINARY_DIR}/libajantv2"
                     EXCLUDE_FROM_ALL)
    # The SDK declares its per-config options (-O0 -Wall in RelWithDebInfo /
    # Debug) PUBLIC, so they propagate to every consumer and clash with the
    # consumers' own flags (clang rejects a PCH built at a different -O level).
    # Keep them for the SDK's own objects only.
    set_target_properties(ajantv2 PROPERTIES INTERFACE_COMPILE_OPTIONS "")
  endif()

  list(APPEND VIDEOIO_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJACaptureSession.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaCaptureExternalInput.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAInput.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAInputNode.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAOutput.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAOutputNode.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaFormatMap.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaOutputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaDmaLock.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaDmaLockPolicy.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaDvpEncoder.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaCpuCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaRdmaCaptureD3D11.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaRdmaCaptureGL.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaRdmaCaptureVulkan.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaRdmaOutputD3D11.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaRdmaOutputD3D12.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaRdmaOutputGL.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaRdmaOutputVulkan.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaWireEncoder.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJACaptureSession.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAInput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAInputNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAOutput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAOutputNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaOutputBackend.cpp")
  list(APPEND VIDEOIO_INCS
    "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes"
    "${VIDEOIO_AJA_SDK_ROOT}")
  list(APPEND VIDEOIO_DEFS SCORE_HAS_AJA=1 SCORE_HAS_AJA_CUDA_BRIDGE=1)
  set(VIDEOIO_HAS_AJA TRUE)
  message(STATUS "score-addon-videoio: AJA enabled (SDK: ${VIDEOIO_AJA_SDK_ROOT})")
else()
  message(STATUS "score-addon-videoio: AJA disabled (submodule 3rdparty/libajantv2 not checked out)")
endif()

# --- V4L2 (Linux, kernel API only: no SDK, no library to find) --------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND VIDEOIO_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2Session.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2CpuCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2InputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2CaptureNode.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2GbmAllocator.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2NvBufAllocator.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2Session.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2InputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2CaptureNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2GbmAllocator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/v4l2/V4L2NvBufAllocator.cpp")
  list(APPEND VIDEOIO_DEFS SCORE_HAS_V4L2=1)
  set(VIDEOIO_HAS_V4L2 TRUE)
  message(STATUS "score-addon-videoio: V4L2 enabled")
endif()

# --- Blackmagic DeckLink -----------------------------------------------------
# Windows + clang64: the SDK ships only .idl (MS MIDL input); we generate the
# COM C++ header + IID source with MinGW's widl at build time.
set(DECKLINK_SDK_ROOT "c:/dev/blackmagic-sdk" CACHE PATH "Blackmagic DeckLink SDK root")
find_program(WIDL_EXECUTABLE widl)
if(WIN32 AND WIDL_EXECUTABLE AND EXISTS "${DECKLINK_SDK_ROOT}/Win/include/DeckLinkAPI.idl")
  set(DL_IDL_DIR "${DECKLINK_SDK_ROOT}/Win/include")
  set(DL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/decklink_gen")
  file(GLOB DL_IDLS "${DL_IDL_DIR}/*.idl")
  add_custom_command(
    OUTPUT "${DL_GEN_DIR}/DeckLinkAPI_h.h" "${DL_GEN_DIR}/DeckLinkAPI_i.c"
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_decklink_headers.sh"
            "${DL_IDL_DIR}" "${DL_GEN_DIR}" "${WIDL_EXECUTABLE}"
    DEPENDS ${DL_IDLS}
    COMMENT "score-addon-videoio: generating DeckLink COM headers via widl"
    VERBATIM)

  list(APPEND VIDEOIO_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLink.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkComPtr.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkFormats.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkDevices.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkOutputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkCpuCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkInputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkModeMap.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkNode.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkCaptureNode.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkDevices.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkOutputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkInputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkCaptureNode.cpp")
  list(APPEND VIDEOIO_GEN_SRCS "${DL_GEN_DIR}/DeckLinkAPI_i.c")
  list(APPEND VIDEOIO_INCS "${DL_GEN_DIR}")
  list(APPEND VIDEOIO_DEFS SCORE_HAS_DECKLINK=1)
  set(VIDEOIO_HAS_DECKLINK TRUE)
  message(STATUS "score-addon-videoio: DeckLink enabled (widl: ${WIDL_EXECUTABLE})")
else()
  # Linux: the SDK ships ready C++ headers (DeckLinkAPI.h) and a dispatch TU
  # that dlopens Desktop Video's libDeckLinkAPI.so at runtime — no codegen, no
  # link-time dependency. Arch's decklink-sdk installs both to /usr/include.
  find_path(DECKLINK_LINUX_INCLUDE_DIR DeckLinkAPI.h
    PATHS "${DECKLINK_SDK_ROOT}/Linux/include" /usr/include /usr/local/include)
  if(UNIX AND NOT APPLE
     AND DECKLINK_LINUX_INCLUDE_DIR
     AND EXISTS "${DECKLINK_LINUX_INCLUDE_DIR}/DeckLinkAPIDispatch.cpp")
    list(APPEND VIDEOIO_HDRS
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLink.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkComPtr.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkFormats.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkDevices.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkOutputBackend.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkCpuCapture.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkInputBackend.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkModeMap.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkNode.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkCaptureNode.hpp")
    list(APPEND VIDEOIO_SRCS
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkDevices.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkOutputBackend.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkInputBackend.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkNode.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkCaptureNode.cpp")
    # The dispatch stub is SDK C++ (dlopen + symbol table); keep it out of the
    # PCH like the generated COM TU on Windows.
    list(APPEND VIDEOIO_GEN_SRCS "${DECKLINK_LINUX_INCLUDE_DIR}/DeckLinkAPIDispatch.cpp")
    list(APPEND VIDEOIO_INCS "${DECKLINK_LINUX_INCLUDE_DIR}")
    list(APPEND VIDEOIO_DEFS SCORE_HAS_DECKLINK=1)
    set(VIDEOIO_HAS_DECKLINK TRUE)
    message(STATUS "score-addon-videoio: DeckLink enabled (Linux SDK: ${DECKLINK_LINUX_INCLUDE_DIR})")
  else()
    message(STATUS "score-addon-videoio: DeckLink disabled (need WIN32+widl+SDK or Linux decklink-sdk headers)")
  endif()
endif()

# --- DELTACAST VideoMaster ---------------------------------------------------
# Plain C headers + import lib (no codegen). Host-staged v1; RDMA (Seam B) later.
set(DELTACAST_SDK_ROOT "c:/dev/deltacast/videomaster" CACHE PATH "DELTACAST VideoMaster SDK root")
if(WIN32 AND EXISTS "${DELTACAST_SDK_ROOT}/resources/include/VideoMasterHD_Core.h"
   AND EXISTS "${DELTACAST_SDK_ROOT}/resources/lib/VideoMasterHD.lib")
  list(APPEND VIDEOIO_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/Deltacast.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastFormats.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastDevices.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastCpuCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastRdmaCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastRdmaOutput.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastOutputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastInputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastNode.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastCaptureNode.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastDevices.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastOutputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastInputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deltacast/DeltacastCaptureNode.cpp")
  list(APPEND VIDEOIO_INCS "${DELTACAST_SDK_ROOT}/resources/include")
  list(APPEND VIDEOIO_DEFS SCORE_HAS_DELTACAST=1)
  set(VIDEOIO_DELTACAST_LIB "${DELTACAST_SDK_ROOT}/resources/lib/VideoMasterHD.lib")
  set(VIDEOIO_DELTACAST_DLL "${DELTACAST_SDK_ROOT}/resources/lib/VideoMasterHD.dll")
  set(VIDEOIO_HAS_DELTACAST TRUE)
  message(STATUS "score-addon-videoio: DELTACAST enabled (SDK: ${DELTACAST_SDK_ROOT})")
else()
  message(STATUS "score-addon-videoio: DELTACAST disabled (need WIN32 + VideoMaster SDK)")
endif()

# --- Bluefish444 BlueVelvetC -------------------------------------------------
# Plain C headers + import lib (no codegen). Host-staged v1; BlueGpuDirect later.
set(BLUEFISH_SDK_ROOT "c:/dev/bluefish444" CACHE PATH "Bluefish444 SDK root")
if(WIN32 AND EXISTS "${BLUEFISH_SDK_ROOT}/inc/BlueVelvetC.h"
   AND EXISTS "${BLUEFISH_SDK_ROOT}/lib/win/BlueVelvetC64.lib")
  list(APPEND VIDEOIO_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/Bluefish.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishFormats.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishDevices.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishCpuCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishOutputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishInputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishNode.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishCaptureNode.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishDevices.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishOutputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishInputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishCaptureNode.cpp")
  list(APPEND VIDEOIO_INCS "${BLUEFISH_SDK_ROOT}/inc")
  list(APPEND VIDEOIO_DEFS SCORE_HAS_BLUEFISH=1)
  set(VIDEOIO_BLUEFISH_LIB "${BLUEFISH_SDK_ROOT}/lib/win/BlueVelvetC64.lib")
  set(VIDEOIO_BLUEFISH_DLL "${BLUEFISH_SDK_ROOT}/driver/Release/BlueVelvetC64.dll")
  set(VIDEOIO_HAS_BLUEFISH TRUE)
  message(STATUS "score-addon-videoio: Bluefish444 enabled (SDK: ${BLUEFISH_SDK_ROOT})")
else()
  message(STATUS "score-addon-videoio: Bluefish444 disabled (need WIN32 + BlueVelvetC SDK)")
endif()

# --- Magewell Pro Capture (LibMWCapture) -------------------------------------
# Plain C API + import lib (no codegen). Capture-only: Magewell PCIe cards have
# no playout, so there is only an input backend, host-staged (no GPU-direct).
set(MAGEWELL_SDK_ROOT "c:/dev/magewell/SDKv3" CACHE PATH "Magewell MWCapture SDK root")
if(WIN32 AND EXISTS "${MAGEWELL_SDK_ROOT}/Include/LibMWCapture/MWCapture.h"
   AND EXISTS "${MAGEWELL_SDK_ROOT}/Lib/x64/LibMWCapture.lib")
  list(APPEND VIDEOIO_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/Magewell.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellFormats.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellDevices.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellCpuCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellInputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellCaptureNode.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellDevices.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellInputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellCaptureNode.cpp")
  list(APPEND VIDEOIO_INCS
    "${MAGEWELL_SDK_ROOT}/Include"
    "${MAGEWELL_SDK_ROOT}/Include/LibMWCapture")
  list(APPEND VIDEOIO_DEFS SCORE_HAS_MAGEWELL=1)
  set(VIDEOIO_MAGEWELL_LIB "${MAGEWELL_SDK_ROOT}/Lib/x64/LibMWCapture.lib")
  # LibMWCapture.lib is an import lib; the runtime DLL ships under the Python
  # example bindings (no DLL under Lib/x64). Copy it post-build if present.
  set(VIDEOIO_MAGEWELL_DLL
    "${MAGEWELL_SDK_ROOT}/Examples/Python/AVCapturePY/mwcapture/bin/x64/LibMWCapture.dll")
  set(VIDEOIO_HAS_MAGEWELL TRUE)
  message(STATUS "score-addon-videoio: Magewell enabled (SDK: ${MAGEWELL_SDK_ROOT})")
elseif(UNIX AND NOT APPLE
       AND EXISTS "${MAGEWELL_SDK_ROOT}/Include/LibMWCapture/MWCapture.h"
       AND EXISTS "${MAGEWELL_SDK_ROOT}/Lib/x64/libMWCapture.a")
  # Linux MWCapture SDK: identical C API + static lib; events go through the
  # SDK's own MWCreateEvent/MWWaitEvent (see magewell/Magewell.hpp).
  list(APPEND VIDEOIO_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/Magewell.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellFormats.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellDevices.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellCpuCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellInputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellCaptureNode.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellDevices.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellInputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellCaptureNode.cpp")
  list(APPEND VIDEOIO_INCS
    "${MAGEWELL_SDK_ROOT}/Include"
    "${MAGEWELL_SDK_ROOT}/Include/LibMWCapture")
  list(APPEND VIDEOIO_DEFS SCORE_HAS_MAGEWELL=1)
  set(VIDEOIO_MAGEWELL_LIB "${MAGEWELL_SDK_ROOT}/Lib/x64/libMWCapture.a")
  set(VIDEOIO_HAS_MAGEWELL TRUE)
  message(STATUS "score-addon-videoio: Magewell enabled (Linux SDK: ${MAGEWELL_SDK_ROOT})")
else()
  message(STATUS "score-addon-videoio: Magewell disabled (need LibMWCapture SDK)")
endif()

# ---------------------------------------------------------------------------
add_library(${PROJECT_NAME}
  ${VIDEOIO_SRCS} ${VIDEOIO_HDRS} ${VIDEOIO_GEN_SRCS})

# The generated DeckLinkAPI_i.c is C with COM GUIDs; compile as-is (no PCH).
if(VIDEOIO_GEN_SRCS)
  set_source_files_properties(${VIDEOIO_GEN_SRCS} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

target_include_directories(${PROJECT_NAME}
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" ${VIDEOIO_INCS})

target_link_libraries(${PROJECT_NAME}
  PUBLIC
    ${QT_PREFIX}::Core ${QT_PREFIX}::Gui ${QT_PREFIX}::Widgets
    score_lib_base score_lib_device
    score_plugin_deviceexplorer score_lib_process
    score_lib_state score_lib_inspector
    score_plugin_gfx
    score_plugin_media)

if(VIDEOIO_DEFS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE ${VIDEOIO_DEFS})
endif()

# --- NVIDIA DVP bridge (vendor-neutral) -------------------------------------
# DVP is not AJA-specific: DeckLink's GPU download rung uses it too. Linking it
# inside the AJA block meant a DeckLink-only build silently lost DVP, because
# the source-side gate was never defined. SCORE_HAS_AJA_DVP_BRIDGE is kept for
# the AJA sources that already spell it that way.
if(TARGET score_nv_dvp_bridge)
  target_link_libraries(${PROJECT_NAME} PRIVATE score_nv_dvp_bridge)
  target_compile_definitions(${PROJECT_NAME} PRIVATE
    SCORE_HAS_VIDEOIO_DVP=1
    SCORE_HAS_AJA_DVP_BRIDGE=1)
  message(STATUS "score-addon-videoio: linking score_nv_dvp_bridge")

  if(WIN32 AND SCORE_NV_DVP_DLL AND EXISTS "${SCORE_NV_DVP_DLL}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SCORE_NV_DVP_DLL}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/dvp.dll"
      COMMENT "Copying dvp.dll alongside ${PROJECT_NAME}")
  endif()
endif()

# --- AJA link ----------------------------------------------------------------
if(VIDEOIO_HAS_AJA)
  target_link_libraries(${PROJECT_NAME} PRIVATE ajantv2)
endif()

# --- DELTACAST link ----------------------------------------------------------
if(VIDEOIO_HAS_DELTACAST)
  target_link_libraries(${PROJECT_NAME} PRIVATE "${VIDEOIO_DELTACAST_LIB}")
  if(WIN32 AND EXISTS "${VIDEOIO_DELTACAST_DLL}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VIDEOIO_DELTACAST_DLL}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/VideoMasterHD.dll"
      COMMENT "Copying VideoMasterHD.dll alongside ${PROJECT_NAME}")
  endif()
endif()

# --- Bluefish444 link --------------------------------------------------------
if(VIDEOIO_HAS_BLUEFISH)
  target_link_libraries(${PROJECT_NAME} PRIVATE "${VIDEOIO_BLUEFISH_LIB}")

  # BlueVelvetC.h uses MSVC __declspec(dllimport, deprecated(...)) which clang
  # only accepts with -fdeclspec; that flag is incompatible with score's shared
  # PCH (built without it). Confine both to the bluefish/*.cpp TUs (the only ones
  # that pull in the SDK) — the neutral wiring stays PCH-clean and SDK-free.
  set(VIDEOIO_BLUEFISH_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishDevices.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishOutputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishInputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bluefish/BluefishCaptureNode.cpp")
  set_source_files_properties(${VIDEOIO_BLUEFISH_SRCS} PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON)
  if(NOT MSVC)
    set_source_files_properties(${VIDEOIO_BLUEFISH_SRCS} PROPERTIES
      COMPILE_OPTIONS -fdeclspec)
  endif()
  if(WIN32 AND EXISTS "${VIDEOIO_BLUEFISH_DLL}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VIDEOIO_BLUEFISH_DLL}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/BlueVelvetC64.dll"
      COMMENT "Copying BlueVelvetC64.dll alongside ${PROJECT_NAME}")
  endif()
endif()

# --- Magewell link -----------------------------------------------------------
if(VIDEOIO_HAS_MAGEWELL)
  target_link_libraries(${PROJECT_NAME} PRIVATE "${VIDEOIO_MAGEWELL_LIB}")
  if(UNIX AND NOT APPLE)
    # Per the Linux SDK examples: MWCapture drives /dev udev enumeration and
    # the capture cards' ALSA audio endpoints from the static lib.
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
      Threads::Threads ${CMAKE_DL_LIBS} udev asound)
  endif()

  # MWCapture.h pulls in <Windows.h>/<ks.h>/<ksmedia.h>; it compiles cleanly
  # under clang64 without -fdeclspec (LIBMWCAPTURE_API stays empty since we do
  # not define LIBMWCAPTURE_DLL, so no __declspec(dllimport)). Keep the SDK TUs
  # out of score's shared PCH to avoid Windows-header/PCH ordering surprises,
  # confined to magewell/*.cpp — the neutral wiring stays PCH-clean and SDK-free.
  set(VIDEOIO_MAGEWELL_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellDevices.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellInputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/magewell/MagewellCaptureNode.cpp")
  set_source_files_properties(${VIDEOIO_MAGEWELL_SRCS} PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON)

  if(WIN32 AND EXISTS "${VIDEOIO_MAGEWELL_DLL}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VIDEOIO_MAGEWELL_DLL}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/LibMWCapture.dll"
      COMMENT "Copying LibMWCapture.dll alongside ${PROJECT_NAME}")
  endif()
endif()

setup_score_plugin(${PROJECT_NAME})

# --- AJA card-to-card round-trip test harness (opt-in) ----------------------
# Relocated from score-addon-aja; off by default since it needs the full plugin
# set + real hardware. Drives the real output path and CPU-staging capture.
option(SCORE_VIDEOIO_BUILD_TESTS "Build the Direct Video I/O (AJA) test harness" OFF)
if(SCORE_VIDEOIO_BUILD_TESTS AND VIDEOIO_HAS_AJA)
  # Personality inspector/switcher: pure ajantv2, no score/Qt dependency, so it
  # stays usable when the graph stack is mid-rebuild.
  add_executable(AJAPersonality tests/AJAPersonality.cpp)
  target_include_directories(AJAPersonality PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes"
    "${VIDEOIO_AJA_SDK_ROOT}")
  target_link_libraries(AJAPersonality PRIVATE ajantv2)

  add_executable(AJARoundtrip tests/AJARoundtrip.cpp)
  target_include_directories(AJARoundtrip PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes"
    "${VIDEOIO_AJA_SDK_ROOT}")
  target_link_libraries(AJARoundtrip PRIVATE
    ${PROJECT_NAME} ajantv2
    ${QT_PREFIX}::Core ${QT_PREFIX}::Gui ${QT_PREFIX}::Widgets)

  if(SCORE_STATIC_PLUGINS)
    target_link_libraries(AJARoundtrip PRIVATE ${SCORE_PLUGINS_LIST})
  endif()

  target_compile_definitions(AJARoundtrip PRIVATE SCORE_HAS_AJA=1)
  target_compile_definitions(AJARoundtrip PRIVATE SCORE_HAS_AJA_CUDA_BRIDGE=1)

  # SDK-only (no score/Qt) reproducer for the ARGB->SDI black-output issue.
  add_executable(aja_rgb_repro tests/aja_rgb_repro.cpp)
  target_include_directories(aja_rgb_repro PRIVATE
    "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes" "${VIDEOIO_AJA_SDK_ROOT}")
  target_link_libraries(aja_rgb_repro PRIVATE ajantv2)

  # SDK-only HDMI monitor-output validation probe. The Kona 5 has no HDMI
  # input on any personality, so this validates the TX side per mode
  # (framestore -> HDMI out routing + live NTV2HDMIOutputStatus readback);
  # --force-hpd brings the TX up without an EDID sink.
  add_executable(aja_hdmi_out_probe tests/AJAHdmiOutProbe.cpp)
  target_include_directories(aja_hdmi_out_probe PRIVATE
    "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes" "${VIDEOIO_AJA_SDK_ROOT}")
  target_link_libraries(aja_hdmi_out_probe PRIVATE ajantv2)
endif()

# --- DeckLink single-card loopback round-trip harness (opt-in) ---------------
if(SCORE_VIDEOIO_BUILD_TESTS AND VIDEOIO_HAS_DECKLINK)
  add_executable(DecklinkRoundtrip tests/DecklinkRoundtrip.cpp)
  target_include_directories(DecklinkRoundtrip PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${VIDEOIO_INCS})
  target_link_libraries(DecklinkRoundtrip PRIVATE
    ${PROJECT_NAME}
    ${QT_PREFIX}::Core ${QT_PREFIX}::Gui ${QT_PREFIX}::Widgets)
  if(SCORE_STATIC_PLUGINS)
    target_link_libraries(DecklinkRoundtrip PRIVATE ${SCORE_PLUGINS_LIST})
  endif()
endif()

# --- Magewell live-capture validation (opt-in) --------------------------------
if(SCORE_VIDEOIO_BUILD_TESTS AND VIDEOIO_HAS_MAGEWELL)
  add_executable(MagewellCaptureTest tests/MagewellCaptureTest.cpp)
  target_include_directories(MagewellCaptureTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${VIDEOIO_INCS})
  target_link_libraries(MagewellCaptureTest PRIVATE
    ${PROJECT_NAME}
    ${QT_PREFIX}::Core ${QT_PREFIX}::Gui ${QT_PREFIX}::Widgets)
  if(SCORE_STATIC_PLUGINS)
    target_link_libraries(MagewellCaptureTest PRIVATE ${SCORE_PLUGINS_LIST})
  endif()
endif()

# --- V4L2 capture validation (Linux, no GPU or Qt dependency) -----------------
if(SCORE_VIDEOIO_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(V4L2CaptureTest
    tests/V4L2CaptureTest.cpp
    tests/V4L2DonorAllocator.cpp
    v4l2/V4L2Session.cpp
    v4l2/V4L2GbmAllocator.cpp
    v4l2/V4L2NvBufAllocator.cpp)
  target_include_directories(V4L2CaptureTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${VIDEOIO_INCS})
  target_link_libraries(V4L2CaptureTest PRIVATE
    score_plugin_gfx
    ${QT_PREFIX}::Core ${QT_PREFIX}::Gui)
endif()
