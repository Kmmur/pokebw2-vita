#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "boot_vita.h"
#include "gx_video_vita.h"
#include "log_vita.h"
#include "overlay_vita.h"
#include "ime_vita.h"
#include "settings_vita.h"
#include "../pc/timing.h"
#include "gfl/key.h"
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <stdbool.h>
#include <stdint.h>

extern void VitaAudio_GetStats(int32_t *peak, unsigned *player, unsigned *bgm_id, bool *active, bool *paused,
                               unsigned *vol, int *mvol, uint32_t *underruns, uint32_t *chunks_out);
extern void VitaAudio_SetPaused(bool paused);
extern void VitaAudio_SetUserVolume(float vol);

static uint16_t PollVitaKeys(const SceCtrlData *pad) {
    const VitaSettings *s = VitaSettings_Get();
    uint16_t keys = 0;

    if (s->button_preset == VITA_BTN_NINTENDO) {
        if (pad->buttons & SCE_CTRL_CIRCLE)   keys |= PAD_BUTTON_A;
        if (pad->buttons & SCE_CTRL_CROSS)    keys |= PAD_BUTTON_B;
    } else {
        if (pad->buttons & SCE_CTRL_CROSS)    keys |= PAD_BUTTON_A;
        if (pad->buttons & SCE_CTRL_CIRCLE)   keys |= PAD_BUTTON_B;
    }
    if (pad->buttons & SCE_CTRL_SELECT)   keys |= PAD_BUTTON_SELECT;
    if (pad->buttons & SCE_CTRL_START)    keys |= PAD_BUTTON_START;
    if (pad->buttons & SCE_CTRL_RIGHT)    keys |= PAD_KEY_RIGHT;
    if (pad->buttons & SCE_CTRL_LEFT)     keys |= PAD_KEY_LEFT;
    if (pad->buttons & SCE_CTRL_UP)       keys |= PAD_KEY_UP;
    if (pad->buttons & SCE_CTRL_DOWN)     keys |= PAD_KEY_DOWN;
    if (pad->buttons & SCE_CTRL_RTRIGGER) keys |= PAD_BUTTON_R;
    if (pad->buttons & SCE_CTRL_LTRIGGER) keys |= PAD_BUTTON_L;
    if (pad->buttons & SCE_CTRL_TRIANGLE) keys |= PAD_BUTTON_X;
    if (pad->buttons & SCE_CTRL_SQUARE)   keys |= PAD_BUTTON_Y;

    // Configurable Left Analog -> D-Pad with configurable deadzone
    if (s->analog_mode) {
        int dz = (s->analog_deadzone == 0) ? 19 : (s->analog_deadzone == 2) ? 51 : 32;
        if ((int)pad->lx < 128 - dz) keys |= PAD_KEY_LEFT;
        if ((int)pad->lx > 128 + dz) keys |= PAD_KEY_RIGHT;
        if ((int)pad->ly < 128 - dz) keys |= PAD_KEY_UP;
        if ((int)pad->ly > 128 + dz) keys |= PAD_KEY_DOWN;
    }

    // Enhanced Edition QoL: Auto-Run on Field Map (invert B while moving so player runs by default)
    if (VitaSettings_EffectiveAutoRun() && VitaBoot_IsFieldScene()) {
        if (keys & (PAD_KEY_UP | PAD_KEY_DOWN | PAD_KEY_LEFT | PAD_KEY_RIGHT))
            keys ^= PAD_BUTTON_B;
    }
    return keys;
}

unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;

