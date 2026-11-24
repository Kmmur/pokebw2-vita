#include "ime_vita.h"
#include "log_vita.h"
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/sysmodule.h>
#include <string.h>

static bool ime_inited = false;
static bool ime_active = false;
static bool ime_triggered_session = false;
static bool ime_has_result = false;
static bool ime_confirmed = false;
static bool ime_canceled = false;

static SceWChar16 ime_title[64];
static SceWChar16 ime_initial[64];
static SceWChar16 ime_output[64];

void VitaIme_Init(void) {
    if (ime_inited) return;
    SceAppUtilInitParam init_param;
    SceAppUtilBootParam boot_param;
    memset(&init_param, 0, sizeof(init_param));
    memset(&boot_param, 0, sizeof(boot_param));
    sceAppUtilInit(&init_param, &boot_param);

    SceCommonDialogConfigParam cd_param;
    sceCommonDialogConfigParamInit(&cd_param);
    sceCommonDialogSetConfigParam(&cd_param);

    int mod = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    VitaLog_Printf("ime: initialized (sysmodule=0x%08x)", mod);
    ime_inited = true;
}

void VitaIme_Shutdown(void) {
    if (ime_active) {
        sceImeDialogAbort();
        sceImeDialogTerm();
        ime_active = false;
    }
}

void VitaIme_ResetSession(void) {
    ime_triggered_session = false;
    ime_has_result = false;
    ime_confirmed = false;
    ime_canceled = false;
}

int VitaIme_WasTriggeredThisSession(void) {
    return ime_triggered_session ? 1 : 0;
}

int VitaIme_IsActive(void) {
    return ime_active ? 1 : 0;
}

int VitaIme_Open(const char *title_ascii, const uint16_t *initial_utf16, uint32_t max_len) {
    if (!ime_inited) VitaIme_Init();
    if (ime_active) return 1;
    if (max_len == 0) max_len = 7;
    if (max_len > 32) max_len = 32;

    memset(ime_title, 0, sizeof(ime_title));
    memset(ime_initial, 0, sizeof(ime_initial));
    memset(ime_output, 0, sizeof(ime_output));

    if (title_ascii) {
        for (uint32_t i = 0; i < 63 && title_ascii[i]; i++)
            ime_title[i] = (SceWChar16)(unsigned char)title_ascii[i];
    }
    if (initial_utf16) {
        for (uint32_t i = 0; i < max_len && initial_utf16[i] && initial_utf16[i] != 0xFFFFu; i++)
            ime_initial[i] = (SceWChar16)initial_utf16[i];
    }

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0; // All system languages
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.option = SCE_IME_OPTION_NO_ASSISTANCE;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = ime_title;
    param.maxTextLength = max_len;
    param.initialText = ime_initial;
    param.inputTextBuffer = ime_output;

    int res = sceImeDialogInit(&param);
    if (res < 0) {
        VitaLog_Printf("ime: sceImeDialogInit failed 0x%08x", res);
        ime_triggered_session = true;
        return 0;
    }
    ime_active = true;
    ime_triggered_session = true;
    ime_has_result = false;
    ime_confirmed = false;
    ime_canceled = false;
    VitaLog_Printf("ime: opened dialog maxLen=%u", max_len);
    return 1;
}

void VitaIme_Update(void) {
    if (!ime_active) return;
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult res;
        memset(&res, 0, sizeof(res));
        sceImeDialogGetResult(&res);
        sceImeDialogTerm();
        ime_active = false;
        ime_has_result = true;
        ime_confirmed = (res.result == SCE_COMMON_DIALOG_RESULT_OK);
        ime_canceled = !ime_confirmed;
        VitaLog_Printf("ime: finished result=%d button=%d len=%u", res.result, res.button,
                       (unsigned)(ime_output[0] ? 1 : 0));
    }
}

int VitaIme_ConsumeResult(uint16_t *out_utf16, uint32_t max_chars, int *confirmed, int *canceled) {
    if (!ime_has_result) return 0;
    ime_has_result = false;
    if (confirmed) *confirmed = ime_confirmed ? 1 : 0;
    if (canceled) *canceled = ime_canceled ? 1 : 0;
    if (out_utf16 && max_chars > 0) {
        uint32_t i = 0;
        for (; i + 1 < max_chars && ime_output[i] != 0; i++)
            out_utf16[i] = (uint16_t)ime_output[i];
        out_utf16[i] = 0;
    }
    return 1;
}
