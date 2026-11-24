#include "../gx_backend.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

PcGxFrame PcGx;
u16 PcGxPower;
u32 PcGxStatus;
u16 PcGxDisplay3D;
u16 PcGx1DotDepth;
PcGxRasterState PcGxRaster;
u32 PcGxMainDisplay, PcGxSubDisplay;
u32 PcGxTexGen = 1, PcGxPlttGen = 1;
PcGx2DMemory PcGx2D[2];
PcGx2DRegisters PcGxRegisters[2];
u8 PcGxBackground[2][524288];
u8 PcGxLcdc[0xa4000];
u32 PcGxCapturedBanks;
u32 PcGxWideCapture[PC_CANVAS_WIDTH * PC_CANVAS_HEIGHT];
BOOL PcGxWideCaptureValid;
static PcGxCaptureCallback captureCallback;
static GXVRamClearImage clearImageBanks;
static void Fail(const char *reason, u32 value);

void PcGx_SetCaptureCallback(PcGxCaptureCallback callback) {
    captureCallback = callback;
}

void PcGx_Capture(void) {
    u32 control = reg_GX_DISPCAPCNT;
    if ((control & 0x80000000u) && captureCallback)
        captureCallback(control);
    reg_GX_DISPCAPCNT &= ~0x80000000u;
}

u32 PcGx_GetPolygonCount(void) {
    return PcGx.vertex_count / 3;
}

u32 PcGx_GetVertexCount(void) {
    return PcGx.vertex_count;
}

void *G2_GetOBJCharPtr(void) {
    return PcGx2D[0].object;
}

void *G2S_GetOBJCharPtr(void) {
    return PcGx2D[1].object;
}

void GX_LoadOBJ(const void *src, u32 offset, u32 size) {
    if (offset > sizeof(PcGx2D[0].object) || size > sizeof(PcGx2D[0].object) - offset) {
        Fail("2D VRAM upload out of bounds", offset);
        return;
    }
    memcpy(PcGx2D[0].object + offset, src, size);
}

void GX_LoadOBJPltt(const void *src, u32 offset, u32 size) {
    if (offset > sizeof(PcGx2D[0].object_palette) || size > sizeof(PcGx2D[0].object_palette) - offset) {
        Fail("2D VRAM upload out of bounds", offset);
        return;
    }
    memcpy(PcGx2D[0].object_palette + offset, src, size);
}

void GX_LoadOBJExtPltt(const void *src, u32 offset, u32 size) {
    if (!size)
        return;
    if (offset > sizeof(PcGx2D[0].object_extended_palette) ||
        size > sizeof(PcGx2D[0].object_extended_palette) - offset) {
        Fail("2D VRAM upload out of bounds", offset);
        return;
    }
    memcpy(PcGx2D[0].object_extended_palette + offset, src, size);
}

void GX_LoadOAM(const void *src, u32 offset, u32 size) {
    if (offset > sizeof(PcGx2D[0].oam) || size > sizeof(PcGx2D[0].oam) - offset) {
        Fail("2D VRAM upload out of bounds", offset);
        return;
    }
    memcpy(PcGx2D[0].oam + offset, src, size);
}

void GX_BeginLoadOBJExtPltt(void) {
}

void GX_EndLoadOBJExtPltt(void) {
}

void GXS_LoadOBJ(const void *src, u32 offset, u32 size) {
    if (offset > sizeof(PcGx2D[1].object) || size > sizeof(PcGx2D[1].object) - offset) {
        Fail("2D VRAM upload out of bounds", offset);
        return;
    }
    memcpy(PcGx2D[1].object + offset, src, size);
}

void GXS_LoadOBJPltt(const void *src, u32 offset, u32 size) {
    if (offset > sizeof(PcGx2D[1].object_palette) || size > sizeof(PcGx2D[1].object_palette) - offset) {
        Fail("2D VRAM upload out of bounds", offset);
        return;
    }
    memcpy(PcGx2D[1].object_palette + offset, src, size);
}