void __wrap_GFL_DebugAssertFailEx(const char *file, uint32_t line, const char *format, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(msg, sizeof(msg), format, ap);
    va_end(ap);
    VitaLog_Printf("[RAM-bound] ASSERT %s:%u: %s", file, line, msg);
    sceKernelDelayThread(100000);
    abort();
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    VitaLog_Init();

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    VitaLog_Printf("pokebw2 Vita v26 starting (clocks: ARM=%d MHz, BUS=%d MHz, GPU=%d MHz)",
                   scePowerGetArmClockFrequency(),
                   scePowerGetBusClockFrequency(),
                   scePowerGetGpuClockFrequency());

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    VitaSettings_Load();

    if (!VitaVideo_Init()) {
        VitaLog_Shutdown();
        return 1;
    }
    VitaOverlay_Init();
    VitaIme_Init();
    VitaAudio_SetUserVolume((float)VitaSettings_Get()->master_volume / 10.0f);

    SceIoStat st;
    const char *data_root = "ux0:data/pokebw2/files";
    bool has_data = sceIoGetstat("ux0:data/pokebw2/files/a/0/0/2", &st) >= 0 &&
                    sceIoGetstat("ux0:data/pokebw2/files/swan_sound_data.sdat", &st) >= 0;

    if (!has_data) {
        VitaLog_Printf("[bandwidth-bound] Missing extracted game data at %s", data_root);
        uint32_t frame = 0;
        while (1) {
            SceCtrlData pad;
            sceCtrlPeekBufferPositive(0, &pad, 1);
            if ((pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT)) == (SCE_CTRL_START | SCE_CTRL_SELECT))
                break;
            uint32_t pulse = 32 + ((frame % 120) < 60 ? (frame % 60) : (60 - (frame % 60)));
            VitaVideo_DrawDiagnostic(16, pulse, 64);
            frame++;
        }
        VitaOverlay_Shutdown();
        VitaVideo_Shutdown();
        VitaLog_Shutdown();
        sceKernelExitProcess(0);
        return 0;
    }

    VitaLog_Printf("Calling VitaBoot_Start...");
    if (!VitaBoot_Start(data_root, VITA_BOOT_MOVIES)) {
        VitaLog_Printf("[CPU-bound] VitaBoot_Start failed");
        VitaOverlay_Shutdown();
        VitaVideo_Shutdown();
        VitaLog_Shutdown();
        return 1;
    }

    VitaLog_Printf("VitaBoot_Start succeeded! Entering 60Hz loop...");
    PcGameClock clock;
    PcGameClock_Init(&clock, sceKernelGetProcessTimeWide() * 1000ULL);
    bool prev_touch = false;
    bool prev_layout_btn = false;
    bool prev_shot_btn = false;
    bool prev_combo_btn = false;
    bool wait_release_after_overlay = false;
    int single_peek_timer = 0;
    float vcur_x = 128.0f, vcur_y = 96.0f;
    int vcur_idle_timer = 0;
    uint32_t frame_count = 0;
    uint64_t stat_start_us = sceKernelGetProcessTimeWide();
    uint64_t cpu_accum_us = 0, gpu_accum_us = 0;

    while (1) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        SceTouchData touch;
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

        // Clean exit: Start + Select + L + R
        if ((pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) ==
            (SCE_CTRL_START | SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))
            break;

        // Configurable Overlay Menu Hotkey
        bool combo_btn = VitaOverlay_CheckCombo(pad.buttons);
        if (combo_btn && !prev_combo_btn && !VitaOverlay_IsOpen()) {
            VitaOverlay_Open();
            VitaAudio_SetPaused(true);
        }
        prev_combo_btn = combo_btn;

        // While PS Vita OS Keyboard (SceImeDialog) is open: keep dialog smooth & pause game
        if (VitaIme_IsActive()) {
            VitaAudio_SetPaused(true);
            VitaIme_Update();
            VitaVideo_PresentPaused();
            if (!VitaIme_IsActive()) {
                VitaAudio_SetPaused(false);
                wait_release_after_overlay = true;
                PcGameClock_Init(&clock, sceKernelGetProcessTimeWide() * 1000ULL);
            }
            continue;
        }

        // While Overlay Menu is open: pause gameplay & audio cleanly, render live overlay
        if (VitaOverlay_IsOpen()) {
            VitaOverlay_Update(&pad, &touch);
            if (!VitaOverlay_IsOpen()) {
                VitaAudio_SetPaused(false);
                wait_release_after_overlay = true;
                PcGameClock_Init(&clock, sceKernelGetProcessTimeWide() * 1000ULL);
            }
            VitaVideo_PresentPaused();
            continue;
        }

        // Prevent close button (Circle / combo) from bleeding into the game
        if (wait_release_after_overlay) {
            if ((pad.buttons & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_START | SCE_CTRL_SELECT)) == 0)
                wait_release_after_overlay = false;
            else
                pad.buttons &= ~(SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_START | SCE_CTRL_SELECT);
        }

        // Quick layout / single-screen cycle hotkey: Select + R (still handy alongside overlay)
        bool layout_btn = (pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER)) == (SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER);
        if (layout_btn && !prev_layout_btn) {
            VitaSettings *ms = VitaSettings_GetMutable();
            if (ms->layout == VITA_LAYOUT_SINGLE) {
                // In Single Screen, Select+R cycles the KH 4-Corner PiP position on the fly!
                ms->pip_corner = (uint8_t)((ms->pip_corner + 1) % VITA_PIP_COUNT);
            } else {
                ms->layout = (uint8_t)((ms->layout + 1) % VITA_LAYOUT_COUNT);
            }
            VitaVideo_ApplySettings();
            VitaSettings_Save();
            VitaLog_Printf("Quick switch -> layout=%u pip_corner=%u", ms->layout, ms->pip_corner);
        }
        prev_layout_btn = layout_btn;

        // Screenshot hotkey: Select + L (only when overlay combo isn't L+Select)
        bool shot_btn = (VitaSettings_Get()->overlay_combo != VITA_COMBO_L_SELECT) &&
                        ((pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER)) == (SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER));
        if (shot_btn && !prev_shot_btn) {
            VitaVideo_RequestScreenshot();
        }
        prev_shot_btn = shot_btn;

        // Touchscreen + Single-Screen Peek + Enhanced Right-Stick Virtual Cursor
        const VitaSettings *s = VitaSettings_Get();
        bool touch_held = false;
        uint32_t tx = 0, ty = 0;

        if (s->touch_mode != 2) { // Not disabled
            if (touch.reportNum > 0) {
                int sx = touch.report[0].x * 960 / 1920;
                int sy = touch.report[0].y * 544 / 1088;
                bool was_vis = VitaVideo_IsTouchVisible();
                VitaRect sub = VitaVideo_GetTouchRect();
                bool in_sub = was_vis && (sx >= sub.x && sx < sub.x + sub.w && sy >= sub.y && sy < sub.y + sub.h);
                if (!was_vis && s->layout == VITA_LAYOUT_SINGLE && (s->touch_mode == 1 || VitaSettings_EffectiveEnhancedTouch())) {
                    // First tap when PiP is hidden reveals it without blindly clicking through
                    single_peek_timer = 210;
                    VitaVideo_SetSinglePeek(true);
                } else if (was_vis && single_peek_timer > 0) {
                    single_peek_timer = 210;
                }
                if (in_sub) {
                    touch_held = true;
                    tx = (uint32_t)((sx - sub.x) * 256 / sub.w);
                    ty = (uint32_t)((sy - sub.y) * 192 / sub.h);
                }
            } else if (single_peek_timer > 0) {
                single_peek_timer--;
                if (single_peek_timer == 0) VitaVideo_SetSinglePeek(false);
            }

            // Enhanced Edition: Right analog stick drives virtual touch cursor (R trigger taps)
            if (VitaSettings_EffectiveEnhancedTouch()) {
                int rx = (int)pad.rx - 128;
                int ry = (int)pad.ry - 128;
                if (rx < -28 || rx > 28 || ry < -28 || ry > 28) {
                    vcur_x += (float)rx * 0.045f;
                    vcur_y += (float)ry * 0.045f;
                    if (vcur_x < 0.0f) vcur_x = 0.0f;
                    if (vcur_x > 255.0f) vcur_x = 255.0f;
                    if (vcur_y < 0.0f) vcur_y = 0.0f;
                    if (vcur_y > 191.0f) vcur_y = 191.0f;
                    vcur_idle_timer = 180;
                    if (s->layout == VITA_LAYOUT_SINGLE) {
                        single_peek_timer = 150;
                        VitaVideo_SetSinglePeek(true);
                    }
                } else if (vcur_idle_timer > 0) {
                    vcur_idle_timer--;
                }
                VitaVideo_SetTouchCursor(vcur_idle_timer > 0, (uint32_t)vcur_x, (uint32_t)vcur_y);
                if (!touch_held && vcur_idle_timer > 0 && (pad.buttons & SCE_CTRL_RTRIGGER)) {
                    touch_held = true;
                    tx = (uint32_t)vcur_x;
                    ty = (uint32_t)vcur_y;
                }
            } else {
                VitaVideo_SetTouchCursor(false, 0, 0);
            }
        } else {
            VitaVideo_SetSinglePeek(false);
            VitaVideo_SetTouchCursor(false, 0, 0);
        }

        bool touch_pressed = touch_held && !prev_touch;
        prev_touch = touch_held;

        VitaBoot_SetInput(PollVitaKeys(&pad), touch_held, touch_pressed, tx, ty);

        uint64_t t0 = sceKernelGetProcessTimeWide();
        unsigned steps = PcGameClock_Advance(&clock, t0 * 1000ULL);
        if (steps == 0) {
            sceKernelDelayThread(1000);
            continue;
        }
        if (steps > 1) {
            steps = 1;
            PcGameClock_Init(&clock, t0 * 1000ULL);
        }
        VitaBoot_UpdateStep(true);
        // Enhanced Edition QoL: 2x Battle Animation Speed (extra visual tick without double-ticking audio)
        if (VitaSettings_EffectiveBattleAnimSpeed() == 1 && VitaBoot_IsBattleScene()) {
            VitaBoot_UpdateStep(false);
        }
        // Enhanced Edition QoL: Fast Menus & Inventory navigation
        if (VitaSettings_EffectiveFastMenus() && VitaBoot_IsMenuScene() && (frame_count & 1u)) {
            VitaBoot_UpdateStep(false);
        }
        VitaBoot_Draw();
        uint64_t t1 = sceKernelGetProcessTimeWide();

        VitaVideo_Present();
        uint64_t t2 = sceKernelGetProcessTimeWide();

        cpu_accum_us += (t1 - t0);
        gpu_accum_us += (t2 - t1);
        frame_count++;
        if (frame_count == 1) VitaLog_Printf("Frame 1 presented cleanly (cpu=%.2fms gpu=%.2fms)", (double)(t1 - t0)/1000.0, (double)(t2 - t1)/1000.0);

        if (frame_count % 120 == 0) {
            uint64_t elapsed_us = t2 - stat_start_us;
            double fps = elapsed_us ? (120.0 * 1000000.0 / (double)elapsed_us) : 0.0;
            double cpu_ms = (double)cpu_accum_us / 120000.0;
            double gpu_ms = (double)gpu_accum_us / 120000.0;
            SceKernelFreeMemorySizeInfo mem;
            mem.size = sizeof(mem);
            sceKernelGetFreeMemorySize(&mem);
            VitaOverlay_SetLiveStats((float)fps, mem.size_user / 1024, mem.size_cdram / 1024);
            const char *bound = (fps < 58.0) ? (cpu_ms > gpu_ms ? "[CPU-bound] game/2D update" : "[GPU-bound] vitaGL present")
                                             : "[CPU-bound] within 60fps budget";
            uint64_t t3d = 0, t2d_w = 0, t2d_u = 0, tsw = 0;
            VitaVideo_GetTimings(&t3d, &t2d_w, &t2d_u, &tsw);
            int32_t a_pk = 0; unsigned a_pl = 0, a_bgm = 0, a_vol = 0; bool a_act = false, a_pau = false;
            int a_mv = 0; uint32_t a_ur = 0, a_out = 0;
            VitaAudio_GetStats(&a_pk, &a_pl, &a_bgm, &a_act, &a_pau, &a_vol, &a_mv, &a_ur, &a_out);
            int rw = 0, rh = 0; bool fb = false;
            VitaVideo_GetActiveResInfo(&rw, &rh, &fb);
            VitaLog_Printf("%s fps=%.1f cpu=%.2fms gpu=%.2fms (3d=%.2f 2dw=%.2f 2du=%.2f swp=%.2f) res=%dx%d aud=(pk=%d p%u:%u a%d/z%d v%u/%d out=%u ur=%u) free_ram=%uKB free_cdram=%uKB",
                           bound, fps, cpu_ms, gpu_ms,
                           (double)t3d / 120000.0, (double)t2d_w / 120000.0,
                           (double)t2d_u / 120000.0, (double)tsw / 120000.0,
                           rw, rh,
                           (int)a_pk, a_pl, a_bgm, (int)a_act, (int)a_pau, a_vol, a_mv, a_out, a_ur,
                           mem.size_user / 1024, mem.size_cdram / 1024);
            stat_start_us = t2;
            cpu_accum_us = gpu_accum_us = 0;
        }
    }

    VitaSettings_Save();
    VitaIme_Shutdown();
    VitaBoot_Stop();
    VitaOverlay_Shutdown();
    VitaVideo_Shutdown();
    VitaLog_Shutdown();
    sceKernelExitProcess(0);
    return 0;
}
