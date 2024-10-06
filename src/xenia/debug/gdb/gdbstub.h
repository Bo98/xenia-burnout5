/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_DEBUG_GDB_GDBSTUB_H_
#define XENIA_DEBUG_GDB_GDBSTUB_H_

#include <memory>
#include <vector>

#include "xenia/base/host_thread_context.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/debug_listener.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/xbox.h"

namespace xe {
namespace debug {
namespace gdb {

class GDBStub : public cpu::DebugListener {
 public:
  virtual ~GDBStub();

  static std::unique_ptr<GDBStub> Create(Emulator* emulator, int listen_port);

  Emulator* emulator() const { return emulator_; }

  void OnFocus() override;
  void OnDetached() override;
  void OnExecutionPaused() override;
  void OnExecutionContinued() override;
  void OnExecutionEnded() override;
  void OnStepCompleted(cpu::ThreadDebugInfo* thread_info) override;
  void OnBreakpointHit(cpu::Breakpoint* breakpoint,
                       cpu::ThreadDebugInfo* thread_info) override;

 private:
  struct GDBCommand {
    std::string cmd{};
    std::string data{};
    uint8_t checksum{};
  };

  explicit GDBStub(Emulator* emulator, int listen_port);
  bool Initialize();

  bool CreateSocket(int port);
  bool Accept();
  void Listen();
  void SendPacket(const std::string& data);
  bool ProcessIncomingData();
  bool ParsePacket(GDBCommand& out_cmd);
  std::string HandleGDBCommand(const GDBCommand& command);

  void UpdateCache();

  std::string ReadRegister(xe::cpu::ThreadDebugInfo* thread, uint32_t rid);
  std::string ReadRegister(const std::string& data);
  std::string ReadRegisters();
  std::string ExecutionPause();
  std::string ExecutionContinue();
  std::string ExecutionStep();
  std::string ReadMemory(const std::string& data);
  std::string BuildTargetXml();
  std::string BuildThreadList();

  std::string GetThreadStateReply(uint32_t thread_id, uint8_t signal);

  void CreateCodeBreakpoint(uint64_t address);
  void DeleteCodeBreakpoint(uint64_t address);
  void DeleteCodeBreakpoint(cpu::Breakpoint* breakpoint);
  cpu::Breakpoint* LookupBreakpointAtAddress(uint64_t address);

  Emulator* emulator_ = nullptr;
  cpu::Processor* processor_ = nullptr;

  int listen_port_;
  std::thread listener_thread_;
  uint64_t server_socket_, client_socket_;
  std::mutex mtx_;
  std::condition_variable cv_;
  bool stop_thread_ = false;
  std::string receive_buffer_;
  std::string current_packet_;

  struct EmulatorStateCache {
    uint32_t cur_thread_id = -1;
    uint32_t last_bp_thread_id = -1;

    uint64_t notify_bp_guest_address = -1;
    uint32_t notify_bp_thread_id = -1;
    bool notify_stopped = false;

    bool is_stopped = false;
    std::vector<kernel::object_ref<kernel::XModule>> modules;
    std::vector<cpu::ThreadDebugInfo*> thread_debug_infos;

    struct {
      char kernel_call_filter[64] = {0};
      std::vector<std::unique_ptr<cpu::Breakpoint>> all_breakpoints;
      std::unordered_map<uint32_t, cpu::Breakpoint*>
          code_breakpoints_by_guest_address;
    } breakpoints;

    cpu::ThreadDebugInfo* thread_info(int threadId) {
      for (auto& thread : thread_debug_infos) {
        if (thread->thread_id == threadId) {
          return thread;
        }
      }
      return nullptr;
    }
    cpu::ThreadDebugInfo* cur_thread_info() {
      return thread_info(cur_thread_id);
    }
  } cache_;
};

}  // namespace gdb
}  // namespace debug
}  // namespace xe

#endif  // XENIA_DEBUG_UI_DEBUG_WINDOW_H_
