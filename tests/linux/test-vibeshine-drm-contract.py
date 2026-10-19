#!/usr/bin/env python3
"""Validate the packaged Vibeshine DRM source and its deterministic HDR EDID."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


class ContractError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def require_source(source: str, needle: str, filename: str) -> None:
    require(needle in source, f"{filename} is missing {needle!r}")


def decode_manufacturer(edid: bytes) -> str:
    encoded = int.from_bytes(edid[8:10], byteorder="big")
    return "".join(chr(((encoded >> shift) & 0x1F) + ord("A") - 1) for shift in (10, 5, 0))


def cta_data_blocks(cta: bytes) -> list[tuple[int, bytes]]:
    require(cta[0] == 0x02, "EDID extension is not a CTA-861 extension")
    require(4 <= cta[2] <= 127, "CTA data-block collection has an invalid end offset")

    blocks: list[tuple[int, bytes]] = []
    offset = 4
    while offset < cta[2]:
        header = cta[offset]
        length = header & 0x1F
        end = offset + 1 + length
        require(end <= cta[2], "CTA data block extends beyond the data-block collection")
        blocks.append((header >> 5, cta[offset + 1 : end]))
        offset = end
    require(offset == cta[2], "CTA data-block collection was not parsed exactly")
    return blocks


def validate_edid(edid: bytes) -> None:
    require(len(edid) == 256, f"expected a 256-byte EDID, got {len(edid)} bytes")
    for index in range(2):
        block = edid[index * 128 : (index + 1) * 128]
        require(sum(block) & 0xFF == 0, f"EDID block {index} checksum is invalid")

    require(decode_manufacturer(edid) == "VBS", "EDID manufacturer is not VBS")
    require(b"Vibeshine HDR" in edid[:128], "EDID monitor name is not Vibeshine HDR")
    require(edid[126] == 1, "base EDID does not advertise exactly one extension block")

    blocks = cta_data_blocks(edid[128:])
    video_blocks = [payload for tag, payload in blocks if tag == 0x02]
    require(video_blocks == [bytes((97, 118, 16, 63))],
            "CTA video block must advertise valid 4K60, 4K120, 1080p60, and 1080p120 VICs")

    extended = [payload for tag, payload in blocks if tag == 0x07 and payload]
    colorimetry = [payload for payload in extended if payload[0] == 0x05]
    require(colorimetry and len(colorimetry[0]) >= 3, "CTA colorimetry block is missing")
    require(colorimetry[0][1] & 0xC0 == 0xC0,
            "CTA colorimetry block does not advertise BT.2020 RGB and YCbCr")

    hdr_metadata = [payload for payload in extended if payload[0] == 0x06]
    require(hdr_metadata and len(hdr_metadata[0]) >= 3, "CTA HDR static metadata block is missing")
    require(hdr_metadata[0][1] & 0x04, "CTA HDR metadata does not advertise SMPTE ST 2084 PQ")
    require(hdr_metadata[0][2] & 0x01, "CTA HDR metadata does not advertise static metadata type 1")


def validate_uapi_layout(driver_root: Path) -> None:
    source = r"""
#include <stddef.h>
#include "vibeshine_drm_uapi.h"

_Static_assert(sizeof(struct vibeshine_drm_wait_present) == 48, "unexpected ABI size");
_Static_assert(offsetof(struct vibeshine_drm_wait_present, sequence) == 8, "unexpected sequence offset");
_Static_assert(offsetof(struct vibeshine_drm_wait_present, timestamp_ns) == 16, "unexpected timestamp offset");
_Static_assert(offsetof(struct vibeshine_drm_wait_present, timeout_ms) == 24, "unexpected timeout offset");
_Static_assert(offsetof(struct vibeshine_drm_wait_present, reserved) == 32, "unexpected reserved offset");
_Static_assert(sizeof(struct vibeshine_drm_frame) == 152, "unexpected frame ABI size");
_Static_assert(offsetof(struct vibeshine_drm_frame, sequence) == 8, "unexpected frame sequence offset");
_Static_assert(offsetof(struct vibeshine_drm_frame, modifier) == 40, "unexpected modifier offset");
_Static_assert(offsetof(struct vibeshine_drm_frame, dma_buf_fds) == 52, "unexpected fd offset");
_Static_assert(offsetof(struct vibeshine_drm_frame, sync_file_fds) == 100, "unexpected fence offset");
_Static_assert(offsetof(struct vibeshine_drm_frame, reserved) == 120, "unexpected frame reserved offset");
_Static_assert(sizeof(struct vibeshine_drm_trace_event) == 16, "unexpected trace event size");
_Static_assert(sizeof(struct vibeshine_drm_present_trace) == 1088, "unexpected trace ABI size");
_Static_assert(offsetof(struct vibeshine_drm_present_trace, after_sequence) == 8, "unexpected trace cursor offset");
_Static_assert(offsetof(struct vibeshine_drm_present_trace, newest_sequence) == 16, "unexpected trace newest offset");
_Static_assert(offsetof(struct vibeshine_drm_present_trace, count) == 24, "unexpected trace count offset");
_Static_assert(offsetof(struct vibeshine_drm_present_trace, events) == 32, "unexpected trace events offset");
_Static_assert(offsetof(struct vibeshine_drm_present_trace, reserved) == 1056, "unexpected trace reserved offset");

int main(void) { return 0; }
"""
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-uapi-") as temporary_dir:
        source_path = Path(temporary_dir) / "uapi-layout.c"
        binary_path = Path(temporary_dir) / "uapi-layout"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Werror", "-I", str(driver_root),
             str(source_path), "-o", str(binary_path)],
            check=True,
        )
        subprocess.run([str(binary_path)], check=True)


def validate_requested_mode_policy(driver_root: Path) -> None:
    source = r"""
