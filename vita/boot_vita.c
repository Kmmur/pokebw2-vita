#include "boot_vita.h"
#include "log_vita.h"
#include "app/bag.h"
#include "app/boot_screens.h"
#include "app/cdemo.h"
#include "app/config_panel.h"
#include "app/demo3d.h"
#include "app/event_battle_return.h"
#include "app/game_start.h"
#include "app/name_entry.h"
#include "app/pms_select.h"
#include "app/pokedex.h"
#include "app/pokedex_list.h"
#include "app/pokedex_registration.h"
#include "app/pokelist.h"
#include "app/psel.h"
#include "app/start_menu.h"
#include "app/title.h"
#include "app/townmap.h"
#include "audio.h"
#include "battle/battle_proc.h"
#include "demo/evolution_demo.h"
#include "demo/intro.h"
#include "field/field.h"
#include "field/field_menu.h"
#include "field/mailsys.h"
#include "settings_vita.h"
#include "field/trcard_sys.h"
#include "filesystem.h"
#include "gfl/arc.h"
#include "gfl/arc_util.h"
#include "gfl/bg_sys.h"
#include "gfl/fade.h"
#include "gfl/g3d.h"
#include "gfl/key.h"
#include "gfl/net.h"
#include "gfl/overlay.h"
#include "gfl/proc.h"
#include "gfl/rtc_cache.h"
#include "gfl/sound.h"
#include "gfl/std.h"
#include "gfl/tcb.h"
#include "gfl/touchpanel.h"
#include "gfl/ui.h"
#include "gx_backend.h"
#include "host_settings.h"
#include "nitro/host_gx.h"
#include "nitro/os.h"
#include "nnsys/gfd.h"
#include "pml/poke_party.h"
#include "pml/species_names.h"
#include "save.h"
#include "save/config.h"
#include "save/high_link.h"
#include "system/boot_proc.h"
#include "system/brightness.h"
#include "system/game_beacon.h"
#include "system/game_system.h"
#include "system/gfl_use.h"
#include "system/printsys.h"
#include "system/wipe.h"
#include "timing.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GameProcManager *manager;
static char *paths[308];
static SystemUI input;
static uint16_t cur_keys;
static bool cur_touch_held, cur_touch_pressed;
static uint32_t cur_touch_x, cur_touch_y;
static bool completed;
u32 PcOsIrqCheck;
u32 PcOsVBlankCount;
static u64 ticks;
static TCBManager *vblank, *hblank;
static NNSGfdVramTransferTask vramTransfers[48];
static PcGxVertex *fieldVertices = NULL;
static u32 fieldVertexCount = 0, fieldVertexCapacity = 0, fieldSparseFrames = 0, fieldCaptureGrace = 0;
static bool fieldCaptureWasActive = false;
static bool isFieldSceneActive = false;
static bool reused3DFrame = false;

