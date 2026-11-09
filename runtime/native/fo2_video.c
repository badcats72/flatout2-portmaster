/* FlatOut 2's videos (menu backgrounds, attract demos) are On2 VP3.1 in AVI. The game's own decoder, lifted, gave
 * block artifacts and slowed the menus down, so its frame function is replaced here (config/overrides.txt): the
 * compressed frame goes to the VP3 decoder of the FFmpeg the system already has, and the picture is written where
 * the player expects it, a 24-bit bottom-up DIB.
 *
 * libavcodec is opened at run time (no build dependency, and no FFmpeg in the port): the sonames are tried newest
 * first. Only fields that have not moved in those versions are touched: AVCodecContext.codec_tag, AVPacket.data
 * and size, AVFrame.data, linesize, width, height and format. Without FFmpeg the videos stay off (recomp_hook,
 * 0x451386). */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "recomp_types.h"

typedef struct {
    void* (*find_decoder_by_name)(const char*);
    void* (*alloc_context3)(const void*);
    int (*open2)(void*, const void*, void**);
    void (*free_context)(void**);
    int (*send_packet)(void*, const void*);
    int (*receive_frame)(void*, void*);
    void* (*packet_alloc)(void);
    void (*packet_free)(void**);
    void* (*frame_alloc)(void);
    void (*frame_free)(void**);
    void (*frame_unref)(void*);
    int (*opt_set)(void*, const char*, const char*, int);
    const void* codec;
} ffmpeg_api;

static ffmpeg_api ff;
static int ff_state;   /* 0 not tried, 1 loaded, -1 not available */

int fo2_video_available(void) {
    static const char* const pairs[][2] = {
        {"libavcodec.so.61", "libavutil.so.59"},
        {"libavcodec.so.60", "libavutil.so.58"},
        {"libavcodec.so.59", "libavutil.so.57"},
        {"libavcodec.so.58", "libavutil.so.56"},
    };
    if (ff_state) return ff_state > 0;
    ff_state = -1;
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; ++i) {
        void* util = dlopen(pairs[i][1], RTLD_NOW | RTLD_GLOBAL);
        void* lav = util ? dlopen(pairs[i][0], RTLD_NOW | RTLD_GLOBAL) : NULL;
        if (!lav) continue;
#define LOAD(lib, field, name) *(void**)&ff.field = dlsym(lib, name); if (!ff.field) continue
        LOAD(lav, find_decoder_by_name, "avcodec_find_decoder_by_name");
        LOAD(lav, alloc_context3, "avcodec_alloc_context3");
        LOAD(lav, open2, "avcodec_open2");
        LOAD(lav, free_context, "avcodec_free_context");
        LOAD(lav, send_packet, "avcodec_send_packet");
        LOAD(lav, receive_frame, "avcodec_receive_frame");
        LOAD(lav, packet_alloc, "av_packet_alloc");
        LOAD(lav, packet_free, "av_packet_free");
        LOAD(util, frame_alloc, "av_frame_alloc");
        LOAD(util, frame_free, "av_frame_free");
        LOAD(util, frame_unref, "av_frame_unref");
        LOAD(util, opt_set, "av_opt_set");
#undef LOAD
        ff.codec = ff.find_decoder_by_name("vp3");
        if (!ff.codec) continue;
        ff_state = 1;
        fprintf(stderr, "[FO2 VIDEO] native VP3 decoder: %s\n", pairs[i][0]);
        break;
    }
    if (ff_state < 0) fprintf(stderr, "[FO2 VIDEO] no usable FFmpeg: videos stay off\n");
    return ff_state > 0;
}

typedef struct { uint32_t player; void* ctx; void* frame; void* packet; int width, height; } video_slot;
static video_slot slots[4];

static void slot_close(video_slot* s) {
    if (s->ctx) ff.free_context(&s->ctx);
    if (s->frame) ff.frame_free(&s->frame);
    if (s->packet) ff.packet_free(&s->packet);
    memset(s, 0, sizeof *s);
}

