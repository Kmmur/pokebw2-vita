#include "gx_video_vita.h"
#include "boot_vita.h"
#include "overlay_vita.h"
#include "ime_vita.h"
#include "log_vita.h"
#include "../pc/gx_backend.h"
#include "../pc/include/nitro/host_gx.h"
#include <vitaGL.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DS_W = 256,
    DS_H = 192,
    VITA_W = 960,
    VITA_H = 544,
    TEX_RING = 5,
    TEX_CACHE_SETS = 256,
    TEX_CACHE_WAYS = 4,
};

static GLuint tex_main[TEX_RING], tex_main_back[TEX_RING], tex_sub[TEX_RING];
static GLuint tex_white = 0, vbo_3d[TEX_RING];
static uint32_t vbo_3d_cap[TEX_RING];
static int last_3d_ring = -1;
static uint32_t last_3d_verts = 0, last_3d_frame = 0;
static bool single_peek_active = false;
static bool cursor_visible = false;
static uint32_t cursor_ds_x = 128, cursor_ds_y = 96;
// Offscreen 3D internal resolution FBO (Native 256x192, 2x 512x384, 4x Experimental 1024x768)
static GLuint fbo_3d = 0, fbo_color_tex = 0, fbo_depth_rb = 0;
static int fbo_w = 0, fbo_h = 0;
static bool fbo_fallback_active = false;
static uint8_t active_scale_cfg = 255;
static uint8_t active_filter_cfg = 255;
static bool active_fbo_wide = false;
static GLuint prog_3d = 0;
static GLint loc_tex_mode = -1, loc_uv_scale = -1, loc_fade = -1;

static uint32_t fb_main[DS_W * DS_H] __attribute__((aligned(16)));
static uint32_t fb_sub[DS_W * DS_H] __attribute__((aligned(16)));
static uint32_t *fb_main_back = NULL;
static uint32_t rgb15_lut[32768];

static volatile bool main_has_back = false;
static volatile bool main_back_solid = false;
static volatile uint32_t main_back_solid_rgba = 0xFF000000u;
static volatile bool main_has_front = false;
static volatile bool screenshot_requested = false;

static uint64_t prof_3d_us = 0, prof_2d_wait_us = 0, prof_2d_up_us = 0, prof_swap_us = 0;

// 2D compositor worker threads: Engine 0 on Core 1, Engine 1 on Core 3
static SceUID comp0_thread = -1, comp1_thread = -1;
static SceUID comp0_start = -1, comp0_done = -1;
static SceUID comp1_start = -1, comp1_done = -1;
static volatile bool comp_quit = false;

// ---- Fast Color & Blending Helpers ----
static inline uint32_t Rgb15ToRgba32(uint16_t c, uint8_t a) {
    if (a == 255) return rgb15_lut[c & 0x7FFFu];
    return (rgb15_lut[c & 0x7FFFu] & 0x00FFFFFFu) | ((uint32_t)a << 24);
}

static inline uint32_t ApplyColorFade(uint32_t p, uint32_t bmode, uint32_t amt) {
    uint32_t a = p >> 24;
    if (!a || amt == 0) return p;
    uint32_t r = p & 0xFFu, g = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
    if (bmode == 1) {
        r += ((255u - r) * amt) >> 4;
        g += ((255u - g) * amt) >> 4;
        b += ((255u - b) * amt) >> 4;
    } else if (bmode == 2) {
        r = (r * (16u - amt)) >> 4;
        g = (g * (16u - amt)) >> 4;
        b = (b * (16u - amt)) >> 4;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static inline uint32_t BlendAlphaPixel(uint32_t dst, uint32_t src, uint32_t eva, uint32_t evb) {
    if ((dst >> 24) == 0) {
        // Drawing translucent 2D layer/sprite onto transparent front buffer (over 3D):
        // emit alpha = eva/16 so GPU blends cleanly over the 3D framebuffer.
        uint32_t out_a = (eva * 255u) >> 4;
        if (out_a > 255u) out_a = 255u;
        return (src & 0x00FFFFFFu) | (out_a << 24);
    }
    uint32_t sr = src & 0xFFu, sg = (src >> 8) & 0xFFu, sb = (src >> 16) & 0xFFu;
    uint32_t dr = dst & 0xFFu, dg = (dst >> 8) & 0xFFu, db = (dst >> 16) & 0xFFu;
    uint32_t r = (sr * eva + dr * evb) >> 4; if (r > 255u) r = 255u;
    uint32_t g = (sg * eva + dg * evb) >> 4; if (g > 255u) g = 255u;
    uint32_t b = (sb * eva + db * evb) >> 4; if (b > 255u) b = 255u;
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static void FillU32(uint32_t *dst, uint32_t val, int count) {
    if (val == 0) {
        memset(dst, 0, (size_t)count * 4u);
        return;
    }
#ifdef __ARM_NEON
    uint32x4_t v = vdupq_n_u32(val);
    int i = 0;
    for (; i + 16 <= count; i += 16) {
        vst1q_u32(dst + i, v);
        vst1q_u32(dst + i + 4, v);
        vst1q_u32(dst + i + 8, v);
        vst1q_u32(dst + i + 12, v);
    }
    for (; i < count; i++) dst[i] = val;
#else
    for (int i = 0; i < count; i++) dst[i] = val;
#endif
}

static inline uint8_t BgByte(unsigned engine, uint32_t addr) {
    addr &= (engine == 0) ? 0x7FFFFu : 0x1FFFFu;
    return PcGxBackground[engine][addr];
}

static inline uint16_t BgHalf(unsigned engine, uint32_t addr) {
    addr &= (engine == 0) ? 0x7FFFEu : 0x1FFFEu;
    return (uint16_t)(PcGxBackground[engine][addr] | (PcGxBackground[engine][addr + 1] << 8));
}

static inline uint16_t PalHalf(const uint8_t *pal, uint32_t idx) {
    return (uint16_t)(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
}

// Compute effective (mul, add) fade for a plane from BLDCNT + MASTER_BRIGHT
static void ComputePlaneFadeFloat(unsigned engine, unsigned plane, float *out_mul, float *out_add) {
    float mul = 1.0f, add = 0.0f;
    uint32_t bldcnt = PcGxRegisters[engine].words[20] & 0xFFFFu;
    if (bldcnt & (1u << plane)) {
        uint32_t eff = (bldcnt >> 6) & 3u;
        uint32_t bldy = PcGxRegisters[engine].words[21] & 31u;
        if (bldy > 16u) bldy = 16u;
        if (bldy > 0) {
            float f = (float)bldy * (1.0f / 16.0f);
            if (eff == 2u) {
                mul *= (1.0f - f);
                add = add * (1.0f - f) + f;
            } else if (eff == 3u) {
                mul *= (1.0f - f);
                add *= (1.0f - f);
            }
        }
    }
    uint32_t bright = PcGxRegisters[engine].words[27] & 0xFFFFu;
    uint32_t bmode = (bright >> 14) & 3u;
    uint32_t amt = bright & 31u;
    if (amt > 16u) amt = 16u;
    if (amt > 0) {
        float a = (float)amt * (1.0f / 16.0f);
        if (bmode == 1u) {
            mul *= (1.0f - a);
            add = add * (1.0f - a) + a;
        } else if (bmode == 2u) {
            mul *= (1.0f - a);
            add *= (1.0f - a);
        }
    }
    *out_mul = mul;
    *out_add = add;
}

static inline void GetPlaneBlendParams(unsigned engine, unsigned plane,
                                       uint32_t *out_fade_mode, uint32_t *out_fade_amt,
                                       bool *out_alpha_blend, uint32_t *out_eva, uint32_t *out_evb) {
    uint32_t bldcnt = PcGxRegisters[engine].words[20] & 0xFFFFu;
    uint32_t eff = (bldcnt >> 6) & 3u;
    *out_fade_mode = 0;
    *out_fade_amt = 0;
    *out_alpha_blend = false;
    *out_eva = 16;
    *out_evb = 0;
    if (!(bldcnt & (1u << plane))) return;
    if (eff == 1u) {
        uint32_t bldalpha = PcGxRegisters[engine].words[20] >> 16;
        uint32_t eva = bldalpha & 31u; if (eva > 16u) eva = 16u;
        uint32_t evb = (bldalpha >> 8) & 31u; if (evb > 16u) evb = 16u;
        if (eva < 16u || evb > 0u) {
            *out_alpha_blend = true;
            *out_eva = eva;
            *out_evb = evb;
        }
    } else if (eff == 2u || eff == 3u) {
        uint32_t bldy = PcGxRegisters[engine].words[21] & 31u;
        if (bldy > 16u) bldy = 16u;
        if (bldy > 0) {
            *out_fade_mode = (eff == 2u) ? 1u : 2u;
            *out_fade_amt = bldy;
        }
    }
}

static void ComposeBG(unsigned engine, unsigned layer, uint32_t *fb) {
    uint32_t display = engine ? PcGxSubDisplay : PcGxMainDisplay;
    uint16_t control = PcGxRegisters[engine].halves[4 + layer];
    uint32_t mode = display & 7u;
    uint32_t size = (control >> 14) & 3u;
    bool affine = (layer == 2) ? (mode == 2 || mode == 4) : (layer == 3 && (mode == 1 || mode == 2));
    bool extended = (layer == 2) ? (mode == 5) : (layer == 3 && mode >= 3 && mode <= 5);
    bool bitmap = extended && (control & 128u);

    uint32_t charBase = ((control >> 2) & 15u) * 16384u;
    uint32_t screenBase = ((control >> 8) & 31u) * (bitmap ? 16384u : 2048u);
    if (engine == 0 && !bitmap) {
        charBase += ((display >> 24) & 7u) * 65536u;
        screenBase += ((display >> 27) & 7u) * 65536u;
    }
    bool extPal = !affine && !bitmap && (control & 128u) && (display & (1u << 30));

    uint32_t fmode, famt, eva, evb;
    bool ablend;
    GetPlaneBlendParams(engine, layer, &fmode, &famt, &ablend, &eva, &evb);

    const uint8_t *vram = PcGxBackground[engine];
    uint32_t vram_mask = (engine == 0) ? 0x7FFFFu : 0x1FFFFu;

    if (affine || extended) {
        uint32_t base = (layer == 2) ? 8u : 12u;
        uint32_t ab = PcGxRegisters[engine].words[base];
        uint32_t cd = PcGxRegisters[engine].words[base + 1];
        int32_t a = (int16_t)(ab & 0xFFFFu), b = (int16_t)(ab >> 16);
        int32_t c = (int16_t)(cd & 0xFFFFu), d = (int16_t)(cd >> 16);
        int32_t x0 = (int32_t)(PcGxRegisters[engine].words[base + 2] << 4) >> 4;
        int32_t y0 = (int32_t)(PcGxRegisters[engine].words[base + 3] << 4) >> 4;
        int w = bitmap ? (size == 0 ? 128 : size == 3 ? 512 : 256) : (int)(128u << size);
        int h = bitmap ? (size == 0 ? 128 : size >= 2 ? 512 : 256) : w;
        bool wrap = (control & 8192u) != 0;
        const uint16_t *bg_pal = (const uint16_t *)PcGx2D[engine].background_palette;
        uint32_t slot = (layer < 2 && (control & 8192u)) ? layer + 2u : layer;
        const uint16_t *ext_pal = (const uint16_t *)(PcGx2D[engine].background_extended_palette + slot * 8192u);

        for (int sy = 0; sy < DS_H; sy++) {
            int32_t rx = x0 + b * sy;
            int32_t ry = y0 + d * sy;
            uint32_t *row = fb + sy * DS_W;
            for (int sx = 0; sx < DS_W; sx++, rx += a, ry += c) {
                int px = rx >> 8, py = ry >> 8;
                if (!wrap) {
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                } else {
                    px &= (w - 1);
                    py &= (h - 1);
                }
                uint32_t rgba = 0;
                if (bitmap) {
                    uint32_t off = (uint32_t)(py * w + px);
                    if (control & 4u) {
                        uint16_t val = BgHalf(engine, screenBase + off * 2u);
                        if (!(val & 0x8000u)) continue;
                        rgba = rgb15_lut[val & 0x7FFFu];
                    } else {
                        uint8_t idx = vram[(screenBase + off) & vram_mask];
                        if (!idx) continue;
                        rgba = rgb15_lut[bg_pal[idx] & 0x7FFFu];
                    }
                } else if (affine) {
                    uint8_t tile = vram[(screenBase + (uint32_t)((py >> 3) * (w >> 3) + (px >> 3))) & vram_mask];
                    uint8_t idx = vram[(charBase + tile * 64u + (uint32_t)((py & 7) * 8 + (px & 7))) & vram_mask];
                    if (!idx) continue;
                    rgba = rgb15_lut[bg_pal[idx] & 0x7FFFu];
                } else {
                    uint32_t mapIdx = (uint32_t)((py >> 3) * (w >> 3) + (px >> 3));
                    uint16_t tile = BgHalf(engine, screenBase + mapIdx * 2u);
                    int tx = (tile & 1024u) ? (7 - (px & 7)) : (px & 7);
                    int ty = (tile & 2048u) ? (7 - (py & 7)) : (py & 7);
                    uint8_t idx = vram[(charBase + (tile & 1023u) * 64u + (uint32_t)(ty * 8 + tx)) & vram_mask];
                    if (!idx) continue;
                    uint16_t col = extPal ? ext_pal[(tile >> 12) * 256u + idx] : bg_pal[idx];
                    rgba = rgb15_lut[col & 0x7FFFu];
                }
                if (fmode) rgba = ApplyColorFade(rgba, fmode, famt);
                row[sx] = ablend ? BlendAlphaPixel(row[sx], rgba, eva, evb) : rgba;
            }
        }
        return;
    }

    // Text mode BG
    uint32_t scroll = PcGxRegisters[engine].words[4 + layer];
    int hofs = (int)(scroll & 511u), vofs = (int)((scroll >> 16) & 511u);
    int w = (size & 1u) ? 512 : 256;
    int h = (size & 2u) ? 512 : 256;
    bool eightBit = (control & 128u) != 0;
    uint32_t slot = (layer < 2 && (control & 8192u)) ? layer + 2u : layer;

    const uint16_t *pal16 = (const uint16_t *)(extPal ? (PcGx2D[engine].background_extended_palette + slot * 8192u)
                                                      : PcGx2D[engine].background_palette);
    bool simple_write = (!fmode && !ablend);

    for (int sy = 0; sy < DS_H; sy++) {
        int py = (sy + vofs) & (h - 1);
        int ty_blk = (py >> 8) * (w >> 8);
        int ty_tile = (py >> 3) & 31;
        int py_in = py & 7;
        uint32_t *row = fb + sy * DS_W;
        int sx = 0;
        while (sx < DS_W) {
            int px = (sx + hofs) & (w - 1);
            int tx_in = px & 7;
            int span = 8 - tx_in;
            if (sx + span > DS_W) span = DS_W - sx;
            uint32_t mapAddr = (screenBase + (uint32_t)((ty_blk + (px >> 8)) * 1024 + ty_tile * 32 + ((px >> 3) & 31)) * 2u) & vram_mask;
            uint16_t tile = *(const uint16_t *)(vram + mapAddr);
            bool hflip = (tile & 1024u) != 0;
            int ty = (tile & 2048u) ? (7 - py_in) : py_in;
            uint32_t pal_off = extPal ? ((tile >> 12) * 256u) : (eightBit ? 0u : ((tile >> 12) * 16u));
            const uint16_t *tpal = pal16 + pal_off;

            if (eightBit) {
                const uint8_t *trow = vram + ((charBase + (tile & 1023u) * 64u + (uint32_t)ty * 8u) & vram_mask);
                uint32_t w0 = *(const uint32_t *)trow;
                uint32_t w1 = *(const uint32_t *)(trow + 4);
                if (w0 | w1) {
                    if (simple_write && span == 8 && !hflip) {
                        if (w0) {
                            uint32_t i0 = w0 & 0xFFu; if (i0) row[sx + 0] = rgb15_lut[tpal[i0] & 0x7FFFu];
                            uint32_t i1 = (w0 >> 8) & 0xFFu; if (i1) row[sx + 1] = rgb15_lut[tpal[i1] & 0x7FFFu];
                            uint32_t i2 = (w0 >> 16) & 0xFFu; if (i2) row[sx + 2] = rgb15_lut[tpal[i2] & 0x7FFFu];
                            uint32_t i3 = w0 >> 24; if (i3) row[sx + 3] = rgb15_lut[tpal[i3] & 0x7FFFu];
                        }
                        if (w1) {
                            uint32_t i4 = w1 & 0xFFu; if (i4) row[sx + 4] = rgb15_lut[tpal[i4] & 0x7FFFu];
                            uint32_t i5 = (w1 >> 8) & 0xFFu; if (i5) row[sx + 5] = rgb15_lut[tpal[i5] & 0x7FFFu];
                            uint32_t i6 = (w1 >> 16) & 0xFFu; if (i6) row[sx + 6] = rgb15_lut[tpal[i6] & 0x7FFFu];
                            uint32_t i7 = w1 >> 24; if (i7) row[sx + 7] = rgb15_lut[tpal[i7] & 0x7FFFu];
                        }
                    } else {
                        for (int k = 0; k < span; k++) {
                            int tx = hflip ? (7 - (tx_in + k)) : (tx_in + k);
                            uint8_t idx = trow[tx];
                            if (!idx) continue;
                            uint32_t rgba = rgb15_lut[tpal[idx] & 0x7FFFu];
                            if (fmode) rgba = ApplyColorFade(rgba, fmode, famt);
                            row[sx + k] = ablend ? BlendAlphaPixel(row[sx + k], rgba, eva, evb) : rgba;
                        }
                    }
                }
            } else {
                const uint8_t *trow = vram + ((charBase + (tile & 1023u) * 32u + (uint32_t)ty * 4u) & vram_mask);
                uint32_t w0 = *(const uint32_t *)trow;
                if (w0) {
                    if (simple_write && span == 8 && !hflip) {
                        uint32_t idx;
                        if ((idx = w0 & 15u))         row[sx + 0] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 4) & 15u))  row[sx + 1] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 8) & 15u))  row[sx + 2] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 12) & 15u)) row[sx + 3] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 16) & 15u)) row[sx + 4] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 20) & 15u)) row[sx + 5] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 24) & 15u)) row[sx + 6] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = w0 >> 28))         row[sx + 7] = rgb15_lut[tpal[idx] & 0x7FFFu];
                    } else if (simple_write && span == 8 && hflip) {
                        uint32_t idx;
                        if ((idx = w0 >> 28))         row[sx + 0] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 24) & 15u)) row[sx + 1] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 20) & 15u)) row[sx + 2] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 16) & 15u)) row[sx + 3] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 12) & 15u)) row[sx + 4] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 8) & 15u))  row[sx + 5] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = (w0 >> 4) & 15u))  row[sx + 6] = rgb15_lut[tpal[idx] & 0x7FFFu];
                        if ((idx = w0 & 15u))         row[sx + 7] = rgb15_lut[tpal[idx] & 0x7FFFu];
                    } else {
                        for (int k = 0; k < span; k++) {
                            int tx = hflip ? (7 - (tx_in + k)) : (tx_in + k);
                            uint32_t idx = (w0 >> (tx * 4)) & 15u;
                            if (!idx) continue;
                            uint32_t rgba = rgb15_lut[tpal[idx] & 0x7FFFu];
                            if (fmode) rgba = ApplyColorFade(rgba, fmode, famt);
                            row[sx + k] = ablend ? BlendAlphaPixel(row[sx + k], rgba, eva, evb) : rgba;
                        }
                    }
                }
            }
            sx += span;
        }
    }
}