bool VitaBoot_IsFieldScene(void) { return isFieldSceneActive; }
bool VitaBoot_Reused3DFrame(void) { return reused3DFrame; }
bool VitaBoot_IsBattleScene(void) {
    return manager && GCTX_ProcMgrCurrentFunctions() == &BATTLE_PROC_FUNCTIONS;
}
bool VitaBoot_IsTextEntryScene(void) {
    if (!manager) return false;
    const GameProcFunctions *fn = GCTX_ProcMgrCurrentFunctions();
    return fn == &NAME_ENTRY_PROC_FUNCTIONS || fn == &PMS_SELECT_PROC_FUNCTIONS;
}
bool VitaBoot_IsTitleScene(void) {
    return manager && GCTX_ProcMgrCurrentFunctions() == &TITLE_PROC_FUNCTIONS;
}
bool VitaBoot_IsIntroScene(void) {
    return manager && GCTX_ProcMgrCurrentFunctions() == &INTRO_PROC_FUNCTIONS;
}
bool VitaBoot_IsStarterSelectScene(void) {
    return manager && GCTX_ProcMgrCurrentFunctions() == &PselProcFunctions;
}
bool VitaBoot_IsFieldMenuOpen(void) {
    return isFieldSceneActive && FieldMenu_IsOpenNative() != FALSE;
}
bool VitaBoot_IsTouchPrimaryScene(void) {
    if (!manager) return false;
    const GameProcFunctions *fn = GCTX_ProcMgrCurrentFunctions();
    return fn == &PselProcFunctions || fn == &START_MENU_PROC_FUNCTIONS ||
           fn == &BAG_PROC_FUNCTIONS || fn == &POKELIST_PROC_FUNCTIONS ||
           fn == &PSTATUS_PROC_FUNCTIONS || fn == &POKEDEX_PROC_FUNCTIONS ||
           fn == &TOWNMAP_PROC_FUNCTIONS || fn == &CONFIG_PANEL_PROC_FUNCTIONS ||
           fn == &TRCARD_SYS_PROC_FUNCTIONS || fn == &MAILSYS_PROC_FUNCTIONS;
}
bool VitaBoot_IsWide3DActive(void) {
    const VitaSettings *s = VitaSettings_Get();
    if (!manager || s->layout != VITA_LAYOUT_SINGLE || !s->single_widescreen) return false;
    const GameProcFunctions *fn = GCTX_ProcMgrCurrentFunctions();
    // Match pc/boot.c: true 16:9 widescreen on 3D overworld, cutscenes, and battles.
    // Title screen has opaque black 2D logo on Sub + 3D on Bottom (pow=0), so keep it cleanly dual-framed!
    return fn == &FIELDMAP_PROC_FUNCS || fn == &gDemo3DProcFunctions ||
           fn == &BATTLE_PROC_FUNCTIONS;
}
bool VitaBoot_IsMenuScene(void) {
    if (!manager) return false;
    const GameProcFunctions *fn = GCTX_ProcMgrCurrentFunctions();
    return fn == &BAG_PROC_FUNCTIONS || fn == &POKELIST_PROC_FUNCTIONS ||
           fn == &PSTATUS_PROC_FUNCTIONS || fn == &POKEDEX_PROC_FUNCTIONS ||
           fn == &TOWNMAP_PROC_FUNCTIONS || fn == &CONFIG_PANEL_PROC_FUNCTIONS ||
           fn == &START_MENU_PROC_FUNCTIONS || fn == &TRCARD_SYS_PROC_FUNCTIONS;
}

void VitaBoot_SetInput(uint16_t keys, bool touch_held, bool touch_pressed, uint32_t touch_x, uint32_t touch_y) {
    cur_keys = keys;
    cur_touch_held = touch_held;
    cur_touch_pressed = touch_pressed;
    cur_touch_x = touch_x;
    cur_touch_y = touch_y;
}

bool VitaBoot_Start(const char *root, VitaBootEntry entry) {
    ticks = 0;
    PcOsVBlankCount = 0;
    input.frameCount = 0;
    PcFs_SetRoot(root);
    NNS_GfdInitVramTransferManager(vramTransfers, NELEMS(vramTransfers));

    VitaLog_Printf("boot: HeapCreateChild");
    GFL_HeapCreateChild(HEAPID_SYSTEM, HEAPID_USER, 0x1a3e00);
    initTableArea(HEAPID_SYSTEM);
    input.keypad = GFL_HIDCreateKeypadManager(HEAPID_SYSTEM);
    input.updateRate = PC_GAME_HZ;
    for (unsigned i = 0; i < NELEMS(paths); i++) {
        paths[i] = malloc(16);
        if (!paths[i]) return false;
        snprintf(paths[i], 16, "a/%u/%u/%u", i / 100, (i / 10) % 10, i % 10);
    }
    VitaLog_Printf("boot: ArcSysInit");
    GFL_ArcSysInit((const char **)paths, NELEMS(paths));
    VitaLog_Printf("boot: PML_SpeciesNames");
    PML_SpeciesNamesResidentInit(HEAPID_SYSTEM);
    VitaLog_Printf("boot: PML_Personal");
    PML_PersonalInit(HEAPID_SYSTEM);
    GFL_FadeCreate(HEAPID_SYSTEM);
    GFL_TextRndInit(HEAPID_SYSTEM);
    RTCCache_Init();
    GFL_NetHeapInit();
    GFL_NetErrInit(HEAPID_NET);
    initKeyBlk();
    GameBeaconSys_Create(HEAPID_USER);
    PassPower_Init();
    VitaLog_Printf("boot: PcAudio_Init");
    if (!PcAudio_Init(root)) {
        VitaLog_Printf("[CPU-bound] PcAudio_Init failed for root=%s", root);
        return false;
    }
    vblank = GFL_TCBMgrCreate(32, GFL_HeapAllocate(HEAPID_SYSTEM, GFL_TCBMgrCalcAllocSize(32), TRUE, __FILE__, __LINE__));
    hblank = GFL_TCBMgrCreate(32, GFL_HeapAllocate(HEAPID_SYSTEM, GFL_TCBMgrCalcAllocSize(32), TRUE, __FILE__, __LINE__));
    GCTX_ProcMgrInit(HEAPID_SYSTEM);
    manager = GCTX_ProcMgrGet();
    completed = false;
    if (entry >= VITA_BOOT_MENU && !PcSave_Init()) {
        VitaLog_Printf("[CPU-bound] PcSave_Init failed");
        return false;
    }
    const GameProcFunctions *process = entry == VITA_BOOT_NEW_GAME ? &NEW_GAME_PROC_FUNCTIONS
                                       : entry == VITA_BOOT_MENU   ? &START_MENU_PROC_FUNCTIONS
                                       : entry == VITA_BOOT_TITLE  ? &TITLE_PROC_FUNCTIONS
                                                                   : &EVENT_TITLE_FUNCS;
    VitaLog_Printf("boot: QueueGameProc");
    QueueGameProc(manager, OVERLAY_NONE, process, NULL);
    return true;
}

