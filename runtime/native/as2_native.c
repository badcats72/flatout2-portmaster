/* Alien Shooter 2: Reloaded - the NFSU2 native modules the runtime calls, without their NFSU2 content.
 *
 * NFSU2 replaced some SPEED2.EXE functions with native code (overrides.c) and patched its HUD and frame
 * pacing for wide screens (widescreen.c). Alien Shooter 2 has neither yet: no lifted function is
 * overridden, and the game keeps its own geometry and pacing. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include "recomp_types.h"
#include "nfs_runtime.h"
#include "nfs_widescreen.h"

/* setjmp/longjmp (config/setjmp.txt; the call-site macro is RECOMP_SETJMP_CALL in arm64_guest.h).
 * Each guest thread keeps a few host jmp_bufs, one per guest jmp_buf address in use. */
typedef struct { uint32_t guest; jmp_buf host; } as2_jump_slot;
static _Thread_local as2_jump_slot as2_slots[16];
static _Thread_local unsigned as2_next_slot;

jmp_buf* as2_setjmp_slot(uint32_t guest_jmp_buf) {
    for (unsigned i = 0; i < 16; ++i)
        if (as2_slots[i].guest == guest_jmp_buf) return &as2_slots[i].host;
    as2_jump_slot* s = &as2_slots[as2_next_slot++ % 16];
    s->guest = guest_jmp_buf;
    return &s->host;
}

/* longjmp(jmp_buf, value), cdecl, as the MSVC CRT's: the guest registers the matching _setjmp3 saved
 * (ebp ebx edi esi, esp at the call's return slot, SEH registration at +0x18), eax = value or 1,
 * then back into the function that called _setjmp3. Its __try levels (local unwind) are not run:
 * the only callers here are D3DX's libjpeg error handlers, which have none. */
void as2_longjmp(void) {
    const uint32_t jb = *(const uint32_t*)ADDR(g_esp + 4), value = *(const uint32_t*)ADDR(g_esp + 8);
    for (unsigned i = 0; i < 16; ++i) {
        if (as2_slots[i].guest != jb) continue;
        g_ebp = *(const uint32_t*)ADDR(jb);
        g_ebx = *(const uint32_t*)ADDR(jb + 4);
        g_edi = *(const uint32_t*)ADDR(jb + 8);
        g_esi = *(const uint32_t*)ADDR(jb + 12);
        g_esp = *(const uint32_t*)ADDR(jb + 16) + 4;
        *(uint32_t*)ADDR(g_fs_base) = *(const uint32_t*)ADDR(jb + 0x18);
        g_eax = value ? value : 1;
        longjmp(as2_slots[i].host, 1);
    }
    fprintf(stderr, "[AS2] longjmp to jmp_buf %08x that no _setjmp3 call site recorded\n", jb);
    abort();
}

/* The game's music is Ogg Vorbis decoded on the game thread by a static libvorbis (1.2-era, float build).
 * Lifted with x87 emulation, its two hot spots took ~30% of the game thread in a level: the inverse MDCT and
 * floor0's LSP curve. They are natively compiled here from libvorbis 1.2.0 (mdct.c, lsp.c's FLOAT_LOOKUP
 * path, lookup.c; BSD licence, see NOTICE.md) and run on the guest's own data (config/overrides.txt).
 * The lookup tables are libvorbis' (the exe's COS_LOOKUP at 0x5E549C holds the same values). */
#include <math.h>
#define FLOAT_LOOKUP
#include "vorbis_lookup_data.h"

#define cPI3_8 .38268343236508977175F
#define cPI2_8 .70710678118654752441F
#define cPI1_8 .92387953251128675613F

typedef struct { int32_t n, log2n; uint32_t trig, bitrev; float scale; } guest_mdct_lookup;   /* 20 bytes */

