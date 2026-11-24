#include "settings_vita.h"
#include "log_vita.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_PATH     "ux0:data/pokebw2/vita_settings.cfg"
#define SETTINGS_TMP_PATH "ux0:data/pokebw2/vita_settings.cfg.tmp"
#define SAVE_PATH         "ux0:data/pokebw2/black2.sav"
#define SAVE_BAK_PATH     "ux0:data/pokebw2/black2.sav.bak"
#define SETTINGS_MAGIC    0x56325742u // "BW2V"
#define SETTINGS_VERSION  2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    VitaSettings data;
    uint32_t checksum;
} VitaSettingsDisk;

static VitaSettings current_settings;

static uint32_t CalcChecksum(const uint8_t *buf, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ buf[i]) * 16777619u;
    return h;
}

static void Sanitize(VitaSettings *s) {
    if (s->edition >= VITA_EDITION_COUNT) s->edition = VITA_EDITION_ORIGINAL;
    if (s->render_scale >= VITA_RES_COUNT) s->render_scale = VITA_RES_NATIVE;
    if (s->layout >= VITA_LAYOUT_COUNT) s->layout = VITA_LAYOUT_HYBRID;
    if (s->hybrid_pos >= VITA_HYBRID_POS_COUNT) s->hybrid_pos = VITA_HYBRID_BOTTOM_RIGHT;
    if (s->hybrid_size >= VITA_HYBRID_SIZE_COUNT) s->hybrid_size = VITA_HYBRID_COMPACT;
    // Always keep Single Screen in automatic smart routing (Select+R now cycles KH PiP corner)
    s->single_screen = VITA_SINGLE_AUTO;
    if (s->bilinear_filter > 1) s->bilinear_filter = 0;
    if (s->single_widescreen > 1) s->single_widescreen = 1;
    if (s->pip_corner >= VITA_PIP_COUNT) s->pip_corner = VITA_PIP_TOP_RIGHT;
    if (s->pip_size >= VITA_PIP_SIZE_COUNT) s->pip_size = VITA_PIP_SMALL;
    if (s->pip_opacity >= VITA_PIP_ALPHA_COUNT) s->pip_opacity = VITA_PIP_ALPHA_GLASS;

    if (s->battle_anim_speed > 2) s->battle_anim_speed = 1;
    if (s->text_speed > 2) s->text_speed = 1;
    if (s->qol_auto_run > 1) s->qol_auto_run = 1;
    if (s->qol_fast_menus > 1) s->qol_fast_menus = 1;
    if (s->qol_enhanced_touch > 1) s->qol_enhanced_touch = 1;

    if (s->overlay_combo >= VITA_COMBO_COUNT) s->overlay_combo = VITA_COMBO_LR_START;
    if (s->button_preset >= VITA_BTN_COUNT) s->button_preset = VITA_BTN_VITA;
    if (s->analog_mode > 1) s->analog_mode = 1;
    if (s->analog_deadzone > 2) s->analog_deadzone = 1;
    if (s->touch_mode > 2) s->touch_mode = 1;
    if (s->master_volume > 10) s->master_volume = 10;
}

void VitaSettings_Defaults(VitaSettings *s) {
    memset(s, 0, sizeof(*s));
    s->edition = VITA_EDITION_ORIGINAL;
    s->render_scale = VITA_RES_2X;
    s->layout = VITA_LAYOUT_HYBRID;
    s->hybrid_pos = VITA_HYBRID_BOTTOM_RIGHT;
    s->hybrid_size = VITA_HYBRID_COMPACT;
    s->single_screen = VITA_SINGLE_AUTO;
    s->bilinear_filter = 0;
    s->single_widescreen = 1;            // True 16:9 Widescreen in Single Screen
    s->pip_corner = VITA_PIP_TOP_RIGHT;  // KH 358/2 Days style Top-Right PiP
    s->pip_size = VITA_PIP_SMALL;        // 224x168 unobtrusive 4:3 PiP
    s->pip_opacity = VITA_PIP_ALPHA_GLASS;

    // Sensible defaults ready whenever player switches to Enhanced Edition
    s->battle_anim_speed = 1;  // 2x Fast
    s->text_speed = 1;         // Fast
    s->qol_auto_run = 1;       // On
    s->qol_fast_menus = 1;     // On
    s->qol_enhanced_touch = 1; // On

    s->overlay_combo = VITA_COMBO_LR_START;
    s->button_preset = VITA_BTN_VITA;
    s->analog_mode = 1;
    s->analog_deadzone = 1;    // 25%
    s->touch_mode = 1;         // Direct + Peek
    s->master_volume = 10;     // 100%
}