void VitaBoot_Update(void) {
    VitaBoot_UpdateStep(true);
}

void VitaBoot_UpdateStep(bool tick_audio) {
    if (!manager || completed) return;
    // Enable true 16:9 horizontal 3D FOV expansion & box-test frustum culling in Single Screen Widescreen!
    static bool prev_wide3d = false;
    bool wide3d = VitaBoot_IsWide3DActive();
    if (wide3d != prev_wide3d) {
        prev_wide3d = wide3d;
        fieldVertexCount = 0;
        fieldSparseFrames = 0;
    }
    PcGx.wide_scene = wide3d;
    PcGx.projection_scale_x = wide3d ? ((4.0f / 3.0f) / (960.0f / 544.0f)) : 1.0f;
    PcGx_BeginFrame();
    GCTX_HIDUpdateKeypad();
    ticks++;
    PcOsVBlankCount++;
    if (tick_audio) PcAudio_Update();
    RTCCache_Update();
    GFL_NetErrUpdate();
    GameBeaconSys_Update();
    PassPower_Update();
    BrightnessController_Update();
    GFL_FadeUpdate();
    GFL_WipeMain();
    PC_GX_REG16(0x04000006) = 192;
    reg_GX_DISPSTAT |= REG_GX_DISPSTAT_VBLK_MASK;
    PcGx_Capture();
    GFL_VBlankExecCallback();
    NNS_GfdDoVramTransfer();
    GFL_TCBMgrUpdate(vblank);
    GFL_BGSysUpdate();
    reg_GX_DISPSTAT &= ~REG_GX_DISPSTAT_VBLK_MASK;
    PC_GX_REG16(0x04000006) = 0;
    if (!completed)
        completed = GFL_ProcMgrUpdate(manager) == 0;
    input.frameCount++;
}

void VitaBoot_Draw(void) {
    GFL_G3DSysCheckSwapBuffers();
    // Ported from pc/boot.c: FIELDMAP submits 3D at 30 Hz and briefly drops actors on capture transitions.
    bool fieldScene =
        GCTX_ProcMgrCurrentFunctions() == &FIELDMAP_PROC_FUNCS || (fieldCaptureWasActive && PcGx_WideCaptureActive());
    isFieldSceneActive = fieldScene;
    bool fieldReady = fieldScene && g_GameSystem != NULL && GSYS_GetField(g_GameSystem) != NULL &&
                      Field_CheckMapLoadFinished(GSYS_GetField(g_GameSystem));
    bool wideCapture = fieldScene && PcGx_WideCaptureActive();
    if (!fieldScene) {
        fieldCaptureWasActive = false;
        fieldCaptureGrace = 0;
    } else if (wideCapture) {
        fieldCaptureWasActive = true;
        fieldCaptureGrace = 0;
    } else if (fieldCaptureWasActive) {
        fieldCaptureWasActive = false;
        fieldCaptureGrace = 24;
    } else if (fieldCaptureGrace) {
        fieldCaptureGrace--;
    }
    reused3DFrame = false;
    if (fieldReady) {
        bool sparse = fieldVertexCount && PcGx.vertex_count < fieldVertexCount &&
                      (fieldCaptureGrace || PcGx.vertex_count * 2 < fieldVertexCount) &&
                      fieldSparseFrames < (fieldCaptureGrace ? 4u : 2u);
        if (PcGx.vertex_count && !sparse) {
            fieldSparseFrames = 0;
            if (PcGx.vertex_count > fieldVertexCapacity) {
                PcGxVertex *vertices = (PcGxVertex *)realloc(fieldVertices, PcGx.vertex_count * sizeof(*fieldVertices));
                if (vertices) {
                    fieldVertices = vertices;
                    fieldVertexCapacity = PcGx.vertex_count;
                }
            }
            if (PcGx.vertex_count <= fieldVertexCapacity) {
                fieldVertexCount = PcGx.vertex_count;
                memcpy(fieldVertices, PcGx.vertices, fieldVertexCount * sizeof(*fieldVertices));
            }
        } else if (fieldVertexCount && fieldVertexCount <= PC_GX_MAX_VERTICES) {
            if (PcGx.vertex_count)
                fieldSparseFrames++;
            memcpy(PcGx.vertices, fieldVertices, fieldVertexCount * sizeof(*fieldVertices));
            PcGx.vertex_count = fieldVertexCount;
            reused3DFrame = true;
        }
    } else {
        fieldVertexCount = 0;
        fieldSparseFrames = 0;
    }
}

