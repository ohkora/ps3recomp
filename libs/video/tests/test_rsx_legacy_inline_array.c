/*
 * ps3recomp - INLINE_ARRAY primitives on the legacy (vtable) path
 *
 * A primitive can carry its vertices in the FIFO itself: NV4097_INLINE_ARRAY
 * (0x1818) words between BEGIN_END(prim) and BEGIN_END(0), with no vertex
 * array, offset or (first, count). rsx_process_method used to ignore those
 * words, so the backend never saw the primitive. This feeds rsx_commands.c
 * real method streams with a stub backend whose draw_arrays reads every
 * vertex back through rsx_fetch_attrib (rsx_vertex_fetch.c), the reader the
 * D3D12 backend's vertex-program path uses, and checks:
 *   - an inline primitive becomes one draw_arrays(prim, 0, n) at its END,
 *     and each vertex reads back from the inline words: declared stride,
 *     packed layout (no stride declared), the 11/11/10 CMP type, a
 *     disabled attribute (constant register) and a vertex past the stream;
 *   - the inline source is gone after the draw: an ordinary DRAW_ARRAYS
 *     after it reads its vertex array from guest memory;
 *   - a BEGIN/END with no inline words draws nothing, INLINE_ARRAY outside
 *     BEGIN/END is ignored, a trailing partial vertex is not drawn, and a
 *     stream over RSX_INLINE_MAX_BYTES is dropped and counted while the
 *     next primitive still draws.
 *
 * Build (standalone):
 *   cc -std=gnu17 -I include -I libs/video libs/video/tests/test_rsx_legacy_inline_array.c \
 *      libs/video/rsx_commands.c libs/video/rsx_vertex_fetch.c -lm -o /tmp/test_rsx_legacy_inline_array
 */
#include "rsx_commands.h"
#include "rsx_vertex_fetch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What rsx_commands.c and rsx_vertex_fetch.c link against. */
uint32_t ppu_hle_inject_base;
void ps3_ms(const char* key) { (void)key; }
uint32_t gcm_io_offset_to_ea(uint32_t io) { return 0x10000000u + io; }
unsigned long long ps3_ms_now(void) { return 1234; }
void vm_write32(uint32_t addr, uint32_t value) { (void)addr; (void)value; }

/* Guest memory: 1 MB, LOCAL offsets map straight onto it. */
#define GUEST_BYTES 0x100000u
u8* vm_base;
u32 ppu_vm_size = GUEST_BYTES;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset & (GUEST_BYTES - 1u); }
u32 cellGcmResolveOffset(u32 offset) { return offset & (GUEST_BYTES - 1u); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("[FAIL] " __VA_ARGS__); printf("\n"); } } while (0)

/* The stub backend: every draw_arrays call, with each vertex read back the way
 * the D3D12 VP path reads it. */
#define MAXV 64
static struct {
    u32 n, prim, first, count, inline_seen;
    float v[MAXV][4][4];       /* [vertex][slot][lane], slots: attrs 0, 3, 8, 9 */
} s_draw;
static rsx_state st;
static const int k_slots[4] = { 0, 3, 8, 9 };

static void draw_arrays(void* ud, u32 prim, u32 first, u32 count)
{
    (void)ud;
    s_draw.n++;
    s_draw.prim = prim; s_draw.first = first; s_draw.count = count;
    s_draw.inline_seen = st.inline_src != NULL;
    for (u32 k = 0; k <= count && k < MAXV; k++)      /* one past the end too */
        for (int s = 0; s < 4; s++)
            rsx_fetch_attrib(&st, k_slots[s], first + k, s_draw.v[k][s]);
}
static void draw_indexed(void* ud, u32 prim, u32 first, u32 count)
{
    (void)ud; (void)prim; (void)first; (void)count;
    fails++; printf("[FAIL] draw_indexed called\n");
}
static rsx_backend s_be;