static const uint8_t oam_sizes[4][4][2] = {
    {{8,8}, {16,16}, {32,32}, {64,64}},
    {{16,8}, {32,8}, {32,16}, {64,32}},
    {{8,16}, {8,32}, {16,32}, {32,64}},
    {{8,8}, {8,8}, {8,8}, {8,8}},
};

static bool ComposeObjects(unsigned engine, int target_prio, uint32_t *fb) {
    uint32_t display = engine ? PcGxSubDisplay : PcGxMainDisplay;
    if (!(display & (1u << 12))) return false;
    const uint8_t *oam_mem = PcGx2D[engine].oam;
    const uint8_t *obj_vram = PcGx2D[engine].object;
    uint32_t obj_mask = (engine == 0) ? 0x3FFFFu : 0x1FFFFu;

    uint32_t fmode, famt, bld_eva, bld_evb;
    bool bld_alpha;
    GetPlaneBlendParams(engine, 4, &fmode, &famt, &bld_alpha, &bld_eva, &bld_evb);
    uint32_t reg_bldalpha = PcGxRegisters[engine].words[20] >> 16;
    uint32_t semi_eva = reg_bldalpha & 31u; if (semi_eva > 16u) semi_eva = 16u;
    uint32_t semi_evb = (reg_bldalpha >> 8) & 31u; if (semi_evb > 16u) semi_evb = 16u;

    bool drew_any = false;
    for (int i = 127; i >= 0; i--) {
        uint16_t a0 = (uint16_t)(oam_mem[i * 8 + 0] | (oam_mem[i * 8 + 1] << 8));
        uint16_t a1 = (uint16_t)(oam_mem[i * 8 + 2] | (oam_mem[i * 8 + 3] << 8));
        uint16_t a2 = (uint16_t)(oam_mem[i * 8 + 4] | (oam_mem[i * 8 + 5] << 8));
        int prio = (a2 >> 10) & 3;
        if (prio != target_prio) continue;
        int rsMode = (a0 >> 8) & 3;
        int shape = (a0 >> 14) & 3;
        int objMode = (a0 >> 10) & 3;
        if (rsMode == 2 || shape == 3 || objMode == 2) continue;

        int size = (a1 >> 14) & 3;
        int w = oam_sizes[shape][size][0];
        int h = oam_sizes[shape][size][1];
        int bw = w * (rsMode == 3 ? 2 : 1);
        int bh = h * (rsMode == 3 ? 2 : 1);
        int ox = a1 & 511; if (ox >= 256) ox -= 512;
        int oy = a0 & 255; if (oy >= 192) oy -= 256;
        if (ox + bw <= 0 || ox >= DS_W || oy + bh <= 0 || oy >= DS_H) continue;

        drew_any = true;
        bool affine = (rsMode & 1) != 0;
        int16_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            int mtx = ((a1 >> 9) & 31) * 32;
            pa = (int16_t)(oam_mem[mtx + 6] | (oam_mem[mtx + 7] << 8));
            pb = (int16_t)(oam_mem[mtx + 14] | (oam_mem[mtx + 15] << 8));
            pc = (int16_t)(oam_mem[mtx + 22] | (oam_mem[mtx + 23] << 8));
            pd = (int16_t)(oam_mem[mtx + 30] | (oam_mem[mtx + 31] << 8));
        }
        bool hflip = !affine && (a1 & (1 << 12));
        bool vflip = !affine && (a1 & (1 << 13));
        bool eightBit = (a0 & (1 << 13)) != 0;
        uint32_t name = a2 & 1023u;
        uint32_t bank = a2 >> 12;
        bool extPal = eightBit && (display & (1u << 31));
        bool do_blend = (objMode == 1) || bld_alpha;
        uint32_t eva = (objMode == 1) ? semi_eva : bld_eva;
        uint32_t evb = (objMode == 1) ? semi_evb : bld_evb;

        int y_start = oy < 0 ? 0 : oy;
        int y_end = oy + bh > DS_H ? DS_H : oy + bh;
        int x_start = ox < 0 ? 0 : ox;
        int x_end = ox + bw > DS_W ? DS_W : ox + bw;
        const uint16_t *pal16 = (const uint16_t *)(extPal ? (PcGx2D[engine].object_extended_palette + bank * 512u)
                                                          : (PcGx2D[engine].object_palette + (eightBit ? 0u : bank * 32u)));
        uint32_t tileBytes = eightBit ? 64u : 32u;
        uint32_t base_1d = name << (5u + ((display >> 20) & 3u));
        uint32_t base_2d = (eightBit ? (name & ~1u) : name) * 32u;
        bool map1d = (display & 16u) != 0;

        if (!affine && objMode != 3) {
            for (int sy = y_start; sy < y_end; sy++) {
                uint32_t *row = fb + sy * DS_W;
                int dy = sy - oy;
                int py = vflip ? (h - 1 - dy) : dy;
                uint32_t row_tile_base = map1d ? (base_1d + (uint32_t)((py >> 3) * (w >> 3)) * tileBytes)
                                               : (base_2d + (uint32_t)(py >> 3) * 1024u);
                uint32_t py_in = (uint32_t)(py & 7);
                if (eightBit) {
                    uint32_t row_off = row_tile_base + py_in * 8u;
                    for (int sx = x_start; sx < x_end; sx++) {
                        int dx = sx - ox;
                        int px = hflip ? (w - 1 - dx) : dx;
                        uint8_t idx = obj_vram[(row_off + (uint32_t)(px >> 3) * 64u + (uint32_t)(px & 7)) & obj_mask];
                        if (!idx) continue;
                        uint32_t rgba = rgb15_lut[pal16[idx] & 0x7FFFu];
                        if (fmode) rgba = ApplyColorFade(rgba, fmode, famt);
                        row[sx] = do_blend ? BlendAlphaPixel(row[sx], rgba, eva, evb) : rgba;
                    }
                } else {
                    uint32_t row_off = row_tile_base + py_in * 4u;
                    for (int sx = x_start; sx < x_end; sx++) {
                        int dx = sx - ox;
                        int px = hflip ? (w - 1 - dx) : dx;
                        uint8_t b = obj_vram[(row_off + (uint32_t)(px >> 3) * 32u + (uint32_t)((px & 7) >> 1)) & obj_mask];
                        uint8_t idx = (px & 1) ? (b >> 4) : (b & 15u);
                        if (!idx) continue;
                        uint32_t rgba = rgb15_lut[pal16[idx] & 0x7FFFu];
                        if (fmode) rgba = ApplyColorFade(rgba, fmode, famt);
                        row[sx] = do_blend ? BlendAlphaPixel(row[sx], rgba, eva, evb) : rgba;
                    }
                }
            }
            continue;
        }

        for (int sy = y_start; sy < y_end; sy++) {
            uint32_t *row = fb + sy * DS_W;
            int dy = sy - oy;
            for (int sx = x_start; sx < x_end; sx++) {
                int dx = sx - ox;
                int px, py;
                if (affine) {
                    int cx = dx - bw / 2, cy = dy - bh / 2;
                    px = ((pa * cx + pb * cy) >> 8) + w / 2;
                    py = ((pc * cx + pd * cy) >> 8) + h / 2;
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                } else {
                    px = hflip ? (w - 1 - dx) : dx;
                    py = vflip ? (h - 1 - dy) : dy;
                }
                if (objMode == 3) {
                    uint32_t addr;
                    if (display & 64u) {
                        addr = (name << (7u + ((display >> 22) & 1u))) + (uint32_t)(py * w + px) * 2u;
                    } else {
                        uint32_t stride = (display & 32u) ? 256u : 128u;
                        addr = ((name / (stride / 8u) * 8u + (uint32_t)py) * stride +
                                (name % (stride / 8u)) * 8u + (uint32_t)px) * 2u;
                    }
                    uint16_t val = (uint16_t)(obj_vram[addr & obj_mask] | (obj_vram[(addr + 1) & obj_mask] << 8));
                    if ((val & 0x8000u) && bank != 0) {
                        uint32_t rgba = rgb15_lut[val & 0x7FFFu];
                        if (fmode) rgba = ApplyColorFade(rgba, fmode, famt);
                        if (bank < 15u) {
                            uint32_t ba = bank + 1u;
                            row[sx] = BlendAlphaPixel(row[sx], rgba, ba, 16u - ba);
                        } else {
                            row[sx] = rgba;
                        }
                    }
                    continue;
                }
                uint32_t addr = map1d ? (base_1d + (uint32_t)((py >> 3) * (w >> 3) + (px >> 3)) * tileBytes)
                                      : (base_2d + (uint32_t)(py >> 3) * 1024u + (uint32_t)(px >> 3) * tileBytes);
                uint32_t pix = (uint32_t)((py & 7) * 8 + (px & 7));
                uint8_t idx;
                if (eightBit) {
                    idx = obj_vram[(addr + pix) & obj_mask];
                } else {
                    uint8_t b = obj_vram[(addr + (pix >> 1)) & obj_mask];
                    idx = (pix & 1u) ? (b >> 4) : (b & 15u);
                }
                if (!idx) continue;
                uint32_t rgba = rgb15_lut[pal16[idx] & 0x7FFFu];
                if (fmode) rgba = ApplyColorFade(rgba, fmode, famt);
                row[sx] = do_blend ? BlendAlphaPixel(row[sx], rgba, eva, evb) : rgba;
            }
        }
    }
    return drew_any;
}