#include <assert.h>
#include "vibeshine_drm_mode.h"

int main(void) {
    const struct vibeshine_drm_requested_mode requested = {3024, 1890, 90000};
    struct vibeshine_drm_requested_mode doubled = {0};
    assert(vibeshine_drm_requested_mode_valid(&requested));
    assert(vibeshine_drm_doubled_mode(&requested, &doubled));
    assert(doubled.width == requested.width);
    assert(doubled.height == requested.height);
    assert(doubled.refresh_millihz == 180000);

    const struct vibeshine_drm_requested_mode limit = {1920, 1080, 500000};
    assert(vibeshine_drm_doubled_mode(&limit, &doubled));
    assert(doubled.refresh_millihz == 1000000);

    const struct vibeshine_drm_requested_mode over_limit = {1920, 1080, 500001};
    assert(!vibeshine_drm_doubled_mode(&over_limit, &doubled));
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-mode-") as temporary_dir:
        source_path = Path(temporary_dir) / "requested-mode.c"
        binary_path = Path(temporary_dir) / "requested-mode"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Werror", "-I", str(driver_root),
             str(source_path), "-o", str(binary_path)],
            check=True,
        )
        subprocess.run([str(binary_path)], check=True)


def validate_shmem_prime_callbacks(driver_root: Path) -> None:
    """Preserve the kernel-era PRIME default when replacing only dumb_create."""
    source = r"""
#include <assert.h>
#include "vibeshine_drm_compat.h"
void drm_gem_shmem_prime_import_no_map(void) {}
void drm_gem_shmem_prime_import_sg_table(void) {}
struct driver_ops {
    void (*gem_prime_import)(void);
    void (*gem_prime_import_sg_table)(void);
};
static const struct driver_ops ops = {VIBESHINE_DRM_SHMEM_PRIME_DRIVER_OPS};
int main(void) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
    assert(ops.gem_prime_import == drm_gem_shmem_prime_import_no_map);
    assert(ops.gem_prime_import_sg_table == 0);
#else
    assert(ops.gem_prime_import == 0);
    assert(ops.gem_prime_import_sg_table == drm_gem_shmem_prime_import_sg_table);
#endif
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-prime-") as temporary_dir:
        root = Path(temporary_dir)
        (root / "linux").mkdir()
        (root / "linux/slab.h").write_text("", encoding="utf-8")
        (root / "linux/version.h").write_text(
            "#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))\n",
            encoding="utf-8",
        )
        source_path = root / "prime-callback.c"
        binary_path = root / "prime-callback"
        source_path.write_text(source, encoding="utf-8")
        for major, minor in ((6, 16), (6, 17), (6, 18), (7, 2)):
            subprocess.run(
                [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                 f"-DLINUX_VERSION_CODE={(major << 16) + (minor << 8)}",
                 "-I", str(root), "-I", str(driver_root), str(source_path), "-o", str(binary_path)],
                check=True,
            )
            subprocess.run([str(binary_path)], check=True)


def validate_native_dumb_allocation(driver_root: Path) -> None:
    """Exercise the real native allocator, including its kernel-helper boundary."""
    driver = (driver_root / "vkms_drv.c").read_text(encoding="utf-8")
    require("static int vkms_dumb_create(" in driver, "native dumb allocator is missing")
    start = driver.index("static int vkms_dumb_create(")
    end = driver.index("static const struct drm_driver vkms_driver", start)
    require(re.search(r"\.dumb_create\s*=\s*vkms_dumb_create", driver[end:]) is not None,
            "native dumb allocator is not installed in the DRM callbacks")
    source = r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096U
#endif
#define check_add_overflow(a, b, result) __builtin_add_overflow(a, b, result)
#define check_mul_overflow(a, b, result) __builtin_mul_overflow(a, b, result)
struct drm_file { int unused; } file;
struct drm_device { int unused; } device;
struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};
static struct drm_mode_create_dumb allocated;
static unsigned int calls;
static int allocation_error;
static int drm_gem_shmem_dumb_create(struct drm_file *f, struct drm_device *d,
                                   struct drm_mode_create_dumb *args) {
    assert(f == &file && d == &device);
    allocated = *args;
    ++calls;
    if (allocation_error) return allocation_error;
    /* Model the oldest supported helper's 32-bit size arithmetic. */
    args->pitch = (args->width * args->bpp + 7U) / 8U;
    args->size = (args->pitch * args->height + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
    args->handle = 42;
    return 0;
}
""" + driver[start:end] + r"""
static void check_layout(uint32_t width, uint32_t height) {
    struct drm_mode_create_dumb args = {
        .width = width, .height = height, .bpp = 32,
        .pitch = 17, .size = 18, .handle = 19,
    };
    uint64_t expected_pitch = ((uint64_t)width * 4U + 255U) / 256U * 256U;
    uint64_t expected_size = (expected_pitch * height + PAGE_SIZE - 1U) &
                             ~((uint64_t)PAGE_SIZE - 1U);
    unsigned int previous_calls = calls;
    assert(vkms_dumb_create(&file, &device, &args) == 0);
    assert(calls == previous_calls + 1);
    assert(args.width == width && args.height == height && args.bpp == 32);
    assert(args.pitch == expected_pitch && args.size == expected_size && args.handle == 42);
    assert(allocated.width == expected_pitch / 4U && allocated.height == height);
    assert(allocated.pitch == 0 && allocated.size == 0 && allocated.handle == 0);
    assert(args.size >= (uint64_t)args.pitch * height);
}

static void check_rejected(uint32_t width, uint32_t height) {
    struct drm_mode_create_dumb args = {.width = width, .height = height, .bpp = 32};
    struct drm_mode_create_dumb before = args;
    unsigned int previous_calls = calls;
    assert(vkms_dumb_create(&file, &device, &args) == -EINVAL);
    assert(calls == previous_calls);
    assert(memcmp(&args, &before, sizeof(args)) == 0);
}

static void check_row_writes(uint32_t width, uint32_t height) {
    struct drm_mode_create_dumb args = {.width = width, .height = height, .bpp = 32};
    assert(vkms_dumb_create(&file, &device, &args) == 0);
    unsigned char *mapping = malloc(args.size);
    assert(mapping);
    memset(mapping, 0xa5, args.size);
    /* KWin keeps the logical size and maps/writes rows using CREATE_DUMB's pitch. */
    for (uint32_t y = 0; y < height; ++y)
        memset(mapping + (size_t)y * args.pitch, 0x3c, (size_t)width * 4U);
    for (uint32_t y = 0; y < height; ++y) {
        assert(mapping[(size_t)y * args.pitch] == 0x3c);
        assert(mapping[(size_t)y * args.pitch + width * 4U - 1U] == 0x3c);
        for (uint32_t x = width * 4U; x < args.pitch; ++x)
            assert(mapping[(size_t)y * args.pitch + x] == 0xa5);
    }
    assert(mapping[args.size - 1U] == 0xa5);
    free(mapping);
}

int main(void) {
    const uint32_t widths[] = {1, 63, 64, 65, 1280, 1920, 2176, 2190, 2448,
                               2560, 3120, 3440, 3456, 3840, 8191, 8192};
    const uint32_t heights[] = {1, 1179, 1440, 1848, 8192};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w)
        for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); ++h)
            check_layout(widths[w], heights[h]);
    check_row_writes(3440, 1440);
    check_row_writes(2190, 1179);

    check_rejected(0, 1440);
    check_rejected(3440, 0);
    check_rejected(U32_MAX, 1); /* width alignment overflow */
    check_rejected(U32_MAX - 63U, 1); /* oldest helper's width * bpp overflow */
    check_rejected(U32_MAX / 32U, 1); /* alignment crosses that arithmetic limit */
    check_layout((U32_MAX / 32U) & ~63U, 1);
    check_rejected(64, U32_MAX / 256U + 1U); /* pitch * height overflow */
    check_rejected(64, (U32_MAX & ~(PAGE_SIZE - 1U)) / 256U + 1U);
    check_layout(64, (U32_MAX & ~(PAGE_SIZE - 1U)) / 256U);

    struct drm_mode_create_dumb args = {.width = 3440, .height = 1440, .bpp = 32};
    struct drm_mode_create_dumb before = args;
    allocation_error = -ENOMEM;
    assert(vkms_dumb_create(&file, &device, &args) == -ENOMEM);
    assert(memcmp(&args, &before, sizeof(args)) == 0);
    allocation_error = -EIO;
    assert(vkms_dumb_create(&file, &device, &args) == -EIO);
    allocation_error = 0;

    /* Other pixel depths retain the existing kernel allocator's contract. */
    const uint32_t depths[] = {8, 16, 24, 30, 48, 64};
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i) {
        args = (struct drm_mode_create_dumb){.width = 65, .height = 17, .bpp = depths[i]};
        before = args;
        unsigned int previous_calls = calls;
        assert(vkms_dumb_create(&file, &device, &args) == 0);
        assert(calls == previous_calls + 1);
        assert(memcmp(&allocated, &before, sizeof(args)) == 0);
    }
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-dumb-") as temporary_dir:
        source_path = Path(temporary_dir) / "dumb-allocation.c"
        binary_path = Path(temporary_dir) / "dumb-allocation"
        source_path.write_text(source, encoding="utf-8")
        for page_size in (4096, 65536):
            subprocess.run(
                [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                 f"-DPAGE_SIZE={page_size}U", str(source_path), "-o", str(binary_path)], check=True,
            )
            subprocess.run([str(binary_path)], check=True)


def validate_frame_export(driver_root: Path) -> None:
    """Exercise the actual export helper with kernel ownership APIs stubbed."""
    driver = (driver_root / "vkms_drv.c").read_text(encoding="utf-8")
    start = driver.index("static struct dma_buf *vkms_get_frame_dmabuf(")
    end = driver.index("bool vkms_framebuffer_has_implicit_import(", start)
    helper = driver[start:end]
    source = r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned int u32;
#define DRM_CLOEXEC 1
#define DRM_RDWR 2
#define ERR_PTR(error) ((void *)(intptr_t)(error))
struct dma_buf { int references; } native_buffer, imported_buffer;
struct dma_buf_attachment { struct dma_buf *dmabuf; } attachment;
struct drm_device { int unused; } device;
struct drm_file { int unused; } file;
struct drm_gem_object {
    struct dma_buf_attachment *import_attach;
    struct drm_device *dev;
};
static int create_error, export_error, live_handles, create_calls, export_calls;
static void get_dma_buf(struct dma_buf *buffer) { ++buffer->references; }
static int drm_gem_handle_create(struct drm_file *f, struct drm_gem_object *o, u32 *h) {
    assert(f == &file && o->dev == &device);
    ++create_calls;
    if (create_error) return create_error;
    ++live_handles;
    *h = 123;
    return 0;
}
static struct dma_buf *drm_gem_prime_handle_to_dmabuf(struct drm_device *d,
        struct drm_file *f, u32 h, u32 flags) {
    assert(d == &device && f == &file && h == 123 && live_handles == 1);
    assert(flags == (DRM_CLOEXEC | DRM_RDWR));
    ++export_calls;
    if (export_error) return ERR_PTR(export_error);
    get_dma_buf(&native_buffer);
    return &native_buffer;
}
static int drm_gem_handle_delete(struct drm_file *f, u32 h) {
    assert(f == &file && h == 123 && live_handles == 1);
    --live_handles;
    return 0;
}
""" + helper + r"""
int main(void) {
    struct drm_gem_object obj = { .dev = &device };
    assert(vkms_get_frame_dmabuf(&file, NULL) == ERR_PTR(-EINVAL));
    assert(create_calls == 0);

    attachment.dmabuf = &imported_buffer;
    obj.import_attach = &attachment;
    assert(vkms_get_frame_dmabuf(&file, &obj) == &imported_buffer);
    assert(imported_buffer.references == 1 && create_calls == 0 && export_calls == 0);
    attachment.dmabuf = NULL;
    assert(vkms_get_frame_dmabuf(&file, &obj) == ERR_PTR(-EINVAL));
    assert(create_calls == 0);

    obj.import_attach = NULL;
    assert(vkms_get_frame_dmabuf(&file, &obj) == &native_buffer);
    assert(native_buffer.references == 1 && live_handles == 0);
    assert(create_calls == 1 && export_calls == 1);

    create_error = -ENOMEM;
    assert(vkms_get_frame_dmabuf(&file, &obj) == ERR_PTR(-ENOMEM));
    assert(live_handles == 0 && export_calls == 1 && native_buffer.references == 1);
    create_error = 0;
    export_error = -EIO;
    assert(vkms_get_frame_dmabuf(&file, &obj) == ERR_PTR(-EIO));
    assert(live_handles == 0 && export_calls == 2 && native_buffer.references == 1);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-export-") as temporary_dir:
        source_path = Path(temporary_dir) / "frame-export.c"
        binary_path = Path(temporary_dir) / "frame-export"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(source_path), "-o", str(binary_path)], check=True,
        )
        subprocess.run([str(binary_path)], check=True)


def validate_implicit_frame_layout(driver_root: Path) -> None:
    """Exercise layout provenance for native, explicit and implicit PRIME frames."""
    driver = (driver_root / "vkms_drv.c").read_text(encoding="utf-8")
    start = driver.index("bool vkms_framebuffer_has_implicit_import(")
    end = driver.index("static int vkms_get_frame_ioctl(", start)
    source = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
typedef uint64_t u64;
struct drm_format_info { unsigned int num_planes; };
struct drm_gem_object { void *import_attach; };
struct drm_framebuffer {
    unsigned int flags;
    u64 modifier;
    const struct drm_format_info *format;
    struct drm_gem_object *obj[4];
};
static struct drm_gem_object *drm_gem_fb_get_obj(struct drm_framebuffer *fb, unsigned int plane) {
    return fb->obj[plane];
}
""" + driver[start:end] + r"""
int main(void) {
    struct drm_format_info format = {1};
    struct drm_gem_object native = {NULL};
    struct drm_gem_object imported = {&format};
    struct drm_framebuffer fb = {0, DRM_FORMAT_MOD_LINEAR, &format, {&native}};
    const u64 tiled = DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 0);
    assert(!vkms_framebuffer_has_implicit_import(&fb));
    assert(vkms_framebuffer_modifier(&fb) == DRM_FORMAT_MOD_LINEAR);
    fb.obj[0] = &imported;
    assert(vkms_framebuffer_has_implicit_import(&fb));
    assert(vkms_framebuffer_modifier(&fb) == DRM_FORMAT_MOD_INVALID);
    fb.flags = DRM_MODE_FB_MODIFIERS;
    assert(!vkms_framebuffer_has_implicit_import(&fb));
    assert(vkms_framebuffer_modifier(&fb) == DRM_FORMAT_MOD_LINEAR);
    fb.modifier = tiled;
    assert(!vkms_framebuffer_has_implicit_import(&fb));
    assert(vkms_framebuffer_modifier(&fb) == tiled);
    fb.flags = 0;
    fb.modifier = DRM_FORMAT_MOD_LINEAR;
    fb.obj[0] = &native;
    fb.obj[1] = &imported;
    format.num_planes = 2;
    assert(vkms_framebuffer_has_implicit_import(&fb));
    assert(vkms_framebuffer_modifier(&fb) == DRM_FORMAT_MOD_INVALID);
    fb.obj[1] = &native;
    assert(!vkms_framebuffer_has_implicit_import(&fb));
    assert(vkms_framebuffer_modifier(&fb) == DRM_FORMAT_MOD_LINEAR);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-layout-") as temporary_dir:
        source_path = Path(temporary_dir) / "frame-layout.c"
        binary_path = Path(temporary_dir) / "frame-layout"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(source_path), "-o", str(binary_path)], check=True,
        )
        subprocess.run([str(binary_path)], check=True)


def validate_scanout_negotiation(driver_root: Path) -> None:
    """Intersect NVIDIA producer layouts with the real plane capabilities."""
    plane = (driver_root / "vkms_plane.c").read_text(encoding="utf-8")
    start = plane.index("static const u32 vkms_formats[]")
    end = plane.index("static struct drm_plane_state *", start)
    source = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <libdrm/drm_fourcc.h>
typedef uint32_t u32;
typedef uint64_t u64;
struct drm_plane;
""" + plane[start:end] + r"""
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static bool can_scanout(u32 format, u64 producer_modifier) {
    size_t f, m;
    for (f = 0; f < ARRAY_SIZE(vkms_formats); ++f) {
        if (vkms_formats[f] != format) continue;
        for (m = 0; vkms_format_modifiers[m] != DRM_FORMAT_MOD_INVALID; ++m) {
            if (vkms_format_modifiers[m] == producer_modifier &&
                vkms_format_mod_supported(NULL, format, producer_modifier))
                return true;
        }
    }
    return false;
}

int main(void) {
    /* nvidia-drm advertises block heights 0..5, sector layout 1, no compression.
     * Fermi-Volta (including GTX 1070): generation 0, generic page kind 0xfe.
     * Turing+: generation 2, generic page kind 0x06. Keep wire values explicit
     * so a mistaken generation/kind in the driver does not alter the fixture.
     */
    const u64 nvidia_layouts[] = {0x03000000004fe010ULL, 0x0300000000606010ULL};
    const u32 rgb_formats[] = {
        DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
        DRM_FORMAT_ABGR2101010, DRM_FORMAT_XBGR2101010,
    };
    size_t g, f, i, j;
    unsigned int height;
    for (g = 0; g < ARRAY_SIZE(nvidia_layouts); ++g) {
        for (height = 0; height <= 5; ++height) {
            const u64 modifier = nvidia_layouts[g] | height;
            for (f = 0; f < ARRAY_SIZE(rgb_formats); ++f) {
                /* Both SDR and HDR must negotiate a GPU layout without
                 * falling back to LINEAR on either GPU generation. */
                assert(can_scanout(rgb_formats[f], modifier));
            }
            /* NVIDIA's scanout RGB whitelist does not include AR30/XR30. */
            assert(!vkms_format_mod_supported(NULL, DRM_FORMAT_ARGB2101010, modifier));
            assert(!vkms_format_mod_supported(NULL, DRM_FORMAT_XRGB2101010, modifier));
            assert(!vkms_format_mod_supported(NULL, DRM_FORMAT_NV12, modifier));
        }
        assert(!vkms_format_mod_supported(NULL, DRM_FORMAT_XRGB8888, nvidia_layouts[g] | 6));
        assert(!vkms_format_mod_supported(NULL, DRM_FORMAT_XRGB8888, nvidia_layouts[g] | (1ULL << 23)));
    }
    /* Do not accept an unadvertised layout through the atomic-check callback. */
    assert(!vkms_format_mod_supported(NULL, DRM_FORMAT_XRGB8888, 0x03000000006fe010ULL));
    assert(!vkms_format_mod_supported(NULL, DRM_FORMAT_XRGB8888, 0x0300000000000010ULL));
    assert(!vkms_format_mod_supported(NULL, DRM_FORMAT_XRGB8888, 0x0100000000000001ULL));
    assert(!vkms_format_mod_supported(NULL, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID));

    assert(vkms_format_modifiers[ARRAY_SIZE(vkms_format_modifiers) - 1] == DRM_FORMAT_MOD_INVALID);
    for (i = 0; i + 1 < ARRAY_SIZE(vkms_format_modifiers); ++i) {
        for (j = i + 1; j < ARRAY_SIZE(vkms_format_modifiers); ++j)
            assert(vkms_format_modifiers[i] != vkms_format_modifiers[j]);
    }
    /* Software/linear compatibility remains available for all plane formats. */
    for (f = 0; f < ARRAY_SIZE(vkms_formats); ++f)
        assert(can_scanout(vkms_formats[f], DRM_FORMAT_MOD_LINEAR));
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-scanout-") as temporary_dir:
        source_path = Path(temporary_dir) / "scanout-negotiation.c"
        binary_path = Path(temporary_dir) / "scanout-negotiation"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", str(source_path), "-o", str(binary_path)], check=True,
        )
        subprocess.run([str(binary_path)], check=True)


def validate_vrr_schedule(driver_root: Path) -> None:
    """Exercise the real nonblocking CRTC scheduler, the sole refresh limiter."""
    driver = (driver_root / "vkms_crtc.c").read_text(encoding="utf-8")
    start = driver.index("static void vkms_schedule_vrr_vblank(")
    end = driver.index("\n#else", start)
    source = r"""