static u32 fbits(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static void m(u32 method, u32 data) { rsx_process_method(&st, method, data); }
/* VTXFMT: type | size << 4 | stride << 8 */
static void fmt(u32 attr, u32 type, u32 size, u32 stride)
{
    m(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + attr * 4, type | size << 4 | stride << 8);
}
static void reset(void)
{
    rsx_state_init(&st);
    memset(&s_draw, 0, sizeof s_draw);
    for (u32 a = 0; a < RSX_MAX_VERTEX_ATTRIBS; a++) fmt(a, 2, 0, 0);   /* PSGL's "off": float, size 0 */
}
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

int main(void)
{
    vm_base = (u8*)calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    memset(&s_be, 0, sizeof s_be);
    s_be.draw_arrays = draw_arrays;
    s_be.draw_indexed = draw_indexed;
    rsx_set_backend(&s_be);

    /* 1. Declared stride: attr 0 float3, attr 3 UB4, stride 16, three
     *    TRIANGLES vertices. Attr 3 is the 4th word of each vertex. */
    reset();
    fmt(0, 2, 3, 16); fmt(3, 4, 4, 16);
    m(NV4097_SET_BEGIN_END, 5);
    for (u32 k = 0; k < 3; k++) {
        m(NV4097_INLINE_ARRAY, fbits(1.0f + k));
        m(NV4097_INLINE_ARRAY, fbits(-2.0f * k));
        m(NV4097_INLINE_ARRAY, fbits(0.25f));
        m(NV4097_INLINE_ARRAY, 0xFF000000u | (k * 0x40u) << 16 | 0x80u << 8 | 0x33u);
    }
    CHECK(s_draw.n == 0, "1: drew before the END (%u)", s_draw.n);
    m(NV4097_SET_BEGIN_END, 0);
    CHECK(s_draw.n == 1 && s_draw.prim == 5 && s_draw.first == 0 && s_draw.count == 3,
          "1: draw n=%u prim=%u first=%u count=%u, want one TRIANGLES draw of 3", s_draw.n,
          s_draw.prim, s_draw.first, s_draw.count);
    CHECK(s_draw.inline_seen, "1: inline source not set during the draw");
    CHECK(st.inline_src == NULL, "1: inline source still set after the draw");
    for (u32 k = 0; k < 3; k++) {
        const float* p = s_draw.v[k][0]; const float* c = s_draw.v[k][1];
        CHECK(near(p[0], 1.0f + k) && near(p[1], -2.0f * k) && near(p[2], 0.25f) && near(p[3], 1.0f),
              "1: vertex %u position (%g,%g,%g,%g)", k, p[0], p[1], p[2], p[3]);
        CHECK(near(c[0], 1.0f) && near(c[1], (k * 0x40u) / 255.0f) && near(c[2], 0x80 / 255.0f) &&
              near(c[3], 0x33 / 255.0f), "1: vertex %u colour (%g,%g,%g,%g)", k, c[0], c[1], c[2], c[3]);
        CHECK(near(s_draw.v[k][2][3], 1.0f) && near(s_draw.v[k][2][0], 0.0f),
              "1: vertex %u disabled attr 8 is not the constant (0,0,0,1)", k);
    }
    CHECK(near(s_draw.v[3][0][0], 0.0f) && near(s_draw.v[3][0][3], 1.0f),
          "1: a vertex past the stream read (%g, ..., %g), want the constant", s_draw.v[3][0][0], s_draw.v[3][0][3]);

    /* 2. Packed layout (no stride declared): attr 0 float2, attr 8 float2,
     *    attr 9 SF2 (half), 20 bytes a vertex, four QUADS vertices. A constant
     *    attribute value set for the disabled attr 3 is what it reads. */
    reset();
    fmt(0, 2, 2, 0); fmt(8, 2, 2, 0); fmt(9, 3, 2, 0);
    m(0x1C00u + 3 * 16 + 0, fbits(0.5f)); m(0x1C00u + 3 * 16 + 4, fbits(0.6f));
    m(0x1C00u + 3 * 16 + 8, fbits(0.7f)); m(0x1C00u + 3 * 16 + 12, fbits(0.8f));
    m(NV4097_SET_BEGIN_END, 8);
    for (u32 k = 0; k < 4; k++) {
        m(NV4097_INLINE_ARRAY, fbits(10.0f * k));
        m(NV4097_INLINE_ARRAY, fbits(20.0f * k));
        m(NV4097_INLINE_ARRAY, fbits(0.125f * k));
        m(NV4097_INLINE_ARRAY, fbits(1.0f - 0.125f * k));
        m(NV4097_INLINE_ARRAY, 0x3C004000u);              /* half 1.0, half 2.0 */
    }
    m(NV4097_SET_BEGIN_END, 0);
    CHECK(s_draw.n == 1 && s_draw.prim == 8 && s_draw.count == 4,
          "2: draw n=%u prim=%u count=%u, want one QUADS draw of 4", s_draw.n, s_draw.prim, s_draw.count);
    CHECK(st.inline_stride == 20 || s_draw.n == 0, "2: stride %u, want the packed 20", st.inline_stride);
    for (u32 k = 0; k < 4; k++) {
        CHECK(near(s_draw.v[k][0][0], 10.0f * k) && near(s_draw.v[k][0][1], 20.0f * k),
              "2: vertex %u position (%g,%g)", k, s_draw.v[k][0][0], s_draw.v[k][0][1]);
        CHECK(near(s_draw.v[k][2][0], 0.125f * k) && near(s_draw.v[k][2][1], 1.0f - 0.125f * k),
              "2: vertex %u texcoord (%g,%g)", k, s_draw.v[k][2][0], s_draw.v[k][2][1]);
        CHECK(near(s_draw.v[k][3][0], 1.0f) && near(s_draw.v[k][3][1], 2.0f),
              "2: vertex %u half attr 9 (%g,%g)", k, s_draw.v[k][3][0], s_draw.v[k][3][1]);
        CHECK(near(s_draw.v[k][1][0], 0.5f) && near(s_draw.v[k][1][3], 0.8f),
              "2: vertex %u disabled attr 3 (%g..%g), want the constant 0.5..0.8", k,
              s_draw.v[k][1][0], s_draw.v[k][1][3]);
    }

    /* 3. An ordinary DRAW_ARRAYS afterwards reads its array from guest memory,
     *    not from the last inline stream. */
    {
        const u32 off = 0x2000u;
        for (u32 k = 0; k < 2; k++) {
            const u32 w0 = fbits(100.0f + k), w1 = fbits(200.0f + k);
            u8* q = vm_base + off + k * 8;
            q[0] = w0 >> 24; q[1] = w0 >> 16; q[2] = w0 >> 8; q[3] = w0;
            q[4] = w1 >> 24; q[5] = w1 >> 16; q[6] = w1 >> 8; q[7] = w1;
        }
        memset(&s_draw, 0, sizeof s_draw);
        fmt(0, 2, 2, 8); fmt(8, 2, 0, 0); fmt(9, 2, 0, 0);
        m(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, off);
        m(NV4097_SET_BEGIN_END, 5);
        m(NV4097_DRAW_ARRAYS, (2u - 1u) << 24 | 0u);
        m(NV4097_SET_BEGIN_END, 0);
        CHECK(s_draw.n == 1 && !s_draw.inline_seen && s_draw.count == 2,
              "3: array draw n=%u inline=%u count=%u", s_draw.n, s_draw.inline_seen, s_draw.count);
        CHECK(near(s_draw.v[0][0][0], 100.0f) && near(s_draw.v[1][0][1], 201.0f),
              "3: array draw read (%g, %g), want 100 and 201 from guest memory",
              s_draw.v[0][0][0], s_draw.v[1][0][1]);
    }

    /* 4. CMP (11/11/10 normalised) at attr 3, after a float3 attr 0, stride 16
     *    declared on attr 0 only (the declared stride is the largest). */
    reset();
    fmt(0, 2, 3, 16); fmt(3, 6, 1, 0);
    m(NV4097_SET_BEGIN_END, 5);
    for (u32 k = 0; k < 3; k++) {
        m(NV4097_INLINE_ARRAY, fbits(1.0f)); m(NV4097_INLINE_ARRAY, fbits(2.0f));
        m(NV4097_INLINE_ARRAY, fbits(3.0f));
        m(NV4097_INLINE_ARRAY, (0x1FFu << 22) | (0x400u << 11) | 0x3FFu);   /* z +max, y -min, x +max */
    }
    m(NV4097_SET_BEGIN_END, 0);
    CHECK(s_draw.n == 1 && s_draw.count == 3, "4: draw n=%u count=%u", s_draw.n, s_draw.count);
    CHECK(near(s_draw.v[2][1][0], 1.0f) && near(s_draw.v[2][1][1], -1024.0f / 1023.0f) &&
          near(s_draw.v[2][1][2], 1.0f), "4: CMP read (%g,%g,%g)",
          s_draw.v[2][1][0], s_draw.v[2][1][1], s_draw.v[2][1][2]);

    /* 5. No inline words: no draw. Words outside BEGIN/END: ignored. A
     *    trailing partial vertex: not drawn. */
    reset();
    fmt(0, 2, 2, 8);
    m(NV4097_INLINE_ARRAY, fbits(9.0f));
    m(NV4097_SET_BEGIN_END, 5);
    m(NV4097_SET_BEGIN_END, 0);
    CHECK(s_draw.n == 0, "5: an empty BEGIN/END drew %u times", s_draw.n);
    m(NV4097_SET_BEGIN_END, 5);
    for (u32 w = 0; w < 7; w++) m(NV4097_INLINE_ARRAY, fbits(1.0f + w));   /* 3.5 vertices */
    m(NV4097_SET_BEGIN_END, 0);
    CHECK(s_draw.n == 1 && s_draw.count == 3, "5: partial stream drew n=%u count=%u, want 3", s_draw.n, s_draw.count);
    CHECK(near(s_draw.v[0][0][0], 1.0f) && near(s_draw.v[2][0][1], 6.0f),
          "5: first word outside BEGIN/END leaked in (%g, %g)", s_draw.v[0][0][0], s_draw.v[2][0][1]);

    /* 6. A stream over the cap is dropped and counted; the next one draws. */
    reset();
    fmt(0, 2, 4, 16);
    m(NV4097_SET_BEGIN_END, 5);
    for (u32 w = 0; w < RSX_INLINE_MAX_BYTES / 4u + 8u; w++) m(NV4097_INLINE_ARRAY, fbits(1.0f));
    m(NV4097_SET_BEGIN_END, 0);
    CHECK(s_draw.n == 0 && st.inline_dropped == 1, "6: oversized stream drew %u times, dropped %u",
          s_draw.n, st.inline_dropped);
    m(NV4097_SET_BEGIN_END, 5);
    for (u32 w = 0; w < 12; w++) m(NV4097_INLINE_ARRAY, fbits(4.0f));
    m(NV4097_SET_BEGIN_END, 0);
    CHECK(s_draw.n == 1 && s_draw.count == 3 && near(s_draw.v[1][0][2], 4.0f),
          "6: the next stream drew n=%u count=%u", s_draw.n, s_draw.count);

    printf("[%s] INLINE_ARRAY primitives reach the backend as draws that read the FIFO's vertices\n",
           fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
