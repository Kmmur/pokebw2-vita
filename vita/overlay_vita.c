#include "overlay_vita.h"
#include "settings_vita.h"
#include "gx_video_vita.h"
#include "log_vita.h"
#include <vitaGL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void VitaAudio_SetUserVolume(float vol);

enum {
    OV_W = 960,
    OV_H = 544,
    TAB_DISPLAY  = 0,
    TAB_GAMEPLAY = 1,
    TAB_CONTROLS = 2,
    TAB_SYSTEM   = 3,
    TAB_COUNT    = 4,
};

// Compact 8x8 ASCII bitmap font (printable 32..126), public-domain classic 8x8 bitmaps
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x36,0x36,0x24,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // '#'
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // '$'
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // '%'
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // '&'
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // apostrophe
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // '('
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ','
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // '.'
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // '/'
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // '0'
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // '1'
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // '2'
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // '3'
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // '4'
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // '5'
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // '6'
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // '7'
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // '8'
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // '9'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // ':'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ';'
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // '<'
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // '='
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // '>'
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // '?'
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // '@'
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 'A'
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 'B'
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 'C'
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 'D'
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 'E'
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 'F'
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 'G'
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 'H'
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'I'
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 'J'
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 'K'
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 'L'
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 'M'
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 'N'
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 'O'
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 'P'
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 'Q'
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 'R'
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 'S'
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'T'
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 'U'
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 'V'
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 'W'
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 'X'
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 'Y'
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 'Z'
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // '['
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ']'
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 'a'
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // 'b'
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 'c'
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, // 'd'
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // 'e'
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // 'f'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 'g'
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 'h'
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 'i'
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 'j'
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 'k'
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'l'
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 'm'
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 'n'
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 'o'
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 'p'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 'q'
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 'r'
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 's'
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 't'
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 'u'
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 'v'
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 'w'
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 'x'
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 'y'
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 'z'
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|'
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // '}'
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // DEL
};

static GLuint tex_font = 0;
static GLuint tex_solid = 0;
static bool overlay_open = false;
static int cur_tab = TAB_DISPLAY;
static int cur_row = 0;
static uint32_t prev_buttons = 0;
static bool prev_touch = false;
static uint32_t repeat_timer = 0;
static char status_toast[96] = "";
static int toast_frames = 0;
static float live_fps = 60.0f;
static uint32_t live_ram_kb = 0, live_cdram_kb = 0;
static bool confirm_restore = false;

static void ShowToast(const char *msg) {
    snprintf(status_toast, sizeof(status_toast), "%s", msg);
    toast_frames = 180;
}

void VitaOverlay_Init(void) {
    // Bake 16x6 grid of 8x8 glyphs into 128x64 RGBA texture
    static uint32_t atlas[128 * 64];
    memset(atlas, 0, sizeof(atlas));
    for (int ch = 0; ch < 96; ch++) {
        int gx = (ch % 16) * 8;
        int gy = (ch / 16) * 8;
        for (int y = 0; y < 8; y++) {
            uint8_t row = font8x8[ch][y];
            for (int x = 0; x < 8; x++) {
                if (row & (1u << x))
                    atlas[(gy + y) * 128 + (gx + x)] = 0xFFFFFFFFu;
            }
        }
    }
    glGenTextures(1, &tex_font);
    glBindTexture(GL_TEXTURE_2D, tex_font);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    uint32_t white = 0xFFFFFFFFu;
    glGenTextures(1, &tex_solid);
    glBindTexture(GL_TEXTURE_2D, tex_solid);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

void VitaOverlay_Shutdown(void) {
    if (tex_font) { glDeleteTextures(1, &tex_font); tex_font = 0; }
    if (tex_solid) { glDeleteTextures(1, &tex_solid); tex_solid = 0; }
}

bool VitaOverlay_IsOpen(void) { return overlay_open; }

void VitaOverlay_Open(void) {
    overlay_open = true;
    confirm_restore = false;
    prev_buttons = 0xFFFFFFFFu; // Require fresh press inside menu
    prev_touch = true;
    repeat_timer = 0;
}

void VitaOverlay_Close(void) {
    if (!overlay_open) return;
    overlay_open = false;
    confirm_restore = false;
    VitaSettings_Save();
}

void VitaOverlay_SetLiveStats(float fps, uint32_t free_ram_kb, uint32_t free_cdram_kb) {
    live_fps = fps;
    live_ram_kb = free_ram_kb;
    live_cdram_kb = free_cdram_kb;
}

bool VitaOverlay_CheckCombo(uint32_t buttons) {
    uint8_t combo = VitaSettings_Get()->overlay_combo;
    if (combo == VITA_COMBO_SELECT_START)
        return (buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) == (SCE_CTRL_SELECT | SCE_CTRL_START);
    if (combo == VITA_COMBO_L_SELECT)
        return (buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_SELECT)) == (SCE_CTRL_LTRIGGER | SCE_CTRL_SELECT);
    return (buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) ==
           (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START);
}

