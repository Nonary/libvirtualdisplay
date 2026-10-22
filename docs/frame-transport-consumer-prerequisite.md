# Frame transport caller and importer implementation

This records the opt-in #626 host and driver source implementation. In the
default build `QueryFrameTransportCapabilities` reports zero. With the explicit
native producer option enabled, target query, WDF register/clear and the IddCx
producer are connected. The host adapter is behind a separate default-off
option. No signed payload or release lock is changed; native qualification
remains open.

## Wire and ownership

The independently versioned layouts are 176-byte registration, 48-byte reply,
104-byte clear and one 4096-byte metadata page. All handle values are 64-bit
source-process values. There is no PID in the payload. Six source handles remain
host-owned. Native import duplicates from an OS-authenticated requestor process
into the receiving process and owns only those duplicates. Partial failure uses
RAII. An acknowledgment reports generation, with no ownership adoption transfer.

A native adapter must obtain identity from the WDF request, not an IOCTL PID,
device opener PID or forwarded pipe payload. The portable peer is PID plus
creation time; tests treat it as already authenticated. The native helper checks
creation time again while retaining the process handle through duplication.
WDF identity capture/process-exit/PID-reuse and UMDF access to the SYSTEM host
must still be qualified. Broker forwarding of handle-bearing requests is
unsupported.

The server checks the live capability-protected owner/display generation before
import. An authenticated process binds the current lease; changing process
requires reclaiming a fresh lease. Client cookies strictly increase within a
process/lease. Exact active retries are idempotent; cleared, canceled and failed
imports leave a high-water tombstone. This rejects delayed registrations after
cancellation/replacement/reset. Tombstones are bounded by live authorized
DisplayStore records and reaped when that owner/lease/display becomes invalid.
The control adapter must share DisplayStore serialization and call the reaper.

## Publication and consumer synchronization

There is no cross-process std::atomic or seqlock ABI. Immutable page header and
fresh section per registration; each slot's metadata is protected by that
texture's keyed mutex. Producer uses key 0 for free or key 1 for reclaim, always
with timeout 0, copies/submits/flushes on its own context, writes generation,
sequence and acquisition QPC, then releases key 1 and signals an event. Native
GPU ordering and pixel correctness have not been executed in this workspace.

The host waits at most 100 ms for the event (a hint), scans exactly four slots
with zero-time key-1 acquire, retains at most two during comparison and one for
copy, rejects wrong cookie/generation/zero or wrapping sequence, copies the
newest sequence into a separate private texture on the same immediate context,
and releases key 0. Duplicate sequences are discarded. Acquisition must equal
S_OK; WAIT_TIMEOUT and WAIT_ABANDONED must not be accepted through SUCCEEDED.
Two private output textures are preallocated. Outstanding encoder tokens must
survive through final GPU use; pool exhaustion drops without waiting.

Windows import validates adapter LUID, format, geometry, exact shared/mutex flags,
usage, bind flags, mapped header and object aliasing. No protected/resource flag
is accepted. Only BGRA8/P709 gamma 2.2 and RGBA16F/linear scRGB are represented.
This does not establish active IddCx mode/HDR metadata/cursor correctness.

## Bounded asynchronous I/O

The portable transaction transport deep-copies inputs and owns outputs and
source resource holds. It retains these through completion after timeout or
endpoint destruction, limits each exchange to 250 ms, refuses new work while
one operation remains unresolved, and limits unresolved transactions globally
to eight. If a driver never completes canceled work, those permits remain
occupied and further use fails closed; memory is not freed under native I/O.

The Windows source uses thread-pool I/O with heap OVERLAPPED ownership. Native
operations retain the endpoint and transaction until completion. CancelIoEx does
not imply completion. A failed immediate start cancels its thread-pool
notification; normal synchronous success still receives a completion callback.
This adapter has not compiled or executed on Windows in this workspace.
Cancellation clear is best effort: a busy unresolved endpoint may reject it,
so lease reaping remains mandatory and no immediate native revocation is claimed.

## Qualification required before enabling or shipping the opt-in path

1. Compile the host/driver with the real MSVC SDK/WDK, including all new native
   translation units. Verify WDF requestor identity and UMDF process rights or
   redesign the authenticated duplication path. Prove process-exit/PID-reuse and
   impersonation/cancellation behavior rather than trusting portable peer data.
2. Verify request identity, bounded control dispatch, reaping, swapchain assign/
   unassign, departure, mode/adapter changes, reset and device loss in native
   runs. Prove retained GPU generations remain bounded under churn.
3. Execute partial import, alias/wrong object, malformed metadata, timeout/lost
   completion, abandoned mutex, consumer crash, replacement and teardown cases.
   Verify cross-device GPU completion and output encoder-token lifetimes.
4. Test old/new host/driver pairs and explicit WGC fallback on real displays.
   Verify SDR/HDR, cursor composition and acquisition QPC, then pixel correctness
   and present-to-received timing at 60/120/240 Hz. Only then build/sign/pin a
   producer payload and migrate a reviewed host change downstream.

Primary references:
- [WDF requestor PID](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestgetrequestorprocessid)
- [UMDF request impersonation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestimpersonate)
- [DuplicateHandle source/target ownership](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle)
- [CompareObjectHandles](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-compareobjecthandles)
- [Keyed mutex return values](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync)
- [StartThreadpoolIo](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-startthreadpoolio)
- [CloseThreadpoolIo](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-closethreadpoolio)