void VitaSettings_Load(void) {
    VitaSettings_Defaults(&current_settings);
    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        VitaLog_Printf("settings: no existing file, using defaults (layout=%u res=%u edition=%u)",
                       current_settings.layout, current_settings.render_scale, current_settings.edition);
        return;
    }
    typedef struct {
        uint32_t magic;
        uint32_t version;
        uint8_t  data[18];
        uint32_t checksum;
    } VitaSettingsDiskV1;
    uint8_t raw[64];
    memset(raw, 0, sizeof(raw));
    int rd = sceIoRead(fd, raw, sizeof(raw));
    sceIoClose(fd);
    if (rd == (int)sizeof(VitaSettingsDiskV1)) {
        VitaSettingsDiskV1 v1;
        memcpy(&v1, raw, sizeof(v1));
        uint32_t exp1 = CalcChecksum((const uint8_t *)&v1, sizeof(v1) - sizeof(v1.checksum));
        if (v1.magic == SETTINGS_MAGIC && v1.version == 1u && v1.checksum == exp1) {
            memcpy(&current_settings, v1.data, sizeof(v1.data));
            current_settings.single_widescreen = 1;
            current_settings.pip_corner = VITA_PIP_TOP_RIGHT;
            current_settings.pip_size = VITA_PIP_SMALL;
            current_settings.pip_opacity = VITA_PIP_ALPHA_GLASS;
            Sanitize(&current_settings);
            VitaSettings_Save();
            VitaLog_Printf("settings: migrated v1->v2 (edition=%u layout=%u res=%u wide=%u pip=%u)",
                           current_settings.edition, current_settings.layout, current_settings.render_scale,
                           current_settings.single_widescreen, current_settings.pip_corner);
            return;
        }
    }
    VitaSettingsDisk disk;
    memcpy(&disk, raw, sizeof(disk));
    if (rd != (int)sizeof(disk) || disk.magic != SETTINGS_MAGIC || disk.version != SETTINGS_VERSION) {
        VitaLog_Printf("settings: invalid header/size (rd=%d vs %zu), using defaults", rd, sizeof(disk));
        return;
    }
    uint32_t expected = CalcChecksum((const uint8_t *)&disk, sizeof(disk) - sizeof(disk.checksum));
    if (disk.checksum != expected) {
        VitaLog_Printf("settings: checksum mismatch, using defaults");
        return;
    }
    Sanitize(&disk.data);
    current_settings = disk.data;
    VitaLog_Printf("settings: loaded (edition=%u layout=%u res=%u combo=%u vol=%u)",
                   current_settings.edition, current_settings.layout, current_settings.render_scale,
                   current_settings.overlay_combo, current_settings.master_volume);
}

