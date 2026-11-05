/*
 * ps3recomp - the polygon offset in rsx_state (rsx_commands.c)
 *
 * SET_POLY_OFFSET_{POINT,LINE,FILL}_ENABLE (0x0A60/0x0A64/0x0A68),
 * SET_POLYGON_OFFSET_SCALE_FACTOR (0x0A78) and SET_POLYGON_OFFSET_BIAS
 * (0x0A7C) are kept, and rsx_poly_offset_z24 gives what a draw of each
 * primitive is drawn with on a 24-bit depth buffer: the factor as it is, the
 * bias in 2^-24 steps (a Z16 surface's step is 256 of them), nothing when the
 * enable for that primitive is off. The D3D12 legacy path builds its
 * DepthBias / SlopeScaledDepthBias from it.
 *
 * Build (standalone):
 *   cc -std=gnu17 -I include -I libs/video libs/video/tests/test_rsx_poly_offset.c \
 *      libs/video/rsx_commands.c -lm -o /tmp/test_rsx_poly_offset
 */
#include "rsx_commands.h"

#include <stdio.h>
#include <string.h>

uint32_t ppu_hle_inject_base;
void ps3_ms(const char* key) { (void)key; }
uint32_t gcm_io_offset_to_ea(uint32_t io) { return 0x10000000u + io; }
unsigned long long ps3_ms_now(void) { return 0; }
void vm_write32(uint32_t addr, uint32_t value) { (void)addr; (void)value; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } \
                            else printf("ok   %s\n", what); } while (0)

static u32 fbits(float f) { u32 u; memcpy(&u, &f, sizeof u); return u; }

int main(void)
{
    rsx_state st;
    rsx_state_init(&st);
    float slope, units;

    CHECK(!rsx_poly_offset_z24(&st, 5, &slope, &units) && slope == 0.0f && units == 0.0f,
          "off after init");

    /* A Z24S8 surface (SET_SURFACE_FORMAT depth 2), fill offset 2.0 / 4.0. */
    rsx_process_method(&st, NV4097_SET_SURFACE_FORMAT, (2u << 5) | 5u);
    CHECK(rsx_process_method(&st, NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR, fbits(2.0f)) == 0,
          "SCALE_FACTOR is a known method");
    CHECK(rsx_process_method(&st, NV4097_SET_POLYGON_OFFSET_BIAS, fbits(4.0f)) == 0,
          "BIAS is a known method");
    CHECK(!rsx_poly_offset_z24(&st, 5, &slope, &units), "factor and bias alone do nothing");
    CHECK(rsx_process_method(&st, NV4097_SET_POLY_OFFSET_FILL_ENABLE, 1) == 0,
          "FILL_ENABLE is a known method");
    CHECK(st.poly_offset_enable == 4u, "fill is bit 2");
    CHECK(rsx_poly_offset_z24(&st, 5, &slope, &units) && slope == 2.0f && units == 4.0f,
          "triangles: factor 2, bias 4 steps on Z24");
    CHECK(rsx_poly_offset_z24(&st, 8, &slope, &units) && units == 4.0f, "quads use fill too");
    CHECK(!rsx_poly_offset_z24(&st, 1, &slope, &units), "points: their own enable is off");
    CHECK(!rsx_poly_offset_z24(&st, 2, &slope, &units) &&
          !rsx_poly_offset_z24(&st, 4, &slope, &units), "lines: their own enable is off");

    rsx_process_method(&st, NV4097_SET_POLY_OFFSET_LINE_ENABLE, 1);
    rsx_process_method(&st, NV4097_SET_POLY_OFFSET_POINT_ENABLE, 1);
    CHECK(st.poly_offset_enable == 7u, "all three enables");
    CHECK(rsx_poly_offset_z24(&st, 3, &slope, &units) && units == 4.0f, "line loop: line enable");
    CHECK(rsx_poly_offset_z24(&st, 1, &slope, &units) && units == 4.0f, "points: point enable");

    /* A Z16 surface: one of its steps is 256 of a D24 buffer's. */
    rsx_process_method(&st, NV4097_SET_SURFACE_FORMAT, (1u << 5) | 5u);
    CHECK(rsx_poly_offset_z24(&st, 5, &slope, &units) && slope == 2.0f && units == 1024.0f,
          "Z16: bias 4 -> 1024 steps of 2^-24");

    rsx_process_method(&st, NV4097_SET_POLY_OFFSET_FILL_ENABLE, 0);
    CHECK(st.poly_offset_enable == 3u, "fill off, the others stay");
    CHECK(!rsx_poly_offset_z24(&st, 5, &slope, &units) && slope == 0.0f && units == 0.0f,
          "triangles after fill off");

    /* Not a number counts as 0; both 0 is off. */
    rsx_process_method(&st, NV4097_SET_POLY_OFFSET_FILL_ENABLE, 1);
    rsx_process_method(&st, NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR, 0x7FC00000u);
    rsx_process_method(&st, NV4097_SET_POLYGON_OFFSET_BIAS, 0x7FC00000u);
    CHECK(!rsx_poly_offset_z24(&st, 5, &slope, &units) && slope == 0.0f && units == 0.0f,
          "NaN factor and bias: off");
    rsx_process_method(&st, NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR, fbits(-1.5f));
    CHECK(rsx_poly_offset_z24(&st, 5, &slope, &units) && slope == -1.5f && units == 0.0f,
          "a negative factor with a NaN bias");

    printf("%s\n", fails ? "FAILED" : "PASS: the polygon offset is kept and converted");
    return fails ? 1 : 0;
}