#include <assert.h>
#include <stdint.h>
#include "vibeshine_drm_vrr.h"
typedef uint64_t u64;
typedef uint64_t ktime_t;
struct drm_vblank_crtc { int framedur_ns; } vblank;
struct drm_crtc { int unused; } crtc;
struct vkms_output { u64 vrr_vblank_timestamp_ns; int vrr_hrtimer; } output;
#define READ_ONCE(x) (x)
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define atomic64_read(value) (*(value))
#define HRTIMER_MODE_ABS 0
static int scheduled, delivered;
static u64 now, deadline;
static struct drm_vblank_crtc *drm_crtc_vblank_crtc(struct drm_crtc *c) {
    assert(c == &crtc); return &vblank;
}
static struct vkms_output *drm_crtc_to_vkms_output(struct drm_crtc *c) {
    assert(c == &crtc); return &output;
}
static u64 ktime_get_ns(void) { return now; }
static ktime_t ns_to_ktime(u64 t) { return t; }
static void hrtimer_start(int *timer, ktime_t expires, int mode) {
    assert(timer == &output.vrr_hrtimer);
    assert(mode == HRTIMER_MODE_ABS);
    assert(expires > now);
    ++scheduled;
    deadline = expires;
}
static void vkms_crtc_handle_vblank_timeout(struct drm_crtc *c) {
    assert(c == &crtc);
    ++delivered;
    output.vrr_vblank_timestamp_ns = now;
}
""" + driver[start:end] + r"""
int main(void) {
    vblank.framedur_ns = 8333333;
    output.vrr_vblank_timestamp_ns = 10000000;
    now = 12000000;
    vkms_schedule_vrr_vblank(&crtc);
    assert(scheduled == 1 && delivered == 0 && deadline == 18333333);
    assert(now == 12000000); /* Scheduling must not block the commit worker. */
    now = deadline;
    vkms_crtc_handle_vblank_timeout(&crtc);
    /* A late publication must not move the next flip's refresh deadline. */
    now += 2000000;
    vkms_schedule_vrr_vblank(&crtc);
    assert(scheduled == 2 && delivered == 1 && deadline == 26666666);
    assert(now == 20333333);
    now = deadline;
    vkms_schedule_vrr_vblank(&crtc);
    assert(scheduled == 2 && delivered == 2);
    now += 10000000; /* Below refresh, deliver immediately. */
    vkms_schedule_vrr_vblank(&crtc);
    assert(scheduled == 2 && delivered == 3);
    output.vrr_vblank_timestamp_ns = 0;
    vkms_schedule_vrr_vblank(&crtc);
    assert(scheduled == 2 && delivered == 4);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-vrr-schedule-") as temporary_dir:
        source_path = Path(temporary_dir) / "vrr-schedule.c"
        binary_path = Path(temporary_dir) / "vrr-schedule"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(driver_root), str(source_path), "-o", str(binary_path)], check=True,
        )
        subprocess.run([str(binary_path)], check=True)


