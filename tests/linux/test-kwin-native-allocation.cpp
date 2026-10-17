// Real allocator/GL interoperability. Allocates private buffers without
// modesetting.
#include "../../linux/kwin_gpu_interposer.cpp"
#include <epoxy/egl.h>
#include <QCoreApplication>
#include <cmath>
#include <core/drmdevice.h>
#include <core/gbmgraphicsbufferallocator.h>
#include <core/graphicsbuffer.h>
#include <core/gpumanager.h>
#include <drm_fourcc.h>
#include <xf86drmMode.h>
#include <opengl/eglswapchain.h>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (!native_allocator_runtime_compatible()) {
      std::cout << "SKIP: KWin library does not match build headers\n";
      return 77;
    }
    configure_allocator_routing(dlsym);
    require(hooks_enabled && native_hooks_enabled,
            "native allocator routing did not enable");
    const int count = drmGetDevices2(0, nullptr, 0);
    if (count <= 0)
      return 77;
    std::vector<drmDevicePtr> devices(count, nullptr);
    const int actual = drmGetDevices2(0, devices.data(), count);
    QString path;
    for (int i = 0; i < actual; ++i)
      if (is_vibeshine_display(devices[i]) &&
          (devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)))
        path = QString::fromLocal8Bit(devices[i]->nodes[DRM_NODE_PRIMARY]);
    if (actual > 0)
      drmFreeDevices(devices.data(), actual);
    if (path.isEmpty()) {
      std::cout << "SKIP: managed virtual DRM device unavailable\n";
      return 77;
    }
    auto device = KWin::DrmDevice::open(path);
    require(bool(device), "opening virtual KMS device failed");
    if (!owned_render_fds.contains(device->gbmDevice())) {
      std::cout << "SKIP: NVIDIA routing unavailable\n";
      return 77;
    }
    // This is the boundary that a direct allocator-only probe misses: KWin
    // must associate the virtual output with its existing hardware renderer.
    // The optional switch reproduces the old bridge's software association.
    if (app.arguments().contains(QStringLiteral("--without-render-selection")))
      native_hooks_enabled = false;
    auto manager = std::make_unique<KWin::GpuManager>();
    auto *renderer = manager->compatibleRenderDevice(device.get());
    require(renderer && !renderer->isSoftwareDevice() && renderer->isNvidia(),
            "virtual output still selects a software renderer");
    const auto renderer_device = renderer->drmDevice();
    require(renderer_device &&
                is_nvidia_render_device(renderer_device->libdrmDevice()),
            "selected render device is not the real NVIDIA GPU");
    require(device->busType() == DRM_BUS_FAUX,
            "render association changed virtual KMS identity");
    std::cout << "KWin virtual-output hardware render selection: PASS\n";
    const auto &fds = owned_render_fds.at(device->gbmDevice());
    drmVersionPtr version = drmGetVersion(fds.kms);
    require(version && std::string_view(version->name, version->name_len) ==
                           "vibeshine_drm",
            "software fd does not refer to virtual KMS");
    drmFreeVersion(version);
    KWin::GraphicsBufferOptions opts;
    opts.size = QSize(64, 64);
    opts.format = DRM_FORMAT_XRGB8888;
    opts.software = true;
    opts.scanout = true;
    opts.render = false;
    auto software = device->allocator()->allocate(opts);
    require(software, "real KWin software scanout allocation failed");
    require(software->dmabufAttributes() &&
                software->dmabufAttributes()->modifier == DRM_FORMAT_MOD_LINEAR,
            "software allocation lost linear scanout");
    software->drop();
    require(!software_allocation,
            "software scope leaked after real KWin allocation");
    opts.software = false;
    opts.render = true;
    opts.modifiers = {DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 5),
                      DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 4),
                      DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 3),
                      DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 2),
                      DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 1),
                      DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 0)};
    auto swapchain = KWin::EglSwapchain::create(
        device->allocator(), renderer->eglContext(), opts);
    require(bool(swapchain) && bool(swapchain->acquire()),
            "KWin failed to create a GPU scanout swapchain");
    std::cout << "KWin GPU scanout swapchain: PASS\n";
    EGLDisplay egl = eglGetCurrentDisplay();
    require(egl != EGL_NO_DISPLAY, "KWin render EGL display unavailable");
    const EGLContext kwin_context = eglGetCurrentContext();
    const EGLenum kwin_api = eglQueryAPI();
    require(eglBindAPI(EGL_OPENGL_API), "OpenGL API unavailable");
    EGLint config_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                             EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config;
    EGLint config_count;
    require(eglChooseConfig(egl, config_attrs, &config, 1, &config_count) && config_count,
            "GL config unavailable");
    EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                              EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
    EGLContext context =
        eglCreateContext(egl, config, EGL_NO_CONTEXT, context_attrs);
    require(context != EGL_NO_CONTEXT &&
                eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, context),
            "GL context unavailable");
    auto target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
        "glEGLImageTargetTexture2DOES");
    require(target, "legacy capture import entry point unavailable");
    for (auto format : {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888}) {
      opts.format = format;
      auto hardware = device->allocator()->allocate(opts);
      require(hardware, "real KWin GPU scanout allocation failed");
      const auto attrs = hardware->dmabufAttributes();
      require(attrs && attrs->modifier != DRM_FORMAT_MOD_LINEAR &&
                  attrs->modifier != DRM_FORMAT_MOD_INVALID,
              "hardware allocation is not explicit tiled GPU storage");
      // Register a private framebuffer on the actual virtual KMS device.
      // It is never attached to an output, so this does not modeset.
      uint32_t handles[4]{}, pitches[4]{}, offsets[4]{};
      uint64_t modifiers[4]{};
      require(drmPrimeFDToHandle(fds.kms, attrs->fd[0].get(), &handles[0]) == 0,
              "virtual KMS PRIME import of GPU scanout failed");
      pitches[0] = attrs->pitch[0];
      offsets[0] = attrs->offset[0];
      modifiers[0] = attrs->modifier;
      uint32_t framebuffer = 0;
      const int added = drmModeAddFB2WithModifiers(
          fds.kms, 64, 64, format, handles, pitches, offsets, modifiers,
          &framebuffer, DRM_MODE_FB_MODIFIERS);
      drmCloseBufferHandle(fds.kms, handles[0]);
      require(added == 0, "virtual KMS rejected explicit tiled framebuffer");
      require(drmModeRmFB(fds.kms, framebuffer) == 0,
              "private scanout framebuffer cleanup failed");
      const EGLAttrib image_attrs[] = {EGL_WIDTH,
                                       64,
                                       EGL_HEIGHT,
                                       64,
                                       EGL_LINUX_DRM_FOURCC_EXT,
                                       format,
                                       EGL_DMA_BUF_PLANE0_FD_EXT,
                                       attrs->fd[0].get(),
                                       EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                       attrs->offset[0],
                                       EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                       attrs->pitch[0],
                                       EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                                       EGLAttrib(attrs->modifier & 0xffffffff),
                                       EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                                       EGLAttrib(attrs->modifier >> 32),
                                       EGL_NONE};
      EGLImage image = eglCreateImage(
          egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, image_attrs);
      require(image != EGL_NO_IMAGE, "tiled EGL import failed");
      GLuint texture, fbo;
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      while (glGetError()) {
      }
      target(GL_TEXTURE_2D, image);
      require(glGetError() == GL_NO_ERROR,
              "HEVC-compatible direct GL import failed");
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, texture, 0);
      require(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                  GL_FRAMEBUFFER_COMPLETE,
              "GPU scanout framebuffer incomplete");
      const float reference[] = {0.25f, 0.5f, 0.75f, 1};
      glClearBufferfv(GL_COLOR, 0, reference);
      float actual[4]{};
      glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, actual);
      require(glGetError() == GL_NO_ERROR, "GPU pixel check failed");
      for (int i = 0; i < 4; ++i)
        require(std::abs(actual[i] - reference[i]) < 0.003,
                "GPU import changed pixel values");
      std::cout << "format=0x" << std::hex << format << " tiled modifier=0x"
                << attrs->modifier << std::dec
                << " direct HEVC GL import and GPU pixels: PASS\n";
      glDeleteFramebuffers(1, &fbo);
      glDeleteTextures(1, &texture);
      eglDestroyImage(egl, image);
      hardware->drop();
    }
    eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl, context);
    require(eglBindAPI(kwin_api) &&
                eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, kwin_context),
            "restoring KWin context for resource cleanup failed");
    swapchain.reset();
    manager.reset();
    device.reset();
    require(owned_render_fds.empty(), "routed descriptors leaked");
    std::cout
        << "native allocator lifecycle: PASS; no outputs or services changed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
