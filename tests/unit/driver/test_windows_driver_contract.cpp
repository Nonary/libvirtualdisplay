#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {
  constexpr std::uintmax_t kMaxContractFileBytes = 2u * 1024u * 1024u;

  std::string read_text_file_limited(const std::filesystem::path &path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
      ADD_FAILURE() << "Failed to stat " << path.string() << ": " << error.message();
      return {};
    }
    if (size > kMaxContractFileBytes) {
      ADD_FAILURE() << "Contract source file too large: " << path.string() << " size=" << size;
      return {};
    }

    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::string content(static_cast<std::size_t>(size), '\0');
    if (!content.empty()) {
      file.read(content.data(), static_cast<std::streamsize>(content.size()));
      if (file.gcount() != static_cast<std::streamsize>(content.size())) {
        ADD_FAILURE() << "Failed to read complete file " << path.string();
        return {};
      }
    }
    content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());
    return content;
  }

  std::string strip_cpp_comments(const std::string &content) {
    enum class State {
      Code,
      String,
      Character,
      LineComment,
      BlockComment,
    };

    std::string stripped;
    stripped.reserve(content.size());

    State state {State::Code};
    for (std::size_t i = 0; i < content.size(); ++i) {
      const char current = content[i];
      const char next = i + 1u < content.size() ? content[i + 1u] : '\0';

      switch (state) {
        case State::Code:
          if (current == '/' && next == '/') {
            stripped.push_back(' ');
            stripped.push_back(' ');
            ++i;
            state = State::LineComment;
          } else if (current == '/' && next == '*') {
            stripped.push_back(' ');
            stripped.push_back(' ');
            ++i;
            state = State::BlockComment;
          } else {
            stripped.push_back(current);
            if (current == '"') {
              state = State::String;
            } else if (current == '\'') {
              state = State::Character;
            }
          }
          break;
        case State::String:
          stripped.push_back(current);
          if (current == '\\' && next != '\0') {
            stripped.push_back(next);
            ++i;
          } else if (current == '"') {
            state = State::Code;
          }
          break;
        case State::Character:
          stripped.push_back(current);
          if (current == '\\' && next != '\0') {
            stripped.push_back(next);
            ++i;
          } else if (current == '\'') {
            state = State::Code;
          }
          break;
        case State::LineComment:
          if (current == '\n') {
            stripped.push_back(current);
            state = State::Code;
          } else {
            stripped.push_back(' ');
          }
          break;
        case State::BlockComment:
          if (current == '*' && next == '/') {
            stripped.push_back(' ');
            stripped.push_back(' ');
            ++i;
            state = State::Code;
          } else {
            stripped.push_back(current == '\n' ? '\n' : ' ');
          }
          break;
      }
    }

    return stripped;
  }

  std::string read_windows_driver_source() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "src/driver/windows_driver/driver_main.cpp";
    return read_text_file_limited(path);
  }

  std::string read_windows_driver_inf() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "src/driver/windows_driver/SunshineVirtualDisplayDriver.inf";
    return read_text_file_limited(path);
  }

  std::string read_windows_driver_cmake() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "src/driver/windows_driver/CMakeLists.txt";
    return read_text_file_limited(path);
  }

  std::string read_windows_control_client_source() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "src/driver/windows_control_client.cpp";
    return read_text_file_limited(path);
  }

  std::string read_driver_cmake() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "src/driver/CMakeLists.txt";
    return read_text_file_limited(path);
  }

  std::string read_root_cmake() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "CMakeLists.txt";
    return read_text_file_limited(path);
  }

  std::string read_readme() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "README.md";
    return read_text_file_limited(path);
  }

  std::string read_support_diagnostics() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "docs/support-diagnostics.md";
    return read_text_file_limited(path);
  }

  std::string read_release_workflow() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      ".github/workflows/release.yml";
    return read_text_file_limited(path);
  }

  std::string read_top_issues_workflow() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      ".github/workflows/_top-issues.yml";
    return read_text_file_limited(path);
  }

  std::string read_codeql_workflow() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      ".github/workflows/_codeql.yml";
    return read_text_file_limited(path);
  }

  std::string read_common_lint_workflow() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      ".github/workflows/_common-lint.yml";
    return read_text_file_limited(path);
  }

  std::string read_ci_workflow() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      ".github/workflows/ci.yml";
    return read_text_file_limited(path);
  }

  std::string read_tests_cmake() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "tests/CMakeLists.txt";
    return read_text_file_limited(path);
  }
}  // namespace

TEST(VirtualDisplayWindowsDriverContract, DeletesMonitorObjectWhenArrivalFails) {
  const auto source = strip_cpp_comments(read_windows_driver_source());

  const auto arrival = source.find("status = IddCxMonitorArrival(create_out.MonitorObject, &arrival_out);");
  ASSERT_NE(arrival, std::string::npos);

  const auto failure = source.find("if (!NT_SUCCESS(status))", arrival);
  ASSERT_NE(failure, std::string::npos);

  const auto cleanup = source.find("WdfObjectDelete(create_out.MonitorObject);", failure);
  ASSERT_NE(cleanup, std::string::npos);

  const auto backend_failure = source.find("return {vdd::BackendError::Failed", failure);
  ASSERT_NE(backend_failure, std::string::npos);
  EXPECT_LT(cleanup, backend_failure);
}

TEST(VirtualDisplayWindowsDriverContract, RequestsSwapChainStopBeforeDeparture) {
  const auto source = read_windows_driver_source();

  const auto depart = source.find("vdd::BackendError depart_display");
  ASSERT_NE(depart, std::string::npos);

  const auto request_stop = source.find("request_stop_swapchain_processor(processor_to_stop);", depart);
  ASSERT_NE(request_stop, std::string::npos);

  const auto departure = source.find("IddCxMonitorDeparture(monitor_handle);", request_stop);
  ASSERT_NE(departure, std::string::npos);
  EXPECT_LT(request_stop, departure);

  const auto stop = source.find("stop_swapchain_processor_without_delete(processor_to_stop);", departure);
  ASSERT_NE(stop, std::string::npos);
  EXPECT_LT(departure, stop);
  EXPECT_NE(source.find("monitor departure", depart), std::string::npos);

  const auto helper = source.find("void stop_swapchain_processor_without_delete");
  ASSERT_NE(helper, std::string::npos);
  const auto abandon = source.find("processor->abandon_swapchain();", helper);
  ASSERT_NE(abandon, std::string::npos);

  const auto cleanup = source.find("processor.reset();", abandon);
  ASSERT_NE(cleanup, std::string::npos);
  EXPECT_LT(abandon, cleanup);
}