static void mdct_butterfly_8(float* x) {
    float r0 = x[6] + x[2], r1 = x[6] - x[2], r2 = x[4] + x[0], r3 = x[4] - x[0];
    x[6] = r0 + r2; x[4] = r0 - r2;
    r0 = x[5] - x[1]; r2 = x[7] - x[3];
    x[0] = r1 + r0; x[2] = r1 - r0;
    r0 = x[5] + x[1]; r1 = x[7] + x[3];
    x[3] = r2 + r3; x[1] = r2 - r3; x[7] = r1 + r0; x[5] = r1 - r0;
}
static void mdct_butterfly_16(float* x) {
    float r0 = x[1] - x[9], r1 = x[0] - x[8];
    x[8] += x[0]; x[9] += x[1];
    x[0] = (r0 + r1) * cPI2_8; x[1] = (r0 - r1) * cPI2_8;
    r0 = x[3] - x[11]; r1 = x[10] - x[2];
    x[10] += x[2]; x[11] += x[3]; x[2] = r0; x[3] = r1;
    r0 = x[12] - x[4]; r1 = x[13] - x[5];
    x[12] += x[4]; x[13] += x[5];
    x[4] = (r0 - r1) * cPI2_8; x[5] = (r0 + r1) * cPI2_8;
    r0 = x[14] - x[6]; r1 = x[15] - x[7];
    x[14] += x[6]; x[15] += x[7]; x[6] = r0; x[7] = r1;
    mdct_butterfly_8(x); mdct_butterfly_8(x + 8);
}
static void mdct_butterfly_32(float* x) {
    float r0 = x[30] - x[14], r1 = x[31] - x[15];
    x[30] += x[14]; x[31] += x[15]; x[14] = r0; x[15] = r1;
    r0 = x[28] - x[12]; r1 = x[29] - x[13];
    x[28] += x[12]; x[29] += x[13];
    x[12] = r0 * cPI1_8 - r1 * cPI3_8; x[13] = r0 * cPI3_8 + r1 * cPI1_8;
    r0 = x[26] - x[10]; r1 = x[27] - x[11];
    x[26] += x[10]; x[27] += x[11];
    x[10] = (r0 - r1) * cPI2_8; x[11] = (r0 + r1) * cPI2_8;
    r0 = x[24] - x[8]; r1 = x[25] - x[9];
    x[24] += x[8]; x[25] += x[9];
    x[8] = r0 * cPI3_8 - r1 * cPI1_8; x[9] = r1 * cPI3_8 + r0 * cPI1_8;
    r0 = x[22] - x[6]; r1 = x[7] - x[23];
    x[22] += x[6]; x[23] += x[7]; x[6] = r1; x[7] = r0;
    r0 = x[4] - x[20]; r1 = x[5] - x[21];
    x[20] += x[4]; x[21] += x[5];
    x[4] = r1 * cPI1_8 + r0 * cPI3_8; x[5] = r1 * cPI3_8 - r0 * cPI1_8;
    r0 = x[2] - x[18]; r1 = x[3] - x[19];
    x[18] += x[2]; x[19] += x[3];
    x[2] = (r1 + r0) * cPI2_8; x[3] = (r1 - r0) * cPI2_8;
    r0 = x[0] - x[16]; r1 = x[1] - x[17];
    x[16] += x[0]; x[17] += x[1];
    x[0] = r1 * cPI3_8 + r0 * cPI1_8; x[1] = r1 * cPI1_8 - r0 * cPI3_8;
    mdct_butterfly_16(x); mdct_butterfly_16(x + 16);
}
static void mdct_butterfly_first(const float* T, float* x, int points) {
    float* x1 = x + points - 8;
    float* x2 = x + (points >> 1) - 8;
    float r0, r1;
    do {
        r0 = x1[6] - x2[6]; r1 = x1[7] - x2[7]; x1[6] += x2[6]; x1[7] += x2[7];
        x2[6] = r1 * T[1] + r0 * T[0]; x2[7] = r1 * T[0] - r0 * T[1];
        r0 = x1[4] - x2[4]; r1 = x1[5] - x2[5]; x1[4] += x2[4]; x1[5] += x2[5];
        x2[4] = r1 * T[5] + r0 * T[4]; x2[5] = r1 * T[4] - r0 * T[5];
        r0 = x1[2] - x2[2]; r1 = x1[3] - x2[3]; x1[2] += x2[2]; x1[3] += x2[3];
        x2[2] = r1 * T[9] + r0 * T[8]; x2[3] = r1 * T[8] - r0 * T[9];
        r0 = x1[0] - x2[0]; r1 = x1[1] - x2[1]; x1[0] += x2[0]; x1[1] += x2[1];
        x2[0] = r1 * T[13] + r0 * T[12]; x2[1] = r1 * T[12] - r0 * T[13];
        x1 -= 8; x2 -= 8; T += 16;
    } while (x2 >= x);
}
static void mdct_butterfly_generic(const float* T, float* x, int points, int trigint) {
    float* x1 = x + points - 8;
    float* x2 = x + (points >> 1) - 8;
    float r0, r1;
    do {
        r0 = x1[6] - x2[6]; r1 = x1[7] - x2[7]; x1[6] += x2[6]; x1[7] += x2[7];
        x2[6] = r1 * T[1] + r0 * T[0]; x2[7] = r1 * T[0] - r0 * T[1];
        T += trigint;
        r0 = x1[4] - x2[4]; r1 = x1[5] - x2[5]; x1[4] += x2[4]; x1[5] += x2[5];
        x2[4] = r1 * T[1] + r0 * T[0]; x2[5] = r1 * T[0] - r0 * T[1];
        T += trigint;
        r0 = x1[2] - x2[2]; r1 = x1[3] - x2[3]; x1[2] += x2[2]; x1[3] += x2[3];
        x2[2] = r1 * T[1] + r0 * T[0]; x2[3] = r1 * T[0] - r0 * T[1];
        T += trigint;
        r0 = x1[0] - x2[0]; r1 = x1[1] - x2[1]; x1[0] += x2[0]; x1[1] += x2[1];
        x2[0] = r1 * T[1] + r0 * T[0]; x2[1] = r1 * T[0] - r0 * T[1];
        T += trigint;
        x1 -= 8; x2 -= 8;
    } while (x2 >= x);
}
static void mdct_butterflies(const guest_mdct_lookup* init, float* x, int points) {
    const float* T = (const float*)ADDR(init->trig);
    int stages = init->log2n - 5;
    if (--stages > 0) mdct_butterfly_first(T, x, points);
    for (int i = 1; --stages > 0; i++)
        for (int j = 0; j < (1 << i); j++) mdct_butterfly_generic(T, x + (points >> i) * j, points >> i, 4 << i);
    for (int j = 0; j < points; j += 32) mdct_butterfly_32(x + j);
}
static void mdct_bitreverse(const guest_mdct_lookup* init, float* x) {
    const int n = init->n;
    const int32_t* bit = (const int32_t*)ADDR(init->bitrev);
    float* w0 = x;
    float* w1 = x = w0 + (n >> 1);
    const float* T = (const float*)ADDR(init->trig) + n;
    do {
        const float* x0 = x + bit[0];
        const float* x1 = x + bit[1];
        float r0 = x0[1] - x1[1], r1 = x0[0] + x1[0];
        float r2 = r1 * T[0] + r0 * T[1], r3 = r1 * T[1] - r0 * T[0];
        w1 -= 4;
        r0 = (x0[1] + x1[1]) * .5f; r1 = (x0[0] - x1[0]) * .5f;
        w0[0] = r0 + r2; w1[2] = r0 - r2; w0[1] = r1 + r3; w1[3] = r3 - r1;
        x0 = x + bit[2]; x1 = x + bit[3];
        r0 = x0[1] - x1[1]; r1 = x0[0] + x1[0];
        r2 = r1 * T[2] + r0 * T[3]; r3 = r1 * T[3] - r0 * T[2];
        r0 = (x0[1] + x1[1]) * .5f; r1 = (x0[0] - x1[0]) * .5f;
        w0[2] = r0 + r2; w1[0] = r0 - r2; w0[3] = r1 + r3; w1[1] = r3 - r1;
        T += 4; bit += 4; w0 += 4;
    } while (w0 < w1);
}
static void mdct_backward(const guest_mdct_lookup* init, const float* in, float* out) {
    const int n = init->n, n2 = n >> 1, n4 = n >> 2;
    const float* trig = (const float*)ADDR(init->trig);
    const float* iX = in + n2 - 7;
    float* oX = out + n2 + n4;
    const float* T = trig + n4;
    do {
        oX -= 4;
        oX[0] = -iX[2] * T[3] - iX[0] * T[2]; oX[1] = iX[0] * T[3] - iX[2] * T[2];
        oX[2] = -iX[6] * T[1] - iX[4] * T[0]; oX[3] = iX[4] * T[1] - iX[6] * T[0];
        iX -= 8; T += 4;
    } while (iX >= in);
    iX = in + n2 - 8; oX = out + n2 + n4; T = trig + n4;
    do {
        T -= 4;
        oX[0] = iX[4] * T[3] + iX[6] * T[2]; oX[1] = iX[4] * T[2] - iX[6] * T[3];
        oX[2] = iX[0] * T[1] + iX[2] * T[0]; oX[3] = iX[0] * T[0] - iX[2] * T[1];
        iX -= 8; oX += 4;
    } while (iX >= in);
    mdct_butterflies(init, out + n2, n2);
    mdct_bitreverse(init, out);
    {   /* rotate + window */
        float* oX1 = out + n2 + n4;
        float* oX2 = out + n2 + n4;
        const float* iX2 = out;
        T = trig + n2;
        do {
            oX1 -= 4;
            oX1[3] = iX2[0] * T[1] - iX2[1] * T[0]; oX2[0] = -(iX2[0] * T[0] + iX2[1] * T[1]);
            oX1[2] = iX2[2] * T[3] - iX2[3] * T[2]; oX2[1] = -(iX2[2] * T[2] + iX2[3] * T[3]);
            oX1[1] = iX2[4] * T[5] - iX2[5] * T[4]; oX2[2] = -(iX2[4] * T[4] + iX2[5] * T[5]);
            oX1[0] = iX2[6] * T[7] - iX2[7] * T[6]; oX2[3] = -(iX2[6] * T[6] + iX2[7] * T[7]);
            oX2 += 4; iX2 += 8; T += 8;
        } while (iX2 < oX1);
        float* iX3 = out + n2 + n4;
        oX1 = out + n4; oX2 = oX1;
        do {
            oX1 -= 4; iX3 -= 4;
            oX2[0] = -(oX1[3] = iX3[3]); oX2[1] = -(oX1[2] = iX3[2]);
            oX2[2] = -(oX1[1] = iX3[1]); oX2[3] = -(oX1[0] = iX3[0]);
            oX2 += 4;
        } while (oX2 < iX3);
        iX3 = out + n2 + n4; oX1 = out + n2 + n4; oX2 = out + n2;
        do {
            oX1 -= 4;
            oX1[0] = iX3[3]; oX1[1] = iX3[2]; oX1[2] = iX3[1]; oX1[3] = iX3[0];
            iX3 += 4;
        } while (oX1 > oX2);
    }
}