static int GetRowCount(int tab) {
    switch (tab) {
        case TAB_DISPLAY:  return 7;
        case TAB_GAMEPLAY: return 6;
        case TAB_CONTROLS: return 6;
        case TAB_SYSTEM:   return 5;
        default: return 0;
    }
}

static void ApplyAllLive(void) {
    const VitaSettings *s = VitaSettings_Get();
    VitaVideo_ApplySettings();
    VitaAudio_SetUserVolume((float)s->master_volume / 10.0f);
}

static void AdjustRow(int tab, int row, int dir) {
    VitaSettings *s = VitaSettings_GetMutable();
    confirm_restore = false;
    if (tab == TAB_DISPLAY) {
        switch (row) {
            case 0: s->render_scale = (uint8_t)((s->render_scale + VITA_RES_COUNT + dir) % VITA_RES_COUNT); break;
            case 1: s->layout = (uint8_t)((s->layout + VITA_LAYOUT_COUNT + dir) % VITA_LAYOUT_COUNT); break;
            case 2: s->single_widescreen ^= 1u; break;
            case 3:
                if (s->layout == VITA_LAYOUT_HYBRID)
                    s->hybrid_pos = (uint8_t)((s->hybrid_pos + VITA_HYBRID_POS_COUNT + dir) % VITA_HYBRID_POS_COUNT);
                else
                    s->pip_corner = (uint8_t)((s->pip_corner + VITA_PIP_COUNT + dir) % VITA_PIP_COUNT);
                break;
            case 4:
                if (s->layout == VITA_LAYOUT_HYBRID)
                    s->hybrid_size = (uint8_t)((s->hybrid_size + VITA_HYBRID_SIZE_COUNT + dir) % VITA_HYBRID_SIZE_COUNT);
                else
                    s->pip_size = (uint8_t)((s->pip_size + VITA_PIP_SIZE_COUNT + dir) % VITA_PIP_SIZE_COUNT);
                break;
            case 5: s->pip_opacity = (uint8_t)((s->pip_opacity + VITA_PIP_ALPHA_COUNT + dir) % VITA_PIP_ALPHA_COUNT); break;
            case 6: s->bilinear_filter ^= 1u; break;
        }
        ApplyAllLive();
        if (row == 0) {
            int rw = 0, rh = 0; bool fb = false;
            VitaVideo_GetActiveResInfo(&rw, &rh, &fb);
            if (fb) ShowToast("4x/2x unsupported or low CDRAM -> fallback active");
            else if (s->render_scale == VITA_RES_4X) ShowToast("4x Experimental active (1024x768 3D FBO)");
        }
    } else if (tab == TAB_GAMEPLAY) {
        switch (row) {
            case 0:
                s->edition = (uint8_t)((s->edition + VITA_EDITION_COUNT + dir) % VITA_EDITION_COUNT);
                ShowToast(s->edition == VITA_EDITION_ENHANCED ? "Switched to Enhanced Edition" : "Switched to Original Edition");
                break;
            case 1: s->battle_anim_speed = (uint8_t)((s->battle_anim_speed + 3 + dir) % 3); break;
            case 2: s->text_speed = (uint8_t)((s->text_speed + 3 + dir) % 3); break;
            case 3: s->qol_auto_run ^= 1u; break;
            case 4: s->qol_fast_menus ^= 1u; break;
            case 5: s->qol_enhanced_touch ^= 1u; break;
        }
    } else if (tab == TAB_CONTROLS) {
        switch (row) {
            case 0: s->overlay_combo = (uint8_t)((s->overlay_combo + VITA_COMBO_COUNT + dir) % VITA_COMBO_COUNT); break;
            case 1: s->button_preset = (uint8_t)((s->button_preset + VITA_BTN_COUNT + dir) % VITA_BTN_COUNT); break;
            case 2: s->analog_mode ^= 1u; break;
            case 3: s->analog_deadzone = (uint8_t)((s->analog_deadzone + 3 + dir) % 3); break;
            case 4: s->touch_mode = (uint8_t)((s->touch_mode + 3 + dir) % 3); break;
            case 5: s->master_volume = (uint8_t)((s->master_volume + 11 + dir) % 11); ApplyAllLive(); break;
        }
    } else if (tab == TAB_SYSTEM) {
        if (dir == 0) { // Activate action
            switch (row) {
                case 0: // Save settings now
                    ShowToast(VitaSettings_Save() ? "Settings saved to ux0:data/pokebw2/" : "Failed to save settings");
                    break;
                case 1: // Backup save
                    ShowToast(VitaSettings_BackupSave() ? "Save backed up to black2.sav.bak" : "No save found to back up");
                    break;
                case 2: // Restore backup (2-step confirm)
                    if (!confirm_restore) {
                        confirm_restore = true;
                        ShowToast("Press again to confirm Restore Save Backup");
                    } else {
                        confirm_restore = false;
                        ShowToast(VitaSettings_RestoreSaveBackup() ? "Backup restored (relaunch to reload save)" : "No backup found");
                    }
                    break;
                case 3: // Take Screenshot
                    VitaOverlay_Close();
                    VitaVideo_RequestScreenshot();
                    break;
                case 4: // Restore Defaults
                    VitaSettings_ResetDefaults();
                    ApplyAllLive();
                    ShowToast("Restored default settings");
                    break;
            }
        }
    }
}

