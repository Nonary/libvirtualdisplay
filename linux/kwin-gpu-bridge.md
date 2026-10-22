# KWin virtual rendering bridge

The native Linux package preloads `libvibeshine-kwin-gpu.so` into the KDE
desktop and login compositors. Only `/usr/bin/kwin_wayland` enables its hooks;
other processes pass through unchanged and compositor children do not inherit
the preload.

For KWin's recognized legacy GBM allocator ABI (including KWin 6.7), the
bridge gives a Vibeshine faux KMS device a GBM allocator opened on a real
NVIDIA render node. The original KMS fd remains the output's scanout endpoint.
Physical DRM devices, libdrm identities and enumeration order are unchanged.
KWin and EGL use their normal per-output GPU import path. This also avoids advertising the virtual card as a second copy of an
NVIDIA PCI device to unrelated GBM/EGL consumers inside the compositor.

KWin 6.8 separates render allocation from KMS scanout allocation. Its software
scanout path calls `CREATE_DUMB` on `gbm_device_get_fd()`, which must remain the
virtual KMS fd: Linux rejects this ioctl on an NVIDIA render node with `EACCES`.
For a matching KWin 6.8 library, the bridge scopes software allocation back to
an owned duplicate of the original virtual KMS fd. Dumb-buffer creation,
mapping and destruction therefore use the KMS device; EGL and hardware
allocation keep the NVIDIA render fd. This scope is thread-local and restored
after nested or failed allocations. Physical seat allocators retain their fds.

KWin 6.8 also resolves each display's compatible render device independently
of its GBM allocator. For the Vibeshine faux device alone, the bridge asks
KWin's compatibility policy for the same verified NVIDIA render device. The
physical seat and libdrm device identities are unchanged. If that renderer is
unavailable, KWin's original compatibility policy remains the fallback. Merely
testing direct GBM allocation does not cover this decision: validation must
also exercise KWin's GPU manager and the selected output renderer.

The native shim requires the installed public KWin 6.8 headers and Qt Core at
build time. At runtime it checks the exact KWin library filename used at build
time before reading `GraphicsBufferOptions`. A different library or unknown
allocator ABI preserves native allocation until the bridge is rebuilt and
validated. Updating KWin without rebuilding the bridge can therefore return
virtual capture to software buffers. The bridge still removes the preload
environment before compositor children start. Physical GPU allocators and DRM
permissions are unchanged. A running compositor must
restart through the normal logout/reboot cycle to load an updated bridge;
restarting the streaming service does not replace its already loaded library.

When the native bridge cannot enable its hooks, its journal diagnostic reports
both the library it was built for and the current resolved allocator pathname
(or `unknown` when resolution fails). This pathname is not verified mapped DSO
identity: a SONAME symlink may have changed since KWin loaded its allocator.
The diagnostic explicitly marks mapped identity as unverified. A pathname mismatch
explains why the safe native allocation path was retained; it does **not** prove
that KWin changed ABI, that CPU upload is occurring, or that rebuilding will
restore a particular frame rate. After installing a matching rebuilt package
and restarting the compositor, verify the virtual GPU association, capture
import path, and requested frame cadence separately. Do not bypass the exact
library guard to suppress the warning.

KWin still chooses its primary GPU using its own renderer/output policy;
accelerating the faux renderer can change that choice.

Association is automatic. With multiple NVIDIA render devices, the lowest PCI
address (domain, bus, device, function) is the stable default, independent of
DRM enumeration and render-node numbering. To choose a different GPU, set its
PCI identity explicitly for each intended KWin service:

```ini
# /etc/systemd/user/plasma-kwin_wayland.service.d/90-virtual-renderer.conf
[Service]
Environment=VIBESHINE_KWIN_RENDER_PCI=0000:01:00.0
```

The login compositor uses `plasma-login-kwin_wayland.service.d` with the same
setting. These settings take effect when the corresponding compositor next
starts. No monitor or dummy plug is required by the bridge. An unrelated seat
GPU, including a VM's Virtio console, retains its own GBM device.

`VIBESHINE_KWIN_RENDER_PCI` selects **virtual-output rendering**. Vibeshine's
`adapter_name` selects **capture/encoding** and must not implicitly change the
compositor's seat primary. To use the same GPU for both, resolve the chosen
render node's `/sys/class/drm/renderD*/device` to its PCI identity and configure
the virtual renderer deliberately. The automatic default does not infer the
encoder or KWin's eventual primary renderer: KWin constructs these allocators
before choosing its primary GPU. Raw `renderD*` numbering can change after
GPU or VM display-device changes; the bridge rechecks PCI identity after open
and refuses a different device. It does not read service-private machine state
or mutate a running compositor's environment.

Invalid or unmatched explicit selection, or no available NVIDIA renderer, leaves the virtual node's
original GBM path intact; physical outputs are unaffected. Such a fallback does
not establish that private output publication or streaming will work. Likewise,
a successful association log establishes only the allocator route: validate
console output, KScreen hotplug, client frames and requested cadence on the
actual GPU topology. The bridge is built from the pinned libvirtualdisplay
source as part of the native Linux package; updating only source without the
packaged library does not update a running compositor.

The bridge-specific desktop and greeter drop-ins also set
`KWIN_DISABLE_UDMABUF_IMPORT=1` before KWin starts. KWin's NVIDIA SHM import
safeguard otherwise sees the faux KMS identity instead of the actual EGL GPU.
This disables SHM udmabuf acceleration for the whole compositor; SHM uses CPU
upload, while ordinary DMA-BUF import and NVIDIA output allocation remain enabled.

When KWin's low-bandwidth retry allocates an implicit NVIDIA buffer, plain
AddFB2 records a zero modifier without establishing linear storage. The virtual
DRM driver preserves that uncertainty for imported PRIME: GET_FRAME reports
`DRM_FORMAT_MOD_INVALID` and skips CPU mapping. Native dumb linear buffers and
explicitly declared modifiers keep their original handling. This preserves layout
provenance; it does not make every implicit buffer importable by NVIDIA EGL.
NVIDIA output buffers should use the negotiated explicit modifier path.