/* vorbis_ftoi: MSVC's fistp in the FPU's round-to-nearest mode (lrint here). */
static int vorbis_ftoi(double f) { return (int)lrint(f); }
static float vorbis_coslook(float a) {
    double d = a * (.31830989 * (float)COS_LOOKUP_SZ);
    int i = vorbis_ftoi(d - .5);
    return COS_LOOKUP[i] + (d - i) * (COS_LOOKUP[i + 1] - COS_LOOKUP[i]);
}
static float vorbis_invsqlook(float a) {
    double d = a * (2.f * (float)INVSQ_LOOKUP_SZ) - (float)INVSQ_LOOKUP_SZ;
    int i = vorbis_ftoi(d - .5f);
    return INVSQ_LOOKUP[i] + (d - i) * (INVSQ_LOOKUP[i + 1] - INVSQ_LOOKUP[i]);
}
static float vorbis_invsq2explook(int a) { return INVSQ2EXP_LOOKUP[a - INVSQ2EXP_LOOKUP_MIN]; }
static float vorbis_fromdBlook(float a) {
    int i = vorbis_ftoi(a * ((float)(-(1 << FROMdB2_SHIFT))) - .5f);
    return (i < 0) ? 1.f : ((i >= (FROMdB_LOOKUP_SZ << FROMdB_SHIFT)) ? 0.f : FROMdB_LOOKUP[i >> FROMdB_SHIFT] * FROMdB2_LOOKUP[i & FROMdB2_MASK]);
}
static void vorbis_lsp_to_curve(float* curve, const int32_t* map, int n, int ln, float* lsp, int m, float amp, float ampoffset) {
    float wdel = 3.14159265358979323846 / ln;
    for (int i = 0; i < m; i++) lsp[i] = vorbis_coslook(lsp[i]);
    int i = 0;
    while (i < n) {
        int k = map[i], qexp;
        float p = .7071067812f, q = .7071067812f;
        float w = vorbis_coslook(wdel * k);
        const float* ftmp = lsp;
        int c = m >> 1;
        do { q *= ftmp[0] - w; p *= ftmp[1] - w; ftmp += 2; } while (--c);
        if (m & 1) { q *= ftmp[0] - w; q *= q; p *= p * (1.f - w * w); }
        else { q *= q * (1.f + w); p *= p * (1.f - w); }
        q = frexp(p + q, &qexp);
        q = vorbis_fromdBlook(amp * vorbis_invsqlook(q) * vorbis_invsq2explook(qexp + m) - ampoffset);
        do { curve[i++] *= q; } while (map[i] == k);
    }
}