void VitaBoot_Stop(void) {
    PcAudio_Shutdown();
    PcSave_Shutdown();
    manager = NULL;
    completed = true;
    GameBeaconSys = NULL;
    GFL_ArcSysInit(NULL, 0);
    PcFs_SetRoot(NULL);
    free(fieldVertices);
    fieldVertices = NULL;
    fieldVertexCount = fieldVertexCapacity = 0;
    fieldSparseFrames = 0;
    fieldCaptureGrace = 0;
    fieldCaptureWasActive = false;
    isFieldSceneActive = false;
    reused3DFrame = false;
    for (unsigned i = 0; i < NELEMS(paths); i++) {
        free(paths[i]);
        paths[i] = NULL;
    }
}

u64 OS_GetTick(void) {
    return (ticks + 1) * OS_SYSTEM_CLOCK / (64 * PC_GAME_HZ);
}

TCB *GFL_VBlankTCBAdd(TCBFunc func, void *data, u32 priority) {
    return GFL_TCBMgrAddTask(vblank, func, data, priority);
}
TCB *GFL_HBlankTCBAdd(TCBFunc func, void *data, u32 priority) {
    return GFL_TCBMgrAddTask(hblank, func, data, priority);
}
TCBManager *GFL_VBlankGetTCBMgr(void) {
    return vblank;
}

void OS_WaitIrq(BOOL clear, OSIrqMask interrupts) {
    (void)clear;
    if (interrupts != OS_IE_V_BLANK)
        GFL_DebugAssertFailEx(__FILE__, __LINE__, "Unsupported native interrupt wait %u", interrupts);
    sceKernelDelayThread(16667);
    PcOsVBlankCount++;
    PC_GX_REG16(0x04000006) = 192;
    reg_GX_DISPSTAT |= REG_GX_DISPSTAT_VBLK_MASK;
    PcGx_Capture();
    GFL_VBlankExecCallback();
    NNS_GfdDoVramTransfer();
    GFL_TCBMgrUpdate(vblank);
    GFL_BGSysUpdate();
    reg_GX_DISPSTAT &= ~REG_GX_DISPSTAT_VBLK_MASK;
    PC_GX_REG16(0x04000006) = 0;
}
void OS_WaitInterrupt(BOOL clear, OSIrqMask interrupts) { OS_WaitIrq(clear, interrupts); }
void OS_WaitVBlankIntr(void) { OS_WaitIrq(TRUE, OS_IE_V_BLANK); }

KeypadManager *GFL_HIDGetKeypadManager(SystemUI *ui) { return ui->keypad; }
u8 GCTX_HIDGetUpdateRate(void) { return input.updateRate; }
void GCTX_HIDSetUpdateRate(u8 rate) { input.updateRate = rate; }

u16 PcPad_Read(void) {
    return cur_keys;
}

