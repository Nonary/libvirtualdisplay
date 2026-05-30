#pragma once

#include "virtual_display/driver/control_protocol.h"

#ifdef _WIN32

#include <Windows.h>
#include <winioctl.h>

// Windows-facing adapter for the clean-room protocol. The underlying request
// structs intentionally stay platform-neutral so they can be exhaustively tested
// without WDK or Win32 headers.
namespace virtual_display::driver {
  inline GUID to_windows_guid(const Guid &guid) {
    return GUID {
      guid.data1,
      guid.data2,
      guid.data3,
      {
        guid.data4[0],
        guid.data4[1],
        guid.data4[2],
        guid.data4[3],
        guid.data4[4],
        guid.data4[5],
        guid.data4[6],
        guid.data4[7]
      }
    };
  }

  inline constexpr Guid from_windows_guid(const GUID &guid) {
    return Guid {
      guid.Data1,
      guid.Data2,
      guid.Data3,
      {
        guid.Data4[0],
        guid.Data4[1],
        guid.Data4[2],
        guid.Data4[3],
        guid.Data4[4],
        guid.Data4[5],
        guid.Data4[6],
        guid.Data4[7]
      }
    };
  }

  inline constexpr AdapterLuid from_windows_luid(const LUID &luid) {
    return {luid.LowPart, luid.HighPart};
  }

  inline constexpr LUID to_windows_luid(const AdapterLuid &luid) {
    return {luid.low_part, luid.high_part};
  }

  inline constexpr DWORD kWinIoctlGetProtocolVersion =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::GetProtocolVersion), METHOD_BUFFERED, FILE_ANY_ACCESS);
  inline constexpr DWORD kWinIoctlCreateTemporaryDisplay =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::CreateTemporaryDisplay), METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD kWinIoctlRemoveTemporaryDisplay =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::RemoveTemporaryDisplay), METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD kWinIoctlFeedLease =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::FeedLease), METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD kWinIoctlReleaseLease =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::ReleaseLease), METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD kWinIoctlQueryLease =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::QueryLease), METHOD_BUFFERED, FILE_READ_DATA);
  inline constexpr DWORD kWinIoctlSetPermanentDisplayCount =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::SetPermanentDisplayCount), METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD kWinIoctlQueryPermanentDisplayCount =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::QueryPermanentDisplayCount), METHOD_BUFFERED, FILE_READ_DATA);
  inline constexpr DWORD kWinIoctlQueryDisplayState =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::QueryDisplayState), METHOD_BUFFERED, FILE_READ_DATA);
  inline constexpr DWORD kWinIoctlSetDisplayManifest =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::SetDisplayManifest), METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD kWinIoctlQueryDisplayManifest =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::QueryDisplayManifest), METHOD_BUFFERED, FILE_READ_DATA);
  inline constexpr DWORD kWinIoctlSetRenderAdapter =
    CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<DWORD>(IoctlFunction::SetRenderAdapter), METHOD_BUFFERED, FILE_WRITE_DATA);

  static_assert(sizeof(GUID) == sizeof(Guid));
  static_assert(sizeof(LUID) == sizeof(AdapterLuid));
  static_assert(kWinIoctlGetProtocolVersion == kIoctlGetProtocolVersion);
  static_assert(kWinIoctlCreateTemporaryDisplay == kIoctlCreateTemporaryDisplay);
  static_assert(kWinIoctlRemoveTemporaryDisplay == kIoctlRemoveTemporaryDisplay);
  static_assert(kWinIoctlFeedLease == kIoctlFeedLease);
  static_assert(kWinIoctlReleaseLease == kIoctlReleaseLease);
  static_assert(kWinIoctlQueryLease == kIoctlQueryLease);
  static_assert(kWinIoctlSetPermanentDisplayCount == kIoctlSetPermanentDisplayCount);
  static_assert(kWinIoctlQueryPermanentDisplayCount == kIoctlQueryPermanentDisplayCount);
  static_assert(kWinIoctlQueryDisplayState == kIoctlQueryDisplayState);
  static_assert(kWinIoctlSetDisplayManifest == kIoctlSetDisplayManifest);
  static_assert(kWinIoctlQueryDisplayManifest == kIoctlQueryDisplayManifest);
  static_assert(kWinIoctlSetRenderAdapter == kIoctlSetRenderAdapter);
}  // namespace virtual_display::driver

#endif