/* cdecl entry points: arguments above the return address, which the callee's ret pops. */
#define AS2_ARG(n) (*(const uint32_t*)ADDR(g_esp + 4 + 4 * (n)))
static float as2_argf(int n) { float f; uint32_t v = AS2_ARG(n); memcpy(&f, &v, 4); return f; }
/* mdct_backward(mdct_lookup*, float* in, float* out) */
static void as2_mdct_backward(void) {
    mdct_backward((const guest_mdct_lookup*)ADDR(AS2_ARG(0)), (const float*)ADDR(AS2_ARG(1)), (float*)ADDR(AS2_ARG(2)));
    g_esp += 4;
}
/* vorbis_lsp_to_curve(float* curve, int* map, int n, int ln, float* lsp, int m, float amp, float ampoffset) */
static void as2_lsp_to_curve(void) {
    vorbis_lsp_to_curve((float*)ADDR(AS2_ARG(0)), (const int32_t*)ADDR(AS2_ARG(1)), (int32_t)AS2_ARG(2), (int32_t)AS2_ARG(3),
                        (float*)ADDR(AS2_ARG(4)), (int32_t)AS2_ARG(5), as2_argf(6), as2_argf(7));
    g_esp += 4;
}

void fo2_video_decode_frame(void);   /* fo2_video.c */

nfs_override_fn nfs_override_lookup(uint32_t va) {
    /* Alien Shooter 2's addresses were longjmp 0x0056FC68, mdct_backward 0x00564060, lsp_to_curve 0x00569970.
       FlatOut2.exe's (FMOD 3.75 links the same libvorbis) are filled in once found. */
    switch (va) {
    case 0x004C8890u: return fo2_video_decode_frame;
    default: (void)as2_longjmp; (void)as2_mdct_backward; (void)as2_lsp_to_curve; return NULL;
    }
}
int nfs_override_count(void) { return 1; }

void nfs_widescreen_init(const char* game_root) { (void)game_root; }
void nfs_widescreen_set_resolution(unsigned width, unsigned height) { (void)width; (void)height; }
int nfs_widescreen_hud_entries(void) { return 0; }
void nfs_widescreen_set_minimap_offset(float dy) { (void)dy; }
void nfs_widescreen_set_frame_rate(unsigned fps) { (void)fps; }
void nfs_widescreen_apply_frame_rate(void) {}