void GXS_LoadOBJExtPltt(const void *src, u32 offset, u32 size) {
    if (!size)
        return;
    if (offset > sizeof(PcGx2D[1].object_extended_palette) ||
        size > sizeof(PcGx2D[1].object_extended_palette) - offset) {
        Fail("2D VRAM upload out of bounds", offset);
        return;
    }
    memcpy(PcGx2D[1].object_extended_palette + offset, src, size);
}

void GXS_LoadOAM(const void *src, u32 offset, u32 size) {
    if (offset > sizeof(PcGx2D[1].oam) || size > sizeof(PcGx2D[1].oam) - offset) {
        Fail("2D VRAM upload out of bounds", offset);
        return;
    }
    memcpy(PcGx2D[1].oam + offset, src, size);
}

void GXS_BeginLoadOBJExtPltt(void) {
}

void GXS_EndLoadOBJExtPltt(void) {
}
typedef struct {
    float m[4][4];
} Matrix;
static struct {
    Matrix projection, position, vector, texture, clip_mtx;
    Matrix position_stack[32], vector_stack[32], projection_stack, texture_stack;
    u32 mode, stack, primitive, polygon, texparam, palette, diffuse_ambient, specular_emission;
    u32 commands, pending, arguments[32], argument_count, direct_op;
    float position_xyz[3], uv[2], base_uv[2], color[3], lights[4][3], light_colors[4][3];
    float mat_diffuse[3], mat_ambient[3], mat_specular[3], mat_emission[3];
    float shininess[128];
    PcGxVertex primitive_vertices[4];
    unsigned primitive_count;
    bool clip_dirty;
} gx;

static Matrix Identity(void) {
    Matrix m = { 0 };
    m.m[0][0] = 1.0f;
    m.m[1][1] = 1.0f;
    m.m[2][2] = 1.0f;
    m.m[3][3] = 1.0f;
    return m;
}
static void MultiplyInto(const Matrix *a, const Matrix *b, Matrix *out) {
#ifdef __ARM_NEON
    float32x4_t b0 = vld1q_f32(b->m[0]);
    float32x4_t b1 = vld1q_f32(b->m[1]);
    float32x4_t b2 = vld1q_f32(b->m[2]);
    float32x4_t b3 = vld1q_f32(b->m[3]);
    for (int i = 0; i < 4; i++) {
        float32x4_t r = vmulq_n_f32(b0, a->m[i][0]);
        r = vmlaq_n_f32(r, b1, a->m[i][1]);
        r = vmlaq_n_f32(r, b2, a->m[i][2]);
        r = vmlaq_n_f32(r, b3, a->m[i][3]);
        vst1q_f32(out->m[i], r);
    }
#else
    Matrix tmp = { 0 };
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) {
            float aik = a->m[i][k];
            for (int j = 0; j < 4; j++)
                tmp.m[i][j] += aik * b->m[k][j];
        }
    *out = tmp;