bool VitaOverlay_Update(const SceCtrlData *pad, const SceTouchData *touch) {
    if (!overlay_open) return false;
    if (toast_frames > 0) toast_frames--;

    uint32_t b = pad->buttons;
    if (pad->ly < 64)  b |= SCE_CTRL_UP;
    if (pad->ly > 192) b |= SCE_CTRL_DOWN;
    if (pad->lx < 64)  b |= SCE_CTRL_LEFT;
    if (pad->lx > 192) b |= SCE_CTRL_RIGHT;

    uint32_t pressed = b & ~prev_buttons;
    if (b & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
        repeat_timer++;
        if (repeat_timer > 18 && (repeat_timer % 6) == 0)
            pressed |= b & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
    } else {
        repeat_timer = 0;
    }
    prev_buttons = b;

    if ((pressed & SCE_CTRL_CIRCLE) || (VitaOverlay_CheckCombo(b) && (pressed & (SCE_CTRL_START | SCE_CTRL_SELECT)))) {
        VitaOverlay_Close();
        return true;
    }
    if (pressed & SCE_CTRL_LTRIGGER) {
        cur_tab = (cur_tab + TAB_COUNT - 1) % TAB_COUNT;
        cur_row = 0;
        confirm_restore = false;
    }
    if (pressed & SCE_CTRL_RTRIGGER) {
        cur_tab = (cur_tab + 1) % TAB_COUNT;
        cur_row = 0;
        confirm_restore = false;
    }

    int rows = GetRowCount(cur_tab);
    if (pressed & SCE_CTRL_UP)    { cur_row = (cur_row + rows - 1) % rows; confirm_restore = false; }
    if (pressed & SCE_CTRL_DOWN)  { cur_row = (cur_row + 1) % rows; confirm_restore = false; }
    if (pressed & SCE_CTRL_LEFT)  AdjustRow(cur_tab, cur_row, -1);
    if (pressed & SCE_CTRL_RIGHT) AdjustRow(cur_tab, cur_row, +1);
    if (pressed & SCE_CTRL_CROSS) AdjustRow(cur_tab, cur_row, cur_tab == TAB_SYSTEM ? 0 : +1);

    // Front touchscreen navigation
    bool t_down = touch && touch->reportNum > 0;
    if (t_down && !prev_touch) {
        int sx = touch->report[0].x * OV_W / 1920;
        int sy = touch->report[0].y * OV_H / 1088;
        // Close pill button top-right (808..922, 34..74)
        if (sx >= 808 && sx <= 922 && sy >= 34 && sy <= 74) {
            VitaOverlay_Close();
        } else if (sx >= 36 && sx <= 220 && sy >= 104 && sy <= 104 + TAB_COUNT * 64) {
            int t = (sy - 104) / 64;
            if (t >= 0 && t < TAB_COUNT) { cur_tab = t; cur_row = 0; confirm_restore = false; }
        } else if (sx >= 244 && sx <= 924 && sy >= 100 && sy <= 100 + rows * 52) {
            int r = (sy - 100) / 52;
            if (r >= 0 && r < rows) {
                cur_row = r;
                int dir = (cur_tab == TAB_SYSTEM) ? 0 : (sx < 690 ? +1 : (sx < 790 ? -1 : +1));
                AdjustRow(cur_tab, cur_row, dir);
            }
        }
    }
    prev_touch = t_down;
    return true;
}

