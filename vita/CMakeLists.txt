cmake_minimum_required(VERSION 3.21)
project(pokebw2_vita VERSION 0.1.0 LANGUAGES C)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        include("$ENV{VITASDK}/share/vita.toolchain.cmake")
    else()
        message(FATAL_ERROR "Please define VITASDK")
    endif()
endif()
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/decomp/compile_all.py --generate-headers-only
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
    RESULT_VARIABLE VITA_HEADER_GENERATION_RESULT)
if(NOT VITA_HEADER_GENERATION_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not generate headers required by the Vita build")
endif()




file(GLOB NNS_G3D_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nnsys/src/g3d/*.c)
file(GLOB NNS_G2D_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nnsys/src/g2d/*.c)
file(GLOB NNS_GFD_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nnsys/src/gfd/*.c)
add_library(bw2_g3d STATIC ${NNS_G3D_SOURCES} ${NNS_G2D_SOURCES} ${NNS_GFD_SOURCES})

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/native-include)
foreach(header gfl system app constants field save pml battle demo struct_decls.h)
    file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/../include/${header}
        ${CMAKE_CURRENT_BINARY_DIR}/native-include/${header} SYMBOLIC COPY_ON_ERROR)
endforeach()

# vita/include MUST precede pc/include so 32-bit types.h is selected.
target_include_directories(bw2_g3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(bw2_g3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../pc)
target_include_directories(bw2_g3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../pc/include)
target_include_directories(bw2_g3d SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/native-include)
target_include_directories(bw2_g3d SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../build/include/generated)

target_sources(bw2_g3d PRIVATE
    ../lib/nitro/src/fx/fx.c ../lib/nitro/src/fx/fx_cp.c
    ../lib/nitro/src/fx/fx_mtx22.c
    ../lib/nitro/src/fx/fx_mtx33.c ../lib/nitro/src/fx/fx_mtx43.c
    ../lib/nitro/src/fx/fx_mtx44.c ../lib/nitro/src/fx/fx_vec.c ../lib/nitro/src/fx/fx_sincos.c
    ../lib/nitro/src/gx/g3imm.c ../lib/nitro/src/gx/gx_bgcnt.c ../lib/nitro/src/gx/g2.c ../lib/nitro/src/gx/g3x.c
    ../lib/nitro/src/gx/gx.c
    ../src/gfl/arc_tool.c ../src/gfl/bg_sys.c ../src/gfl/areaman.c ../src/gfl/arc_util.c ../src/gfl/net.c
    ../src/gfl/calctool.c
    ../src/system/gf_font.c ../src/gfl/bmp.c ../src/gfl/bmp_win.c ../src/gfl/strbuf.c ../src/system/printsys.c
    ../src/gfl/msgdata.c
    ../src/gfl/g3d_system.c ../src/gfl/g3d_util.c ../src/gfl/g3d_camera.c ../src/gfl/g3d_light.c
    ../src/gfl/procsys.c ../src/gfl/fade.c ../src/gfl/gx_layers.c ../src/ov162/boot_screens.c
    ../src/gfl/tcb.c ../src/gfl/tcbl.c
    ../src/system/wipe.c ../src/system/wipe_sub.c ../src/system/wipe_wnd.c
    ../src/ov264/cdemo_main.c ../src/ov264/cdemo_script.c ../src/ov264/cdemo_control.c
    ../src/ov162/title.c ../src/gfl/clact.c ../src/gfl/clact_itcm.c ../src/system/ica_anime.c
    ../src/ov162/startmenu.c ../src/ov162/game_start.c ../src/ov162/delete_save.c ../src/ov253/mictest.c
    ../src/system/version.c
    ../src/gfl/vman.c ../src/gfl/net_handle.c
    ../src/system/snd_strm.c ../src/system/pm_voice.c ../src/system/perapvoice.c
    ../lib/nnsys/src/snd/snd_arc.c
    ../src/gfl/savedata.c ../src/gfl/backup_system.c ../src/gfl/backup_card.c
    ../src/gfl/gf_standard.c ../lib/nitro/src/math/math_crc.c
    ../pc/sdk/cp.c ../pc/sdk/memory.c ../pc/sdk/gx.c ../pc/sdk/bg.c ../pc/sdk/obj.c
    ../pc/sdk/fs.c ../pc/sdk/g2d_resources.c ../pc/sdk/display.c ../pc/sdk/card.c
    ../pc/sdk/power.c ../pc/sdk/mic.c ../pc/sdk/rtc.c ../pc/sdk/wireless.c ../pc/sdk/math.c)

target_link_libraries(bw2_g3d PUBLIC m)
include(../pc/game_sources.cmake)

target_include_directories(bw2_g3d SYSTEM PUBLIC
    ../lib/nitro/include ../lib/twl/include ../lib/nnsys/include ../lib/dwc/include ../lib/nitrowifi/include
    ../lib/spl/include ../lib/dsprot/include ../lib/irc/include)
target_compile_definitions(bw2_g3d PUBLIC POKEBW2_PC=1 POKEBW2_VITA=1 BLACK2=1)
# Match pc/CMakeLists.txt: enable documented use-after-free & native-allocator fixes (plist_sys.c, evolution, battle, field, etc.)
set_source_files_properties(../src/gfl/g3d_system.c ../src/gfl/arc_util.c ../src/ov012/field_actor.c
    ../src/ov284/evolution_demo.c
    ../src/ov165/plist_sys.c
    ../src/ov036/scrcmd_field_actor.c ../src/ov055/wbt_party.c ../src/ov209/musical_mcss.c
    ../src/ov148/encounter_effect_capture.c ../src/system/screentex.c
    ../src/ov168/btlv_mcss.c
    ../src/ov167/btlv_scu.c ../src/ov168/btlv_effvm.c
    ../src/ov209/sta_acting.c
    ../src/ov055/wbt_tool.c
    ../src/ov086/dowsing.c
    ../src/ov082/union_subdisp.c
    ../src/ov104/field_gimmick_gate.c
    ../src/ov036/field_map_chunk_rd.c
    ../src/system/fest_mission_beacon.c
    ../src/ov012/eventdata_system.c
    PROPERTIES COMPILE_DEFINITIONS BUGFIX=1)
set_target_properties(bw2_g3d PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_options(bw2_g3d PRIVATE -O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wall -Wno-error -ffunction-sections -fdata-sections)

# ---- Sequence engine (libntr + pc/sequence.c) ----
include(FetchContent)
FetchContent_Declare(pc_libntr
    GIT_REPOSITORY https://github.com/cybervisi0n/libntr.git
    GIT_TAG 1555d911ac80c0190ac1c321f99f1323de8b3c09
    PATCH_COMMAND ${CMAKE_COMMAND} -DPC_LIBNTR_SOURCE_DIR=<SOURCE_DIR>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/../pc/patches/apply-libntr-patch.cmake
    SOURCE_SUBDIR pc-unused-build)
FetchContent_MakeAvailable(pc_libntr)

add_library(pc_sequence_core STATIC
    ${pc_libntr_SOURCE_DIR}/libraries/sim7/src/snd/snd_seq.c
    ${pc_libntr_SOURCE_DIR}/libraries/sim7/src/snd/snd_exchannel.c
    ${pc_libntr_SOURCE_DIR}/libraries/snd/src/snd_util.c)
set_target_properties(pc_sequence_core PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(pc_sequence_core SYSTEM PUBLIC ${pc_libntr_SOURCE_DIR}/include)
target_compile_definitions(pc_sequence_core PRIVATE SDK_FROM_TOOL SDK_PORT SDK_BUILD_LINUX __stdcall=)

add_library(pc_sequence STATIC ../pc/sequence.c)
set_target_properties(pc_sequence PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(pc_sequence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../pc)
target_compile_definitions(pc_sequence PRIVATE SDK_FROM_TOOL SDK_PORT SDK_BUILD_LINUX __stdcall=)
target_link_libraries(pc_sequence PRIVATE pc_sequence_core)

# ---- Vita executable & VPK ----
add_executable(pokebw2.elf
    main.c
    boot_vita.c
    gx_video_vita.c
    audio_vita.c
    settings_vita.c
    overlay_vita.c
    ime_vita.c
    save_vita.c
    log_vita.c
    ../pc/timing.c
    sdl_shim_vita.c)
set_target_properties(pokebw2.elf PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(pokebw2.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../pc
    ${CMAKE_CURRENT_SOURCE_DIR}/../pc/include)
target_link_options(pokebw2.elf PRIVATE -Wl,--gc-sections -Wl,-z,nocopyreloc -Wl,--wrap=GFL_DebugAssertFailEx)
target_link_libraries(pokebw2.elf PRIVATE
    bw2_g3d
    pc_sequence
    vitaGL
    stdc++
    vitashark
    SceShaccCgExt
    taihen_stub
    SceShaccCg_stub
    SceGxm_stub
    SceDisplay_stub
    SceKernelDmacMgr_stub
    SceSysmodule_stub
    SceAppUtil_stub
    SceAppMgr_stub
    SceLibKernel_stub
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    ScePower_stub
    SceRtc_stub
    SceIme_stub
    SceCommonDialog_stub
    mathneon
    pthread
    m
    c)

vita_create_self(eboot.bin pokebw2.elf)
vita_create_vpk(pokebw2.vpk "PKBW20001" eboot.bin
    VERSION "01.00"
    NAME "Pokemon Black 2")

add_custom_command(TARGET bw2_g3d POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --redefine-sym region=bw2_version_region $<TARGET_FILE:bw2_g3d>
    COMMENT "Renaming version.c region symbol to avoid clash with vitaGL")

target_compile_options(pokebw2.elf PRIVATE -O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ffunction-sections -fdata-sections)
target_link_options(pokebw2.elf PRIVATE -Wl,--gc-sections)
target_link_options(pokebw2.elf PRIVATE -Wl,-z,max-page-size=0x10000)
