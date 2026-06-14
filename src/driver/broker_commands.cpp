#include "virtual_display/driver/broker_commands.h"

#include "virtual_display/driver/display_identity.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace virtual_display::driver {
  std::string trim_broker_command(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
      value.pop_back();
    }
    while (!value.empty() && value.front() == ' ') {
      value.erase(value.begin());
    }
    return value;
  }

  bool parse_broker_u32(const std::string_view value, std::uint32_t &parsed) {
    std::uint32_t output {};
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, output);
    if (result.ec != std::errc {} || result.ptr != end) {
      return false;
    }
    parsed = output;
    return true;
  }

  bool parse_broker_i32(const std::string_view value, std::int32_t &parsed) {
    std::int32_t output {};
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, output);
    if (result.ec != std::errc {} || result.ptr != end) {
      return false;
    }
    parsed = output;
    return true;
  }

  bool broker_luid_text_is_valid(const std::string_view value) {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= value.size()) {
      return false;
    }

    std::int32_t high {};
    std::uint32_t low {};
    return parse_broker_i32(value.substr(0, separator), high) && parse_broker_u32(value.substr(separator + 1), low);
  }

  bool broker_u32_text_is_valid(const std::string_view value) {
    std::uint32_t parsed {};
    return parse_broker_u32(value, parsed);
  }

  std::vector<std::string_view> split_broker_words(const std::string_view text, const std::size_t max_words) {
    std::vector<std::string_view> words;
    std::size_t cursor = 0;
    while (cursor < text.size() && words.size() < max_words) {
      while (cursor < text.size() && text[cursor] == ' ') {
        ++cursor;
      }
      if (cursor >= text.size()) {
        break;
      }

      const auto begin = cursor;
      while (cursor < text.size() && text[cursor] != ' ') {
        ++cursor;
      }
      words.emplace_back(text.substr(begin, cursor - begin));
    }
    return words;
  }

  std::wstring widen_ascii(const std::string_view text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const unsigned char ch: text) {
      wide.push_back(static_cast<wchar_t>(ch));
    }
    return wide;
  }

  std::wstring quote_broker_argument(const std::wstring &value) {
    std::wstring quoted {L"\""};
    std::size_t backslashes = 0;
    for (wchar_t ch: value) {
      if (ch == L'"') {
        quoted.append(backslashes * 2 + 1, L'\\');
        quoted += ch;
        backslashes = 0;
        continue;
      }
      if (ch == L'\\') {
        ++backslashes;
        continue;
      }
      if (backslashes != 0) {
        quoted.append(backslashes, L'\\');
        backslashes = 0;
      }
      quoted += ch;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
  }

  std::string escape_key_value(const std::string_view value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size());
    for (const unsigned char ch : value) {
      if (ch == '\\') {
        output += "\\\\";
      } else if (ch == '\n') {
        output += "\\n";
      } else if (ch == '\r') {
        output += "\\r";
      } else if (ch == '\t') {
        output += "\\t";
      } else if (ch < 0x20 || ch == 0x7f) {
        output += "\\x";
        output += kHex[ch >> 4];
        output += kHex[ch & 0x0f];
      } else {
        output += static_cast<char>(ch);
      }
    }
    return output;
  }

  std::string broker_guid_string(const Guid &guid) {
    char text[37] {};
    std::snprintf(
      text,
      sizeof(text),
      "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      static_cast<unsigned int>(guid.data1),
      static_cast<unsigned int>(guid.data2),
      static_cast<unsigned int>(guid.data3),
      static_cast<unsigned int>(guid.data4[0]),
      static_cast<unsigned int>(guid.data4[1]),
      static_cast<unsigned int>(guid.data4[2]),
      static_cast<unsigned int>(guid.data4[3]),
      static_cast<unsigned int>(guid.data4[4]),
      static_cast<unsigned int>(guid.data4[5]),
      static_cast<unsigned int>(guid.data4[6]),
      static_cast<unsigned int>(guid.data4[7])
    );
    return text;
  }

  namespace {
    std::string display_name(const char (&value)[kDisplayNameChars]) {
      return std::string {trim_display_name(value)};
    }

    std::string output_display_name(const char (&value)[kDisplayNameChars]) {
      return escape_key_value(display_name(value));
    }

    const char *display_kind(const std::uint32_t kind) {
      if (kind == kDisplayStateKindPermanent) {
        return "permanent";
      }
      if (kind == kDisplayStateKindTemporary) {
        return "temporary";
      }
      return "unknown";
    }

    void set_display_name(char (&target)[kDisplayNameChars], const std::string_view name) {
      std::fill(std::begin(target), std::end(target), '\0');
      std::memcpy(target, name.data(), (std::min)(name.size(), static_cast<std::size_t>(kDisplayNameChars - 1)));
    }
  }  // namespace

  std::string format_permanent_state(const PermanentDisplayCountResult &state) {
    std::ostringstream output;
    output
      << "permanent_displays=" << state.current_display_count << '\n'
      << "max_permanent_displays=" << state.max_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "mode=" << state.width << 'x' << state.height << '@'
      << (state.refresh_rate_millihz / 1000.0) << "Hz\n"
      << "physical_size_mm=" << state.physical_width_mm << 'x' << state.physical_height_mm << '\n'
      << "name=" << output_display_name(state.display_name) << '\n';
    return output.str();
  }

  std::string format_display_state(const QueryDisplayStateResult &state) {
    std::ostringstream output;
    output
      << "permanent_displays=" << state.permanent_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "display_entries=" << state.entry_count << '\n';

    for (std::uint32_t index = 0; index < state.entry_count && index < kMaxDisplayStateEntries; ++index) {
      const auto &entry = state.entries[index];
      output
        << "display." << index << ".kind=" << display_kind(entry.kind) << '\n'
        << "display." << index << ".display_id=" << entry.display_id << '\n'
        << "display." << index << ".lease_id=" << entry.lease_id << '\n'
        << "display." << index << ".connector_index=" << entry.connector_index << '\n'
        << "display." << index << ".container_id=" << broker_guid_string(entry.container_id) << '\n'
        << "display." << index << ".product_code=" << entry.product_code << '\n'
        << "display." << index << ".serial_number=" << entry.serial_number << '\n'
        << "display." << index << ".mode=" << entry.width << 'x' << entry.height << '@'
        << (entry.refresh_rate_millihz / 1000.0) << "Hz\n"
        << "display." << index << ".physical_size_mm=" << entry.physical_width_mm << 'x'
        << entry.physical_height_mm << '\n'
        << "display." << index << ".hdr_supported="
        << ((entry.flags & kDisplayStateFlagHdrSupported) ? 1 : 0) << '\n'
        << "display." << index << ".retain_identity="
        << ((entry.flags & kDisplayStateFlagRetainIdentity) ? 1 : 0) << '\n'
        << "display." << index << ".name=" << output_display_name(entry.display_name) << '\n';
    }

    return output.str();
  }

  std::string format_display_manifest(const DisplayManifest &manifest) {
    std::ostringstream output;
    output
      << "manifest_version=" << manifest.version << '\n'
      << "manifest_profiles=" << manifest.profile_count << '\n'
      << "manifest_max_profiles=" << manifest.max_profile_count << '\n';

    for (std::uint32_t index = 0; index < manifest.profile_count && index < kMaxPermanentDisplayProfiles; ++index) {
      const auto &profile = manifest.profiles[index];
      const bool mode_valid =
        profile.allowed_mode_count > 0 &&
        profile.allowed_mode_count <= kMaxAllowedModesPerProfile &&
        profile.native_mode_index < profile.allowed_mode_count;
      const auto mode = mode_valid ? profile.allowed_modes[profile.native_mode_index] : DisplayMode {};
      output
        << "profile." << index << ".connector_index=" << profile.connector_index << '\n'
        << "profile." << index << ".display_id=" << profile.display_id << '\n'
        << "profile." << index << ".container_id=" << broker_guid_string(profile.container_id) << '\n'
        << "profile." << index << ".product_code=" << profile.product_code << '\n'
        << "profile." << index << ".serial_number=" << profile.serial_number << '\n'
        << "profile." << index << ".mode=";
      if (mode_valid) {
        output << mode.width << 'x' << mode.height << '@'
               << (mode.refresh_rate_millihz / 1000.0) << "Hz\n";
      } else {
        output << "invalid\n";
      }
      output
        << "profile." << index << ".physical_size_mm=" << profile.physical_width_mm << 'x'
        << profile.physical_height_mm << '\n'
        << "profile." << index << ".hdr_supported="
        << ((profile.flags & kDisplayManifestProfileFlagHdrSupported) ? 1 : 0) << '\n'
        << "profile." << index << ".retain_identity="
        << ((profile.flags & kDisplayManifestProfileFlagRetainIdentity) ? 1 : 0) << '\n'
        << "profile." << index << ".layout_policy=" << profile.layout_policy << '\n'
        << "profile." << index << ".position=" << profile.position_x << ',' << profile.position_y << '\n'
        << "profile." << index << ".name=" << output_display_name(profile.display_name) << '\n';
    }

    return output.str();
  }

  std::optional<PermanentDisplayCountRequest> parse_permanent_set_command(const std::string_view command) {
    constexpr std::string_view prefix {"permanent-set "};
    if (!command.starts_with(prefix)) {
      return std::nullopt;
    }

    const auto payload = command.substr(prefix.size());
    const auto fields = split_broker_words(payload, 6);
    if (fields.size() != 6) {
      return std::nullopt;
    }

    std::size_t name_offset = 0;
    for (const auto field: fields) {
      name_offset = static_cast<std::size_t>((field.data() + field.size()) - payload.data());
    }
    while (name_offset < payload.size() && payload[name_offset] == ' ') {
      ++name_offset;
    }
    if (name_offset >= payload.size()) {
      return std::nullopt;
    }

    PermanentDisplayCountRequest request {};
    if (!parse_broker_u32(fields[0], request.display_count) ||
        !parse_broker_u32(fields[1], request.width) ||
        !parse_broker_u32(fields[2], request.height) ||
        !parse_broker_u32(fields[3], request.physical_width_mm) ||
        !parse_broker_u32(fields[4], request.physical_height_mm) ||
        !parse_broker_u32(fields[5], request.refresh_rate_millihz)) {
      return std::nullopt;
    }
    set_display_name(request.display_name, payload.substr(name_offset));
    return request;
  }

  std::optional<DisplayManifestProfile> parse_manifest_profile_set_command(const std::string_view command) {
    constexpr std::string_view prefix {"manifest-profile-set "};
    if (!command.starts_with(prefix)) {
      return std::nullopt;
    }

    const auto payload = command.substr(prefix.size());
    const auto fields = split_broker_words(payload, 10);
    if (fields.size() != 10) {
      return std::nullopt;
    }

    std::size_t name_offset = 0;
    for (const auto field: fields) {
      name_offset = static_cast<std::size_t>((field.data() + field.size()) - payload.data());
    }
    while (name_offset < payload.size() && payload[name_offset] == ' ') {
      ++name_offset;
    }
    if (name_offset >= payload.size()) {
      return std::nullopt;
    }

    std::uint32_t slot {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t physical_width_mm {};
    std::uint32_t physical_height_mm {};
    std::uint32_t refresh_rate_millihz {};
    std::uint32_t hdr_supported {};
    std::uint32_t layout_policy {};
    std::int32_t position_x {};
    std::int32_t position_y {};
    if (!parse_broker_u32(fields[0], slot) ||
        !parse_broker_u32(fields[1], width) ||
        !parse_broker_u32(fields[2], height) ||
        !parse_broker_u32(fields[3], physical_width_mm) ||
        !parse_broker_u32(fields[4], physical_height_mm) ||
        !parse_broker_u32(fields[5], refresh_rate_millihz) ||
        !parse_broker_u32(fields[6], hdr_supported) ||
        !parse_broker_u32(fields[7], layout_policy) ||
        !parse_broker_i32(fields[8], position_x) ||
        !parse_broker_i32(fields[9], position_y)) {
      return std::nullopt;
    }

    DisplayManifestProfile profile {};
    profile.flags = kDisplayManifestProfileFlagRetainIdentity |
      kDisplayManifestProfileFlagPermanentIdentity;
    if (hdr_supported != 0) {
      profile.flags |= kDisplayManifestProfileFlagHdrSupported;
    }
    profile.connector_index = slot;
    profile.display_id = permanent_display_id(slot);
    profile.container_id = container_guid_from_display_id(profile.display_id);
    std::copy(
      std::begin(kSunshineDriverManufacturerId),
      std::end(kSunshineDriverManufacturerId),
      std::begin(profile.manufacturer_id)
    );
    profile.product_code = permanent_product_code(slot);
    profile.serial_number = serial_number_from_display_id(profile.display_id);
    profile.physical_width_mm = physical_width_mm;
    profile.physical_height_mm = physical_height_mm;
    profile.native_mode_index = 0;
    profile.allowed_mode_count = 1;
    profile.layout_policy = layout_policy;
    profile.position_x = position_x;
    profile.position_y = position_y;
    profile.orientation = kDisplayManifestOrientationDefault;
    profile.allowed_modes[0] = DisplayMode {width, height, refresh_rate_millihz};
    set_display_name(profile.display_name, payload.substr(name_offset));
    return profile;
  }

  std::optional<std::wstring> helper_arguments_for_broker_command(const std::string_view command) {
    if (command == "helper-diagnose") {
      return L"--diagnose";
    }
    if (command == "helper-apply-extended-topology") {
      return L"--apply-extended-topology";
    }
    if (command == "helper-apply-manifest-topology") {
      return L"--apply-manifest-topology";
    }
    if (command == "helper-query-color-profiles") {
      return L"--query-color-profiles";
    }
    if (command == "helper-stress-capture-remove") {
      return L"--stress-capture-remove";
    }
    constexpr std::string_view stress_prefix {"helper-stress-capture-remove "};
    if (command.starts_with(stress_prefix)) {
      const auto payload = command.substr(stress_prefix.size());
      const auto fields = split_broker_words(payload, 5);
      if (fields.size() != 4) {
        return std::nullopt;
      }
      for (const auto field: fields) {
        if (!broker_u32_text_is_valid(field)) {
          return std::nullopt;
        }
      }

      std::wstring arguments = quote_broker_argument(L"--stress-capture-remove");
      for (const auto field: fields) {
        arguments += L" ";
        arguments += quote_broker_argument(widen_ascii(field));
      }
      return arguments;
    }
    constexpr std::string_view associate_prefix {"helper-associate-color-profile "};
    if (command.starts_with(associate_prefix)) {
      const auto payload = command.substr(associate_prefix.size());
      const auto fields = split_broker_words(payload, 4);
      if (fields.size() != 4) {
        return std::nullopt;
      }

      std::size_t profile_offset = 0;
      for (const auto field: fields) {
        profile_offset = static_cast<std::size_t>((field.data() + field.size()) - payload.data());
      }
      while (profile_offset < payload.size() && payload[profile_offset] == ' ') {
        ++profile_offset;
      }
      if (profile_offset >= payload.size()) {
        return std::nullopt;
      }

      if (!broker_luid_text_is_valid(fields[0]) || !broker_u32_text_is_valid(fields[1])) {
        return std::nullopt;
      }

      std::wstring arguments = quote_broker_argument(L"--associate-color-profile");
      arguments += L" ";
      arguments += quote_broker_argument(widen_ascii(fields[0]));
      arguments += L" ";
      arguments += quote_broker_argument(widen_ascii(fields[1]));
      arguments += L" ";
      arguments += quote_broker_argument(widen_ascii(payload.substr(profile_offset)));
      if (fields[2] == "advanced") {
        arguments += L" --advanced-color";
      } else if (fields[2] != "standard") {
        return std::nullopt;
      }
      if (fields[3] == "default") {
        arguments += L" --default";
      } else if (fields[3] != "nodefault") {
        return std::nullopt;
      }
      return arguments;
    }
    return std::nullopt;
  }

  BrokerPipeServerPolicy broker_pipe_server_policy() {
    return {
      true,
      true,
      "error access_denied\n"
    };
  }

  std::uint32_t broker_pipe_mode(const BrokerPipeServerPolicy &policy) {
    return kBrokerPipeTypeMessage |
           kBrokerPipeReadmodeMessage |
           kBrokerPipeWait |
           (policy.reject_remote_clients ? kBrokerPipeRejectRemoteClients : 0u);
  }

  bool broker_pipe_should_serve_client(
    const BrokerPipeServerPolicy &policy,
    const bool client_authorized
  ) {
    return !policy.require_client_authorization || client_authorized;
  }

  std::string broker_pipe_rejection_response(const BrokerPipeServerPolicy &policy) {
    return broker_pipe_should_serve_client(policy, false) ? std::string {} : policy.access_denied_response;
  }
}  // namespace virtual_display::driver