TEST(VirtualDisplayWindowsDriverContract, AbandonsSwapChainAssignedDuringDeparture) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN"), std::string::npos);
  EXPECT_NE(source.find("generic failures trip verifier 0x700"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, UsesDigitalSinkTechnologyForHdrClassification) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL"), std::string::npos);
  EXPECT_NE(source.find("legacy bandwidth ceiling"), std::string::npos);
  EXPECT_NE(source.find("WCG-only"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, StoresTemporaryIdentityInWdfPersistentState) {
  const auto source = read_windows_driver_source();
  const auto inf = read_windows_driver_inf();

  EXPECT_NE(source.find("WdfDriverOpenPersistentStateRegistryKey(driver, desired_access"), std::string::npos);
  EXPECT_NE(source.find("WdfDeviceOpenRegistryKey("), std::string::npos);
  EXPECT_NE(source.find("KEY_READ"), std::string::npos);
  EXPECT_NE(source.find("KEY_SET_VALUE"), std::string::npos);
  EXPECT_NE(source.find("kPersistentStateSchemaVersion"), std::string::npos);
  EXPECT_NE(source.find("TemporaryDisplayProfiles"), std::string::npos);
  EXPECT_NE(source.find("REG_BINARY"), std::string::npos);
  EXPECT_NE(source.find("entry.connector_index == descriptor.connector_index"), std::string::npos);
  const auto persistent_state = source.find("std::vector<TemporaryDisplayProfile> load_temporary_display_profiles");
  ASSERT_NE(persistent_state, std::string::npos);
  const auto persistent_state_end = source.find("bool read_registry_string", persistent_state);
  ASSERT_NE(persistent_state_end, std::string::npos);
  const auto persistent_state_body = source.substr(persistent_state, persistent_state_end - persistent_state);
  EXPECT_EQ(source.find("KEY_ALL_ACCESS"), std::string::npos);
  EXPECT_EQ(persistent_state_body.find("KEY_WRITE"), std::string::npos);
  EXPECT_EQ(source.find("WdfRegistryCreateKey"), std::string::npos);
  EXPECT_EQ(source.find("SOFTWARE\\\\Sunshine\\\\VirtualDisplayDriver"), std::string::npos);
  EXPECT_EQ(persistent_state_body.find("RegEnumKeyExW"), std::string::npos);
  EXPECT_EQ(persistent_state_body.find("RegCreateKeyExW"), std::string::npos);
  EXPECT_NE(inf.find("HKR,,\"ConfigVersion\",0x00010001,1"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, ValidatesTemporaryProfileConnectorRange) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("bool has_bytes(const std::vector<std::uint8_t> &blob"), std::string::npos);
  EXPECT_NE(source.find("if (!has_bytes(blob, offset, sizeof(std::uint32_t)))"), std::string::npos);
  EXPECT_NE(source.find("if (!has_bytes(blob, offset, sizeof(GUID)))"), std::string::npos);
  EXPECT_NE(source.find("if (!has_bytes(blob, offset, 12))"), std::string::npos);
  EXPECT_NE(source.find("connector_index >= kMaxPermanentDisplays"), std::string::npos);
  EXPECT_NE(source.find("connector_index < kMaxPermanentDisplays + kMaxTemporaryDisplays"), std::string::npos);
  EXPECT_EQ(source.find("blob.size() - offset <"), std::string::npos);
  EXPECT_EQ(source.find("query_registry_value"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, LeavesPermanentPolicyPersistenceToBroker) {
  const auto source = read_windows_driver_source();

  EXPECT_EQ(source.find("kDisplayManifestValue"), std::string::npos);
  EXPECT_EQ(source.find("load_persistent_display_manifest"), std::string::npos);
  EXPECT_EQ(source.find("save_display_manifest(driver_, device_, manifest)"), std::string::npos);
  EXPECT_EQ(source.find("persisted_manifest_"), std::string::npos);
  EXPECT_NE(source.find("vdd::BackendError apply_display_manifest(const vdd::DisplayManifest &manifest) override"), std::string::npos);
  EXPECT_EQ(source.find("permanent_display_count_"), std::string::npos);
  EXPECT_NE(source.find("load_temporary_connector_reservations(driver, device)"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, SetsSwapChainDeviceFromProcessingThread) {
  const auto source = read_windows_driver_source();

  const auto start = source.find("HRESULT start(const LUID &render_adapter_luid)");
  ASSERT_NE(start, std::string::npos);
  const auto process_call = source.find("process_frames(render_adapter_luid);", start);
  ASSERT_NE(process_call, std::string::npos);

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto reset = source.find("reset_render_device(render_adapter_luid);", process_frames);
  ASSERT_NE(reset, std::string::npos);

  const auto assign = source.find("HRESULT assign_swapchain_device()");
  ASSERT_NE(assign, std::string::npos);
  const auto set_device = source.find("IddCxSwapChainSetDevice(swapchain_, &set_device);", assign);
  ASSERT_NE(set_device, std::string::npos);
  EXPECT_NE(source.find("HandleNewSwapChain still owns IddCx's internal OPM cleanup", assign), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, SwapChainStartupDoesNotBlockAssignCallback) {
  const auto source = read_windows_driver_source();

  const auto start = source.find("HRESULT start(const LUID &render_adapter_luid)");
  ASSERT_NE(start, std::string::npos);
  const auto start_end = source.find("void stop()", start);
  ASSERT_NE(start_end, std::string::npos);
  const auto start_body = source.substr(start, start_end - start);
  EXPECT_NE(start_body.find("return S_OK;"), std::string::npos);
  EXPECT_EQ(start_body.find("wait("), std::string::npos);

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto reset = source.find("reset_render_device(render_adapter_luid);", process_frames);
  ASSERT_NE(reset, std::string::npos);
  const auto assign = source.find("HRESULT assign_swapchain_device()");
  ASSERT_NE(assign, std::string::npos);
  EXPECT_NE(source.find("IddCxSwapChainSetDevice(swapchain_, &set_device);", assign), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, FailedAsyncStartupDeletesSwapChain) {
  const auto source = read_windows_driver_source();

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto startup_failure = source.find("if (FAILED(hr))", process_frames);
  ASSERT_NE(startup_failure, std::string::npos);
  const auto normal_loop = source.find("while (!stop_requested_.load(std::memory_order_acquire))", startup_failure);
  ASSERT_NE(normal_loop, std::string::npos);
  EXPECT_NE(source.substr(startup_failure, normal_loop - startup_failure).find("delete_swapchain();"), std::string::npos);

  const auto assign_swapchain = source.find("NTSTATUS assign_swapchain");
  ASSERT_NE(assign_swapchain, std::string::npos);
  const auto failed_start = source.find("if (FAILED(hr))", assign_swapchain);
  ASSERT_NE(failed_start, std::string::npos);
  EXPECT_NE(source.find("processor->abandon_swapchain();", failed_start), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, WorkerClosesSwapChainOnStartupException) {
  const auto source = read_windows_driver_source();

  const auto start = source.find("HRESULT start(const LUID &render_adapter_luid)");
  ASSERT_NE(start, std::string::npos);
  const auto thread_body = source.find("worker_ = std::thread([this, render_adapter_luid]()", start);
  ASSERT_NE(thread_body, std::string::npos);
  EXPECT_NE(source.find("catch (...)", thread_body), std::string::npos);
  EXPECT_NE(source.find("delete_swapchain();", thread_body), std::string::npos);
  EXPECT_EQ(source.find("publish_startup_result"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, StopsSwapChainOnFrameCompletionFailure) {
  const auto source = read_windows_driver_source();

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto finished = source.find("const HRESULT finished_result = IddCxSwapChainFinishedProcessingFrame(swapchain_);", process_frames);
  ASSERT_NE(finished, std::string::npos);
  const auto failure = source.find("SwapChainFinishedFrameFailed", finished);
  ASSERT_NE(failure, std::string::npos);
  EXPECT_NE(source.find("delete_swapchain();", failure), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, RegistersSwapChainWorkerWithMmcss) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("#include <avrt.h>"), std::string::npos);
  EXPECT_NE(source.find("kSwapchainMmcssTask[] = L\"DisplayPostProcessing\""), std::string::npos);
  EXPECT_NE(source.find("AvSetMmThreadCharacteristicsW(task_name, &task_index_)"), std::string::npos);
  EXPECT_NE(source.find("AvRevertMmThreadCharacteristics(handle_)"), std::string::npos);

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto mmcss = source.find("MmcssRegistration mmcss {kSwapchainMmcssTask};", process_frames);
  ASSERT_NE(mmcss, std::string::npos);
  const auto create_device = source.find("reset_render_device(render_adapter_luid);", process_frames);
  ASSERT_NE(create_device, std::string::npos);
  EXPECT_LT(mmcss, create_device);
}

TEST(VirtualDisplayWindowsDriverContract, SynchronizesAdapterReadinessPublication) {
  const auto source = read_windows_driver_source();

  const auto init_finished = source.find("NTSTATUS adapter_init_finished");
  ASSERT_NE(init_finished, std::string::npos);
  const auto lock = source.find("std::lock_guard lock {mutex_};", init_finished);
  ASSERT_NE(lock, std::string::npos);
  const auto status = source.find("adapter_init_status_ = args->AdapterInitStatus;", init_finished);
  ASSERT_NE(status, std::string::npos);
  const auto ready = source.find("adapter_ready_ = NT_SUCCESS(adapter_init_status_);", status);
  ASSERT_NE(ready, std::string::npos);
  EXPECT_LT(lock, status);
  EXPECT_LT(status, ready);
}

TEST(VirtualDisplayWindowsDriverContract, AppliesPreferredRenderAdapterThroughIddCx) {
  const auto source = read_windows_driver_source();

  const auto setter = source.find("vdd::BackendError set_render_adapter(const vdd::SetRenderAdapterRequest &request) override");
  ASSERT_NE(setter, std::string::npos);
  EXPECT_NE(source.find("preferred_render_adapter_luid_ = preferred_render_adapter;", setter), std::string::npos);
  EXPECT_NE(source.find("IddCxAdapterSetRenderAdapter(adapter, &args);", setter), std::string::npos);

  const auto init_finished = source.find("NTSTATUS adapter_init_finished");
  ASSERT_NE(init_finished, std::string::npos);
  EXPECT_NE(source.find("preferred_render_adapter = preferred_render_adapter_luid_;", init_finished), std::string::npos);
  EXPECT_NE(source.find("IddCxAdapterSetRenderAdapter(adapter, &set_render_adapter);", init_finished), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, AdapterInitFailureIsNotReportedAsReady) {
  const auto source = read_windows_driver_source();

  const auto initialize_adapter = source.find("NTSTATUS initialize_adapter(WDFDEVICE device)");
  ASSERT_NE(initialize_adapter, std::string::npos);
  const auto adapter_ready = source.find("if (adapter_ready_)", initialize_adapter);
  ASSERT_NE(adapter_ready, std::string::npos);
  const auto adapter_exists = source.find("if (adapter_)", adapter_ready);
  ASSERT_NE(adapter_exists, std::string::npos);
  const std::string failed_or_pending_status =
    "return NT_SUCCESS(adapter_init_status_) ? STATUS_DEVICE_NOT_READY : adapter_init_status_;";
  EXPECT_NE(
    source.find(failed_or_pending_status, adapter_exists),
    std::string::npos
  );

  const auto async_init = source.find("IddCxAdapterInitAsync(&adapter_init, &adapter_out)", initialize_adapter);
  ASSERT_NE(async_init, std::string::npos);
  const auto missing_adapter_object = source.find("if (!adapter_out.AdapterObject)", async_init);
  ASSERT_NE(missing_adapter_object, std::string::npos);
  const auto publish_pending = source.find("adapter_init_status_ = STATUS_PENDING;", missing_adapter_object);
  ASSERT_NE(publish_pending, std::string::npos);
  EXPECT_NE(source.find("NTSTATUS adapter_init_status_ {STATUS_DEVICE_NOT_READY};"), std::string::npos);
  EXPECT_NE(source.find("\"AdapterInitFailed\""), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, RecoversRenderDeviceBeforeAbandoningSwapChain) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("D3D_DRIVER_TYPE_WARP"), std::string::npos);
  EXPECT_NE(source.find("is_device_lost_hresult"), std::string::npos);
  EXPECT_NE(source.find("DXGI_ERROR_DEVICE_REMOVED"), std::string::npos);
  EXPECT_NE(source.find("DXGI_ERROR_DEVICE_RESET"), std::string::npos);
  EXPECT_NE(source.find("SwapChainProcessingFailed"), std::string::npos);

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto acquire_failure = source.find("if (FAILED(acquire_result))", process_frames);
  ASSERT_NE(acquire_failure, std::string::npos);
  const auto device_lost = source.find("is_device_lost_hresult(acquire_result)", acquire_failure);
  ASSERT_NE(device_lost, std::string::npos);
  const auto reset = source.find("reset_render_device(render_adapter_luid)", device_lost);
  ASSERT_NE(reset, std::string::npos);
  const auto abandon = source.find("return;", reset);
  ASSERT_NE(abandon, std::string::npos);
  EXPECT_LT(reset, abandon);
  EXPECT_NE(source.find("delete_swapchain();", reset), std::string::npos);
  const auto assign_swapchain = source.find("NTSTATUS assign_swapchain");
  ASSERT_NE(assign_swapchain, std::string::npos);
  const auto unassign_swapchain = source.find("NTSTATUS unassign_swapchain", assign_swapchain);
  ASSERT_NE(unassign_swapchain, std::string::npos);
  EXPECT_EQ(
    source.substr(assign_swapchain, unassign_swapchain - assign_swapchain).find("return STATUS_UNSUCCESSFUL;"),
    std::string::npos
  );
}

TEST(VirtualDisplayWindowsDriverContract, SetsReleaseAndAcquireBuffer2MetadataSize) {
  const auto source = read_windows_driver_source();

  const auto buffer2_call = source.find("IddCxSwapChainReleaseAndAcquireBuffer2(swapchain_, &input, &acquired)");
  ASSERT_NE(buffer2_call, std::string::npos);
  const auto acquired = source.rfind("IDARG_OUT_RELEASEANDACQUIREBUFFER2 acquired {}", buffer2_call);
  ASSERT_NE(acquired, std::string::npos);
  const auto metadata_size = source.find("acquired.MetaData.Size = sizeof(acquired.MetaData);", acquired);
  ASSERT_NE(metadata_size, std::string::npos);
  EXPECT_LT(metadata_size, buffer2_call);
}

TEST(VirtualDisplayWindowsDriverContract, DeletesSwapChainOnUnexpectedWaitFailure) {
  const auto source = read_windows_driver_source();

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto wait_failure = source.find("if (wait_result != WAIT_OBJECT_0)", process_frames);
  ASSERT_NE(wait_failure, std::string::npos);
  const auto acquire_loop = source.find("for (;;) {", wait_failure);
  ASSERT_NE(acquire_loop, std::string::npos);
  const auto delete_swapchain = source.find("delete_swapchain();", wait_failure);
  ASSERT_NE(delete_swapchain, std::string::npos);
  const auto return_statement = source.find("return;", delete_swapchain);
  ASSERT_NE(return_statement, std::string::npos);
  EXPECT_LT(delete_swapchain, acquire_loop);
  EXPECT_LT(return_statement, acquire_loop);
}

TEST(VirtualDisplayWindowsDriverContract, MonitorCallbacksValidateCurrentHandle) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(
    source.find("auto find_current_monitor_locked(const std::uint64_t display_id, const IDDCX_MONITOR monitor)"),
    std::string::npos
  );
  EXPECT_NE(source.find("record->second.monitor != monitor"), std::string::npos);

  const std::vector<std::string> callback_sections {
    "NTSTATUS assign_swapchain",
    "NTSTATUS unassign_swapchain",
    "NTSTATUS set_default_hdr_metadata",
    "NTSTATUS set_gamma_ramp",
    "std::optional<ModeShape> requested_mode_shape"
  };
  for (const auto &section_name: callback_sections) {
    const auto section = source.find(section_name);
    ASSERT_NE(section, std::string::npos) << section_name;
    EXPECT_NE(
      source.find("find_current_monitor_locked(context->display_id, monitor)", section),
      std::string::npos
    ) << section_name;
  }
}

TEST(VirtualDisplayWindowsDriverContract, DeviceStateQuiescesBackendBeforeDestruction) {
  const auto source = read_windows_driver_source();

  const auto backend = source.find("class IddCxBackend");
  ASSERT_NE(backend, std::string::npos);
  EXPECT_NE(source.find("void shutdown()", backend), std::string::npos);
  EXPECT_NE(source.find("shutting_down_ = true;", backend), std::string::npos);
  EXPECT_NE(source.find("(void) depart_display(display_id);", backend), std::string::npos);
  EXPECT_NE(source.find("if (shutting_down_ || !adapter_ready_)", backend), std::string::npos);
  EXPECT_NE(source.find("record == monitors_.end() || record->second.departing || shutting_down_", backend), std::string::npos);
  EXPECT_NE(source.find("bool shutting_down_ {};"), std::string::npos);

  const auto destructor = source.find("~DeviceState()");
  ASSERT_NE(destructor, std::string::npos);
  const auto stop_reaper = source.find("stop_reaper();", destructor);
  ASSERT_NE(stop_reaper, std::string::npos);
  const auto shutdown = source.find("backend.shutdown();", stop_reaper);
  ASSERT_NE(shutdown, std::string::npos);
  EXPECT_LT(stop_reaper, shutdown);
}

TEST(VirtualDisplayWindowsDriverContract, AssignSwapChainDoesNotJoinWorkerWhileBackendLocked) {
  const auto source = read_windows_driver_source();

  const auto assign_swapchain = source.find("NTSTATUS assign_swapchain");
  ASSERT_NE(assign_swapchain, std::string::npos);
  const auto unassign_swapchain = source.find("NTSTATUS unassign_swapchain", assign_swapchain);
  ASSERT_NE(unassign_swapchain, std::string::npos);
  const auto assign_body = source.substr(assign_swapchain, unassign_swapchain - assign_swapchain);

  EXPECT_NE(assign_body.find("std::unique_ptr<SwapChainProcessor> previous_processor;"), std::string::npos);
  EXPECT_NE(assign_body.find("previous_processor = std::move(record->second.swapchain_processor);"), std::string::npos);
  EXPECT_NE(assign_body.find("record->second.swapchain_processor = std::move(processor);"), std::string::npos);

  const auto retire = assign_body.find("if (previous_processor)");
  ASSERT_NE(retire, std::string::npos);
  const auto request_stop = assign_body.find("previous_processor->request_stop();", retire);
  ASSERT_NE(request_stop, std::string::npos);
  EXPECT_EQ(assign_body.find("previous_processor->stop();", retire), std::string::npos);
  const auto retire_lock = assign_body.find("std::lock_guard lock {mutex_};", request_stop);
  ASSERT_NE(retire_lock, std::string::npos);
  EXPECT_LT(request_stop, retire_lock);
}

TEST(VirtualDisplayWindowsDriverContract, DepartureWaitsOnlyForInflightAssignCallbacks) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("std::uint32_t assign_callbacks_in_flight {};"), std::string::npos);
  EXPECT_NE(source.find("void finish_assign_callback"), std::string::npos);
  EXPECT_NE(source.find("AssignCallbackScope assign_scope {*this, context->display_id, monitor};"), std::string::npos);
  EXPECT_NE(source.find("++record->second.assign_callbacks_in_flight;"), std::string::npos);
  EXPECT_NE(source.find("--record->second.assign_callbacks_in_flight;"), std::string::npos);

  const auto depart_display = source.find("vdd::BackendError depart_display");
  ASSERT_NE(depart_display, std::string::npos);
  EXPECT_NE(source.find("departure_cv_.wait_for(lock, std::chrono::milliseconds(250)", depart_display), std::string::npos);
  const auto depart_display_end = source.find("NTSTATUS assign_swapchain", depart_display);
  ASSERT_NE(depart_display_end, std::string::npos);
  const auto depart_display_body = source.substr(depart_display, depart_display_end - depart_display);
  EXPECT_EQ(depart_display_body.find("std::this_thread::sleep_for(std::chrono::milliseconds(250));"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, GatesHdrDescriptorsOnRuntimeIddCxSupport) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("bool runtime_hdr_supported()"), std::string::npos);
  EXPECT_NE(source.find("has_hdr_iddcx_ddi() &&"), std::string::npos);
  EXPECT_NE(source.find("vdd::supports_windows_hdr_toggle(vdd::hdr_output_capabilities())"), std::string::npos);
  EXPECT_NE(source.find("options.hdr_supported = runtime_hdr_supported();"), std::string::npos);
  EXPECT_NE(source.find("descriptor_with_runtime_hdr_policy"), std::string::npos);
  EXPECT_NE(source.find("options.hdr_supported = false;"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, PrefersTenBitRgbModesWhenAvailable) {
  const auto source = read_windows_driver_source();

  const auto bits_function = source.find("IDDCX_BITS_PER_COMPONENT preferred_rgb_bits_per_component()");
  ASSERT_NE(bits_function, std::string::npos);

  const auto ten_bit_check = source.find("if (capabilities.output_bits.rgb_10bpc)", bits_function);
  ASSERT_NE(ten_bit_check, std::string::npos);
  const auto ten_bit_return = source.find("return IDDCX_BITS_PER_COMPONENT_10;", ten_bit_check);
  ASSERT_NE(ten_bit_return, std::string::npos);
  const auto eight_bit_check = source.find("if (capabilities.output_bits.rgb_8bpc)", ten_bit_return);
  ASSERT_NE(eight_bit_check, std::string::npos);
  EXPECT_LT(ten_bit_return, eight_bit_check);

  EXPECT_NE(
    source.find("populate_rgb_wire_bits(mode.BitsPerComponent, preferred_rgb_bits_per_component());"),
    std::string::npos
  );
  EXPECT_NE(
    source.find("populate_rgb_wire_bits(output->DitheringSupport, IDDCX_BITS_PER_COMPONENT_10);"),
    std::string::npos
  );
}

TEST(VirtualDisplayWindowsDriverContract, RegistersTraceLoggingProvider) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("#include <TraceLoggingProvider.h>"), std::string::npos);
  EXPECT_NE(source.find("TRACELOGGING_DEFINE_PROVIDER("), std::string::npos);
  EXPECT_NE(source.find("TraceLoggingRegister(g_trace_provider);"), std::string::npos);
  EXPECT_NE(source.find("config.EvtDriverUnload = SunshineEvtDriverUnload;"), std::string::npos);
  EXPECT_NE(source.find("TraceLoggingUnregister(g_trace_provider);"), std::string::npos);
  EXPECT_NE(source.find("TraceLoggingWrite("), std::string::npos);
  EXPECT_NE(source.find("\"DeviceIoControl\""), std::string::npos);
  EXPECT_NE(source.find("\"RenderDeviceLost\""), std::string::npos);
  EXPECT_NE(source.find("\"RenderDeviceCreated\""), std::string::npos);
  EXPECT_NE(source.find("\"MonitorArrived\""), std::string::npos);
  EXPECT_NE(source.find("\"MonitorDeparted\""), std::string::npos);
  EXPECT_NE(source.find("\"TemporaryIdentityUnreserveFailed\""), std::string::npos);
  EXPECT_NE(source.find("\"SwapChainAssigned\""), std::string::npos);
  EXPECT_NE(source.find("\"SwapChainUnassigned\""), std::string::npos);
  EXPECT_NE(source.find("\"DefaultHdrMetadataSet\""), std::string::npos);
  EXPECT_NE(source.find("\"GammaRampSet\""), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, BoundsControllerSerializationWaits) {
  const auto source = read_windows_driver_source();

  const auto device_state = source.find("class DeviceState");
  ASSERT_NE(device_state, std::string::npos);
  const auto dispatch = source.find("vdd::IoctlDispatchResult dispatch", device_state);
  ASSERT_NE(dispatch, std::string::npos);
  const auto reaper_loop = source.find("void reaper_loop()", dispatch);
  ASSERT_NE(reaper_loop, std::string::npos);
  const auto state_body = source.substr(device_state, source.find("NTSTATUS ntstatus_from_ioctl_status", device_state) - device_state);

  EXPECT_NE(state_body.find("static constexpr auto kControllerLockTimeout = std::chrono::seconds(30);"), std::string::npos);
  EXPECT_NE(state_body.find("std::timed_mutex controller_mutex {};"), std::string::npos);
  EXPECT_NE(state_body.find("lock.try_lock_for(kControllerLockTimeout)"), std::string::npos);
  EXPECT_NE(state_body.find("\"DeviceIoControlControllerBusy\""), std::string::npos);
  EXPECT_NE(state_body.find("controller_lock.try_lock_for(std::chrono::milliseconds(10))"), std::string::npos);
  EXPECT_NE(state_body.find("\"LeaseReaperSkippedBusyController\""), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, EnablesWppInflightRecorder) {
  const auto source = read_windows_driver_source();
  const auto cmake = read_windows_driver_cmake();
  const auto docs = read_support_diagnostics();

  EXPECT_NE(source.find("#include \"driver_main.tmh\""), std::string::npos);
  EXPECT_NE(source.find("WPP_CONTROL_GUIDS"), std::string::npos);
  EXPECT_NE(source.find("WPP_INIT_TRACING(driver_object, registry_path);"), std::string::npos);
  EXPECT_NE(source.find("WPP_CLEANUP(WdfDriverWdmGetDriverObject(driver));"), std::string::npos);
  EXPECT_NE(source.find("TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, \"DriverEntry\");"), std::string::npos);
  EXPECT_NE(source.find("TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, \"MonitorArrived\");"), std::string::npos);
  EXPECT_NE(source.find("TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, \"MonitorDeparted\");"), std::string::npos);
  EXPECT_NE(source.find("TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, \"SwapChainAssigned\");"), std::string::npos);
  EXPECT_NE(source.find("TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, \"RenderDeviceLost\");"), std::string::npos);

  EXPECT_NE(cmake.find("TraceWPP.exe"), std::string::npos);
  EXPECT_NE(cmake.find("ENABLE_WPP_RECORDER=1"), std::string::npos);
  EXPECT_NE(cmake.find("WPP_MACRO_USE_KM_VERSION_FOR_UM=1"), std::string::npos);
  EXPECT_NE(cmake.find("NO_DEFAULT_PATH"), std::string::npos);
  EXPECT_NE(cmake.find("SUNSHINE_DRIVER_WDK_ROOT"), std::string::npos);
  EXPECT_EQ(cmake.find("HINTS"), std::string::npos);

  EXPECT_NE(docs.find("WPP provider GUID: `{b0dcb744-045b-463b-9c2f-6a3c897d3458}`"), std::string::npos);
  EXPECT_NE(docs.find("Inflight Trace Recorder"), std::string::npos);
  EXPECT_NE(docs.find("!wdfkd.wdflogdump"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, ControlClientHandlesSetupApiAndHandleFailuresDefensively) {
  const auto source = read_windows_control_client_source();

  EXPECT_NE(source.find("struct UniqueHandle"), std::string::npos);
  EXPECT_NE(source.find("std::exchange(value, INVALID_HANDLE_VALUE)"), std::string::npos);
  EXPECT_NE(source.find("FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED"), std::string::npos);
  EXPECT_NE(source.find("constexpr DWORD kControlIoTimeoutMs = 30'000;"), std::string::npos);
  EXPECT_NE(source.find("CancelIoEx(handle_, &overlapped);"), std::string::npos);
  EXPECT_NE(source.find("native_error = ERROR_TIMEOUT;"), std::string::npos);
  EXPECT_NE(source.find("auto transport = std::make_unique<WindowsControlTransport>(handle.release());"), std::string::npos);
  EXPECT_NE(source.find("detail_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)"), std::string::npos);
  EXPECT_NE(source.find("native_error = paths.empty() ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;"), std::string::npos);
  EXPECT_NE(source.find("native_error = detail_size == 0 ? GetLastError() : ERROR_INVALID_DATA;"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, RecordsAdvancedColorCallbacksPerMonitor) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("IDDCX_DEFAULT_HDR_METADATA_TYPE default_hdr_metadata_type"), std::string::npos);
  EXPECT_NE(source.find("IDDCX_GAMMARAMP_TYPE gamma_ramp_type"), std::string::npos);
  EXPECT_NE(source.find("NTSTATUS set_default_hdr_metadata("), std::string::npos);
  EXPECT_NE(source.find("record->second.default_hdr_metadata_type = args->Type;"), std::string::npos);
  EXPECT_NE(source.find("record->second.default_hdr_metadata_size = args->Size;"), std::string::npos);
  EXPECT_NE(source.find("NTSTATUS set_gamma_ramp("), std::string::npos);
  EXPECT_NE(source.find("record->second.gamma_ramp_type = args->Type;"), std::string::npos);
  EXPECT_NE(source.find("record->second.gamma_ramp_size = args->GammaRampSizeInBytes;"), std::string::npos);
  EXPECT_NE(source.find("return context->backend->set_default_hdr_metadata(monitor, args);"), std::string::npos);
  EXPECT_NE(source.find("return context->backend->set_gamma_ramp(monitor, args);"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, DoesNotAdvertiseUnimplementedEndpointGammaTransform) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(
    source.find("caps.EndPointDiagnostics.GammaSupport = hdr_capabilities.endpoint_gamma_transform ?"),
    std::string::npos
  );
  EXPECT_NE(source.find("IDDCX_FEATURE_IMPLEMENTATION_NONE;"), std::string::npos);
  EXPECT_NE(source.find("SetGammaRamp callback registered for FP16/HDR diagnostics"), std::string::npos);
  EXPECT_NE(source.find("endpoint pixel-processing stage where this transform can be applied"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, RetainsHdrProfileAssociationForStableTemporaryIdentity) {
  const auto source = read_windows_driver_source();
  const auto cmake = read_windows_driver_cmake();

  EXPECT_NE(source.find("#include <Wtsapi32.h>"), std::string::npos);
  EXPECT_NE(source.find("active_console_user_sid_string()"), std::string::npos);
  EXPECT_NE(source.find("WTSQueryUserToken(session_id, &token)"), std::string::npos);
  EXPECT_NE(source.find("HKEY_USERS"), std::string::npos);
  EXPECT_NE(source.find("ICMProfileAC"), std::string::npos);
  EXPECT_NE(source.find("monitor_driver_value_for_identity"), std::string::npos);
  EXPECT_NE(source.find("schedule_hdr_profile_association_retention(device_, descriptor, arrival_out.OsTargetId);"), std::string::npos);
  EXPECT_NE(cmake.find("wtsapi32"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, RegistersGammaRampIndependentOfHdrCallbacks) {
  const auto source = read_windows_driver_source();

  const auto device_add = source.find("SunshineEvtDeviceAdd");
  ASSERT_NE(device_add, std::string::npos);
  const auto gamma = source.find("EvtIddCxMonitorSetGammaRamp = SunshineEvtSetGammaRamp;", device_add);
  ASSERT_NE(gamma, std::string::npos);
  const auto hdr = source.find("if (has_hdr_iddcx_ddi())", device_add);
  ASSERT_NE(hdr, std::string::npos);
  EXPECT_LT(gamma, hdr);
}

TEST(VirtualDisplayWindowsDriverContract, LinksTraceLoggingRuntime) {
  const auto cmake = read_windows_driver_cmake();
  EXPECT_NE(cmake.find("advapi32"), std::string::npos);
  EXPECT_NE(cmake.find("SUNSHINE_DRIVER_INF2CAT_OS"), std::string::npos);
  EXPECT_NE(cmake.find("/os:${SUNSHINE_DRIVER_INF2CAT_OS}"), std::string::npos);
  EXPECT_EQ(cmake.find("/os:10_X64"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, ResolvesSdkByRequiredWdkFiles) {
  const auto cmake = read_windows_driver_cmake();

  EXPECT_NE(cmake.find("_sunshine_driver_find_sdk_version"), std::string::npos);
  EXPECT_NE(cmake.find("_sunshine_driver_sdk_is_usable"), std::string::npos);
  EXPECT_NE(cmake.find("Include/wdf/umdf/${SUNSHINE_DRIVER_WDF_VERSION}"), std::string::npos);
  EXPECT_NE(cmake.find("WdfDriverStubUm.lib"), std::string::npos);
  EXPECT_NE(cmake.find("IddCxStub.lib"), std::string::npos);
  EXPECT_NE(cmake.find("Unable to find a WDK/SDK version with required WDF and IddCx files"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, PackagesCatalogFromCompleteDriverDirectory) {
  const auto cmake = read_windows_driver_cmake();

  EXPECT_NE(cmake.find("set(SUNSHINE_DRIVER_PACKAGE_DIR"), std::string::npos);
  EXPECT_NE(cmake.find("set(SUNSHINE_DRIVER_PACKAGED_DLL"), std::string::npos);
  EXPECT_NE(cmake.find("sunshinevirtualdisplaydriver.cat"), std::string::npos);
  EXPECT_NE(cmake.find("$<TARGET_FILE:${SUNSHINE_DRIVER_TARGET}>"), std::string::npos);
  EXPECT_NE(cmake.find("\"/driver:${SUNSHINE_DRIVER_PACKAGE_DIR}\""), std::string::npos);
  EXPECT_NE(
    cmake.find("DEPENDS \"${SUNSHINE_DRIVER_PACKAGED_INF}\" \"${SUNSHINE_DRIVER_PACKAGED_DLL}\""),
    std::string::npos
  );
  EXPECT_NE(cmake.find("VERBATIM"), std::string::npos);
  EXPECT_EQ(cmake.find("/driver:\"${CMAKE_CURRENT_BINARY_DIR}\""), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, RestrictsDriverPackageToAmd64InfSection) {
  const auto cmake = read_windows_driver_cmake();
  const auto inf = read_windows_driver_inf();

  EXPECT_NE(cmake.find("set(SUNSHINE_DRIVER_PLATFORM"), std::string::npos);
  EXPECT_NE(cmake.find("set(SUNSHINE_DRIVER_WDK_ARCH \"x64\")"), std::string::npos);
  EXPECT_NE(cmake.find("supports only x64/amd64"), std::string::npos);
  EXPECT_EQ(cmake.find("10_ARM64"), std::string::npos);
  EXPECT_EQ(cmake.find("10_X86"), std::string::npos);
  EXPECT_NE(inf.find("%ManufacturerName%=Standard,NTamd64"), std::string::npos);
  EXPECT_EQ(inf.find("Standard.NTarm64"), std::string::npos);
  EXPECT_EQ(inf.find("Standard.NTx86"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, DefaultsPreferredModeToFullHdSixtyHertz) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("std::uint32_t default_preferred_mode_index"), std::string::npos);
  EXPECT_NE(source.find("mode.width == 1920 && mode.height == 1080 && mode.refresh_rate_millihz == 60'000"), std::string::npos);
  EXPECT_NE(source.find("std::uint32_t preferred_index = default_preferred_mode_index(modes);"), std::string::npos);
  EXPECT_EQ(source.find("std::uint32_t preferred_index = 1;"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, DocumentsSupportDiagnosticsCapture) {
  const auto readme = read_readme();
  const auto docs = read_support_diagnostics();

  EXPECT_NE(readme.find("docs/support-diagnostics.md"), std::string::npos);
  EXPECT_NE(docs.find("Sunshine.VirtualDisplayDriver"), std::string::npos);
  EXPECT_NE(docs.find("{3d5d3bd9-8500-4523-9334-583f4b5e6f80}"), std::string::npos);
  EXPECT_NE(docs.find("logman start SunshineVDD"), std::string::npos);
  EXPECT_NE(docs.find("SunshineVirtualDisplayBroker"), std::string::npos);
  EXPECT_NE(docs.find("wevtutil qe Application"), std::string::npos);
  EXPECT_NE(docs.find("broker query-state"), std::string::npos);
  EXPECT_NE(docs.find("broker helper-query-color-profiles"), std::string::npos);
  EXPECT_NE(docs.find("HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers"), std::string::npos);
  EXPECT_NE(docs.find("IddCxDebugCtrl=0x0f4"), std::string::npos);
  EXPECT_NE(docs.find("logman create trace IddCx"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, PackagesDriverSymbolsWithReleaseZip) {
  const auto root_cmake = read_root_cmake();
  const auto cmake = read_windows_driver_cmake();
  const auto driver_cmake = read_driver_cmake();
  const auto readme = read_readme();

  EXPECT_NE(root_cmake.find("LICENSE is still the template placeholder"), std::string::npos);
  EXPECT_NE(root_cmake.find("CPACK_PACKAGE_FILE_NAME"), std::string::npos);
  EXPECT_NE(root_cmake.find("_libvirtualdisplay_package_arch"), std::string::npos);
  EXPECT_NE(driver_cmake.find("add_library(${MODULE} STATIC"), std::string::npos);
  EXPECT_NE(driver_cmake.find("option(BUILD_VIRTUALDISPLAY_TOOLS"), std::string::npos);
  EXPECT_NE(driver_cmake.find("add_custom_target(virtualdisplay_tools DEPENDS"), std::string::npos);
  EXPECT_NE(
    cmake.find("install(FILES \"$<TARGET_PDB_FILE:${SUNSHINE_DRIVER_TARGET}>\" DESTINATION driver OPTIONAL)"),
    std::string::npos
  );
  EXPECT_NE(
    readme.find("driver/SunshineVirtualDisplayDriver.pdb"),
    std::string::npos
  );
  EXPECT_NE(
    readme.find("tools/virtualdisplay_broker.exe"),
    std::string::npos
  );
  EXPECT_NE(
    readme.find("cmake --build build-driver --target SunshineVirtualDisplayDriver virtualdisplay_tools"),
    std::string::npos
  );
}

TEST(VirtualDisplayWindowsDriverContract, ReleaseWorkflowPublishesSelfSignedPackageEvidence) {
  const auto workflow = read_release_workflow();

  EXPECT_NE(workflow.find("Find-UsableSdkVersion"), std::string::npos);
  EXPECT_NE(workflow.find("Unable to find a WDK/SDK version with required WDF and IddCx files."), std::string::npos);
  EXPECT_NE(workflow.find("-DBUILD_VIRTUALDISPLAY_TOOLS=ON"), std::string::npos);
  EXPECT_NE(workflow.find("$semverPattern = '^v(0|[1-9]\\d*)"), std::string::npos);
  EXPECT_NE(workflow.find("Release version must be a v-prefixed semantic version tag"), std::string::npos);
  EXPECT_NE(workflow.find("git checkout --detach $version"), std::string::npos);
  EXPECT_NE(workflow.find("Import release signing certificate"), std::string::npos);
  EXPECT_NE(workflow.find("VDD_SIGNING_CERT_PFX_BASE64 is required for release driver catalog signing."), std::string::npos);
  EXPECT_NE(workflow.find("cert_path=$cerPath"), std::string::npos);
  EXPECT_NE(workflow.find("Build driver package files"), std::string::npos);
  EXPECT_NE(workflow.find("SunshineVirtualDisplayDriverPackageFiles"), std::string::npos);
  EXPECT_NE(workflow.find("Sign driver catalog"), std::string::npos);
  EXPECT_NE(workflow.find("signtool.FullName sign /fd SHA256 /sha1"), std::string::npos);
  EXPECT_NE(workflow.find("signtool.FullName verify /pa"), std::string::npos);
  EXPECT_NE(workflow.find("terminated in a root\\s+certificate which is not trusted"), std::string::npos);
  EXPECT_NE(workflow.find("exit 0"), std::string::npos);
  EXPECT_NE(workflow.find("Add signing certificate to release package"), std::string::npos);
  EXPECT_NE(workflow.find("SunshineVirtualDisplayDriver.cer"), std::string::npos);
  EXPECT_NE(workflow.find("Generate package evidence"), std::string::npos);
  EXPECT_NE(workflow.find("Get-FileHash -LiteralPath $package[0].FullName -Algorithm SHA256"), std::string::npos);
  EXPECT_NE(workflow.find("package_sha256"), std::string::npos);
  EXPECT_NE(workflow.find("channel = 'self-signed'"), std::string::npos);
  EXPECT_NE(workflow.find("certificate_thumbprint"), std::string::npos);
  EXPECT_NE(workflow.find("driver_catalog = 'self-signed'"), std::string::npos);
  EXPECT_NE(workflow.find("Driver package signing model: self-signed catalog."), std::string::npos);
  EXPECT_EQ(workflow.find("no EV, HLK, WHQL, or Windows certification claim"), std::string::npos);
  EXPECT_NE(workflow.find("prerelease: false"), std::string::npos);
  EXPECT_NE(workflow.find("overwrite_files: true"), std::string::npos);
  EXPECT_NE(workflow.find("body_path: build-driver/release-notes.md"), std::string::npos);
  EXPECT_NE(workflow.find("build-driver/libvirtualdisplay-*-evidence.json"), std::string::npos);
  EXPECT_EQ(workflow.find("release_evidence_json"), std::string::npos);
  EXPECT_EQ(workflow.find("LIBVIRTUALDISPLAY_RELEASE_EVIDENCE_JSON"), std::string::npos);
  EXPECT_EQ(workflow.find("Validate release evidence"), std::string::npos);
  EXPECT_EQ(workflow.find("Validate release package evidence"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, CiWorkflowResolvesUsableWdkBeforePackaging) {
  const auto workflow = read_ci_workflow();

  EXPECT_NE(workflow.find("Find-UsableSdkVersion"), std::string::npos);
  EXPECT_NE(workflow.find("Include\\wdf\\umdf\\2.25"), std::string::npos);
  EXPECT_NE(workflow.find("WdfDriverStubUm.lib"), std::string::npos);
  EXPECT_NE(workflow.find("IddCxStub.lib"), std::string::npos);
  EXPECT_NE(workflow.find("WindowsSdkDir=$kitRoot"), std::string::npos);
  EXPECT_NE(workflow.find("WindowsSDKVersion=$sdkVersion"), std::string::npos);
  EXPECT_NE(workflow.find("-DBUILD_VIRTUALDISPLAY_TOOLS=ON"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, ThirdPartyWorkflowInputsUseImmutableRefs) {
  const auto top_issues = read_top_issues_workflow();
  const auto tests_cmake = read_tests_cmake();

  EXPECT_NE(
    top_issues.find("LizardByte/.github/.github/workflows/__call-top-issues.yml@e870dffe4106859863743b27d4cd9301a3359b7f"),
    std::string::npos
  );
  EXPECT_EQ(top_issues.find("__call-top-issues.yml@master"), std::string::npos);
  EXPECT_NE(tests_cmake.find("GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723"), std::string::npos);
  EXPECT_EQ(tests_cmake.find("GIT_TAG main"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, AbandonsInvalidatedSwapchainHandlesDuringTeardown) {
  const auto source = read_windows_driver_source();

  const auto helper = source.find("void stop_swapchain_processor_without_delete");
  ASSERT_NE(helper, std::string::npos);
  EXPECT_NE(source.find("processor->stop();", helper), std::string::npos);
  EXPECT_NE(source.find("processor->abandon_swapchain();", helper), std::string::npos);

  const auto depart_display = source.find("vdd::BackendError depart_display");
  ASSERT_NE(depart_display, std::string::npos);
  EXPECT_NE(source.find("request_stop_swapchain_processor(processor_to_stop);", depart_display), std::string::npos);
  EXPECT_NE(source.find("request_stop_swapchain_processors(retired_processors_to_stop);", depart_display), std::string::npos);
  EXPECT_NE(source.find("stop_swapchain_processor_without_delete(processor_to_stop);", depart_display), std::string::npos);
  EXPECT_NE(source.find("stop_swapchain_processors_without_delete(retired_processors_to_stop);", depart_display), std::string::npos);

  const auto unassign_swapchain = source.find("NTSTATUS unassign_swapchain");
  ASSERT_NE(unassign_swapchain, std::string::npos);
  EXPECT_NE(
    source.find("retired_processors_to_stop = std::move(record->second.retired_swapchain_processors);", unassign_swapchain),
    std::string::npos
  );
  EXPECT_NE(source.find("stop_swapchain_processor_without_delete(processor_to_stop);", unassign_swapchain), std::string::npos);
  EXPECT_NE(
    source.find("stop_swapchain_processors_without_delete(retired_processors_to_stop);", unassign_swapchain),
    std::string::npos
  );
  const auto unassign_end = source.find("NTSTATUS set_default_hdr_metadata", unassign_swapchain);
  ASSERT_NE(unassign_end, std::string::npos);
  EXPECT_EQ(
    source.substr(unassign_swapchain, unassign_end - unassign_swapchain).find("monitors_.erase(record);"),
    std::string::npos
  );
}

TEST(VirtualDisplayWindowsDriverContract, MonitorDepartureFailureKeepsRecordRetryable) {
  const auto source = read_windows_driver_source();

  const auto depart_display = source.find("vdd::BackendError depart_display");
  ASSERT_NE(depart_display, std::string::npos);
  const auto failure = source.find("MonitorDepartureFailed", depart_display);
  ASSERT_NE(failure, std::string::npos);
  const auto failure_return = source.find("return vdd::BackendError::Failed;", failure);
  ASSERT_NE(failure_return, std::string::npos);
  const auto failure_body = source.substr(failure, failure_return - failure);
  EXPECT_NE(failure_body.find("monitor->second.departing = false;"), std::string::npos);
  EXPECT_EQ(failure_body.find("monitors_.erase(monitor);"), std::string::npos);
  EXPECT_EQ(source.find("monitor->second.swapchain_processor = std::move(processor_to_stop);", failure), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, MonitorWrappersValidateHandleBeforeContextLookup) {
  const auto source = read_windows_driver_source();

  const std::vector<std::string> wrappers {
    "NTSTATUS SunshineEvtQueryTargetModes(",
    "NTSTATUS SunshineEvtQueryTargetModes2(",
    "NTSTATUS SunshineEvtAssignSwapChain(",
    "NTSTATUS SunshineEvtUnassignSwapChain("
  };
  for (const auto &wrapper_name: wrappers) {
    const auto wrapper = source.find(wrapper_name);
    ASSERT_NE(wrapper, std::string::npos) << wrapper_name;
    const auto null_check = source.find("if (!monitor)", wrapper);
    ASSERT_NE(null_check, std::string::npos) << wrapper_name;
    const auto context_lookup = source.find("auto *context = GetMonitorContext(monitor);", wrapper);
    ASSERT_NE(context_lookup, std::string::npos) << wrapper_name;
    EXPECT_LT(null_check, context_lookup) << wrapper_name;
  }
}

TEST(VirtualDisplayWindowsDriverContract, TargetModesUseRequestedDescriptorTiming) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("ModeShape mode_shape_from_descriptor(const vdd::DisplayDescriptor &descriptor)"), std::string::npos);
  EXPECT_NE(source.find("descriptor.refresh_rate_millihz"), std::string::npos);
  EXPECT_EQ(source.find("caps.MaxDisplayPipelineRate = kMaxDisplayPipelineRate;"), std::string::npos);
  EXPECT_EQ(source.find("mode.RequiredBandwidth = shape.pixel_rate;"), std::string::npos);
  EXPECT_NE(source.find("{5120, 1440, 240'000}"), std::string::npos);
  EXPECT_NE(source.find("{5120, 1440, 480'000}"), std::string::npos);

  const auto query_target_modes = source.find("NTSTATUS query_target_modes(");
  ASSERT_NE(query_target_modes, std::string::npos);

  const auto requested_shape = source.find("requested_mode_shape(monitor)", query_target_modes);
  ASSERT_NE(requested_shape, std::string::npos);

  const auto fill_with_requested_shape = source.find("fill_target_modes(input, output, &*requested_shape)", requested_shape);
  ASSERT_NE(fill_with_requested_shape, std::string::npos);

  const auto query_target_modes2 = source.find("NTSTATUS query_target_modes2(");
  ASSERT_NE(query_target_modes2, std::string::npos);

  const auto requested_shape2 = source.find("requested_mode_shape(monitor)", query_target_modes2);
  ASSERT_NE(requested_shape2, std::string::npos);

  const auto fill_with_requested_shape2 = source.find("fill_target_modes2(input, output, &*requested_shape)", requested_shape2);
  ASSERT_NE(fill_with_requested_shape2, std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, InvalidEdidDescriptionDoesNotCreatePreferredMode) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(
    source.find("std::optional<ModeShape> mode_shape_from_description(const IDDCX_MONITOR_DESCRIPTION &description)"),
    std::string::npos
  );
  EXPECT_EQ(
    source.find("ModeShape mode_shape_from_description(const IDDCX_MONITOR_DESCRIPTION &description)"),
    std::string::npos
  );
  EXPECT_NE(source.find("return std::nullopt;"), std::string::npos);

  const auto target_modes = source.find("NTSTATUS fill_target_modes(");
  ASSERT_NE(target_modes, std::string::npos);
  EXPECT_NE(
    source.find(": mode_shape_from_description(input->MonitorDescription)", target_modes),
    std::string::npos
  );
  EXPECT_EQ(
    source.find("std::optional<ModeShape> {mode_shape_from_description(input->MonitorDescription)}", target_modes),
    std::string::npos
  );
}

TEST(VirtualDisplayWindowsDriverContract, PermanentDisplaysUseControlPlaneModeSettings) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("make_permanent_descriptor("), std::string::npos);
  EXPECT_NE(source.find("options.width = settings.width;"), std::string::npos);
  EXPECT_NE(source.find("options.height = settings.height;"), std::string::npos);
  EXPECT_NE(source.find("options.physical_width_mm = settings.physical_width_mm;"), std::string::npos);
  EXPECT_NE(source.find("options.physical_height_mm = settings.physical_height_mm;"), std::string::npos);
  EXPECT_NE(source.find("options.refresh_rate_millihz = settings.refresh_rate_millihz;"), std::string::npos);
  EXPECT_NE(source.find("options.monitor_name = vdd::trim_display_name(settings.display_name);"), std::string::npos);
  EXPECT_NE(source.find("vdd::set_default_permanent_display_settings(normalized);"), std::string::npos);
  EXPECT_NE(source.find("make_permanent_descriptor(const vdd::DisplayManifestProfile &profile)"), std::string::npos);
  EXPECT_NE(source.find("profile.manufacturer_id[0]"), std::string::npos);
  EXPECT_NE(source.find("profile.product_code > 0xffffu"), std::string::npos);
  EXPECT_NE(source.find("static_cast<std::uint16_t>(profile.product_code)"), std::string::npos);
  EXPECT_NE(source.find("const auto monitor_name = vdd::read_monitor_name(descriptor.edid);"), std::string::npos);
  EXPECT_NE(source.find("options.monitor_name = monitor_name.value_or(\"Sunshine Display\");"), std::string::npos);
  EXPECT_NE(source.find("vdd::BackendError apply_display_manifest(const vdd::DisplayManifest &manifest) override"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, PermanentManifestUpdatesRestorePreviousDisplaysOnFailure) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("return apply_display_manifest(vdd::display_manifest_from_permanent_settings(normalized, kMaxPermanentDisplays));"), std::string::npos);
  EXPECT_NE(source.find("vdd::validate_display_manifest(manifest, kMaxPermanentDisplays)"), std::string::npos);
  EXPECT_NE(source.find("std::vector<vdd::DisplayDescriptor> active_permanent_descriptors"), std::string::npos);
  EXPECT_NE(source.find("active_permanent_descriptors.push_back(record.descriptor);"), std::string::npos);
  const auto depart_failure = source.find("if (depart_display(descriptor.display_id) != vdd::BackendError::None)");
  ASSERT_NE(depart_failure, std::string::npos);
  const auto restore_all = source.find("for (const auto &restore_descriptor: active_permanent_descriptors)", depart_failure);
  ASSERT_NE(restore_all, std::string::npos);
  EXPECT_NE(source.find("(void) arrive_display(restore_descriptor, true);", restore_all), std::string::npos);
  EXPECT_EQ(source.find("save_display_manifest(driver_, device_, manifest)"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, TemporaryProfileSaveKeepsConnectorReservationsUnique) {
  const auto source = read_windows_driver_source();

  const auto save_profile = source.find("vdd::BackendError save_temporary_display_profile");
  ASSERT_NE(save_profile, std::string::npos);
  const auto remove_conflict = source.find("entry.connector_index == descriptor.connector_index &&", save_profile);
  ASSERT_NE(remove_conflict, std::string::npos);
  EXPECT_NE(source.find("entry.display_id != descriptor.display_id", remove_conflict), std::string::npos);
  const auto find_existing = source.find("const auto existing = std::find_if", remove_conflict);
  ASSERT_NE(find_existing, std::string::npos);
  EXPECT_LT(remove_conflict, find_existing);
}

TEST(VirtualDisplayWindowsDriverContract, ClearsPartialRenderDeviceWhenDxgiQueryFails) {
  const auto source = read_windows_driver_source();

  const auto create_device = source.find("const auto create_device = [&](IDXGIAdapter *adapter");
  ASSERT_NE(create_device, std::string::npos);
  const auto as_result = source.find("const HRESULT as_hr = device.As(&dxgi_device);", create_device);
  ASSERT_NE(as_result, std::string::npos);
  const auto failure = source.find("if (FAILED(as_hr))", as_result);
  ASSERT_NE(failure, std::string::npos);
  EXPECT_NE(source.find("dxgi_device.Reset();", failure), std::string::npos);
  EXPECT_NE(source.find("device.Reset();", failure), std::string::npos);
  EXPECT_NE(source.find("return as_hr;", failure), std::string::npos);
}
