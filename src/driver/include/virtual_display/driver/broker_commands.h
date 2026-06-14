#pragma once

#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/control_protocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace virtual_display::driver {
  struct BrokerPipeServerPolicy {
    bool reject_remote_clients {};
    bool require_client_authorization {};
    std::string access_denied_response;
  };

  inline constexpr std::uint32_t kBrokerPipeTypeMessage = 0x00000004u;
  inline constexpr std::uint32_t kBrokerPipeReadmodeMessage = 0x00000002u;
  inline constexpr std::uint32_t kBrokerPipeWait = 0x00000000u;
  inline constexpr std::uint32_t kBrokerPipeRejectRemoteClients = 0x00000008u;

  std::string trim_broker_command(std::string value);
  bool parse_broker_u32(std::string_view value, std::uint32_t &parsed);
  bool parse_broker_i32(std::string_view value, std::int32_t &parsed);
  bool broker_luid_text_is_valid(std::string_view value);
  bool broker_u32_text_is_valid(std::string_view value);
  std::vector<std::string_view> split_broker_words(std::string_view text, std::size_t max_words);
  std::wstring widen_ascii(std::string_view text);
  std::wstring quote_broker_argument(const std::wstring &value);
  std::string escape_key_value(std::string_view value);
  std::string broker_guid_string(const Guid &guid);
  std::string format_permanent_state(const PermanentDisplayCountResult &state);
  std::string format_display_state(const QueryDisplayStateResult &state);
  std::string format_display_manifest(const DisplayManifest &manifest);
  std::optional<PermanentDisplayCountRequest> parse_permanent_set_command(std::string_view command);
  std::optional<DisplayManifestProfile> parse_manifest_profile_set_command(std::string_view command);
  std::optional<std::wstring> helper_arguments_for_broker_command(std::string_view command);
  BrokerPipeServerPolicy broker_pipe_server_policy();
  std::uint32_t broker_pipe_mode(const BrokerPipeServerPolicy &policy);
  bool broker_pipe_should_serve_client(const BrokerPipeServerPolicy &policy, bool client_authorized);
  std::string broker_pipe_rejection_response(const BrokerPipeServerPolicy &policy);
}  // namespace virtual_display::driver