#endif
}
static inline void Transform(const float in[4], const Matrix *m, float out[4]) {
#ifdef __ARM_NEON
    float32x4_t r = vmulq_n_f32(vld1q_f32(m->m[0]), in[0]);
    r = vmlaq_n_f32(r, vld1q_f32(m->m[1]), in[1]);
    r = vmlaq_n_f32(r, vld1q_f32(m->m[2]), in[2]);
    r = vmlaq_n_f32(r, vld1q_f32(m->m[3]), in[3]);
    vst1q_f32(out, r);
#else
    float x = in[0], y = in[1], z = in[2], w = in[3];
    for (int j = 0; j < 4; j++)
        out[j] = x * m->m[0][j] + y * m->m[1][j] + z * m->m[2][j] + w * m->m[3][j];
#endif
}
static inline void TransformPos3(const float in[3], const Matrix *m, float out[4]) {
#ifdef __ARM_NEON
    float32x4_t r = vld1q_f32(m->m[3]);
    r = vmlaq_n_f32(r, vld1q_f32(m->m[0]), in[0]);
    r = vmlaq_n_f32(r, vld1q_f32(m->m[1]), in[1]);
    r = vmlaq_n_f32(r, vld1q_f32(m->m[2]), in[2]);
    vst1q_f32(out, r);
#else
    float x = in[0], y = in[1], z = in[2];
    for (int j = 0; j < 4; j++)
        out[j] = x * m->m[0][j] + y * m->m[1][j] + z * m->m[2][j] + m->m[3][j];
#endif
}
static inline const Matrix *ClipMatrix(void) {
    if (gx.clip_dirty) {
        MultiplyInto(&gx.position, &gx.projection, &gx.clip_mtx);
        gx.clip_dirty = false;
    }
    return &gx.clip_mtx;
}
static Matrix *Current(void) {
    return gx.mode == 0 ? &gx.projection : gx.mode == 3 ? &gx.texture : &gx.position;
}
static void Fail(const char *reason, u32 value) {
    if (!PcGx.failed)
        snprintf(PcGx.error, sizeof(PcGx.error), "%s: 0x%x", reason, value);
    PcGx.failed = true;
}
static inline void Color(u32 packed, float out[3]) {
    const float inv31 = 1.0f / 31.0f;
    out[0] = (float)(packed & 31u) * inv31;
    out[1] = (float)((packed >> 5) & 31u) * inv31;
    out[2] = (float)((packed >> 10) & 31u) * inv31;
}
static inline float Signed10(u32 word) {
    return (float)((s32)(word << 22) >> 22);
}
static inline void Triangle(const PcGxVertex *a, const PcGxVertex *b, const PcGxVertex *c) {
    if (PcGx.vertex_count + 3 > PC_GX_MAX_VERTICES) {
        Fail("Geometry capacity exceeded", PcGx.vertex_count);
        return;
    }
    PcGxVertex *dst = &PcGx.vertices[PcGx.vertex_count];
    dst[0] = *a;
    dst[1] = *b;
    dst[2] = *c;
    PcGx.vertex_count += 3;
}
static void Vertex(void) {
    unsigned n = gx.primitive_count++;
    PcGxVertex *p = gx.primitive_vertices;
    PcGxVertex *v = &p[n & 3u];
    v->texture = gx.texparam;
    v->palette = gx.palette;
    v->polygon = gx.polygon;
    TransformPos3(gx.position_xyz, ClipMatrix(), v->clip);
    if (PcGx.wide_scene)
        v->clip[0] *= PcGx.projection_scale_x;
    v->uv[0] = gx.uv[0];
    v->uv[1] = gx.uv[1];
    if ((gx.texparam >> 30) == 3) {
        float tex[4];
        TransformPos3(gx.position_xyz, &gx.texture, tex);
        v->uv[0] += tex[0];
        v->uv[1] += tex[1];
    }
    v->color[0] = gx.color[0];
    v->color[1] = gx.color[1];
    v->color[2] = gx.color[2];
    u32 poly_a = (gx.polygon >> 16) & 31u;
    v->color[3] = poly_a ? ((float)poly_a * (1.0f / 31.0f)) : 1.0f;
    if (gx.primitive == 0) {
        if (n % 3 == 2)
            Triangle(&p[(n - 2) & 3u], &p[(n - 1) & 3u], v);
    } else if (gx.primitive == 1) {
        if ((n & 3u) == 3u) {
            Triangle(&p[0], &p[1], &p[2]);
            Triangle(&p[0], &p[2], &p[3]);
        }
    } else if (gx.primitive == 2) {
        if (n >= 2)
            Triangle(&p[(n - 2 + (n & 1u)) & 3u], &p[(n - 1 - (n & 1u)) & 3u], v);
    } else if (gx.primitive == 3) {
        if (n >= 3 && (n & 1u)) {
            Triangle(&p[(n - 3) & 3u], &p[(n - 2) & 3u], v);
            Triangle(&p[(n - 3) & 3u], v, &p[(n - 1) & 3u]);
        }
    }
}
static const u8 gx_arg_count_lut[256] = {
    [0x00] = 0,
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0,
    [0x16] = 16, [0x17] = 12, [0x18] = 16, [0x19] = 12, [0x1a] = 9, [0x1b] = 3, [0x1c] = 3,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1,
    [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2a] = 1, [0x2b] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1,
    [0x70] = 3, [0x71] = 2, [0x72] = 1,
};
static const u8 gx_op_valid_lut[256] = {
    [0x00] = 1,
    [0x10] = 1, [0x11] = 1, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 1,
    [0x16] = 1, [0x17] = 1, [0x18] = 1, [0x19] = 1, [0x1a] = 1, [0x1b] = 1, [0x1c] = 1,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 1, [0x24] = 1, [0x25] = 1, [0x26] = 1,
    [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2a] = 1, [0x2b] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 1,
    [0x40] = 1, [0x41] = 1, [0x50] = 1, [0x60] = 1,
    [0x70] = 1, [0x71] = 1, [0x72] = 1,
};
static inline unsigned ArgumentCount(u32 op) {
    if (op < 256 && gx_op_valid_lut[op])
        return gx_arg_count_lut[op];
    Fail("Unknown geometry command", op);
    return 0;
}
static void Execute(u32 op, const u32 *a) {
    Matrix *current = Current();
    switch (op) {
    case 0:
        break;
    case 0x10:
        gx.mode = a[0] & 3;
        break;
    case 0x11:
        if (gx.mode == 0) {
            gx.projection_stack = gx.projection;
            PcGxStatus |= REG_G3X_GXSTAT_PJ_MASK;
        } else if (gx.mode == 3)
            gx.texture_stack = gx.texture;
        else {
            gx.position_stack[gx.stack & 31] = gx.position;
            gx.vector_stack[gx.stack & 31] = gx.vector;
            gx.stack = (gx.stack + 1) & 31;
            PcGxStatus = (PcGxStatus & ~REG_G3X_GXSTAT_PV_MASK) | (gx.stack << REG_G3X_GXSTAT_PV_SHIFT);
        }
        break;
    case 0x12:
        if (gx.mode == 0) {
            gx.projection = gx.projection_stack;
            PcGxStatus &= ~REG_G3X_GXSTAT_PJ_MASK;
            gx.clip_dirty = true;
        } else if (gx.mode == 3)
            gx.texture = gx.texture_stack;
        else {
            int amount = (s32)(a[0] << 26) >> 26;
            gx.stack = (gx.stack - amount) & 31;
            gx.position = gx.position_stack[gx.stack];
            gx.vector = gx.vector_stack[gx.stack];
            PcGxStatus = (PcGxStatus & ~REG_G3X_GXSTAT_PV_MASK) | (gx.stack << REG_G3X_GXSTAT_PV_SHIFT);
            gx.clip_dirty = true;
        }
        break;
    case 0x13:
        if (gx.mode == GX_MTXMODE_PROJECTION)
            gx.projection_stack = gx.projection;
        else if (gx.mode == GX_MTXMODE_TEXTURE)
            gx.texture_stack = gx.texture;
        else {
            gx.position_stack[a[0] & 31] = gx.position;
            gx.vector_stack[a[0] & 31] = gx.vector;
        }
        break;
    case 0x14:
        if (gx.mode == GX_MTXMODE_PROJECTION) {
            gx.projection = gx.projection_stack;
            gx.clip_dirty = true;
        } else if (gx.mode == GX_MTXMODE_TEXTURE)
            gx.texture = gx.texture_stack;
        else {
            gx.position = gx.position_stack[a[0] & 31];
            gx.vector = gx.vector_stack[a[0] & 31];
            gx.clip_dirty = true;
        }
        break;
    case 0x15:
        *current = Identity();
        if (gx.mode == 2)
            gx.vector = Identity();
        if (gx.mode != 3)
            gx.clip_dirty = true;
        break;
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1a: {
        Matrix m = Identity();
        unsigned rows = op == 0x1a ? 3 : 4, cols = op == 0x16 || op == 0x18 ? 4 : 3;
        const float inv4096 = 1.0f / 4096.0f;
        for (unsigned i = 0; i < rows; i++)
            for (unsigned j = 0; j < cols; j++)
                m.m[i][j] = (float)(s32)a[i * cols + j] * inv4096;
        bool load = op == 0x16 || op == 0x17;
        if (load) {
            *current = m;
        } else {
            Matrix res;
            MultiplyInto(&m, current, &res);
            *current = res;
        }
        if (gx.mode == 2) {
            m.m[3][0] = m.m[3][1] = m.m[3][2] = 0;
            if (load) {
                gx.vector = m;
            } else {
                Matrix res;
                MultiplyInto(&m, &gx.vector, &res);
                gx.vector = res;
            }
        }
        if (gx.mode != 3)
            gx.clip_dirty = true;
        break;
    }
    case 0x1b: {
        const float inv4096 = 1.0f / 4096.0f;
        float sx = (float)(s32)a[0] * inv4096;
        float sy = (float)(s32)a[1] * inv4096;
        float sz = (float)(s32)a[2] * inv4096;
        for (int j = 0; j < 4; j++) {
            current->m[0][j] *= sx;
            current->m[1][j] *= sy;
            current->m[2][j] *= sz;
        }
        if (gx.mode != 3)
            gx.clip_dirty = true;
        break;
    }
    case 0x1c: {
        const float inv4096 = 1.0f / 4096.0f;
        float tx = (float)(s32)a[0] * inv4096;
        float ty = (float)(s32)a[1] * inv4096;
        float tz = (float)(s32)a[2] * inv4096;
        for (int j = 0; j < 4; j++)
            current->m[3][j] += tx * current->m[0][j] + ty * current->m[1][j] + tz * current->m[2][j];
        if (gx.mode != 3)
            gx.clip_dirty = true;
        break;
    }
    case 0x20:
        Color(a[0], gx.color);
        break;
    case 0x21: {
        const float inv512 = 1.0f / 512.0f;
        float local[4] = { Signed10(a[0]) * inv512, Signed10(a[0] >> 10) * inv512, Signed10(a[0] >> 20) * inv512, 0.0f }, normal[4];
        Transform(local, &gx.vector, normal);
        gx.color[0] = gx.mat_emission[0];
        gx.color[1] = gx.mat_emission[1];
        gx.color[2] = gx.mat_emission[2];
        u32 lmask = gx.polygon & 0xFu;
        if (lmask) {
            for (int light = 0; light < 4; light++) {
                if (lmask & (1u << light)) {
                    const float *ld = gx.lights[light];
                    float dot = -(normal[0] * ld[0] + normal[1] * ld[1] + normal[2] * ld[2]);
                    float halfdot = (dot + normal[2]) * 0.5f;
                    float shine = halfdot > 0.0f ? halfdot * halfdot : 0.0f;
                    if (gx.specular_emission & 0x8000) {
                        int idx = (int)(shine * 128.0f);
                        if (idx > 127) idx = 127;
                        shine = gx.shininess[idx];
                    }
                    float dpos = dot > 0.0f ? dot : 0.0f;
                    const float *lc = gx.light_colors[light];
                    gx.color[0] += lc[0] * (gx.mat_ambient[0] + gx.mat_diffuse[0] * dpos + gx.mat_specular[0] * shine);
                    gx.color[1] += lc[1] * (gx.mat_ambient[1] + gx.mat_diffuse[1] * dpos + gx.mat_specular[1] * shine);
                    gx.color[2] += lc[2] * (gx.mat_ambient[2] + gx.mat_diffuse[2] * dpos + gx.mat_specular[2] * shine);
                }
            }
            if (gx.color[0] > 1.0f) gx.color[0] = 1.0f;
            if (gx.color[1] > 1.0f) gx.color[1] = 1.0f;
            if (gx.color[2] > 1.0f) gx.color[2] = 1.0f;
        }
        if ((gx.texparam >> 30) == 2) {
            float tex[4];
            Transform(local, &gx.texture, tex);
            gx.uv[0] = gx.base_uv[0] + tex[0];
            gx.uv[1] = gx.base_uv[1] + tex[1];
        }
        break;
    }
    case 0x22: {
        float u = (float)(s16)a[0] * (1.0f / 16.0f);
        float v = (float)(s16)(a[0] >> 16) * (1.0f / 16.0f);
        gx.base_uv[0] = u;
        gx.base_uv[1] = v;
        if ((gx.texparam >> 30) == 1) {
            float uv[4] = { u, v, 1.0f / 16.0f, 1.0f / 16.0f }, transformed[4];
            Transform(uv, &gx.texture, transformed);
            gx.uv[0] = transformed[0];
            gx.uv[1] = transformed[1];
        } else {
            gx.uv[0] = u;
            gx.uv[1] = v;
        }
        break;
    }
    case 0x23:
        gx.position_xyz[0] = (s16)a[0] / 4096.0f;
        gx.position_xyz[1] = (s16)(a[0] >> 16) / 4096.0f;
        gx.position_xyz[2] = (s16)a[1] / 4096.0f;
        Vertex();
        break;
    case 0x24:
        for (int i = 0; i < 3; i++)
            gx.position_xyz[i] = Signed10(a[0] >> (i * 10)) / 64;
        Vertex();
        break;
    case 0x25:
    case 0x26:
    case 0x27:
        gx.position_xyz[op == 0x27 ? 1 : 0] = (s16)a[0] / 4096.0f;
        gx.position_xyz[op == 0x25 ? 1 : 2] = (s16)(a[0] >> 16) / 4096.0f;
        Vertex();
        break;
    case 0x28:
        for (int i = 0; i < 3; i++)
            gx.position_xyz[i] += Signed10(a[0] >> (i * 10)) / 4096;
        Vertex();
        break;
    case 0x29:
        gx.polygon = a[0];
        break;
    case 0x2a:
        gx.texparam = a[0];
        break;
    case 0x2b:
        gx.palette = a[0];
        break;
    case 0x30:
        gx.diffuse_ambient = a[0];
        Color(a[0], gx.mat_diffuse);
        Color(a[0] >> 16, gx.mat_ambient);
        if (a[0] & 0x8000) {
            gx.color[0] = gx.mat_diffuse[0];
            gx.color[1] = gx.mat_diffuse[1];
            gx.color[2] = gx.mat_diffuse[2];
        }
        break;
    case 0x31:
        gx.specular_emission = a[0];
        Color(a[0], gx.mat_specular);
        Color(a[0] >> 16, gx.mat_emission);
        break;
    case 0x32: {
        const float inv512 = 1.0f / 512.0f;
        float v[4] = { Signed10(a[0]) * inv512, Signed10(a[0] >> 10) * inv512, Signed10(a[0] >> 20) * inv512, 0.0f }, out[4];
        Transform(v, &gx.vector, out);
        memcpy(gx.lights[a[0] >> 30], out, 3 * sizeof(float));
        break;
    }
    case 0x33:
        Color(a[0], gx.light_colors[a[0] >> 30]);
        break;
    case 0x34:
        for (unsigned i = 0; i < 128; i++)
            gx.shininess[i] = ((a[i / 4] >> ((i % 4) * 8)) & 255) / 256.0f;
        break;
    case 0x40:
        gx.primitive = a[0] & 3;
        gx.primitive_count = 0;
        break;
    case 0x41:
    case 0x50:
    case 0x60:
        break;
    case 0x70: {
        s16 box[6] = { (s16)a[0], (s16)(a[0] >> 16), (s16)a[1], (s16)(a[1] >> 16), (s16)a[2], (s16)(a[2] >> 16) };
        unsigned outside_all = 63;
        const Matrix *cm = ClipMatrix();
        const float inv4096 = 1.0f / 4096.0f;
        for (unsigned corner = 0; corner < 8; corner++) {
            float local[3] = { (float)(box[0] + ((corner & 1) ? box[3] : 0)) * inv4096,
                               (float)(box[1] + ((corner & 2) ? box[4] : 0)) * inv4096,
                               (float)(box[2] + ((corner & 4) ? box[5] : 0)) * inv4096 },
                  clip[4];
            TransformPos3(local, cm, clip);
            if (PcGx.wide_scene)
                clip[0] *= PcGx.projection_scale_x;
            unsigned outside = (clip[0] < -clip[3] ? 1u : 0u) | (clip[0] > clip[3] ? 2u : 0u) |
                               (clip[1] < -clip[3] ? 4u : 0u) | (clip[1] > clip[3] ? 8u : 0u) |
                               (clip[2] < -clip[3] ? 16u : 0u) | (clip[2] > clip[3] ? 32u : 0u);
            outside_all &= outside;
        }
        PcGxStatus = (PcGxStatus & ~REG_G3X_GXSTAT_TR_MASK) | (outside_all ? 0 : REG_G3X_GXSTAT_TR_MASK);
        break;
    }
    default:
        Fail("Unimplemented geometry operation", op);
        break;
    }
}
static void Advance(void) {
    while (gx.commands && !PcGx.failed) {
        gx.pending = gx.commands & 255;
        if (ArgumentCount(gx.pending))
            return;
        Execute(gx.pending, gx.arguments);
        gx.commands >>= 8;
    }
    gx.pending = 0;
}
void PcGx_Write(uptr address, u32 value) {
    if (PcGx.failed)
        return;
    if (address == 0x04000400) {
        if (!gx.commands) {
            gx.commands = value;
            gx.argument_count = 0;
            Advance();
        } else {
            gx.arguments[gx.argument_count++] = value;
            if (gx.argument_count == ArgumentCount(gx.pending)) {
                Execute(gx.pending, gx.arguments);
                gx.argument_count = 0;
                gx.commands >>= 8;
                Advance();
            }
        }
    } else {
        u32 op = (u32)(address - 0x04000400) / 4;
        if (gx.argument_count && gx.direct_op != op) {
            Fail("Interrupted geometry command", op);
            return;
        }
        gx.direct_op = op;
        gx.arguments[gx.argument_count++] = value;
        if (gx.argument_count >= ArgumentCount(op)) {
            Execute(op, gx.arguments);
            gx.argument_count = 0;
        }
    }
}
void PcGx_WriteFifoWords(const u32 *words, u32 count) {
    if (PcGx.failed || !count)
        return;
    u32 i = 0;
    // Finish any partially-buffered command from a previous call
    while (i < count && gx.commands && !PcGx.failed) {
        u32 needed = ArgumentCount(gx.pending);
        while (i < count && gx.argument_count < needed)
            gx.arguments[gx.argument_count++] = words[i++];
        if (gx.argument_count < needed)
            return;
        Execute(gx.pending, gx.arguments);
        gx.argument_count = 0;
        gx.commands >>= 8;
        Advance();
    }
    // Fast direct streaming path: execute packed 4-op command words directly out of words[]
    while (i < count && !PcGx.failed) {
        u32 cmds = words[i++];
        while (cmds) {
            u32 op = cmds & 0xFFu;
            if (!gx_op_valid_lut[op]) {
                Fail("Unknown geometry command", op);
                return;
            }
            u32 argc = gx_arg_count_lut[op];
            if (argc == 0) {
                if (op)
                    Execute(op, gx.arguments);
                cmds >>= 8;
            } else if (i + argc <= count) {
                Execute(op, &words[i]);
                i += argc;
                cmds >>= 8;
            } else {
                // Command arguments split across DMA/FIFO calls: save remainder
                gx.commands = cmds;
                gx.pending = op;
                gx.argument_count = 0;
                while (i < count)
                    gx.arguments[gx.argument_count++] = words[i++];
                return;
            }
        }
    }
}
void PcGx_BeginFrame(void) {
    PcGx.vertex_count = 0;
    PcGx.composite_scene = false;
    PcGx.dex_ui = false;
    PcGx.party_ui = false;
    PcGx.menu_layer_count = 0;
    PcGx.battle_ui = false;
}
void PcGx_ClearFifo(void) {
    gx.commands = gx.pending = gx.argument_count = gx.direct_op = 0;
}
void PcGx_ResetHardware(void) {
    memset(&gx, 0, sizeof(gx));
    PcGx.vertex_count = 0;
    PcGx.failed = false;
    PcGx.error[0] = 0;
    PcGxStatus = 0;
    PcGx1DotDepth = 0;
    PcGx.wide_scene = false;
    PcGx.projection_scale_x = 0.75f;
    gx.projection = gx.position = gx.vector = gx.texture = gx.clip_mtx = Identity();
    gx.clip_dirty = false;
    for (int i = 0; i < 32; i++)
        gx.position_stack[i] = gx.vector_stack[i] = Identity();
    gx.projection_stack = gx.texture_stack = Identity();
    gx.polygon = 0x001f00c0;
    gx.color[0] = gx.color[1] = gx.color[2] = 1;
}
void PcGx_ReadClipMatrix(void *output) {
    MtxFx44 *dest = output;
    Matrix clip = *ClipMatrix();
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            dest->m[i][j] = (fx32)lrintf(clip.m[i][j] * 4096);
}
void PcGx_ReadVectorMatrix(void *output) {
    MtxFx33 *dest = output;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            dest->m[i][j] = (fx32)lrintf(gx.vector.m[i][j] * 4096);
}
void GX_BeginLoadTex(void) {
}
void GX_EndLoadTex(void) {
}
void GX_BeginLoadTexPltt(void) {
}
void GX_EndLoadTexPltt(void) {
}
void GX_LoadTex(const void *src, u32 offset, u32 size) {
    if (offset > sizeof(PcGx.textures) || size > sizeof(PcGx.textures) - offset) {
        Fail("Texture upload out of bounds", offset);
        return;
    }
    memcpy(PcGx.textures + offset, src, size);
    PcGxTexGen++;
}
void GX_LoadTexPltt(const void *src, u32 offset, u32 size) {
    if (offset > sizeof(PcGx.palettes) || size > sizeof(PcGx.palettes) - offset) {
        Fail("Palette upload out of bounds", offset);
        return;
    }
    memcpy(PcGx.palettes + offset, src, size);
    PcGxPlttGen++;
}
void PcGx_ReadRearPlane(PcGxRearPixel pixels[256 * 192]) {
    bool image = (PcGxDisplay3D & REG_G3X_DISP3DCNT_PRI_MASK) != 0;
    u32 offset = reg_G3X_CLRIMAGE_OFFSET;
    for (unsigned y = 0; y < 192; y++) {
        for (unsigned x = 0; x < 256; x++) {
            unsigned source = (((y + (offset >> 8)) & 255) * 256) + ((x + offset) & 255);
            u32 color = reg_G3X_CLEAR_COLOR;
            u32 depth = reg_G3X_CLEAR_DEPTH & 0x7fff;
            if (image) {
                color = (PcGx.clear_color[source] & 0x7fff) | ((PcGx.clear_color[source] & 0x8000) ? 31u << 16 : 0);
                depth = PcGx.clear_depth[source] & 0x7fff;
            }
            pixels[y * 256 + x].color = color;
            // The hardware expands 15-bit Z to 24 bits; only the far endpoint fills the low nine bits.
            pixels[y * 256 + x].depth = (float)((depth << 9) + (depth == 0x7fff ? 511 : 0)) / 16777215.0f;
        }
    }
}

