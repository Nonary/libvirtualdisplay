#pragma once

#include <stdint.h>
#include <libdrm/drm_mode.h>

typedef uint32_t u32;
struct drm_file { int unused; };
struct drm_device { int unused; };

/* Kernel API boundaries only; allocation policy comes from production .c. */
int drm_gem_shmem_dumb_create(struct drm_file *, struct drm_device *,
                            struct drm_mode_create_dumb *);
void *drm_gem_shmem_prime_import_no_map(struct drm_device *, void *);
void *drm_gem_shmem_prime_import_sg_table(struct drm_device *, void *, void *);

struct drm_driver {
    int (*dumb_create)(struct drm_file *, struct drm_device *, struct drm_mode_create_dumb *);
    void *(*gem_prime_import)(struct drm_device *, void *);
    void *(*gem_prime_import_sg_table)(struct drm_device *, void *, void *);
};