static void ApplyMasterBrightness(unsigned engine, uint32_t *fb) {
    uint32_t bright = PcGxRegisters[engine].words[27] & 0xFFFFu;
    uint32_t bmode = (bright >> 14) & 3u;
    uint32_t amt = bright & 31u;
    if (amt > 16u) amt = 16u;
    if ((bmode != 1 && bmode != 2) || amt == 0) return;
    if (bmode == 2 && amt == 16u) {
        // Full fade-to-black: preserve alpha, zero RGB
        for (int i = 0; i < DS_W * DS_H; i++)
            fb[i] &= 0xFF000000u;
        return;
    }
    if (bmode == 1 && amt == 16u) {
        // Full fade-to-white: preserve alpha, set RGB to 0xFFFFFF where alpha != 0
        for (int i = 0; i < DS_W * DS_H; i++)
            if (fb[i] >> 24) fb[i] |= 0x00FFFFFFu;
        return;
    }
    for (int i = 0; i < DS_W * DS_H; i++) {
        uint32_t p = fb[i];
        uint32_t a = p >> 24;
        if (!a) continue;
        uint32_t r = p & 0xFFu, g = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
        if (bmode == 1) {
            r += ((255u - r) * amt) >> 4;
            g += ((255u - g) * amt) >> 4;
            b += ((255u - b) * amt) >> 4;
        } else {
            r = (r * (16u - amt)) >> 4;
            g = (g * (16u - amt)) >> 4;
            b = (b * (16u - amt)) >> 4;
        }
        fb[i] = r | (g << 8) | (b << 16) | (a << 24);
    }
}