void GX_SetBankForClearImage(GXVRamClearImage banks) {
    clearImageBanks = banks;
    if (banks == GX_VRAM_CLEARIMAGE_NONE)
        PcGxDisplay3D &= ~REG_G3X_DISP3DCNT_PRI_MASK;
    else
        PcGxDisplay3D |= REG_G3X_DISP3DCNT_PRI_MASK;
}

GXVRamClearImage GX_ResetBankForClearImage(void) {
    GXVRamClearImage previous = clearImageBanks;
    clearImageBanks = GX_VRAM_CLEARIMAGE_NONE;
    return previous;
}

GXVRamClearImage GX_DisableBankForClearImage(void) {
    return GX_ResetBankForClearImage();
}

void GX_BeginLoadClearImage(void) {
    // Host clear images have dedicated storage; no VRAM bank remap is required.
}
void GX_LoadClearImageColor(const void *src, u32 size) {
    if (size > sizeof(PcGx.clear_color)) {
        Fail("Clear color image upload out of bounds", size);
        return;
    }
    memcpy(PcGx.clear_color, src, size);
}
void GX_LoadClearImageDepth(const void *src, u32 size) {
    if (size > sizeof(PcGx.clear_depth)) {
        Fail("Clear depth image upload out of bounds", size);
        return;
    }
    memcpy(PcGx.clear_depth, src, size);
}
void GX_EndLoadClearImage(void) {
    // Uploads are synchronous on the game thread.
}
