#ifndef POKEBW2_VITA_SETTINGS_H
#define POKEBW2_VITA_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VITA_EDITION_ORIGINAL = 0,
    VITA_EDITION_ENHANCED = 1,
    VITA_EDITION_COUNT = 2
} VitaEdition;

typedef enum {
    VITA_RES_NATIVE = 0, // 256x192 (1x)
    VITA_RES_2X     = 1, // 512x384 (2x)
    VITA_RES_4X     = 2, // 1024x768 (4x Experimental)
    VITA_RES_COUNT  = 3
} VitaRenderScale;

typedef enum {
    VITA_LAYOUT_SINGLE       = 0, // Primary DS screen maximized (4:3)
    VITA_LAYOUT_HYBRID       = 1, // Large primary + configurable secondary window
    VITA_LAYOUT_SIDE_BY_SIDE = 2, // Both DS screens side-by-side (4:3 each)
    VITA_LAYOUT_COUNT        = 3
} VitaScreenLayout;

typedef enum {
    VITA_HYBRID_BOTTOM_RIGHT = 0,
    VITA_HYBRID_TOP_RIGHT    = 1,
    VITA_HYBRID_CENTER_RIGHT = 2,
    VITA_HYBRID_POS_COUNT    = 3
} VitaHybridPos;

typedef enum {
    VITA_HYBRID_COMPACT = 0, // Primary 725x544, Secondary 235x176
    VITA_HYBRID_LARGE   = 1, // Primary 680x510, Secondary 280x210
    VITA_HYBRID_SIZE_COUNT = 2
} VitaHybridSize;

typedef enum {
    VITA_SINGLE_AUTO   = 0, // Follow DS power swap (Main screen)
    VITA_SINGLE_TOP    = 1, // Force Top DS screen
    VITA_SINGLE_BOTTOM = 2, // Force Bottom (Touch) DS screen
    VITA_SINGLE_COUNT  = 3
} VitaSingleScreen;

typedef enum {
    VITA_PIP_AUTO         = 0, // Show in Top-Right for Battles, X-Menu, or Touch-Peek
    VITA_PIP_TOP_RIGHT    = 1, // Always visible Top-Right (KH 358/2 Days style)
    VITA_PIP_BOTTOM_RIGHT = 2, // Always visible Bottom-Right
    VITA_PIP_TOP_LEFT     = 3, // Always visible Top-Left
    VITA_PIP_BOTTOM_LEFT  = 4, // Always visible Bottom-Left
    VITA_PIP_OFF          = 5, // Hidden (swap via Select+R)
    VITA_PIP_COUNT        = 6
} VitaPipCorner;

typedef enum {
    VITA_PIP_SMALL  = 0, // 224x168
    VITA_PIP_MEDIUM = 1, // 280x210
    VITA_PIP_LARGE  = 2, // 340x255
    VITA_PIP_SIZE_COUNT = 3
} VitaPipSize;

typedef enum {
    VITA_PIP_ALPHA_SOLID = 0, // 100%
    VITA_PIP_ALPHA_GLASS = 1, // 82%
    VITA_PIP_ALPHA_GHOST = 2, // 60%
    VITA_PIP_ALPHA_COUNT = 3
} VitaPipOpacity;

typedef enum {
    VITA_COMBO_LR_START     = 0, // L + R + Start
    VITA_COMBO_SELECT_START = 1, // Select + Start
    VITA_COMBO_L_SELECT     = 2, // L + Select
    VITA_COMBO_COUNT        = 3
} VitaOverlayCombo;

typedef enum {
    VITA_BTN_VITA     = 0, // Cross=A, Circle=B, Triangle=X, Square=Y
    VITA_BTN_NINTENDO = 1, // Circle=A, Cross=B, Triangle=X, Square=Y
    VITA_BTN_COUNT    = 2
} VitaButtonPreset;

typedef struct {
    // Independent Edition Selection
    uint8_t edition;            // VitaEdition

    // Display (independent of Edition)
    uint8_t render_scale;       // VitaRenderScale
    uint8_t layout;             // VitaScreenLayout
    uint8_t hybrid_pos;         // VitaHybridPos
    uint8_t hybrid_size;        // VitaHybridSize
    uint8_t single_screen;      // VitaSingleScreen
    uint8_t bilinear_filter;    // 0=Nearest (Sharp), 1=Linear (Smooth)

    // Gameplay QoL (only active in Enhanced Edition)
    uint8_t battle_anim_speed;  // 0=1x Normal, 1=2x Fast, 2=Skip/Off
    uint8_t text_speed;         // 0=Game Default, 1=Fast, 2=Instant
    uint8_t qol_auto_run;       // 0=Off, 1=On (hold B to walk)
    uint8_t qol_fast_menus;     // 0=Off, 1=On (faster menu repeat/nav)
    uint8_t qol_enhanced_touch; // 0=Off, 1=On (Single-screen touch peek + R-stick cursor)

    // Controls
    uint8_t overlay_combo;      // VitaOverlayCombo
    uint8_t button_preset;      // VitaButtonPreset
    uint8_t analog_mode;        // 0=Off, 1=D-Pad
    uint8_t analog_deadzone;    // 0=15%, 1=25%, 2=40%
    uint8_t touch_mode;         // 0=Direct, 1=Direct+Peek, 2=Disabled
    uint8_t master_volume;      // 0..10 (10 = 100%)

    // v2 Widescreen Single-Screen & KH-style 4-Corner PiP (appended for v1 migration)
    uint8_t single_widescreen;  // 0=4:3 Original, 1=16:9 True Widescreen (960x544)
    uint8_t pip_corner;         // VitaPipCorner (4-corner KH PiP overlay in Single Screen)
    uint8_t pip_size;           // VitaPipSize
    uint8_t pip_opacity;        // VitaPipOpacity
} VitaSettings;

void VitaSettings_Defaults(VitaSettings *s);
void VitaSettings_Load(void);
bool VitaSettings_Save(void);
VitaSettings *VitaSettings_GetMutable(void);
const VitaSettings *VitaSettings_Get(void);
void VitaSettings_ResetDefaults(void);

// Effective QoL helpers (always return 0/original behavior in Original Edition)
bool VitaSettings_IsEnhanced(void);
uint8_t VitaSettings_EffectiveBattleAnimSpeed(void);
uint8_t VitaSettings_EffectiveTextSpeed(void);
bool VitaSettings_EffectiveAutoRun(void);
bool VitaSettings_EffectiveFastMenus(void);
bool VitaSettings_EffectiveEnhancedTouch(void);

// Save management helpers (ux0:data/pokebw2/black2.sav)
bool VitaSettings_GetSaveInfo(bool *exists, uint32_t *size_bytes, bool *backup_exists);
bool VitaSettings_BackupSave(void);
bool VitaSettings_RestoreSaveBackup(void);

#endif