static void ComposeEngine2D(unsigned engine, uint32_t *fb) {
    uint32_t display = engine ? PcGxSubDisplay : PcGxMainDisplay;
    uint32_t dispMode = (display >> 16) & 3u;
    if (engine == 0) {
        main_has_back = false;
        main_back_solid = false;
        main_has_front = true;
    }
    if (dispMode == 0) {
        FillU32(fb, 0xFFFFFFFFu, DS_W * DS_H);
        return;
    }
    if (dispMode == 2 && engine == 0) {
        uint32_t bank = (display >> 18) & 3u;
        if ((PcGxCapturedBanks & (1u << bank)) || !(display & 8u)) {
            const uint8_t *src = PcGxLcdc + bank * 0x20000u;
            for (int i = 0; i < DS_W * DS_H; i++) {
                uint16_t c = (uint16_t)(src[i * 2] | (src[i * 2 + 1] << 8));
                fb[i] = rgb15_lut[c & 0x7FFFu];
            }
            return;
        }
    }

    uint32_t bd_fmode, bd_famt, bd_eva, bd_evb;
    bool bd_ablend;
    GetPlaneBlendParams(engine, 5, &bd_fmode, &bd_famt, &bd_ablend, &bd_eva, &bd_evb);
    uint32_t bg0_col = rgb15_lut[PalHalf(PcGx2D[engine].background_palette, 0) & 0x7FFFu];
    if (bd_fmode) bg0_col = ApplyColorFade(bg0_col, bd_fmode, bd_famt);

    bool bg0_3d = (engine == 0) && (display & 8u) && (display & (1u << 8));
    if (bg0_3d) {
        // On FIELDMAP, background_palette[0] is a purple chroma-key color; pc/boot.c omits backdrop bit 4.
        if (VitaBoot_IsFieldScene()) {
            bg0_col = 0xFF000000u;
            if (bd_fmode) bg0_col = ApplyColorFade(bg0_col, bd_fmode, bd_famt);
        }
        int bg0_prio = (int)(PcGxRegisters[0].halves[4] & 3u);
        bool clear_img = (PcGxDisplay3D & 0x4000u) != 0;

        // Check whether any BG/OBJ layer sits behind BG0 3D
        bool any_back_layer = clear_img;
        if (!any_back_layer) {
            for (int layer = 1; layer <= 3; layer++) {
                if (!(display & (1u << (8 + layer)))) continue;
                if ((int)(PcGxRegisters[0].halves[4 + layer] & 3u) >= bg0_prio) {
                    any_back_layer = true;
                    break;
                }
            }
            if (!any_back_layer && (display & (1u << 12))) {
                // Check if any visible OAM sits strictly behind BG0
                const uint8_t *oam_mem = PcGx2D[0].oam;
                for (int i = 0; i < 128; i++) {
                    uint16_t a0 = (uint16_t)(oam_mem[i * 8] | (oam_mem[i * 8 + 1] << 8));
                    if (((a0 >> 8) & 3) == 2) continue;
                    uint16_t a2 = (uint16_t)(oam_mem[i * 8 + 4] | (oam_mem[i * 8 + 5] << 8));
                    if (((a2 >> 10) & 3) > bg0_prio) {
                        any_back_layer = true;
                        break;
                    }
                }
            }
        }

        // Compute 3D rear clear color
        uint32_t clr_reg = PC_GX_RASTER32(0x04000350);
        uint32_t clr_a = (clr_reg >> 16) & 31u;
        uint32_t clr_rgb = rgb15_lut[clr_reg & 0x7FFFu];
        uint32_t f3_mode, f3_amt, f3_eva, f3_evb;
        bool f3_ab;
        GetPlaneBlendParams(0, 0, &f3_mode, &f3_amt, &f3_ab, &f3_eva, &f3_evb);
        if (f3_mode) clr_rgb = ApplyColorFade(clr_rgb, f3_mode, f3_amt);

        if (!any_back_layer) {
            // Fast path: solid backdrop + 3D clear color, NO fb_main_back fill or upload!
            uint32_t solid = bg0_col;
            if (clr_a >= 31u) {
                solid = clr_rgb;
            } else if (clr_a > 0u) {
                uint32_t sr = clr_rgb & 0xFFu, sg = (clr_rgb >> 8) & 0xFFu, sb = (clr_rgb >> 16) & 0xFFu;
                uint32_t dr = solid & 0xFFu, dg = (solid >> 8) & 0xFFu, db = (solid >> 16) & 0xFFu;
                uint32_t r = (sr * clr_a + dr * (31u - clr_a)) / 31u;
                uint32_t g = (sg * clr_a + dg * (31u - clr_a)) / 31u;
                uint32_t b = (sb * clr_a + db * (31u - clr_a)) / 31u;
                solid = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
            uint32_t bright = PcGxRegisters[0].words[27] & 0xFFFFu;
            uint32_t bmode = (bright >> 14) & 3u;
            uint32_t amt = bright & 31u; if (amt > 16u) amt = 16u;
            if ((bmode == 1 || bmode == 2) && amt > 0)
                solid = ApplyColorFade(solid, bmode, amt);
            main_back_solid = true;
            main_back_solid_rgba = solid;
            main_has_back = false;
        } else {
            FillU32(fb_main_back, bg0_col, DS_W * DS_H);
            for (int prio = 3; prio >= bg0_prio; prio--) {
                for (int layer = 3; layer >= 1; layer--) {
                    if (!(display & (1u << (8 + layer)))) continue;
                    if ((int)(PcGxRegisters[0].halves[4 + layer] & 3u) != prio) continue;
                    ComposeBG(0, (unsigned)layer, fb_main_back);
                }
                if (prio > bg0_prio && (display & (1u << 12)))
                    ComposeObjects(0, prio, fb_main_back);
            }
            if (clear_img) {
                uint32_t off = PC_GX_RASTER32(0x04000356);
                uint32_t ox = off & 255u, oy = (off >> 8) & 255u;
                for (int y = 0; y < DS_H; y++) {
                    uint32_t *row = fb_main_back + y * DS_W;
                    uint32_t sy = (y + oy) & 255u;
                    for (int x = 0; x < DS_W; x++) {
                        uint16_t c = PcGx.clear_color[sy * 256u + ((x + ox) & 255u)];
                        if (c & 0x8000u) row[x] = rgb15_lut[c & 0x7FFFu];
                    }
                }
            } else if (clr_a > 0u) {
                if (clr_a >= 31u) {
                    FillU32(fb_main_back, clr_rgb, DS_W * DS_H);
                } else {
                    uint32_t sr = clr_rgb & 0xFFu, sg = (clr_rgb >> 8) & 0xFFu, sb = (clr_rgb >> 16) & 0xFFu;
                    uint32_t inv = 31u - clr_a;
                    for (int i = 0; i < DS_W * DS_H; i++) {
                        uint32_t d = fb_main_back[i];
                        uint32_t r = (sr * clr_a + (d & 0xFFu) * inv) / 31u;
                        uint32_t g = (sg * clr_a + ((d >> 8) & 0xFFu) * inv) / 31u;
                        uint32_t b = (sb * clr_a + ((d >> 16) & 0xFFu) * inv) / 31u;
                        fb_main_back[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
                    }
                }
            }
            ApplyMasterBrightness(0, fb_main_back);
            main_back_solid = false;
            main_has_back = true;
        }

        // Front layers (in front of BG0 3D)
        bool has_front_candidate = false;
        for (int layer = 1; layer <= 3; layer++) {
            if ((display & (1u << (8 + layer))) && (int)(PcGxRegisters[0].halves[4 + layer] & 3u) < bg0_prio) {
                has_front_candidate = true;
                break;
            }
        }
        if (!has_front_candidate && (display & (1u << 12)))
            has_front_candidate = true;

        if (!has_front_candidate) {
            main_has_front = false;
            return;
        }

        memset(fb, 0, DS_W * DS_H * 4);
        bool drew_front = false;
        for (int prio = bg0_prio; prio >= 0; prio--) {
            if (prio < bg0_prio) {
                for (int layer = 3; layer >= 1; layer--) {
                    if (!(display & (1u << (8 + layer)))) continue;
                    if ((int)(PcGxRegisters[0].halves[4 + layer] & 3u) != prio) continue;
                    ComposeBG(0, (unsigned)layer, fb);
                    drew_front = true;
                }
            }
            if (display & (1u << 12)) {
                if (ComposeObjects(0, prio, fb))
                    drew_front = true;
            }
        }
        if (drew_front) ApplyMasterBrightness(0, fb);
        main_has_front = drew_front;
        return;
    }

    // Pure 2D screen (Main without BG0 3D, or Sub)
    FillU32(fb, bg0_col, DS_W * DS_H);
    for (int prio = 3; prio >= 0; prio--) {
        for (int layer = 3; layer >= 0; layer--) {
            if (!(display & (1u << (8 + layer)))) continue;
            if (engine == 0 && layer == 0 && (display & 8u)) continue;
            if ((PcGxRegisters[engine].halves[4 + layer] & 3u) != (uint32_t)prio) continue;
            ComposeBG(engine, (unsigned)layer, fb);
        }
        ComposeObjects(engine, prio, fb);
    }
    ApplyMasterBrightness(engine, fb);
}

static int Compositor0Thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (!comp_quit) {
        sceKernelWaitSema(comp0_start, 1, NULL);
        if (comp_quit) break;
        ComposeEngine2D(0, fb_main);
        sceKernelSignalSema(comp0_done, 1);
    }
    return 0;
}

static int Compositor1Thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (!comp_quit) {
        sceKernelWaitSema(comp1_start, 1, NULL);
        if (comp_quit) break;
        ComposeEngine2D(1, fb_sub);
        sceKernelSignalSema(comp1_done, 1);
    }
    return 0;
}

// ---- O(1) 4-Way Set-Associative DS 3D Texture Cache ----
typedef struct {
    uint32_t texparam, palette, hash;
    uint32_t tex_gen, pltt_gen;
    GLuint gl_tex;
    uint32_t last_used;
    int w, h;
} TexCacheEntry;

static TexCacheEntry tex_cache[TEX_CACHE_SETS][TEX_CACHE_WAYS];
static uint32_t tex_frame = 1;
static uint32_t tex_decode_buf[1024 * 1024]; // max DS texture 1024x1024

static inline uint32_t HashBytesFast(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    uint32_t step = (n > 64) ? (n >> 4) : 1;
    if (step == 0) step = 1;
    for (uint32_t i = 0; i < n; i += step)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static inline uint16_t TexPltt16(uint32_t addr) {
    if (addr >= PC_GX_PLTT_BYTES) {
        static bool logged_oob = false;
        if (!logged_oob) {
            logged_oob = true;
            VitaLog_Printf("TexPltt16 OOB addr=0x%x", addr);
        }
        return 0x0000u;
    }
    return (uint16_t)(PcGx.palettes[addr] | (PcGx.palettes[addr + 1] << 8));
}

static GLuint GetDs3DTexture(uint32_t texparam, uint32_t palette, int *out_w, int *out_h) {
    uint32_t fmt = (texparam >> 26) & 7u;
    if (fmt == 0) return 0;
    int w = 8 << ((texparam >> 20) & 7u);
    int h = 8 << ((texparam >> 23) & 7u);
    *out_w = w;
    *out_h = h;

    uint32_t set_idx = ((texparam * 2654435761u) ^ (palette * 2246822519u)) & (TEX_CACHE_SETS - 1);
    TexCacheEntry *ways = tex_cache[set_idx];
    uint32_t cur_tgen = PcGxTexGen;
    uint32_t cur_pgen = PcGxPlttGen;

    // 1. Instant generation-counter hit check (ZERO hashing!)
    int lru = 0;
    int match = -1;
    for (int i = 0; i < TEX_CACHE_WAYS; i++) {
        if (ways[i].gl_tex && ways[i].texparam == texparam && ways[i].palette == palette) {
            if (ways[i].tex_gen == cur_tgen && (fmt == 7 || ways[i].pltt_gen == cur_pgen)) {
                ways[i].last_used = tex_frame;
                return ways[i].gl_tex;
            }
            match = i;
            break;
        }
        if (ways[i].last_used < ways[lru].last_used)
            lru = i;
    }

    uint32_t tex_addr = (texparam & 0xFFFFu) << 3;
    uint32_t pltt_addr = (palette & 0x1FFFu) << (fmt == 2 ? 3 : 4);
    uint32_t tex_bytes = (uint32_t)(w * h);
    if (fmt == 2) tex_bytes >>= 2;
    else if (fmt == 3 || fmt == 5) tex_bytes >>= 1;
    else if (fmt == 7) tex_bytes <<= 1;

    uint32_t hsh = HashBytesFast(&PcGx.textures[tex_addr & (PC_GX_TEX_BYTES - 1)],
                                 tex_bytes < 4096 ? tex_bytes : 4096);
    if (fmt == 5) {
        uint32_t b0 = tex_addr & (PC_GX_TEX_BYTES - 1);
        uint32_t slot0 = 0x20000u + ((b0 & 0x1FFFFu) >> 1) + ((b0 & 0x40000u) >> 2);
        hsh ^= HashBytesFast(&PcGx.textures[slot0 & (PC_GX_TEX_BYTES - 1)],
                             (tex_bytes >> 1) < 2048 ? (tex_bytes >> 1) : 2048);
    }
    if (fmt != 7 && pltt_addr < PC_GX_PLTT_BYTES)
        hsh ^= HashBytesFast(&PcGx.palettes[pltt_addr], 64);

    if (match >= 0 && ways[match].hash == hsh) {
        ways[match].tex_gen = cur_tgen;
        ways[match].pltt_gen = cur_pgen;
        ways[match].last_used = tex_frame;
        return ways[match].gl_tex;
    }

    TexCacheEntry *e = (match >= 0) ? &ways[match] : &ways[lru];
    if (e->gl_tex && (tex_frame - e->last_used) < TEX_RING) {
        glDeleteTextures(1, &e->gl_tex);
        e->gl_tex = 0;
    }
    if (!e->gl_tex) glGenTextures(1, &e->gl_tex);
    e->texparam = texparam;
    e->palette = palette;
    e->hash = hsh;
    e->tex_gen = cur_tgen;
    e->pltt_gen = cur_pgen;
    e->last_used = tex_frame;
    e->w = w;
    e->h = h;

    bool col0_trans = (texparam & (1u << 29)) != 0;
    int total = w * h;

    if (fmt == 5) {
        // 4x4 texel compressed: decode block-by-block with EXACT DS Slot 1 address (0x20000)
        int bw = w >> 2, bh = h >> 2;
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                uint32_t blk_idx = (uint32_t)(by * bw + bx);
                uint32_t b_addr = (tex_addr + blk_idx * 4u) & (PC_GX_TEX_BYTES - 1);
                uint32_t bits = (uint32_t)PcGx.textures[b_addr] |
                                ((uint32_t)PcGx.textures[(b_addr + 1) & (PC_GX_TEX_BYTES - 1)] << 8) |
                                ((uint32_t)PcGx.textures[(b_addr + 2) & (PC_GX_TEX_BYTES - 1)] << 16) |
                                ((uint32_t)PcGx.textures[(b_addr + 3) & (PC_GX_TEX_BYTES - 1)] << 24);
                uint32_t slot = 0x20000u + ((b_addr & 0x1FFFFu) >> 1) + ((b_addr & 0x40000u) >> 2);
                uint16_t pinfo = (uint16_t)(PcGx.textures[slot & (PC_GX_TEX_BYTES - 1)] |
                                            (PcGx.textures[(slot + 1) & (PC_GX_TEX_BYTES - 1)] << 8));
                uint32_t p_off = pltt_addr + (pinfo & 0x3FFFu) * 4u;
                uint32_t pmode = (pinfo >> 14) & 3u;

                uint16_t c0 = TexPltt16(p_off);
                uint16_t c1 = TexPltt16(p_off + 2);
                uint32_t pal4[4];
                pal4[0] = rgb15_lut[c0 & 0x7FFFu];
                pal4[1] = rgb15_lut[c1 & 0x7FFFu];
                if (pmode == 0) {
                    pal4[2] = rgb15_lut[TexPltt16(p_off + 4) & 0x7FFFu];
                    pal4[3] = 0u;
                } else if (pmode == 1) {
                    uint32_t r = (((c0 & 31u) + (c1 & 31u)) >> 1) * 255u / 31u;
                    uint32_t g = ((((c0 >> 5) & 31u) + ((c1 >> 5) & 31u)) >> 1) * 255u / 31u;
                    uint32_t bl = ((((c0 >> 10) & 31u) + ((c1 >> 10) & 31u)) >> 1) * 255u / 31u;
                    pal4[2] = r | (g << 8) | (bl << 16) | 0xFF000000u;
                    pal4[3] = 0u;
                } else if (pmode == 2) {
                    pal4[2] = rgb15_lut[TexPltt16(p_off + 4) & 0x7FFFu];
                    pal4[3] = rgb15_lut[TexPltt16(p_off + 6) & 0x7FFFu];
                } else {
                    uint32_t r2 = (((c0 & 31u) * 5u + (c1 & 31u) * 3u) >> 3) * 255u / 31u;
                    uint32_t g2 = ((((c0 >> 5) & 31u) * 5u + ((c1 >> 5) & 31u) * 3u) >> 3) * 255u / 31u;
                    uint32_t b2 = ((((c0 >> 10) & 31u) * 5u + ((c1 >> 10) & 31u) * 3u) >> 3) * 255u / 31u;
                    pal4[2] = r2 | (g2 << 8) | (b2 << 16) | 0xFF000000u;
                    uint32_t r3 = (((c0 & 31u) * 3u + (c1 & 31u) * 5u) >> 3) * 255u / 31u;
                    uint32_t g3 = ((((c0 >> 5) & 31u) * 3u + ((c1 >> 5) & 31u) * 5u) >> 3) * 255u / 31u;
                    uint32_t b3 = ((((c0 >> 10) & 31u) * 3u + ((c1 >> 10) & 31u) * 5u) >> 3) * 255u / 31u;
                    pal4[3] = r3 | (g3 << 8) | (b3 << 16) | 0xFF000000u;
                }
                for (int py = 0; py < 4; py++) {
                    uint32_t *dst_row = tex_decode_buf + (by * 4 + py) * w + bx * 4;
                    uint32_t rbits = bits >> (py * 8);
                    dst_row[0] = pal4[rbits & 3u];
                    dst_row[1] = pal4[(rbits >> 2) & 3u];
                    dst_row[2] = pal4[(rbits >> 4) & 3u];
                    dst_row[3] = pal4[(rbits >> 6) & 3u];
                }
            }
        }
    } else if (fmt == 1) { // A3I5
        for (int i = 0; i < total; i++) {
            uint8_t b = PcGx.textures[(tex_addr + i) & (PC_GX_TEX_BYTES - 1)];
            uint8_t idx = b & 31u;
            uint8_t a = (uint8_t)(((b >> 5) * 255u) / 7u);
            tex_decode_buf[i] = Rgb15ToRgba32(TexPltt16(pltt_addr + idx * 2u), a);
        }
    } else if (fmt == 2) { // 4-color
        uint32_t p4[4];
        for (int k = 0; k < 4; k++)
            p4[k] = (k == 0 && col0_trans) ? 0u : rgb15_lut[TexPltt16(pltt_addr + (uint32_t)k * 2u) & 0x7FFFu];
        for (int i = 0; i < total; i += 4) {
            uint8_t b = PcGx.textures[(tex_addr + (i >> 2)) & (PC_GX_TEX_BYTES - 1)];
            tex_decode_buf[i + 0] = p4[b & 3u];
            tex_decode_buf[i + 1] = p4[(b >> 2) & 3u];
            tex_decode_buf[i + 2] = p4[(b >> 4) & 3u];
            tex_decode_buf[i + 3] = p4[(b >> 6) & 3u];
        }
    } else if (fmt == 3) { // 16-color
        uint32_t p16[16];
        for (int k = 0; k < 16; k++)
            p16[k] = (k == 0 && col0_trans) ? 0u : rgb15_lut[TexPltt16(pltt_addr + (uint32_t)k * 2u) & 0x7FFFu];
        for (int i = 0; i < total; i += 2) {
            uint8_t b = PcGx.textures[(tex_addr + (i >> 1)) & (PC_GX_TEX_BYTES - 1)];
            tex_decode_buf[i + 0] = p16[b & 15u];
            tex_decode_buf[i + 1] = p16[b >> 4];
        }
    } else if (fmt == 4) { // 256-color
        uint32_t p256[256];
        for (int k = 0; k < 256; k++)
            p256[k] = (k == 0 && col0_trans) ? 0u : rgb15_lut[TexPltt16(pltt_addr + (uint32_t)k * 2u) & 0x7FFFu];
        for (int i = 0; i < total; i++) {
            uint8_t idx = PcGx.textures[(tex_addr + i) & (PC_GX_TEX_BYTES - 1)];
            tex_decode_buf[i] = p256[idx];
        }
    } else if (fmt == 6) { // A5I3
        for (int i = 0; i < total; i++) {
            uint8_t b = PcGx.textures[(tex_addr + i) & (PC_GX_TEX_BYTES - 1)];
            uint8_t idx = b & 7u;
            uint8_t a = (uint8_t)(((b >> 3) * 255u) / 31u);
            tex_decode_buf[i] = Rgb15ToRgba32(TexPltt16(pltt_addr + idx * 2u), a);
        }
    } else if (fmt == 7) { // Direct 16-bit
        for (int i = 0; i < total; i++) {
            uint32_t a = (tex_addr + (uint32_t)i * 2u) & (PC_GX_TEX_BYTES - 2);
            uint16_t val = (uint16_t)(PcGx.textures[a] | (PcGx.textures[a + 1] << 8));
            tex_decode_buf[i] = (val & 0x8000u) ? rgb15_lut[val & 0x7FFFu] : 0u;
        }
    }

    glBindTexture(GL_TEXTURE_2D, e->gl_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex_decode_buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLenum wrap_s = (texparam & (1u << 16)) ? ((texparam & (1u << 18)) ? GL_MIRRORED_REPEAT : GL_REPEAT) : GL_CLAMP_TO_EDGE;
    GLenum wrap_t = (texparam & (1u << 17)) ? ((texparam & (1u << 19)) ? GL_MIRRORED_REPEAT : GL_REPEAT) : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
    return e->gl_tex;
}

