// SPDX-License-Identifier: GPL-2.0+
/* Link the production allocator .c unchanged; mock only kernel DRM APIs. */
#include <drm/drm_gem_shmem_helper.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include "vibeshine_drm_dumb.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct drm_file file;
static struct drm_device device;
static struct drm_mode_create_dumb allocated;
static unsigned int calls, prime_calls;
static int allocation_error, map_allocation;
static unsigned char *mapping;
static size_t mapped_size;
static int imported;

static const struct drm_driver ops = {VIBESHINE_DRM_SHMEM_DRIVER_OPS};

#ifdef VIBESHINE_TEST_BASELINE
/* This bridge is compiled with the unmodified pre-fix vkms_drv.c. */
extern int vibeshine_baseline_dumb_create(struct drm_file *, struct drm_device *,
                                        struct drm_mode_create_dumb *);
#define allocate vibeshine_baseline_dumb_create
#else
#define allocate ops.dumb_create
#endif

int drm_gem_shmem_dumb_create(struct drm_file *f, struct drm_device *d,
                            struct drm_mode_create_dumb *args)
{
    assert(f == &file && d == &device);
    allocated = *args;
    ++calls;
    if (allocation_error)
        return allocation_error;
    /* Linux 6.16 shmem allocation semantics, without a real GEM object. */
    args->pitch = (args->width * args->bpp + 7U) / 8U;
    args->size = (args->pitch * args->height + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
    args->handle = 42;
    if (map_allocation) {
        assert(!mapping);
        mapped_size = args->size;
        mapping = malloc(mapped_size + 64U);
        assert(mapping);
        memset(mapping, 0xa5, mapped_size + 64U);
    }
    return 0;
}

void *drm_gem_shmem_prime_import_no_map(struct drm_device *d, void *buffer)
{
    assert(d == &device && buffer == &imported);
    ++prime_calls;
    return buffer;
}

void *drm_gem_shmem_prime_import_sg_table(struct drm_device *d, void *attachment, void *table)
{
    assert(d == &device && attachment == &imported && table == &allocated);
    ++prime_calls;
    return attachment;
}

static int check_layout(uint32_t width, uint32_t height)
{
    struct drm_mode_create_dumb args = {.width = width, .height = height, .bpp = 32};
    uint64_t expected_pitch = ((uint64_t)width * 4U + 255U) / 256U * 256U;
    uint64_t expected_size = (expected_pitch * height + PAGE_SIZE - 1U) &
                             ~((uint64_t)PAGE_SIZE - 1U);
    unsigned int before_calls = calls;
    assert(allocate(&file, &device, &args) == 0);
    printf("%ux%u: kernel allocation width=%u pitch=%u size=%llu; logical=%ux%u\n",
           width, height, allocated.width, args.pitch, (unsigned long long)args.size,
           args.width, args.height);
    if (args.pitch != expected_pitch || args.size != expected_size) {
        fprintf(stderr, "FAIL: %ux%u returned pitch=%u size=%llu; expected pitch=%llu size=%llu\n",
                width, height, args.pitch, (unsigned long long)args.size,
                (unsigned long long)expected_pitch, (unsigned long long)expected_size);
        return 1;
    }
    assert(calls == before_calls + 1);
    assert(args.width == width && args.height == height && args.bpp == 32);
    assert(args.handle == 42 && args.size >= (uint64_t)args.pitch * height);
    assert(allocated.width == expected_pitch / 4U && allocated.height == height);
    assert(allocated.pitch == 0 && allocated.size == 0 && allocated.handle == 0);
    return 0;
}

static void check_rejected(uint32_t width, uint32_t height)
{
    struct drm_mode_create_dumb args = {.width = width, .height = height, .bpp = 32};
    struct drm_mode_create_dumb before = args;
    unsigned int previous_calls = calls;
    assert(allocate(&file, &device, &args) == -EINVAL);
    assert(calls == previous_calls);
    assert(memcmp(&args, &before, sizeof(args)) == 0);
}

static void check_row_writes(uint32_t width, uint32_t height)
{
    struct drm_mode_create_dumb args = {.width = width, .height = height, .bpp = 32};
    map_allocation = 1;
    assert(allocate(&file, &device, &args) == 0);
    assert(args.size == mapped_size && args.width == width && args.height == height);
    for (uint32_t y = 0; y < height; ++y)
        memset(mapping + (size_t)y * args.pitch, 0x3c, (size_t)width * 4U);
    for (uint32_t y = 0; y < height; ++y) {
        assert(mapping[(size_t)y * args.pitch] == 0x3c);
        assert(mapping[(size_t)y * args.pitch + width * 4U - 1U] == 0x3c);
        for (uint32_t x = width * 4U; x < args.pitch; ++x)
            assert(mapping[(size_t)y * args.pitch + x] == 0xa5);
    }
    for (size_t x = (size_t)args.pitch * height; x < mapped_size + 64U; ++x)
        assert(mapping[x] == 0xa5);
    free(mapping);
    mapping = NULL;
    map_allocation = 0;
}

int main(void)
{
    /* Keep the logged failing case first so baseline failure is behavioral. */
    if (check_layout(3440, 1440))
        return 1;
    const uint32_t widths[] = {1, 63, 64, 65, 1280, 1920, 2176, 2190, 2448,
                               2560, 3120, 3456, 3840, 8191, 8192};
    const uint32_t heights[] = {1, 1179, 1440, 1848, 8192};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w)
        for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); ++h)
            if (check_layout(widths[w], heights[h]))
                return 1;
    check_row_writes(3440, 1440);
    check_row_writes(2190, 1179);

    check_rejected(0, 1440);
    check_rejected(3440, 0);
    check_rejected(U32_MAX, 1);
    check_rejected(U32_MAX - 63U, 1);
    check_rejected(U32_MAX / 32U, 1);
    assert(!check_layout((U32_MAX / 32U) & ~63U, 1));
    check_rejected(64, U32_MAX / 256U + 1U);
    check_rejected(64, (U32_MAX & ~(PAGE_SIZE - 1U)) / 256U + 1U);
    assert(!check_layout(64, (U32_MAX & ~(PAGE_SIZE - 1U)) / 256U));

    struct drm_mode_create_dumb args = {.width = 3440, .height = 1440, .bpp = 32};
    struct drm_mode_create_dumb before = args;
    const int errors[] = {-ENOMEM, -EIO};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        unsigned int previous_calls = calls;
        allocation_error = errors[i];
        assert(allocate(&file, &device, &args) == errors[i]);
        assert(calls == previous_calls + 1 && memcmp(&args, &before, sizeof(args)) == 0);
    }
    allocation_error = 0;

    const uint32_t depths[] = {8, 16, 24, 30, 48, 64};
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i) {
        args = (struct drm_mode_create_dumb){.width = 65, .height = 17, .bpp = depths[i]};
        before = args;
        unsigned int previous_calls = calls;
        assert(allocate(&file, &device, &args) == 0);
        assert(calls == previous_calls + 1 && memcmp(&allocated, &before, sizeof(args)) == 0);
        assert(args.pitch == (65U * depths[i] + 7U) / 8U);
    }

    unsigned int before_calls = calls;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
    assert(!ops.gem_prime_import_sg_table);
    assert(ops.gem_prime_import(&device, &imported) == &imported);
#else
    assert(!ops.gem_prime_import);
    assert(ops.gem_prime_import_sg_table(&device, &imported, &allocated) == &imported);
#endif
    assert(prime_calls == 1 && calls == before_calls);
    puts("PASS: production allocator component and PRIME callback behavior");
    return 0;
}