BOOL GCTX_HIDIsLidClosed(void) { return FALSE; }
BOOL GetIsShouldUICallback(void) { return FALSE; }
void SetUICallbackStatic(UICallback callback, void *work) { input.lowBatteryCallback = callback; input.lowBatteryWork = work; }
void GCTX_HIDSetSleepCheckCallback(UISleepCheckCallback callback, void *work) { input.sleepCheckCallback = callback; input.sleepCheckWork = work; }
void GCTX_HIDSetSleepCallback(UICallback callback, void *work) { input.sleepCallback = callback; input.sleepWork = work; }
void GCTX_HIDSetWakeCallback(UICallback callback, void *work) { input.wakeCallback = callback; input.sleepWork = work; }
UICallback GCTX_HIDGetLidCloseCallback(void) { return input.lidCloseCallback; }
int GCTX_HIDGetBatteryLevel(void) {
    int pct = scePowerGetBatteryLifePercent();
    if (pct < 0) return 5;
    return (pct * 5 + 99) / 100;
}
void GCTX_HIDSetSoftResetCallback(SoftResetCallback callback, void *work) { input.softResetCallback = callback; input.softResetWork = work; }

BOOL GFL_HIDIsTouchPressed(void) { return cur_touch_pressed; }
BOOL GFL_HIDIsTouchHeld(void) { return cur_touch_held; }
BOOL GCTX_HIDIsUsingTouch(void) { return input.usingTouch; }
void GCTX_HIDSetUsingTouch(BOOL touch) { input.usingTouch = touch; }

BOOL GFL_HIDGetTouchPressedPoint(u32 *x, u32 *y) {
    if (!cur_touch_pressed) return FALSE;
    return GFL_HIDGetTouchHeldPoint(x, y);
}
BOOL GFL_HIDGetTouchHeldPoint(u32 *x, u32 *y) {
    if (!cur_touch_held) return FALSE;
    *x = cur_touch_x;
    *y = cur_touch_y;
    return TRUE;
}
s32 GFL_HIDGetTouchRectAt(const TouchRect *rects, u32 x, u32 y) {
    for (s32 i = 0; rects[i].top != TOUCH_RECT_END; i++) {
        const TouchRect *rect = &rects[i];
        if (rect->top == TOUCH_RECT_SKIP) continue;
        if (rect->top == TOUCH_RECT_CIRCLE) {
            u32 dx = rect->bottom - x, dy = rect->left - y;
            if (dx * dx + dy * dy < (u32)rect->right * rect->right) return i;
        } else if (x - rect->left <= (u32)(rect->right - rect->left) &&
                   y - rect->top <= (u32)(rect->bottom - rect->top)) {
            return i;
        }
    }
    return TOUCH_RECT_NONE;
}
s32 GFL_HIDGetTouchHeldRect(const TouchRect *rects) {
    u32 x, y;
    return GFL_HIDGetTouchHeldPoint(&x, &y) ? GFL_HIDGetTouchRectAt(rects, x, y) : TOUCH_RECT_NONE;
}
s32 GFL_HIDGetTouchPressedRect(const TouchRect *rects) {
    u32 x, y;
    return GFL_HIDGetTouchPressedPoint(&x, &y) ? GFL_HIDGetTouchRectAt(rects, x, y) : TOUCH_RECT_NONE;
}

void PcSettings_ApplyGame(Config *config) { (void)config; }