static GLuint CompileShader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        VitaLog_Printf("[GPU-bound] Cg shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool VitaVideo_Init(void) {
    for (uint32_t c = 0; c < 32768; c++) {
        uint32_t r = (c & 31u) * 255u / 31u;
        uint32_t g = ((c >> 5u) & 31u) * 255u / 31u;
        uint32_t b = ((c >> 10u) & 31u) * 255u / 31u;
        rgb15_lut[c] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    fb_main_back = (uint32_t *)malloc(DS_W * DS_H * 4);
    if (!fb_main_back) return false;

    vglInitWithCustomSizes(0, VITA_W, VITA_H, 16 * 1024 * 1024, 32 * 1024 * 1024, 4 * 1024 * 1024, 0, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(GL_TRUE);

    const char *vs_3d =
        "void main(float4 aPos, float2 aUv, float4 aCol, uniform float2 uUvScale,\n"
        "          float4 out vPos : POSITION, float2 out vUv : TEXCOORD0, float4 out vCol : COLOR0) {\n"
        "    vPos = aPos;\n"
        "    vUv = aUv * uUvScale;\n"
        "    vCol = aCol;\n"
        "}\n";
    const char *fs_3d =
        "float4 main(float2 vUv : TEXCOORD0, float4 vCol : COLOR0,\n"
        "            uniform sampler2D uTex : TEXUNIT0,\n"
        "            uniform float uTexMode,\n"
        "            uniform float2 uFade) : COLOR {\n"
        "    float4 c = vCol;\n"
        "    if (uTexMode > 1.5) {\n"
        "        float4 t = tex2D(uTex, vUv);\n"
        "        c = float4(lerp(vCol.rgb, t.rgb, t.a), vCol.a);\n"
        "    } else if (uTexMode > 0.5) {\n"
        "        c *= tex2D(uTex, vUv);\n"
        "    }\n"
        "    if (c.a < 0.01) discard;\n"
        "    c.rgb = saturate(c.rgb * uFade.x + uFade.y);\n"
        "    return c;\n"
        "}\n";
    GLuint vs = CompileShader(GL_CG_VERTEX_SHADER_EXT, vs_3d);
    GLuint fs = CompileShader(GL_CG_FRAGMENT_SHADER_EXT, fs_3d);
    if (vs && fs) {
        prog_3d = glCreateProgram();
        glAttachShader(prog_3d, vs);
        glAttachShader(prog_3d, fs);
        glBindAttribLocation(prog_3d, 0, "aPos");
        glBindAttribLocation(prog_3d, 1, "aUv");
        glBindAttribLocation(prog_3d, 2, "aCol");
        glLinkProgram(prog_3d);
        GLint linked = 0;
        glGetProgramiv(prog_3d, GL_LINK_STATUS, &linked);
        if (!linked) {
            char plog[512];
            glGetProgramInfoLog(prog_3d, sizeof(plog), NULL, plog);
            VitaLog_Printf("[GPU-bound] prog_3d link failed: %s", plog);
        }
        loc_tex_mode = glGetUniformLocation(prog_3d, "uTexMode");
        loc_uv_scale = glGetUniformLocation(prog_3d, "uUvScale");
        loc_fade = glGetUniformLocation(prog_3d, "uFade");
    }

    glGenBuffers(TEX_RING, vbo_3d);
    memset(vbo_3d_cap, 0, sizeof(vbo_3d_cap));
    last_3d_ring = -1; last_3d_verts = 0; last_3d_frame = 0;
    glGenTextures(TEX_RING, tex_main);
    glGenTextures(TEX_RING, tex_main_back);
    glGenTextures(TEX_RING, tex_sub);
    for (int i = 0; i < TEX_RING; i++) {
        GLuint ids[3] = { tex_main[i], tex_main_back[i], tex_sub[i] };
        for (int k = 0; k < 3; k++) {
            glBindTexture(GL_TEXTURE_2D, ids[k]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DS_W, DS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    uint32_t white = 0xFFFFFFFFu;
    glGenTextures(1, &tex_white);
    glBindTexture(GL_TEXTURE_2D, tex_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    comp0_start = sceKernelCreateSema("comp0_start", 0, 0, 1, NULL);
    comp0_done = sceKernelCreateSema("comp0_done", 0, 0, 1, NULL);
    comp1_start = sceKernelCreateSema("comp1_start", 0, 0, 1, NULL);
    comp1_done = sceKernelCreateSema("comp1_done", 0, 0, 1, NULL);
    comp_quit = false;
    comp0_thread = sceKernelCreateThread("bw2_2d_0", Compositor0Thread, 0x10000100, 0x10000, 0, 0x20000, NULL);
    comp1_thread = sceKernelCreateThread("bw2_2d_1", Compositor1Thread, 0x10000100, 0x10000, 0, 0x80000, NULL);
    if (comp0_thread >= 0) sceKernelStartThread(comp0_thread, 0, NULL);
    if (comp1_thread >= 0) sceKernelStartThread(comp1_thread, 0, NULL);

    VitaVideo_ApplySettings();
    VitaLog_Printf("vitaGL v26 initialized (960x544, prog_3d=%u, fbo=%u %dx%d fb=%d, vbo0=%u)",
                   prog_3d, fbo_3d, fbo_w, fbo_h, (int)fbo_fallback_active, vbo_3d[0]);
    return true;
}

void VitaVideo_Shutdown(void) {
    comp_quit = true;
    if (comp0_thread >= 0) {
        sceKernelSignalSema(comp0_start, 1);
        sceKernelWaitThreadEnd(comp0_thread, NULL, NULL);
        sceKernelDeleteThread(comp0_thread);
        comp0_thread = -1;
    }
    if (comp1_thread >= 0) {
        sceKernelSignalSema(comp1_start, 1);
        sceKernelWaitThreadEnd(comp1_thread, NULL, NULL);
        sceKernelDeleteThread(comp1_thread);
        comp1_thread = -1;
    }
    if (comp0_start >= 0) sceKernelDeleteSema(comp0_start);
    if (comp0_done >= 0) sceKernelDeleteSema(comp0_done);
    if (comp1_start >= 0) sceKernelDeleteSema(comp1_start);
    if (comp1_done >= 0) sceKernelDeleteSema(comp1_done);
    if (fbo_depth_rb) { glDeleteRenderbuffers(1, &fbo_depth_rb); fbo_depth_rb = 0; }
    if (fbo_color_tex) { glDeleteTextures(1, &fbo_color_tex); fbo_color_tex = 0; }
    if (fbo_3d) { glDeleteFramebuffers(1, &fbo_3d); fbo_3d = 0; }
    if (fb_main_back) { free(fb_main_back); fb_main_back = NULL; }
}

static void Destroy3DFBO(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo_3d || fbo_color_tex || fbo_depth_rb) {
        // Drain in-flight vitaGL GPU commands before deleting FBO resources (prevents corruption!)
        glFinish();
    }
    if (fbo_depth_rb) { glDeleteRenderbuffers(1, &fbo_depth_rb); fbo_depth_rb = 0; }
    if (fbo_color_tex) { glDeleteTextures(1, &fbo_color_tex); fbo_color_tex = 0; }
    if (fbo_3d) { glDeleteFramebuffers(1, &fbo_3d); fbo_3d = 0; }
    fbo_w = fbo_h = 0;
}

static bool TryCreate3DFBO(int w, int h, bool linear_filter) {
    Destroy3DFBO();
    SceKernelFreeMemorySizeInfo mem;
    mem.size = sizeof(mem);
    sceKernelGetFreeMemorySize(&mem);
    uint32_t need_bytes = (uint32_t)(w * h * 6) + (8u * 1024u * 1024u); // RGBA8 + Depth16 + 8 MiB safety margin
    if (mem.size_cdram < need_bytes) {
        VitaLog_Printf("FBO %dx%d skipped: free_cdram=%uKB < need=%uKB", w, h, mem.size_cdram / 1024u, need_bytes / 1024u);
        return false;
    }
    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &fbo_color_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GLenum filt = (linear_filter || w > 512) ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &fbo_depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, fbo_depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);

    glGenFramebuffers(1, &fbo_3d);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo_depth_rb);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GLenum err = glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE || err != GL_NO_ERROR) {
        VitaLog_Printf("FBO %dx%d incomplete/error (status=0x%x err=0x%x)", w, h, (unsigned)status, (unsigned)err);
        Destroy3DFBO();
        return false;
    }
    fbo_w = w; fbo_h = h;
    VitaLog_Printf("FBO 3D active: %dx%d (free_cdram=%uKB)", w, h, mem.size_cdram / 1024u);
    return true;
}

void VitaVideo_ApplySettings(void) {
    const VitaSettings *s = VitaSettings_Get();
    if (active_filter_cfg != s->bilinear_filter) {
        active_filter_cfg = s->bilinear_filter;
        GLenum filt = s->bilinear_filter ? GL_LINEAR : GL_NEAREST;
        for (int i = 0; i < TEX_RING; i++) {
            GLuint ids[3] = { tex_main[i], tex_main_back[i], tex_sub[i] };
            for (int k = 0; k < 3; k++) {
                glBindTexture(GL_TEXTURE_2D, ids[k]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
            }
        }
        if (fbo_color_tex) {
            GLenum ffilt = (s->bilinear_filter || fbo_w > 512) ? GL_LINEAR : GL_NEAREST;
            glBindTexture(GL_TEXTURE_2D, fbo_color_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, ffilt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, ffilt);
        }
    }
    // Allocate ONE stable FBO per render_scale tier (16:9-wide backing texture, sub-viewport for 4:3).
    // This eliminates ALL mid-frame FBO destroy/recreate corruption and stutter on scene/menu switches!
    (void)active_fbo_wide;
    if (active_scale_cfg != s->render_scale) {
        active_scale_cfg = s->render_scale;
        fbo_fallback_active = false;
        // Exact PowerVR SGX543 32x32 macrotile-aligned backing FBOs (top tier = 1:1 native 960x544 OLED!)
        static const int widths[3]  = { 352, 704, 960 };
        static const int heights[3] = { 192, 384, 544 };
        int req = s->render_scale < 3 ? s->render_scale : 0;
        bool ok = TryCreate3DFBO(widths[req], heights[req], s->bilinear_filter != 0);
        if (!ok && req > 1) {
            fbo_fallback_active = true;
            ok = TryCreate3DFBO(widths[1], heights[1], s->bilinear_filter != 0);
        }
        if (!ok && req > 0) {
            fbo_fallback_active = true;
            ok = TryCreate3DFBO(widths[0], heights[0], s->bilinear_filter != 0);
        }
        if (!ok) {
            fbo_fallback_active = (req != 0);
            Destroy3DFBO();
        }
    }
}

void VitaVideo_SetSinglePeek(bool peek) { single_peek_active = peek; }
bool VitaVideo_IsSinglePeek(void) { return single_peek_active; }
void VitaVideo_SetTouchCursor(bool visible, uint32_t ds_x, uint32_t ds_y) {
    cursor_visible = visible;
    cursor_ds_x = ds_x;
    cursor_ds_y = ds_y;
}

void VitaVideo_RequestScreenshot(void) {
    screenshot_requested = true;
}

static VitaRect ComputePipRect(const VitaSettings *s) {
    static const int pw[3] = { 224, 280, 340 };
    static const int ph[3] = { 168, 210, 255 };
    int sz = (s->pip_size < 3) ? s->pip_size : 0;
    int w = pw[sz], h = ph[sz];
    const int m = 12;
    switch (s->pip_corner) {
        case VITA_PIP_BOTTOM_RIGHT: return (VitaRect){ VITA_W - w - m, VITA_H - h - m, w, h };
        case VITA_PIP_TOP_LEFT:     return (VitaRect){ m, m, w, h };
        case VITA_PIP_BOTTOM_LEFT:  return (VitaRect){ m, VITA_H - h - m, w, h };
        case VITA_PIP_TOP_RIGHT:
        case VITA_PIP_AUTO:
        default:                    return (VitaRect){ VITA_W - w - m, m, w, h };
    }
}

static bool SingleNeedsPip(const VitaSettings *s) {
    if (s->pip_corner == VITA_PIP_OFF) return single_peek_active;
    if (s->pip_corner >= VITA_PIP_TOP_RIGHT && s->pip_corner <= VITA_PIP_BOTTOM_LEFT) return true;
    // VITA_PIP_AUTO: pop up cleanly for Battles, Field X-Menu, Intro Pokeball, or Touch-Peek
    return single_peek_active || VitaBoot_IsBattleScene() || VitaBoot_IsFieldMenuOpen() || VitaBoot_IsIntroScene();
}

static void ComputeLayoutRects(VitaRect *top_r, VitaRect *bot_r, bool *draw_top, bool *draw_bot, bool *top_is_pip, bool *bot_is_pip) {
    const VitaSettings *s = VitaSettings_Get();
    *draw_top = true;
    *draw_bot = true;
    if (top_is_pip) *top_is_pip = false;
    if (bot_is_pip) *bot_is_pip = false;
    if (s->layout == VITA_LAYOUT_SINGLE) {
        if (VitaBoot_IsTextEntryScene() || VitaBoot_IsTitleScene() || VitaBoot_IsStarterSelectScene()) {
            // Text entry, Title screen, & Bianca Starter Select need BOTH screens clearly visible Side-by-Side!
            *top_r = (VitaRect){ 0, 92, 480, 360 };
            *bot_r = (VitaRect){ 480, 92, 480, 360 };
            *draw_top = true;
            *draw_bot = true;
        } else if (VitaBoot_IsTouchPrimaryScene()) {
            // Touch-primary menus (Start Menu, Bag, Party, Pokedex, Status): Bottom centered, Top info ALWAYS in KH PiP
            *bot_r = (VitaRect){ 117, 0, 725, 544 };
            *top_r = ComputePipRect(s);
            *draw_bot = true;
            bool show_top_pip = (s->pip_corner != VITA_PIP_OFF);
            *draw_top = show_top_pip;
            if (top_is_pip) *top_is_pip = show_top_pip;
            if (bot_is_pip) *bot_is_pip = false;
        } else {
            // Gameplay (Overworld, Battles, 3D Cutscenes, Intro): Top primary (16:9 3D + 4:3 HUD), Bottom in KH 4-Corner PiP!
            *top_r = (VitaRect){ 117, 0, 725, 544 };
            bool show_pip = SingleNeedsPip(s);
            *bot_r = ComputePipRect(s);
            *draw_top = true;
            *draw_bot = show_pip;
            if (bot_is_pip) *bot_is_pip = show_pip;
        }
    } else if (s->layout == VITA_LAYOUT_HYBRID) {
        if (s->hybrid_size == VITA_HYBRID_LARGE) {
            // Exact 4:3: Primary 680x510 (y=17), Secondary 280x210
            *top_r = (VitaRect){ 0, 17, 680, 510 };
            int by = (s->hybrid_pos == VITA_HYBRID_TOP_RIGHT) ? 17 :
                     (s->hybrid_pos == VITA_HYBRID_CENTER_RIGHT) ? 167 : 317;
            *bot_r = (VitaRect){ 680, by, 280, 210 };
        } else {
            // Exact 4:3: Primary 725x544, Secondary 235x176
            *top_r = (VitaRect){ 0, 0, 725, 544 };
            int by = (s->hybrid_pos == VITA_HYBRID_TOP_RIGHT) ? 0 :
                     (s->hybrid_pos == VITA_HYBRID_CENTER_RIGHT) ? 184 : 368;
            *bot_r = (VitaRect){ 725, by, 235, 176 };
        }
    } else {
        // Side-by-Side: Equal exact 4:3 (480x360 each, centered vertically)
        *top_r = (VitaRect){ 0, 92, 480, 360 };
        *bot_r = (VitaRect){ 480, 92, 480, 360 };
    }
}

VitaRect VitaVideo_GetSubRect(void) {
    VitaRect top_r, bot_r; bool dt, db, tp, bp;
    ComputeLayoutRects(&top_r, &bot_r, &dt, &db, &tp, &bp);
    return bot_r;
}

VitaRect VitaVideo_GetTouchRect(void) {
    VitaRect top_r, bot_r; bool dt, db, tp, bp;
    ComputeLayoutRects(&top_r, &bot_r, &dt, &db, &tp, &bp);
    return bot_r;
}

bool VitaVideo_IsTouchVisible(void) {
    VitaRect top_r, bot_r; bool dt, db, tp, bp;
    ComputeLayoutRects(&top_r, &bot_r, &dt, &db, &tp, &bp);
    return db;
}

static void GetActive3DViewport(int *out_w, int *out_h) {
    if (fbo_w <= 0 || fbo_h <= 0) { *out_w = 0; *out_h = 0; return; }
    if (VitaBoot_IsWide3DActive()) {
        int w169 = (fbo_h * VITA_W + VITA_H / 2) / VITA_H;
        if (w169 > fbo_w) w169 = fbo_w;
        *out_w = w169;
        *out_h = fbo_h;
    } else {
        int w43 = (fbo_h * 4 + 1) / 3;
        if (w43 > fbo_w) w43 = fbo_w;
        *out_w = w43;
        *out_h = fbo_h;
    }
}

void VitaVideo_GetActiveResInfo(int *w, int *h, bool *is_fallback) {
    if (fbo_3d && fbo_w > 0 && fbo_h > 0) {
        int aw = 0, ah = 0;
        GetActive3DViewport(&aw, &ah);
        if (w) *w = aw;
        if (h) *h = ah;
    } else {
        VitaRect top_r, bot_r; bool dt, db, tp, bp;
        ComputeLayoutRects(&top_r, &bot_r, &dt, &db, &tp, &bp);
        if (w) *w = top_r.w;
        if (h) *h = top_r.h;
    }
    if (is_fallback) *is_fallback = fbo_fallback_active;
}

static void DrawSolidRect(VitaRect r, uint32_t rgba) {
    float x0 = (float)r.x * 2.0f / VITA_W - 1.0f;
    float x1 = (float)(r.x + r.w) * 2.0f / VITA_W - 1.0f;
    float y0 = 1.0f - (float)r.y * 2.0f / VITA_H;
    float y1 = 1.0f - (float)(r.y + r.h) * 2.0f / VITA_H;
    float pos[12] = { x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1 };
    float uvs[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glViewport(0, 0, VITA_W, VITA_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glColor4f((rgba & 0xFFu) * (1.0f / 255.0f),
              ((rgba >> 8) & 0xFFu) * (1.0f / 255.0f),
              ((rgba >> 16) & 0xFFu) * (1.0f / 255.0f), 1.0f);
    glBindTexture(GL_TEXTURE_2D, tex_white);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

static void DrawQuad2DSub(VitaRect r, GLuint tex, float u_max, bool flip_v) {
    float x0 = (float)r.x * 2.0f / VITA_W - 1.0f;
    float x1 = (float)(r.x + r.w) * 2.0f / VITA_W - 1.0f;
    float y0 = 1.0f - (float)r.y * 2.0f / VITA_H;
    float y1 = 1.0f - (float)(r.y + r.h) * 2.0f / VITA_H;
    float v0 = flip_v ? 1.0f : 0.0f;
    float v1 = flip_v ? 0.0f : 1.0f;
    float pos[12] = { x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1 };
    float uvs[12] = { 0, v0, u_max, v0, u_max, v1, 0, v0, u_max, v1, 0, v1 };
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}
static void DrawQuad2D(VitaRect r, GLuint tex) { DrawQuad2DSub(r, tex, 1.0f, false); }
static void DrawQuad2DFlippedV(VitaRect r, GLuint tex, float u_max) { DrawQuad2DSub(r, tex, u_max, true); }

static void DrawPipBezel(VitaRect r, float alpha) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float bx0 = (float)(r.x - 3) * 2.0f / VITA_W - 1.0f;
    float bx1 = (float)(r.x + r.w + 3) * 2.0f / VITA_W - 1.0f;
    float by0 = 1.0f - (float)(r.y - 3) * 2.0f / VITA_H;
    float by1 = 1.0f - (float)(r.y + r.h + 3) * 2.0f / VITA_H;
    float bpos[12] = { bx0, by0, bx1, by0, bx1, by1, bx0, by0, bx1, by1, bx0, by1 };
    float buvs[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    glBindTexture(GL_TEXTURE_2D, tex_white);
    glColor4f(0.18f, 0.56f, 0.88f, 0.60f * alpha);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, bpos);
    glTexCoordPointer(2, GL_FLOAT, 0, buvs);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

static void DrawPipScreen(VitaRect r, GLuint tex, float alpha) {
    DrawPipBezel(r, alpha);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, alpha);
    DrawQuad2D(r, tex);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

static void SaveScreenBmp(const char *path) {
    uint8_t *shot = (uint8_t *)malloc(VITA_W * VITA_H * 4);
    if (!shot) return;
    glReadPixels(0, 0, VITA_W, VITA_H, GL_RGBA, GL_UNSIGNED_BYTE, shot);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint8_t hdr[54] = {
            'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0,
            40,0,0,0, 0xC0,0x03,0,0, 0x20,0x02,0,0, 1,0, 32,0
        };
        uint32_t sz = 54 + VITA_W * VITA_H * 4;
        hdr[2] = sz & 0xFF; hdr[3] = (sz >> 8) & 0xFF; hdr[4] = (sz >> 16) & 0xFF; hdr[5] = (sz >> 24) & 0xFF;
        fwrite(hdr, 1, 54, f);
        for (int y = 0; y < VITA_H; y++) {
            uint8_t *row = shot + y * VITA_W * 4;
            for (int x = 0; x < VITA_W; x++) {
                uint8_t r = row[x * 4 + 0], b = row[x * 4 + 2];
                row[x * 4 + 0] = b; row[x * 4 + 2] = r;
            }
            fwrite(row, 1, VITA_W * 4, f);
        }
        fclose(f);
        VitaLog_Printf("Saved screenshot %s (verts=%u, disp0=0x%08x, disp1=0x%08x, pow=0x%04x, glErr=0x%x)",
                       path, PcGx.vertex_count, PcGxMainDisplay, PcGxSubDisplay, PcGxPower, (unsigned)glGetError());
    }
    free(shot);
}

void VitaVideo_GetTimings(uint64_t *t3d, uint64_t *t2d_wait, uint64_t *t2d_up, uint64_t *tswap) {
    if (t3d) *t3d = prof_3d_us;
    if (t2d_wait) *t2d_wait = prof_2d_wait_us;
    if (t2d_up) *t2d_up = prof_2d_up_us;
    if (tswap) *tswap = prof_swap_us;
    prof_3d_us = prof_2d_wait_us = prof_2d_up_us = prof_swap_us = 0;
}

static inline bool IsVertexTranslucent(const PcGxVertex *v) {
    uint32_t fmt = (v->texture >> 26) & 7u;
    return v->color[3] < 0.99f || fmt == 1u || fmt == 6u;
}

void VitaVideo_Present(void) {
    tex_frame++;
    uint32_t disp0 = PcGxMainDisplay;

    // Kick BOTH 2D compositors (Engine 0 on Core 1, Engine 1 on Core 3)
    if (comp0_thread >= 0) sceKernelSignalSema(comp0_start, 1);
    else ComposeEngine2D(0, fb_main);
    if (comp1_thread >= 0) sceKernelSignalSema(comp1_start, 1);
    else ComposeEngine2D(1, fb_sub);

    VitaRect top_r, bot_r; bool draw_top = true, draw_bot = true, top_is_pip = false, bot_is_pip = false;
    ComputeLayoutRects(&top_r, &bot_r, &draw_top, &draw_bot, &top_is_pip, &bot_is_pip);
    bool wide3d = VitaBoot_IsWide3DActive();
    bool main_on_top = (PcGxPower & 0x8000u) != 0;
    VitaRect main_r = main_on_top ? top_r : bot_r;
    VitaRect sub_r  = main_on_top ? bot_r : top_r;
    bool draw_main  = main_on_top ? draw_top : draw_bot;
    bool draw_sub   = main_on_top ? draw_bot : draw_top;
    bool main_is_pip = main_on_top ? top_is_pip : bot_is_pip;
    bool sub_is_pip  = main_on_top ? bot_is_pip : top_is_pip;
    VitaRect main_3d_r = (wide3d && !main_is_pip) ? (VitaRect){ 0, 0, VITA_W, VITA_H } : main_r;

    // Wait ONLY for Core 1 (Main Engine 0) before 3D; Core 3 (Sub Engine 1) overlaps with 3D!
    uint64_t p_w0 = sceKernelGetProcessTimeWide();
    if (comp0_thread >= 0) sceKernelWaitSema(comp0_done, 1, NULL);
    uint64_t p_w1 = sceKernelGetProcessTimeWide();

    uint32_t ring_idx = tex_frame % TEX_RING;
    bool bg0_3d = (disp0 & 8u) && (disp0 & (1u << 8));
    bool use_fbo = draw_main && bg0_3d && prog_3d && fbo_3d != 0 && fbo_w > 0 && fbo_h > 0;

    int act_3d_w = fbo_w, act_3d_h = fbo_h;
    GetActive3DViewport(&act_3d_w, &act_3d_h);
    float fbo_u_max = (fbo_w > 0 && act_3d_w < fbo_w) ? (((float)act_3d_w - 0.5f) / (float)fbo_w) : 1.0f;
    if (use_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, act_3d_w, act_3d_h);
        uint32_t cc = main_back_solid ? main_back_solid_rgba : 0xFF000000u;
        glClearColor((cc & 0xFFu) * (1.0f / 255.0f),
                     ((cc >> 8) & 0xFFu) * (1.0f / 255.0f),
                     ((cc >> 16) & 0xFFu) * (1.0f / 255.0f), 1.0f);
        glClearDepth(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (!main_back_solid && main_has_back) {
            glUseProgram(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glMatrixMode(GL_PROJECTION); glLoadIdentity();
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);
            glEnable(GL_TEXTURE_2D);
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            glBindTexture(GL_TEXTURE_2D, tex_main_back[ring_idx]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DS_W, DS_H, GL_RGBA, GL_UNSIGNED_BYTE, fb_main_back);
            float pos[12] = { -1, 1, 1, 1, 1, -1, -1, 1, 1, -1, -1, -1 };
            float uvs[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glVertexPointer(2, GL_FLOAT, 0, pos);
            glTexCoordPointer(2, GL_FLOAT, 0, uvs);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepth(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (draw_main) {
            if (main_back_solid) {
                uint32_t c = main_back_solid_rgba;
                if ((c & 0x00FFFFFFu) != 0) DrawSolidRect(main_r, c);
            } else if (main_has_back) {
                glUseProgram(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glMatrixMode(GL_PROJECTION); glLoadIdentity();
                glMatrixMode(GL_MODELVIEW); glLoadIdentity();
                glViewport(0, 0, VITA_W, VITA_H);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                glDisable(GL_BLEND);
                glEnable(GL_TEXTURE_2D);
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                glBindTexture(GL_TEXTURE_2D, tex_main_back[ring_idx]);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DS_W, DS_H, GL_RGBA, GL_UNSIGNED_BYTE, fb_main_back);
                DrawQuad2D(main_r, tex_main_back[ring_idx]);
            }
        }
    }

    uint64_t p0 = sceKernelGetProcessTimeWide();
    if (draw_main && bg0_3d && prog_3d && PcGx.vertex_count > 0) {
        static bool logged_diag = false;
        if (!logged_diag && PcGx.vertex_count >= 30) {
            logged_diag = true;
            const PcGxVertex *v0 = &PcGx.vertices[0];
            VitaLog_Printf("3D_DIAG_V18 verts=%u disp0=0x%08x disp1=0x%08x pow=0x%04x bg0_prio=%u back=%d solid=%d front=%d v0=(%.2f,%.2f,%.2f,%.2f) tex=0x%08x pal=0x%x poly=0x%08x",
                           PcGx.vertex_count, disp0, PcGxSubDisplay, PcGxPower,
                           PcGxRegisters[0].halves[4] & 3u, (int)main_has_back, (int)main_back_solid, (int)main_has_front,
                           v0->clip[0], v0->clip[1], v0->clip[2], v0->clip[3],
                           v0->texture, v0->palette, v0->polygon);
        }

        float fade_mul, fade_add;
        ComputePlaneFadeFloat(0, 0, &fade_mul, &fade_add);

        glUseProgram(prog_3d);
        glUniform2f(loc_fade, fade_mul, fade_add);
        if (use_fbo) {
            glViewport(0, 0, act_3d_w, act_3d_h);
            if (act_3d_w < fbo_w) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(0, 0, act_3d_w, act_3d_h);
            } else {
                glDisable(GL_SCISSOR_TEST);
            }
        } else {
            glViewport(main_3d_r.x, VITA_H - (main_3d_r.y + main_3d_r.h), main_3d_r.w, main_3d_r.h);
            glEnable(GL_SCISSOR_TEST);
            glScissor(main_3d_r.x, VITA_H - (main_3d_r.y + main_3d_r.h), main_3d_r.w, main_3d_r.h);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glFrontFace(GL_CCW);
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_TEXTURE_2D);

        bool reuse_vbo = VitaBoot_Reused3DFrame() && last_3d_ring >= 0 &&
                         last_3d_verts == PcGx.vertex_count && (tex_frame - last_3d_frame) < TEX_RING;
        if (reuse_vbo) {
            glBindBuffer(GL_ARRAY_BUFFER, vbo_3d[last_3d_ring]);
        } else {
            uint32_t bytes = PcGx.vertex_count * (uint32_t)sizeof(PcGxVertex);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_3d[ring_idx]);
            if (bytes > vbo_3d_cap[ring_idx]) {
                uint32_t alloc_bytes = bytes + 65536u;
                glBufferData(GL_ARRAY_BUFFER, alloc_bytes, NULL, GL_DYNAMIC_DRAW);
                vbo_3d_cap[ring_idx] = alloc_bytes;
            }
            glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, PcGx.vertices);
            last_3d_ring = (int)ring_idx;
            last_3d_verts = PcGx.vertex_count;
            last_3d_frame = tex_frame;
        }
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(PcGxVertex), (const void *)offsetof(PcGxVertex, clip));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(PcGxVertex), (const void *)offsetof(PcGxVertex, uv));
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(PcGxVertex), (const void *)offsetof(PcGxVertex, color));

        bool has_translucent = false;
        uint32_t prev_tex = 0xFFFFFFFFu, prev_pal = 0xFFFFFFFFu;
        GLuint prev_gl_tex = 0xFFFFFFFFu;
        int prev_tw = -1, prev_th = -1;
        int prev_tmode = -1;
        int prev_cull = -1;

        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0) {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            } else {
                if (!has_translucent) break;
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            int prev_dwrite = (pass == 0) ? 1 : -1;
            uint32_t i = 0;
            while (i < PcGx.vertex_count) {
                const PcGxVertex *vi = &PcGx.vertices[i];
                uint32_t poly = vi->polygon;
                if (poly & 0xC0000000u) { i += 3; continue; }
                int cull = (int)((poly >> 6) & 3u);
                if (cull == 0) { i += 3; continue; }

                bool trans = IsVertexTranslucent(vi);
                if (pass == 0) {
                    if (trans) { has_translucent = true; i += 3; continue; }
                } else {
                    if (!trans) { i += 3; continue; }
                }

                uint32_t start = i;
                uint32_t tex = vi->texture;
                uint32_t pal = vi->palette;
                int decal = (((poly >> 4) & 3u) == 1u) ? 1 : 0;
                int dwrite = (pass == 0) ? 1 : ((poly & 0x800u) ? 1 : 0);

                i += 3;
                while (i < PcGx.vertex_count) {
                    const PcGxVertex *vn = &PcGx.vertices[i];
                    uint32_t np = vn->polygon;
                    if ((np & 0xC0000000u) || vn->texture != tex || vn->palette != pal) break;
                    if ((int)((np >> 6) & 3u) != cull) break;
                    if (((((np >> 4) & 3u) == 1u) ? 1 : 0) != decal) break;
                    bool nt = IsVertexTranslucent(vn);
                    if (nt != trans) break;
                    if (pass == 1 && (((np & 0x800u) ? 1 : 0) != dwrite)) break;
                    i += 3;
                }

                if (cull != prev_cull) {
                    if (cull == 3) {
                        glDisable(GL_CULL_FACE);
                    } else {
                        glEnable(GL_CULL_FACE);
                        glCullFace(cull == 2 ? GL_BACK : GL_FRONT);
                    }
                    prev_cull = cull;
                }
                if (pass == 1 && dwrite != prev_dwrite) {
                    glDepthMask(dwrite ? GL_TRUE : GL_FALSE);
                    prev_dwrite = dwrite;
                }

                if (tex != prev_tex || pal != prev_pal) {
                    int tw = 1, th = 1;
                    GLuint gl_tex = GetDs3DTexture(tex, pal, &tw, &th);
                    prev_tex = tex;
                    prev_pal = pal;
                    if (gl_tex != prev_gl_tex) {
                        glBindTexture(GL_TEXTURE_2D, gl_tex ? gl_tex : tex_white);
                        prev_gl_tex = gl_tex;
                    }
                    if (!gl_tex) { tw = 1; th = 1; }
                    if (tw != prev_tw || th != prev_th) {
                        glUniform2f(loc_uv_scale, 1.0f / (float)tw, 1.0f / (float)th);
                        prev_tw = tw;
                        prev_th = th;
                    }
                }
                int tmode = (prev_gl_tex == 0) ? 0 : (decal ? 2 : 1);
                if (tmode != prev_tmode) {
                    glUniform1f(loc_tex_mode, (float)tmode);
                    prev_tmode = tmode;
                }
                glDrawArrays(GL_TRIANGLES, (GLint)start, (GLsizei)(i - start));
            }
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
    }
    uint64_t p1 = sceKernelGetProcessTimeWide();

    // Now wait for Core 3 (Sub Engine 1) — overlapped with 3D above!
    uint64_t p_w2 = sceKernelGetProcessTimeWide();
    if (comp1_thread >= 0) sceKernelWaitSema(comp1_done, 1, NULL);
    uint64_t p_w3 = sceKernelGetProcessTimeWide();

    // Switch back to default 960x544 Vita framebuffer if 3D rendered into offscreen FBO
    if (use_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glViewport(0, 0, VITA_W, VITA_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    // Upload Main front & Sub textures once, then draw Primary first and any KH PiP window LAST (on top)!
    if (draw_main && main_has_front) {
        glBindTexture(GL_TEXTURE_2D, tex_main[ring_idx]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DS_W, DS_H, GL_RGBA, GL_UNSIGNED_BYTE, fb_main);
    }
    if (draw_sub) {
        glBindTexture(GL_TEXTURE_2D, tex_sub[ring_idx]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DS_W, DS_H, GL_RGBA, GL_UNSIGNED_BYTE, fb_sub);
    }

    // Pass 1: Non-PiP primary screen(s)
    if (draw_main && !main_is_pip) {
        if (use_fbo) {
            glDisable(GL_BLEND);
            DrawQuad2DFlippedV(main_3d_r, fbo_color_tex, fbo_u_max);
        }
        if (main_has_front) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            DrawQuad2D(main_r, tex_main[ring_idx]);
        }
    }
    if (draw_sub && !sub_is_pip) {
        glDisable(GL_BLEND);
        DrawQuad2D(sub_r, tex_sub[ring_idx]);
    }

    // Pass 2: Floating KH 4-Corner PiP window (always on top with bezel + full background)
    const VitaSettings *ps = VitaSettings_Get();
    float pip_a = (ps->pip_opacity == VITA_PIP_ALPHA_GHOST) ? 0.60f : (ps->pip_opacity == VITA_PIP_ALPHA_GLASS ? 0.82f : 1.0f);
    if (VitaBoot_IsBattleScene() || VitaBoot_IsFieldMenuOpen() || VitaBoot_IsIntroScene() || single_peek_active)
        pip_a = (pip_a < 0.92f) ? 0.92f : pip_a;
    if (draw_main && main_is_pip) {
        DrawPipBezel(main_r, pip_a);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(1.0f, 1.0f, 1.0f, pip_a);
        if (use_fbo) DrawQuad2DFlippedV(main_r, fbo_color_tex, fbo_u_max);
        if (main_has_front) DrawQuad2D(main_r, tex_main[ring_idx]);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (draw_sub && sub_is_pip) {
        DrawPipScreen(sub_r, tex_sub[ring_idx], pip_a);
    }

    // Optional virtual touch cursor (Enhanced Edition Right-Stick cursor)
    if (cursor_visible && draw_bot) {
        int cx = bot_r.x + (int)(cursor_ds_x * (uint32_t)bot_r.w / DS_W);
        int cy = bot_r.y + (int)(cursor_ds_y * (uint32_t)bot_r.h / DS_H);
        DrawSolidRect((VitaRect){ cx - 6, cy - 2, 12, 4 }, 0xFFFFD020u);
        DrawSolidRect((VitaRect){ cx - 2, cy - 6, 4, 12 }, 0xFFFFD020u);
    }
    VitaOverlay_Draw();
    uint64_t p3 = sceKernelGetProcessTimeWide();

    // Auto verification screenshot once after 120 frames of 3D, or on-demand via Select+L
    static bool dumped_v19 = false;
    static bool dumped_city = false;
    static uint32_t seen_3d_frames = 0, seen_city_frames = 0;
    if (bg0_3d && PcGx.vertex_count > 100) seen_3d_frames++;
    if (!dumped_v19 && seen_3d_frames == 120) {
        dumped_v19 = true;
        SaveScreenBmp("ux0:data/pokebw2/v26_3d.bmp");
    }
    if (VitaBoot_IsFieldScene() && bg0_3d && PcGx.vertex_count > 100) seen_city_frames++;
    if (!dumped_city && seen_city_frames == 180) {
        dumped_city = true;
        SaveScreenBmp("ux0:data/pokebw2/v26_city.bmp");
    }
    if (screenshot_requested) {
        screenshot_requested = false;
        SaveScreenBmp("ux0:data/pokebw2/snap_960x544.bmp");
    }

    vglSwapBuffers(GL_FALSE);
    uint64_t p4 = sceKernelGetProcessTimeWide();
    prof_3d_us += (p1 - p0);
    prof_2d_wait_us += (p_w1 - p_w0) + (p_w3 - p_w2);
    prof_2d_up_us += (p3 - p_w3);
    prof_swap_us += (p4 - p3);
}

void VitaVideo_PresentPaused(void) {
    VitaRect top_r, bot_r; bool draw_top = true, draw_bot = true, top_is_pip = false, bot_is_pip = false;
    ComputeLayoutRects(&top_r, &bot_r, &draw_top, &draw_bot, &top_is_pip, &bot_is_pip);
    bool wide3d = VitaBoot_IsWide3DActive();
    bool main_on_top = (PcGxPower & 0x8000u) != 0;
    VitaRect main_r = main_on_top ? top_r : bot_r;
    VitaRect sub_r  = main_on_top ? bot_r : top_r;
    VitaRect main_3d_r = wide3d ? (VitaRect){ 0, 0, VITA_W, VITA_H } : main_r;
    bool draw_main  = main_on_top ? draw_top : draw_bot;
    bool draw_sub   = main_on_top ? draw_bot : draw_top;
    bool main_is_pip = main_on_top ? top_is_pip : bot_is_pip;
    bool sub_is_pip  = main_on_top ? bot_is_pip : top_is_pip;
    uint32_t ring_idx = tex_frame % TEX_RING;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glViewport(0, 0, VITA_W, VITA_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    int act_3d_w = fbo_w, act_3d_h = fbo_h;
    GetActive3DViewport(&act_3d_w, &act_3d_h);
    float fbo_u_max = (fbo_w > 0 && act_3d_w < fbo_w) ? (((float)act_3d_w - 0.5f) / (float)fbo_w) : 1.0f;
    if (draw_main && !main_is_pip) {
        glDisable(GL_BLEND);
        if (fbo_3d && fbo_color_tex && (PcGxMainDisplay & 8u) && (PcGxMainDisplay & (1u << 8))) {
            DrawQuad2DFlippedV(main_3d_r, fbo_color_tex, fbo_u_max);
        } else if (main_has_back) {
            DrawQuad2D(main_r, tex_main_back[ring_idx]);
        }
        if (main_has_front) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            DrawQuad2D(main_r, tex_main[ring_idx]);
        }
    }
    if (draw_sub && !sub_is_pip) {
        glDisable(GL_BLEND);
        DrawQuad2D(sub_r, tex_sub[ring_idx]);
    }
    if (draw_main && main_has_front && main_is_pip) DrawPipScreen(main_r, tex_main[ring_idx], 0.85f);
    if (draw_sub && sub_is_pip) DrawPipScreen(sub_r, tex_sub[ring_idx], 0.85f);
    VitaOverlay_Draw();
    vglSwapBuffers(VitaIme_IsActive() ? GL_TRUE : GL_FALSE);
}

void VitaVideo_DrawDiagnostic(uint32_t r, uint32_t g, uint32_t b) {
    glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_FALSE);
}

const uint8_t vita_seg0_pad[8192] = { 1 };