def validate_reboot_notifier(driver_root: Path) -> None:
    """Every shutdown action must release the device and continue normally."""
    driver = (driver_root / "vkms_drv.c").read_text(encoding="utf-8")
    start = driver.index("static int vkms_reboot_notifier(")
    end = driver.index("int vkms_create(", start)
    source = r"""
#include <assert.h>
#include <stddef.h>
#define SYS_RESTART 1
#define SYS_HALT 2
#define SYS_POWER_OFF 3
#define NOTIFY_DONE 0
#define DRM_INFO(...) ((void)0)
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
struct notifier_block { int unused; };
struct vkms_device { int unused; struct notifier_block reboot_notifier; };
static struct vkms_device *released;
static unsigned int release_count;
static void vkms_shutdown_release(struct vkms_device *device) {
    released = device;
    ++release_count;
}
""" + driver[start:end] + r"""
int main(void) {
    struct vkms_device devices[2] = {0};
    const unsigned long actions[] = {SYS_RESTART, SYS_HALT, SYS_POWER_OFF};
    for (unsigned int i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        for (unsigned int j = 0; j < 2; ++j) {
            released = NULL;
            release_count = 0;
            assert(vkms_reboot_notifier(&devices[j].reboot_notifier,
                                       actions[i], NULL) == NOTIFY_DONE);
            assert(released == &devices[j] && release_count == 1);
        }
    }
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-reboot-") as temporary_dir:
        source_path = Path(temporary_dir) / "reboot-notifier.c"
        binary_path = Path(temporary_dir) / "reboot-notifier"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(source_path), "-o", str(binary_path)], check=True,
        )
        subprocess.run([str(binary_path)], check=True)


def validate_source_contract(driver_root: Path) -> None:
    compat_path = driver_root / "vibeshine_drm_compat.h"
    connector_path = driver_root / "vkms_connector.c"
    mode_header_path = driver_root / "vibeshine_drm_mode.h"
    driver_header_path = driver_root / "vkms_drv.h"
    plane_path = driver_root / "vkms_plane.c"
    configfs_path = driver_root / "vkms_configfs.c"
    config_path = driver_root / "vkms_config.c"
    crtc_path = driver_root / "vkms_crtc.c"
    drv_path = driver_root / "vkms_drv.c"
    compat = compat_path.read_text(encoding="utf-8")
    connector = connector_path.read_text(encoding="utf-8")
    mode_header = mode_header_path.read_text(encoding="utf-8")
    driver_header = driver_header_path.read_text(encoding="utf-8")
    plane = plane_path.read_text(encoding="utf-8")
    configfs = configfs_path.read_text(encoding="utf-8")
    config = config_path.read_text(encoding="utf-8")
    crtc = crtc_path.read_text(encoding="utf-8")
    drv = drv_path.read_text(encoding="utf-8")

    require_source(drv, '#include "vibeshine_drm_version.h"', drv_path.name)
    require_source(drv, "MODULE_VERSION(VIBESHINE_DRM_VERSION)", drv_path.name)
    require("MODULE_VERSION(\"" not in drv,
            "kernel module version must come from the package build")

    for needle in (
        "VIBESHINE_DRM_MIN_KERNEL_VERSION KERNEL_VERSION(6, 16, 0)",
        "VIBESHINE_DRM_HAS_COLOR_PIPELINE",
        "VIBESHINE_DRM_HAS_BACKGROUND_COLOR",
        "VIBESHINE_DRM_HAS_VBLANK_HELPER",
        "VIBESHINE_DRM_HAS_SHARPNESS_STRENGTH",
        "#define kzalloc_obj(P, ...) kzalloc(sizeof(P), GFP_KERNEL)",
        "#define kzalloc_objs(P, COUNT, ...) kcalloc((COUNT), sizeof(P), GFP_KERNEL)",
    ):
        require_source(compat, needle, compat_path.name)

    for needle in (
        "#if VIBESHINE_DRM_HAS_VBLANK_HELPER",
        "static enum hrtimer_restart vkms_vblank_simulate",
        "drm_calc_timestamping_constants(crtc, &crtc->mode)",
        "drm_crtc_vblank_on(crtc)",
        "drm_crtc_vblank_off(crtc)",
        "#if VIBESHINE_DRM_HAS_BACKGROUND_COLOR",
    ):
        require_source(crtc, needle, crtc_path.name)

    for needle in (
        "#include \"vibeshine_drm_compat.h\"",
        "drm_connector_attach_vrr_capable_property(&connector->base)",
        "drm_connector_set_vrr_capable_property(&connector->base, true)",
        "drm_connector_attach_max_bpc_property(&connector->base, 8, 16)",
        "drm_connector_attach_hdr_output_metadata_property(&connector->base)",
        "DRM_MODE_COLORIMETRY_BT2020_RGB",
        "DRM_MODE_COLORIMETRY_BT2020_YCC",
        "drm_connector_attach_colorspace_property(&connector->base)",
        "vibeshine_drm_doubled_mode(&requested, &doubled)",
        "entry->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED",
        "entry->type = DRM_MODE_TYPE_DRIVER;",
    ):
        require_source(connector, needle, connector_path.name)
    for needle in (
        "VIBESHINE_DRM_MAX_REFRESH_MILLIHZ / 2",
        "doubled->refresh_millihz *= 2",
        "vibeshine_drm_requested_mode_clock_khz",
    ):
        require_source(mode_header, needle, mode_header_path.name)
    require(
        re.search(
            r"#if VIBESHINE_DRM_HAS_VBLANK_HELPER\s+"
            r"ret = drm_connector_attach_vrr_capable_property\(&connector->base\);.*?"
            r"drm_connector_set_vrr_capable_property\(&connector->base, true\);\s+"
            r"#endif",
            connector,
            flags=re.DOTALL,
        )
        is not None,
        "vkms_connector.c advertises VRR without the VRR-capable vblank path",
    )

    for pixel_format in (
        "DRM_FORMAT_XRGB2101010",
        "DRM_FORMAT_XBGR2101010",
        "DRM_FORMAT_ARGB2101010",
        "DRM_FORMAT_ABGR2101010",
        "DRM_FORMAT_P010",
        "DRM_FORMAT_P012",
        "DRM_FORMAT_P016",
    ):
        require_source(plane, pixel_format, plane_path.name)

    for needle in (
        "DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 5)",
        "vkms_format_mod_supported",
        "fb->modifier != DRM_FORMAT_MOD_LINEAR",
        "vkms_format_modifiers,",
    ):
        require_source(plane, needle, plane_path.name)
    require(
        re.search(
            r"static int vkms_prepare_fb\(.*?"
            r"drm_gem_plane_helper_prepare_fb\(plane, state\);.*?"
            r"vkms_state->presentation_content_update \|= state->fence != NULL;",
            plane,
            flags=re.DOTALL,
        )
        is not None,
        "vkms_prepare_fb does not preserve implicit fences after GEM preparation",
    )
    require_source(driver_header, "bool frame_mapped", driver_header_path.name)
    require_source(driver_header, "struct drm_framebuffer *present_fb", driver_header_path.name)
    require_source(driver_header, "struct vibeshine_drm_trace_event *present_trace", driver_header_path.name)
    require_source(driver_header, "atomic64_t synthetic_vblank_counter", driver_header_path.name)
    require_source(driver_header, "struct hrtimer vrr_hrtimer", driver_header_path.name)
    require_source(driver_header, "struct mutex shutdown_lock", driver_header_path.name)
    require_source(driver_header, "struct notifier_block reboot_notifier", driver_header_path.name)
    for needle in (
        "if (crtc->state->vrr_enabled)",
        "drm_crtc_vblank_cancel_timer(crtc)",
        "vkms_crtc_get_vblank_timestamp",
        "vkms_crtc_get_vblank_counter",
        ".get_vblank_counter     = vkms_crtc_get_vblank_counter",
        "atomic64_set(&output->vrr_vblank_timestamp_ns, ktime_get_ns())",
        "atomic64_inc(&output->synthetic_vblank_counter)",
        "atomic64_set(&vkms_output->vrr_vblank_timestamp_ns",
        "*vblank_time = ns_to_ktime(timestamp_ns)",
        "return vkms_crtc_handle_vblank(crtc)",
        "HRTIMER_RESTART : HRTIMER_NORESTART",
        "vkms_crtc_atomic_enable",
        "drm_crtc_vblank_start_timer(crtc)",
        "vkms_schedule_vrr_vblank(crtc)",
        "hrtimer_start(&output->vrr_hrtimer, ns_to_ktime(deadline_ns)",
        "hrtimer_cancel(&output->vrr_hrtimer)",
    ):
        require_source(crtc, needle, crtc_path.name)
    require("if (in_vblank_irq)\n\t\treturn false" not in crtc,
            "VRR timestamping still disables precise timestamps in interrupt context")
    require(
        crtc.index("atomic64_set(&output->vrr_vblank_timestamp_ns, ktime_get_ns())") <
        crtc.index("atomic64_inc(&output->synthetic_vblank_counter)") <
        crtc.index("drm_crtc_handle_vblank(crtc)"),
        "synthetic vblank timestamp and counter are not published before delivery",
    )
    require("drm_crtc_set_max_vblank_count" not in crtc,
            "static synthetic counters still use the runtime per-CRTC vblank setter")
    require("if (vrr_flip) {\n\t\tvkms_crtc_handle_vblank_timeout(crtc);" not in crtc,
            "VRR flips still bypass the mode's maximum refresh interval")
    failed_delivery = crtc.index("if (!ret) {", crtc.index("drm_crtc_handle_vblank(crtc)"))
    composer_read = crtc.index("state = output->composer_state", failed_delivery)
    require(failed_delivery < crtc.index("return false;", failed_delivery) < composer_read,
            "rejected vblank delivery can still queue composer work")
    for needle in (
        "DRM_IOCTL_DEF_DRV(VIBESHINE_GET_FRAME, vkms_get_frame_ioctl, 0)",
        "DRM_IOCTL_DEF_DRV(VIBESHINE_GET_PRESENT_TRACE, vkms_get_present_trace_ioctl, 0)",
        "obj->import_attach->dmabuf",
        "dma_buf_fd(dma_buf, O_CLOEXEC)",
        "drm_framebuffer_get(new_present_fb)",
        "capable(CAP_SYS_ADMIN)",
        "static bool enable_cursor;",
        "devm_register_reboot_notifier",
        "devm_device_add_group",
        "vkms_reboot_notifier",
        "quiesce_store",
        "vkms_crtc_release_presented_frame",
        "vkms_device->drm.max_vblank_count = U32_MAX",
    ):
        require_source(drv, needle, drv_path.name)
    require("vkms_wait_for_vrr_presentation_slot" not in drv and
            "schedule_hrtimeout" not in drv and
            "vibeshine_drm_vrr_presentation_deadline_ns" not in drv,
            "commit publication adds a second refresh clock after flip completion")
    tail_start = drv.index("static void vkms_atomic_commit_tail(")
    tail_end = drv.index("static const struct drm_driver", tail_start)
    tail = drv[tail_start:tail_end]
    require(tail.index("drm_atomic_helper_wait_for_flip_done(dev, old_state)") <
            tail.index("vkms_signal_presented_crtcs(old_state)") <
            tail.index("drm_atomic_helper_commit_hw_done(old_state)") <
            tail.index("drm_atomic_helper_cleanup_planes(dev, old_state)"),
            "publication must follow flip completion before releasing later commits and buffers")
    for obsolete in (
        "vkms_bugcheck", "panic_timeout", "VKMS_POWEROFF_DEADLINE_SECS",
        "shutdown_deadline", "vkms_module_reboot_", "<linux/panic.h>", "<linux/timer.h>",
    ):
        require(obsolete not in drv, f"obsolete forced-shutdown machinery remains: {obsolete}")
    require(re.search(r"\b(?:panic|emergency_restart|kernel_restart)\s*\(", drv) is None,
            "the display driver still forces a system panic or restart")
    require(
        drv.index("vkms_device->drm.max_vblank_count = U32_MAX") <
        drv.index("drm_vblank_init(&vkms_device->drm"),
        "device-wide synthetic counter maximum is not configured before vblank initialization",
    )
    require(
        re.search(
            r"static int vkms_wait_present_ioctl\(.*?"
            r"if \(!capable\(CAP_SYS_ADMIN\)\)\s*return -EACCES;",
            drv,
            flags=re.DOTALL,
        )
        is not None,
        "presentation waits are not restricted to the privileged capture host",
    )
    require(
        re.search(
            r"static void vkms_quiesce\(.*?"
            r"drm_atomic_helper_shutdown\(&vkmsdev->drm\);.*?"
            r"vkms_release_presented_frames\(vkmsdev\);",
            drv,
            flags=re.DOTALL,
        )
        is not None,
        "reboot quiescing does not release presented frames after atomic shutdown",
    )
    require(".get_crc_sources" not in crtc, "GPU pass-through CRTC still advertises software CRC")
    require_source(configfs, "if (writeback)", configfs_path.name)
    require_source(configfs, "return -EOPNOTSUPP", configfs_path.name)
    require_source(configfs, "CONFIGFS_ATTR(connector_, requested_mode)", configfs_path.name)
    require_source(configfs, "CONFIGFS_ATTR_RO(connector_, drm_name)", configfs_path.name)
    require_source(configfs, "connector->config->connector->base.name", configfs_path.name)
    require_source(configfs, "vkms_trigger_connector_hotplug", configfs_path.name)

    require(re.search(r'\.ci_name\s*=\s*"vibeshine-drm"', configfs) is not None,
            "configfs root is not named vibeshine-drm")
    require("connector_cfg->status = connector_status_disconnected" in config,
            "new Vibeshine DRM connectors do not default to disconnected")

    exported = []
    for source_path in sorted(driver_root.rglob("*")):
        if source_path.suffix not in {".c", ".h"}:
            continue
        source = source_path.read_text(encoding="utf-8")
        if re.search(r"^\s*EXPORT_SYMBOL\w*\s*\(", source, flags=re.MULTILINE):
            exported.append(source_path.relative_to(driver_root).as_posix())
    require(not exported, "out-of-tree source exports kernel symbols: " + ", ".join(exported))


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} DRIVER_ROOT", file=sys.stderr)
        return 2

    driver_root = Path(sys.argv[1]).resolve()
    generator = driver_root / "generate_hdr_edid.py"
    header = driver_root / "vibeshine_hdr_edid.h"
    require(generator.is_file(), f"missing EDID generator: {generator}")
    require(header.is_file(), f"missing generated EDID header: {header}")

    subprocess.run([sys.executable, str(generator), "--check", str(header)], check=True)
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-contract-") as temporary_dir:
        binary = Path(temporary_dir) / "vibeshine-hdr.bin"
        subprocess.run([sys.executable, str(generator), "--binary", str(binary)], check=True)
        validate_edid(binary.read_bytes())

    validate_source_contract(driver_root)
    validate_reboot_notifier(driver_root)
    validate_uapi_layout(driver_root)
    validate_requested_mode_policy(driver_root)
    validate_shmem_prime_callbacks(driver_root)
    validate_native_dumb_allocation(driver_root)
    validate_frame_export(driver_root)
    validate_implicit_frame_layout(driver_root)
    validate_scanout_negotiation(driver_root)
    validate_vrr_schedule(driver_root)
    print("Vibeshine DRM source and HDR EDID contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ContractError, OSError, subprocess.CalledProcessError) as error:
        print(f"Vibeshine DRM source contract failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