// ---- Lightweight Immediate-Mode UI Drawing ----
static void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a) {
    float x0 = x * 2.0f / OV_W - 1.0f;
    float x1 = (x + w) * 2.0f / OV_W - 1.0f;
    float y0 = 1.0f - y * 2.0f / OV_H;
    float y1 = 1.0f - (y + h) * 2.0f / OV_H;
    float pos[12] = { x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1 };
    float uvs[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    glBindTexture(GL_TEXTURE_2D, tex_solid);
    glColor4f(r, g, b, a);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

static void DrawText(float x, float y, float scale, const char *str, float r, float g, float b, float a) {
    if (!str || !*str) return;
    static float pos[256 * 12];
    static float uvs[256 * 12];
    int n = 0;
    float cx = x;
    float cw = 8.0f * scale;
    float ch = 8.0f * scale;
    for (const char *p = str; *p && n < 256; p++) {
        int c = (unsigned char)*p;
        if (c < 32 || c > 126) c = 32;
        c -= 32;
        if (c != 0) {
            float u0 = ((c % 16) * 8) / 128.0f;
            float u1 = ((c % 16) * 8 + 8) / 128.0f;
            float v0 = ((c / 16) * 8) / 64.0f;
            float v1 = ((c / 16) * 8 + 8) / 64.0f;
            float x0 = cx * 2.0f / OV_W - 1.0f;
            float x1 = (cx + cw) * 2.0f / OV_W - 1.0f;
            float y0 = 1.0f - y * 2.0f / OV_H;
            float y1 = 1.0f - (y + ch) * 2.0f / OV_H;
            float *P = &pos[n * 12];
            float *U = &uvs[n * 12];
            P[0]=x0; P[1]=y0; P[2]=x1; P[3]=y0; P[4]=x1; P[5]=y1;
            P[6]=x0; P[7]=y0; P[8]=x1; P[9]=y1; P[10]=x0; P[11]=y1;
            U[0]=u0; U[1]=v0; U[2]=u1; U[3]=v0; U[4]=u1; U[5]=v1;
            U[6]=u0; U[7]=v0; U[8]=u1; U[9]=v1; U[10]=u0; U[11]=v1;
            n++;
        }
        cx += cw;
    }
    if (n == 0) return;
    glBindTexture(GL_TEXTURE_2D, tex_font);
    glColor4f(r, g, b, a);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLES, 0, n * 6);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

static void FormatRow(int tab, int row, char *label, size_t lsz, char *val, size_t vsz, bool *dim) {
    const VitaSettings *s = VitaSettings_Get();
    *dim = false;
    val[0] = '\0';
    if (tab == TAB_DISPLAY) {
        static const char *res_names[] = { "Native (1x)", "2x Resolution", "Vita HD / 4x (Exp)" };
        static const char *lay_names[] = { "Single Screen", "Hybrid", "Side-by-Side" };
        static const char *hpos_names[] = { "Bottom-Right", "Top-Right", "Center-Right" };
        static const char *hsz_names[] = { "Compact (235x176)", "Large (280x210)" };
        static const char *pip_pos[] = { "Auto (Menu/Battle)", "Top-Right (KH)", "Bottom-Right", "Top-Left", "Bottom-Left", "Off" };
        static const char *pip_sz[]  = { "Small (224x168)", "Medium (280x210)", "Large (340x255)" };
        static const char *pip_op[]  = { "Solid (100%)", "Glass (82%)", "Ghost (60%)" };
        switch (row) {
            case 0: {
                int rw = 0, rh = 0; bool fb = false;
                VitaVideo_GetActiveResInfo(&rw, &rh, &fb);
                snprintf(label, lsz, "3D Internal Resolution");
                if (fb) snprintf(val, vsz, "%s [FB:%dx%d]", res_names[s->render_scale], rw, rh);
                else snprintf(val, vsz, "%s (%dx%d)", res_names[s->render_scale], rw, rh);
                break;
            }
            case 1: snprintf(label, lsz, "Screen Layout"); snprintf(val, vsz, "%s", lay_names[s->layout]); break;
            case 2: snprintf(label, lsz, "Single Screen Aspect"); snprintf(val, vsz, "%s", s->single_widescreen ? "16:9 Widescreen" : "4:3 Original"); *dim = (s->layout != VITA_LAYOUT_SINGLE); break;
            case 3:
                if (s->layout == VITA_LAYOUT_HYBRID) {
                    snprintf(label, lsz, "Hybrid Window Position"); snprintf(val, vsz, "%s", hpos_names[s->hybrid_pos]);
                } else {
                    snprintf(label, lsz, "Bottom Screen PiP Corner"); snprintf(val, vsz, "%s", pip_pos[s->pip_corner]);
                    *dim = (s->layout != VITA_LAYOUT_SINGLE);
                }
                break;
            case 4:
                if (s->layout == VITA_LAYOUT_HYBRID) {
                    snprintf(label, lsz, "Hybrid Window Size"); snprintf(val, vsz, "%s", hsz_names[s->hybrid_size]);
                } else {
                    snprintf(label, lsz, "Bottom Screen PiP Size"); snprintf(val, vsz, "%s", pip_sz[s->pip_size]);
                    *dim = (s->layout != VITA_LAYOUT_SINGLE || s->pip_corner == VITA_PIP_OFF);
                }
                break;
            case 5: snprintf(label, lsz, "Bottom Screen PiP Opacity"); snprintf(val, vsz, "%s", pip_op[s->pip_opacity]); *dim = (s->layout != VITA_LAYOUT_SINGLE || s->pip_corner == VITA_PIP_OFF); break;
            case 6: snprintf(label, lsz, "Display Scaling Filter"); snprintf(val, vsz, "%s", s->bilinear_filter ? "Smooth (Linear)" : "Sharp (Nearest)"); break;
        }
    } else if (tab == TAB_GAMEPLAY) {
        bool enh = VitaSettings_IsEnhanced();
        static const char *ed_names[] = { "Original Edition", "Enhanced Edition" };
        static const char *bspd[] = { "1x (Original)", "2x (Fast)", "Skip Animations" };
        static const char *tspd[] = { "Game Default", "Fast", "Instant" };
        switch (row) {
            case 0: snprintf(label, lsz, "Game Edition"); snprintf(val, vsz, "%s", ed_names[s->edition]); break;
            case 1: snprintf(label, lsz, "Battle Animation Speed"); snprintf(val, vsz, "%s", enh ? bspd[s->battle_anim_speed] : "1x (Original Lock)"); *dim = !enh; break;
            case 2: snprintf(label, lsz, "Text Speed Override"); snprintf(val, vsz, "%s", enh ? tspd[s->text_speed] : "Original Lock"); *dim = !enh; break;
            case 3: snprintf(label, lsz, "Auto-Run (Hold B Walk)"); snprintf(val, vsz, "%s", enh ? (s->qol_auto_run ? "ON" : "OFF") : "OFF (Original)"); *dim = !enh; break;
            case 4: snprintf(label, lsz, "Fast Menus & Inventory"); snprintf(val, vsz, "%s", enh ? (s->qol_fast_menus ? "ON" : "OFF") : "OFF (Original)"); *dim = !enh; break;
            case 5: snprintf(label, lsz, "Enhanced Touch + R-Stick"); snprintf(val, vsz, "%s", enh ? (s->qol_enhanced_touch ? "ON" : "OFF") : "OFF (Original)"); *dim = !enh; break;
        }
    } else if (tab == TAB_CONTROLS) {
        static const char *combos[] = { "L + R + Start", "Select + Start", "L + Select" };
        static const char *btns[] = { "Vita (X=A, O=B)", "Nintendo (O=A, X=B)" };
        static const char *dzs[] = { "Tight (15%)", "Normal (25%)", "Wide (40%)" };
        static const char *tmodes[] = { "Direct Touch", "Direct + Single Peek", "Disabled" };
        switch (row) {
            case 0: snprintf(label, lsz, "Overlay Menu Hotkey"); snprintf(val, vsz, "%s", combos[s->overlay_combo]); break;
            case 1: snprintf(label, lsz, "Face Button Layout"); snprintf(val, vsz, "%s", btns[s->button_preset]); break;
            case 2: snprintf(label, lsz, "Left Analog -> D-Pad"); snprintf(val, vsz, "%s", s->analog_mode ? "ON" : "OFF"); break;
            case 3: snprintf(label, lsz, "Analog Deadzone"); snprintf(val, vsz, "%s", dzs[s->analog_deadzone]); *dim = !s->analog_mode; break;
            case 4: snprintf(label, lsz, "Touchscreen Mode"); snprintf(val, vsz, "%s", tmodes[s->touch_mode]); break;
            case 5: snprintf(label, lsz, "Master Audio Volume"); snprintf(val, vsz, "%u%%", (unsigned)s->master_volume * 10u); break;
        }
    } else if (tab == TAB_SYSTEM) {
        bool ex = false, bak = false; uint32_t sz = 0;
        VitaSettings_GetSaveInfo(&ex, &sz, &bak);
        switch (row) {
            case 0: snprintf(label, lsz, "Save Settings Now"); snprintf(val, vsz, "vita_settings.cfg"); break;
            case 1: snprintf(label, lsz, "Backup Save File"); snprintf(val, vsz, ex ? "black2.sav (%uKB)" : "No save yet", sz / 1024u); break;
            case 2: snprintf(label, lsz, "Restore Save Backup"); snprintf(val, vsz, confirm_restore ? "[PRESS AGAIN TO CONFIRM]" : (bak ? "black2.sav.bak Ready" : "No backup")); break;
            case 3: snprintf(label, lsz, "Capture 960x544 Screenshot"); snprintf(val, vsz, "snap_960x544.bmp"); break;
            case 4: snprintf(label, lsz, "Restore Default Settings"); snprintf(val, vsz, "Reset All"); break;
        }
    }
}

void VitaOverlay_Draw(void) {
    if (!overlay_open || !tex_font || !tex_solid) return;

    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glViewport(0, 0, OV_W, OV_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Dim live game background
    DrawRect(0, 0, OV_W, OV_H, 0.03f, 0.05f, 0.10f, 0.78f);

    // Main glass panel
    DrawRect(24, 20, 912, 504, 0.07f, 0.10f, 0.17f, 0.94f);
    DrawRect(24, 20, 912, 3,   0.22f, 0.74f, 0.97f, 1.0f); // Vita-blue top accent

    // Header bar
    DrawRect(24, 23, 912, 62,  0.09f, 0.14f, 0.24f, 0.98f);
    DrawText(44, 42, 2.0f, "POKEMON BLACK 2", 0.96f, 0.98f, 1.0f, 1.0f);

    const VitaSettings *s = VitaSettings_Get();
    bool enh = VitaSettings_IsEnhanced();
    // Edition pill badge
    DrawRect(318, 36, 174, 34, enh ? 0.10f : 0.24f, enh ? 0.56f : 0.30f, enh ? 0.88f : 0.42f, 1.0f);
    DrawText(334, 46, 1.5f, enh ? "ENHANCED ED." : "ORIGINAL ED.", 1.0f, 1.0f, 1.0f, 1.0f);

    // Live hardware stats
    char stats[64];
    int rw = 0, rh = 0; bool fb = false;
    VitaVideo_GetActiveResInfo(&rw, &rh, &fb);
    snprintf(stats, sizeof(stats), "%.0fFPS  3D:%dx%d  CDRAM:%uM", live_fps, rw, rh, live_cdram_kb / 1024u);
    DrawText(508, 48, 1.25f, stats, 0.72f, 0.84f, 0.96f, 1.0f);

    // Close button (touch-friendly)
    DrawRect(808, 34, 114, 38, 0.78f, 0.20f, 0.26f, 0.92f);
    DrawText(822, 46, 1.5f, "O CLOSE", 1.0f, 1.0f, 1.0f, 1.0f);

    // Left sidebar tabs
    static const char *tab_names[TAB_COUNT] = { "Display", "Gameplay", "Controls", "System" };
    DrawRect(36, 98, 192, 376, 0.05f, 0.08f, 0.14f, 0.92f);
    for (int t = 0; t < TAB_COUNT; t++) {
        float ty = 104.0f + t * 64.0f;
        bool sel = (t == cur_tab);
        DrawRect(42, ty, 180, 54, sel ? 0.14f : 0.08f, sel ? 0.48f : 0.12f, sel ? 0.82f : 0.20f, sel ? 1.0f : 0.85f);
        if (sel) DrawRect(42, ty, 5, 54, 0.42f, 0.88f, 1.0f, 1.0f);
        DrawText(60, ty + 19, 1.75f, tab_names[t], sel ? 1.0f : 0.72f, sel ? 1.0f : 0.78f, sel ? 1.0f : 0.88f, 1.0f);
    }

    // Content area rows
    int rows = GetRowCount(cur_tab);
    for (int r = 0; r < rows; r++) {
        float ry = 100.0f + r * 52.0f;
        bool sel = (r == cur_row);
        char label[64], val[64];
        bool dim = false;
        FormatRow(cur_tab, r, label, sizeof(label), val, sizeof(val), &dim);

        DrawRect(244, ry, 680, 46, sel ? 0.13f : 0.09f, sel ? 0.26f : 0.13f, sel ? 0.44f : 0.21f, 0.94f);
        if (sel) DrawRect(244, ry, 5, 46, 0.28f, 0.82f, 1.0f, 1.0f);

        float lr = dim ? 0.50f : (sel ? 1.0f : 0.86f);
        float lg = dim ? 0.54f : (sel ? 1.0f : 0.90f);
        float lb = dim ? 0.62f : (sel ? 1.0f : 0.96f);
        DrawText(262, ry + 16, 1.45f, label, lr, lg, lb, 1.0f);

        float vr = dim ? 0.48f : (sel ? 0.45f : 0.72f);
        float vg = dim ? 0.52f : (sel ? 0.92f : 0.86f);
        float vb = dim ? 0.58f : (sel ? 1.00f : 0.98f);
        char vfmt[72];
        if (cur_tab == TAB_SYSTEM) snprintf(vfmt, sizeof(vfmt), "[ %s ]", val);
        else snprintf(vfmt, sizeof(vfmt), "< %s >", val);
        float vw = (float)strlen(vfmt) * 8.0f * 1.3f;
        DrawText(908.0f - vw, ry + 17, 1.3f, vfmt, vr, vg, vb, 1.0f);
    }

    // Footer bar + hints / toast
    DrawRect(24, 480, 912, 44, 0.05f, 0.08f, 0.14f, 0.98f);
    if (toast_frames > 0 && status_toast[0]) {
        DrawText(44, 495, 1.4f, status_toast, 0.42f, 0.96f, 0.72f, 1.0f);
    } else {
        DrawText(44, 495, 1.3f, "L/R: Tabs   D-Pad/Touch: Select   Left/Right/X: Change   O: Resume", 0.68f, 0.78f, 0.90f, 1.0f);
    }
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}