/* The player is closing its video (0x4C8320, config/hooks.txt). */
void fo2_video_close(uint32_t player) {
    for (int i = 0; i < 4; ++i) if (slots[i].player == player) slot_close(&slots[i]);
}

static video_slot* slot_for(uint32_t player, int width, int height, uint32_t tag) {
    video_slot* s = NULL;
    for (int i = 0; i < 4 && !s; ++i) if (slots[i].player == player) s = &slots[i];
    if (s && (s->width != width || s->height != height)) slot_close(s);
    if (s && s->ctx) return s;
    if (!s) for (int i = 0; i < 4 && !s; ++i) if (!slots[i].ctx) s = &slots[i];
    if (!s) { slot_close(&slots[0]); s = &slots[0]; }
    char size[32];
    snprintf(size, sizeof size, "%dx%d", width, height);
    s->ctx = ff.alloc_context3(ff.codec);
    s->frame = ff.frame_alloc();
    s->packet = ff.packet_alloc();
    if (!s->ctx || !s->frame || !s->packet) goto fail;
    *(uint32_t*)((char*)s->ctx + 28) = tag;   /* AVCodecContext.codec_tag */
    /* Slow consoles (launcher): FO2_VIDEO_THREADS=N decodes on N threads, FO2_VIDEO_FAST=1 skips the deblocking filter. */
    { const char* e = getenv("FO2_VIDEO_THREADS"); if (e && e[0]) ff.opt_set(s->ctx, "threads", e, 0); }
    { const char* e = getenv("FO2_VIDEO_FAST"); if (e && e[0] == '1') ff.opt_set(s->ctx, "skip_loop_filter", "all", 0); }
    if (ff.opt_set(s->ctx, "video_size", size, 0) < 0 || ff.open2(s->ctx, ff.codec, NULL) < 0) goto fail;
    s->player = player; s->width = width; s->height = height;
    fprintf(stderr, "[FO2 VIDEO] %dx%d stream opened (fourcc %08x)\n", width, height, tag);
    return s;
fail:
    fprintf(stderr, "[FO2 VIDEO] could not open the %dx%d decoder\n", width, height);
    slot_close(s);
    return NULL;
}

/* YUV 4:2:0 (BT.601) to the player's 24-bit DIB, whose last row is the picture's top row.
 * FO2_VIDEO_FLIP=1 writes the rows the other way, FO2_VIDEO_RGB=1 swaps red and blue. */
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

static void yuv_to_bgr(const uint8_t* const* plane, const int* pitch, int w, int h, uint8_t* out, int outPitch, int rows, int fullRange) {
    static int swapRB = -1, flipEnv = -1;
    if (swapRB < 0) { const char* e = getenv("FO2_VIDEO_RGB"); swapRB = e && e[0] == '1'; e = getenv("FO2_VIDEO_FLIP"); flipEnv = e && e[0] == '1'; }
    const int r = swapRB ? 0 : 2, b = swapRB ? 2 : 0;
    const int ys = fullRange ? 256 : 298, yo = fullRange ? 0 : 16;
    if (h > rows) h = rows;
    for (int y = 0; y < h; ++y) {
        const int row = flipEnv ? y : rows - 1 - y;
        uint8_t* d = out + (size_t)row * outPitch;
        const uint8_t* py = plane[0] + (ptrdiff_t)y * pitch[0];
        const uint8_t* pu = plane[1] + (ptrdiff_t)(y >> 1) * pitch[1];
        const uint8_t* pv = plane[2] + (ptrdiff_t)(y >> 1) * pitch[2];
        int x = 0;
#if defined(__aarch64__)
        /* Eight pixels at a time, the same conversion in 6-bit fixed point: 298/256 is 74/64 and so on. */
        const int16x8_t yScale = vdupq_n_s16(fullRange ? 64 : 74), yOffset = vdupq_n_s16((int16_t)yo), mid = vdupq_n_s16(128);
        for (; x + 8 <= w; x += 8, d += 24) {
            const uint8x8_t u8 = vld1_u8(pu + (x >> 1)), v8 = vld1_u8(pv + (x >> 1));
            const int16x8_t luma = vmulq_s16(vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(py + x))), yOffset), yScale);
            const int16x8_t u16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vzip1_u8(u8, u8))), mid);
            const int16x8_t v16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vzip1_u8(v8, v8))), mid);
            uint8x8x3_t pixel;
            pixel.val[r] = vqshrun_n_s16(vqaddq_s16(luma, vmulq_n_s16(v16, 102)), 6);
            pixel.val[1] = vqshrun_n_s16(vqsubq_s16(luma, vaddq_s16(vmulq_n_s16(u16, 25), vmulq_n_s16(v16, 52))), 6);
            pixel.val[b] = vqshrun_n_s16(vqaddq_s16(luma, vmulq_n_s16(u16, 129)), 6);
            vst3_u8(d, pixel);
        }
