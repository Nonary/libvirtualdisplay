# Experimental frame transport

This is the opt-in source path for Nonary/Vibepollo#626. It implements the
owner-authenticated ring, WDF request identity adapter, IddCx producer, and
matching host capture integration. Both host and native driver build options
default off. No signed driver, install, or release is part of this source change.

Protocol 3.8 adds `QueryFrameTransportCapabilities` (read access, no input, a
40-byte `FrameTransportCapabilities` reply) and an owner-authenticated target
query. The schema version is independent of the display-control minor version.
The dispatcher reports zero capabilities in the default build and reports the
four-slot keyed-mutex ring only when the explicit producer option is enabled.
The host still requires a live target query and successful ring registration
before selecting frame push. The client treats compatible 3.6/3.7 drivers as
having zero capabilities without issuing the new IOCTL. Old control requests
and protocol minimum 3.6 are unchanged.

`FrameRingLifetime` and `FrameRingServer` implement registration and lifetime.
The WDF path obtains the requestor PID from the actual request, binds the live
owner lease/display generation, duplicates only the requestor's handles, and
rejects broker-forwarded handle payloads. The IddCx worker validates its
assigned adapter and active surface mode, copies into a bounded four-texture
ring without waiting on busy slots, publishes keyed-mutex-protected metadata,
and retires resources with the display lifetime. The host adapter consumes a
private D3D11 copy and is compiled only by its matching default-off option.

## Ownership and retirement implemented here

- Only a live temporary display created with an owner capability can register.
  Lease ID, display ID, owner capability and the driver's display generation
  must all match. Enumeration, permanent displays, plain unowned creation,
  expired leases and pending departure grant no pixel access.
- Registration retains a validated resource bundle only on success. Failed
  authorization or import leaves caller ownership unchanged. Imported native
  resources belong to that bundle, never to a raw numeric-handle field here.
- Each replacement receives a driver-assigned nonzero ring generation. Scoped
  clear, device-loss completion and display departure cannot retire a newer
  ring/display generation. Reset retires registrations without reusing tokens
  during that device lifetime. Unavailable registrations are pruned on new
  registration so display-ID churn cannot retain an unbounded map.
- A producer snapshot retains the bundle across replacement, reset or clear.
  Native code must retain that snapshot through its final GPU access. Removing
  the registration cannot release resources while a snapshot remains alive.
  Rechecking authorization before a new snapshot stops expired/reclaimed or
  departing displays from supplying new work. Work already submitted may finish.
- Retirement also revokes an atomic publication gate shared with every retained
  snapshot. Native producers can check `can_publish()` without calling the store
  or taking the control lock on their frame hot path. The reaper must call
  `retire_unavailable()` even for an idle producer. A native generation-fenced
  publication protocol must handle revocation racing with an already queued copy;
  this primitive alone cannot cancel a GPU operation or publish shared metadata.
- Callers must serialize this object with the referenced `DisplayStore`, whose
  lifetime must exceed its own. It performs no waits or GPU operations. Native
  resource destructors must release owned references without waiting for a host.

## Native constraints and remaining qualification

1. Preserve the control interface ACL (SYSTEM and the broker service). The SYSTEM
   host already opens the driver directly; broker-mediated requests remain
   unsupported and need a separate authenticated handle-transfer design. The
   handle source process comes from the WDF request context, not a PID claimed
   in the request. Direct requestor identity, process-exit and cancellation
   behavior still require Windows qualification.
2. Duplicate only validated texture/section/event handles into driver ownership;
   never adopt or close an arbitrary number in WUDFHost's handle table. Keep
   requestor process identity alive through duplication. Define exactly which
   side cleans up each duplicate on validation failure, partial import,
   cancellation, successful registration and IOCTL completion loss. Unnamed
   handles avoid globally named objects and cross-session namespace collisions.
3. Import on the render adapter LUID of the assigned IddCx swap chain. Check
   each actual texture's dimensions, format, array/mip/sample layout, bind flags,
   shared-NT-handle and keyed-mutex support. Restrict v1 to four textures with
   matching SDR BGRA8 or HDR FP16 surfaces. A mode/HDR/adapter change retires the
   old ring; it must never `CopyResource` across incompatible descriptions.
   Preserve protected-content restrictions, HDR/color metadata and the host's
   cursor-composition contract; do not export a hardware-protected source. The
   first host adapter accepts verified SDR P709 only. FP16/scRGB is described
   by the native ring but falls back until end-to-end color mapping is added.
4. The producer tries keyed mutex key 0 with zero timeout; it may reclaim an
   unconsumed key-1 slot only under the shared sequence protocol. Only `S_OK`
   means ownership: `WAIT_TIMEOUT` and `WAIT_ABANDONED` are nonnegative statuses.
   A busy ring drops a frame without waiting. An abandoned mutex, copy/device
   failure or sequence exhaustion retires the exact ring and requests recovery.
5. Publish metadata only after queuing the copy and releasing to key 1 with
   proper GPU submission. The consumer acquires key 1, rechecks generation and
   sequence under ownership, then returns key 0 after its last GPU read completes.
   CPU image destruction alone is insufficient when the encoder/converter is
   still reading. Explicit D3D11/D3D12 fences require a separately negotiated ABI
   and completion protocol; they are not implied by keyed-mutex support.
6. Define a fixed-layout shared header with interprocess atomics, publication
   ordering and sequence-wrap behavior. Stamp driver acquisition QPC, separately
   from scan-out QPC. The event is a hint; the sequence is authoritative. Bound
   retained ring generations when a producer/consumer stalls. Stop, clear,
   unassign, departure, lease reclaim/expiry, consumer crash, device loss and
   driver cleanup must quiesce publication without blocking IddCx callbacks.

## Release and validation gates

Portable tests cover the actual capability client/dispatcher and registration
primitive, including wrong owner, expiry, failed registration, late clear/error,
display recreation and in-flight resource lifetime. They do not execute WDF,
DXGI handle duplication, GPU synchronization or IddCx callbacks.

The current host packages self-signed release v1.6.4 from revision
`2a29ea71a52d15522c95b3de532b6612c54d75cc`, independently of the submodule's
source revision. Neither build option enables frame push by default. A native
Windows/WDK build, direct requestor identity/process access checks, four old/new
host-driver combinations, handle-negative tests, and lifecycle stress must
precede enabling or shipping it. Keep WGC/DDA available throughout.

Validate frame correctness and acquire-to-receiver timing at 60/120/240 Hz on
AMD/NVIDIA/Intel, SDR/HDR, and across pause, resume, mode/adapter changes and
host/driver restart. The linked Rubylight implementation is a useful reference,
but its separate IOCTL namespace and WUDFHost handle-transfer contract are not
compatible with this driver's existing broker interface.