const GameProcFunctions *PcProc_Find(const char *name) {
    VitaLog_Printf("Starting original application %s", name);
    if (strcmp(name, "gDemo3DProcFunctions") == 0) return &gDemo3DProcFunctions;
    if (strcmp(name, "CONTINUE_PROC_FUNCTIONS") == 0) return &CONTINUE_PROC_FUNCTIONS;
    if (strcmp(name, "CONFIG_PANEL_PROC_FUNCTIONS") == 0) return &CONFIG_PANEL_PROC_FUNCTIONS;
    if (strcmp(name, "FIELDMAP_PROC_FUNCS") == 0) return &FIELDMAP_PROC_FUNCS;
    if (strcmp(name, "BATTLE_PROC_FUNCTIONS") == 0) return &BATTLE_PROC_FUNCTIONS;
    if (strcmp(name, "EVENT_BATTLE_RETURN_PROC_FUNCTIONS") == 0) return &EVENT_BATTLE_RETURN_PROC_FUNCTIONS;
    if (strcmp(name, "POKEDEX_REGISTRATION_PROC_FUNCTIONS") == 0) return &POKEDEX_REGISTRATION_PROC_FUNCTIONS;
    if (strcmp(name, "EVOLUTION_DEMO_PROC_FUNCTIONS") == 0) return &EVOLUTION_DEMO_PROC_FUNCTIONS;
    if (strcmp(name, "GAMESYSTEM_PROC_FUNCTIONS") == 0) return &GAMESYSTEM_PROC_FUNCTIONS;
    if (strcmp(name, "INTRO_PROC_FUNCTIONS") == 0) return &INTRO_PROC_FUNCTIONS;
    if (strcmp(name, "NAME_ENTRY_PROC_FUNCTIONS") == 0) return &NAME_ENTRY_PROC_FUNCTIONS;
    if (strcmp(name, "NEW_GAME_PROC_FUNCTIONS") == 0) return &NEW_GAME_PROC_FUNCTIONS;
    if (strcmp(name, "PselProcFunctions") == 0) return &PselProcFunctions;
    if (strcmp(name, "POKELIST_PROC_FUNCTIONS") == 0) return &POKELIST_PROC_FUNCTIONS;
    if (strcmp(name, "PSTATUS_PROC_FUNCTIONS") == 0) return &PSTATUS_PROC_FUNCTIONS;
    if (strcmp(name, "BAG_PROC_FUNCTIONS") == 0) return &BAG_PROC_FUNCTIONS;
    if (strcmp(name, "MAILSYS_PROC_FUNCTIONS") == 0) return &MAILSYS_PROC_FUNCTIONS;
    if (strcmp(name, "TOWNMAP_PROC_FUNCTIONS") == 0) return &TOWNMAP_PROC_FUNCTIONS;
    if (strcmp(name, "TRCARD_SYS_PROC_FUNCTIONS") == 0) return &TRCARD_SYS_PROC_FUNCTIONS;
    if (strcmp(name, "PMS_SELECT_PROC_FUNCTIONS") == 0) return &PMS_SELECT_PROC_FUNCTIONS;
    if (strcmp(name, "POKEDEX_PROC_FUNCTIONS") == 0) return &POKEDEX_PROC_FUNCTIONS;
    if (strcmp(name, "START_MENU_PROC_FUNCTIONS") == 0) {
        if (!PcSave_Init())
            GFL_DebugAssertFailEx(__FILE__, __LINE__, "Opening native save failed");
        return &START_MENU_PROC_FUNCTIONS;
    }
    if (strcmp(name, "TITLE_PROC_FUNCTIONS") == 0) return &TITLE_PROC_FUNCTIONS;
    if (strcmp(name, "BOOT_SCREENS_PROC_FUNCTIONS") == 0) return &BOOT_SCREENS_PROC_FUNCTIONS;
    if (strcmp(name, "CDEMO_PROC_FUNCTIONS") == 0) return &CDEMO_PROC_FUNCTIONS;
    GFL_DebugAssertFailEx(__FILE__, __LINE__, "Native application %s has not been linked", name);
    return NULL;
}

BOOL GFL_OvlLoad(u32 overlayId) { (void)overlayId; return TRUE; }
void GFL_OvlUnload(u32 overlayId) { (void)overlayId; }
void OS_Halt(void) { GFL_DebugAssertFailEx(__FILE__, __LINE__, "Unexpected halt"); }
void OS_Terminate(void) { GFL_DebugAssertFailEx(__FILE__, __LINE__, "Unexpected terminate"); }

SystemUI *GCTX_HIDGetInstance(void) { return &input; }
void GCTX_HIDUnblockSoftReset(u8 flags) { input.softResetBlock &= ~flags; }
void GCTX_HIDBlockSoftReset(u8 flags) { input.softResetBlock |= flags; }
BOOL GCTX_HIDIsSoftResetBlocked(u8 flags) { return (input.softResetBlock & flags) != 0; }
void GCTX_HIDResetFrameCount(void) { input.frameCount = 0; }
void GCTX_HIDChangeFPS(u8 rate) {
    input.updateRate = rate;
    GFL_HIDClearKeypadState(&input);
    cur_touch_held = cur_touch_pressed = false;
}
void GCTX_HIDSetLidCallbacks(UICallback close, UICallback open, void *work) {
    input.lidCloseCallback = close;
    input.lidOpenCallback = open;
    input.lidWork = work;
}
void GCTX_HIDUpdate(void) {
    GCTX_HIDUpdateKeypad();
    input.frameCount++;
}
void OS_ResetSystem(u32 parameter) {
    (void)parameter;
    VitaLog_Printf("OS_ResetSystem(%u) called", parameter);
}