bool VitaSettings_Save(void) {
    sceIoMkdir("ux0:data/pokebw2", 0777);
    Sanitize(&current_settings);
    VitaSettingsDisk disk;
    memset(&disk, 0, sizeof(disk));
    disk.magic = SETTINGS_MAGIC;
    disk.version = SETTINGS_VERSION;
    disk.data = current_settings;
    disk.checksum = CalcChecksum((const uint8_t *)&disk, sizeof(disk) - sizeof(disk.checksum));

    SceUID fd = sceIoOpen(SETTINGS_TMP_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        VitaLog_Printf("settings: failed to open tmp file (0x%08x)", fd);
        return false;
    }
    int wr = sceIoWrite(fd, &disk, sizeof(disk));
    sceIoClose(fd);
    if (wr != (int)sizeof(disk)) {
        sceIoRemove(SETTINGS_TMP_PATH);
        return false;
    }
    sceIoRemove(SETTINGS_PATH);
    if (sceIoRename(SETTINGS_TMP_PATH, SETTINGS_PATH) < 0) {
        // Fallback direct write
        fd = sceIoOpen(SETTINGS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd < 0) return false;
        wr = sceIoWrite(fd, &disk, sizeof(disk));
        sceIoClose(fd);
        sceIoRemove(SETTINGS_TMP_PATH);
        return wr == (int)sizeof(disk);
    }
    VitaLog_Printf("settings: saved (edition=%u layout=%u res=%u)",
                   current_settings.edition, current_settings.layout, current_settings.render_scale);
    return true;
}

VitaSettings *VitaSettings_GetMutable(void) { return &current_settings; }
const VitaSettings *VitaSettings_Get(void) { return &current_settings; }

void VitaSettings_ResetDefaults(void) {
    VitaSettings_Defaults(&current_settings);
    VitaSettings_Save();
}

uint8_t VitaSettings_GetLayout(void) { return current_settings.layout; }

bool VitaSettings_IsEnhanced(void) {
    return current_settings.edition == VITA_EDITION_ENHANCED;
}
uint8_t VitaSettings_EffectiveBattleAnimSpeed(void) {
    return VitaSettings_IsEnhanced() ? current_settings.battle_anim_speed : 0;
}
uint8_t VitaSettings_EffectiveTextSpeed(void) {
    return VitaSettings_IsEnhanced() ? current_settings.text_speed : 0;
}
bool VitaSettings_EffectiveAutoRun(void) {
    return VitaSettings_IsEnhanced() && current_settings.qol_auto_run != 0;
}
bool VitaSettings_EffectiveFastMenus(void) {
    return VitaSettings_IsEnhanced() && current_settings.qol_fast_menus != 0;
}
bool VitaSettings_EffectiveEnhancedTouch(void) {
    return VitaSettings_IsEnhanced() && current_settings.qol_enhanced_touch != 0;
}

static bool CopyFileVita(const char *src, const char *dst) {
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    SceUID out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out < 0) { sceIoClose(in); return false; }
    static uint8_t buf[32768];
    bool ok = true;
    while (1) {
        int rd = sceIoRead(in, buf, sizeof(buf));
        if (rd < 0) { ok = false; break; }
        if (rd == 0) break;
        if (sceIoWrite(out, buf, rd) != rd) { ok = false; break; }
    }
    sceIoClose(in);
    sceIoClose(out);
    return ok;
}

bool VitaSettings_GetSaveInfo(bool *exists, uint32_t *size_bytes, bool *backup_exists) {
    SceIoStat st;
    if (sceIoGetstat(SAVE_PATH, &st) >= 0) {
        if (exists) *exists = true;
        if (size_bytes) *size_bytes = (uint32_t)st.st_size;
    } else {
        if (exists) *exists = false;
        if (size_bytes) *size_bytes = 0;
    }
    if (backup_exists)
        *backup_exists = (sceIoGetstat(SAVE_BAK_PATH, &st) >= 0);
    return true;
}

bool VitaSettings_BackupSave(void) {
    bool ok = CopyFileVita(SAVE_PATH, SAVE_BAK_PATH);
    VitaLog_Printf("settings: backup save -> %s", ok ? "OK" : "FAILED");
    return ok;
}

bool VitaSettings_RestoreSaveBackup(void) {
    bool ok = CopyFileVita(SAVE_BAK_PATH, SAVE_PATH);
    VitaLog_Printf("settings: restore save backup -> %s", ok ? "OK" : "FAILED");
    return ok;
}