#endif
        for (; x < w; ++x, d += 3) {
            const int c = ys * (py[x] - yo), u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
            d[r] = clamp8((c + 409 * v + 128) >> 8);
            d[1] = clamp8((c - 100 * u - 208 * v + 128) >> 8);
            d[b] = clamp8((c + 516 * u + 128) >> 8);
        }
    }
}

#define R32(a) (*(uint32_t*)ADDR(a))

/* 0x4C8890 (eax = the player): decodes the next queued frame into the DIB at +0x2DC (width +0x2B8, height +0x2BC,
 * 24 bits). The queue is at +0x5C: capacity +0x70, count +0x74, read index +0x78, entries {data, size} at +0x7C.
 * Returns 0 when there is nothing to show, 1 otherwise, as the original does. */
static void fo2_video_pop(uint32_t queue) {
    const uint32_t index = R32(queue + 0x78);
    R32(queue + 0x74) -= 1;
    R32(queue + index * 8 + 0x80) = 0;
    R32(queue + 0x78) = index + 1 >= R32(queue + 0x70) ? 0 : index + 1;
}

void fo2_video_decode_frame(void) {
    const uint32_t player = g_eax, queue = player + 0x5C;
    g_esp += 4;
    R32(player + 0x2A8) = 1;
    if ((int32_t)R32(player + 0xD0) < 1) { g_eax = 0; return; }
    const uint32_t index = R32(queue + 0x78);
    const uint32_t data = R32(queue + index * 8 + 0x7C), size = R32(queue + index * 8 + 0x80);
    if (!size) { fo2_video_pop(queue); g_eax = 0; return; }
    if (fo2_video_available()) {
        const int width = abs((int32_t)R32(player + 0xA8)), height = abs((int32_t)R32(player + 0xAC));
        video_slot* s = slot_for(player, width, height, R32(player + 0xB4));
        if (s) {
            struct packet_head { void* buf; int64_t pts, dts; uint8_t* data; int size; }* p = (struct packet_head*)s->packet;
            p->data = (uint8_t*)ADDR(data);
            p->size = (int)size;
            int sent = ff.send_packet(s->ctx, s->packet);
            p->data = NULL; p->size = 0;
            if (sent >= 0 && ff.receive_frame(s->ctx, s->frame) >= 0) {
                struct frame_head { uint8_t* data[8]; int linesize[8]; uint8_t** extended; int width, height, samples, format; }* f = (struct frame_head*)s->frame;
                if (f->format == 0 /* yuv420p */ || f->format == 12 /* yuvj420p */) {
                    const uint32_t out = R32(player + 0x2DC);
                    const int w = (int)R32(player + 0x2B8), rows = abs((int32_t)R32(player + 0x2BC));
                    const int shown = f->width < w ? f->width : w;
                    yuv_to_bgr((const uint8_t* const*)f->data, f->linesize, shown, f->height, (uint8_t*)ADDR(out), w * 3, rows, f->format == 12);
                }
                ff.frame_unref(s->frame);
            }
        }
    }
    fo2_video_pop(queue);
    g_eax = 1;
}
