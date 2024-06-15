/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_app.h"

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/xam/xam_net.h"
#include "xenia/kernel/xenumerator.h"

#ifdef XE_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#elif XE_PLATFORM_LINUX
#include <netinet/in.h>
#endif

struct XONLINE_SERVICE_INFO {
  xe::be<uint32_t> id;
  in_addr ip;
  xe::be<uint16_t> port;
  xe::be<uint16_t> unk;
};

#pragma pack(push, 4)
struct XONLINE_PRESENCE {
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> state;
  uint8_t session_id[8];
  xe::be<uint32_t> title_id;
  xe::be<uint64_t> state_change_time;  // filetime
  xe::be<uint32_t> num_rich_presence;
  xe::be<char16_t> rich_presence[64];
};
#pragma pack(pop)
static_assert_size(XONLINE_PRESENCE, 164);

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state)
    : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t arg1,
                                            uint32_t arg2) {
  switch (message) {
    case 0x0005008C: {
      // Called on startup of blades dashboard v1888 to v2858
      XELOGD("XLiveBaseUnk5008C, unimplemented");
      return X_E_FAIL;
    }
    case 0x00050094: {
      // Called on startup of blades dashboard v4532 to v4552
      XELOGD("XLiveBaseUnk50094, unimplemented");
      return X_E_FAIL;
    }
    case 0x00058003: {
      // Called on startup of dashboard (netplay build)
      XELOGD("XLiveBaseLogonGetHR, unimplemented");
      return X_E_SUCCESS;
    }
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!arg2 || arg2 == 4);
      auto buffer = memory_->TranslateVirtual(arg1);
      XELOGD("XLiveBaseGetLogonId({:08X})", arg1);
      xe::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!arg2 || arg2 == 4);
      auto buffer = memory_->TranslateVirtual(arg1);
      XELOGD("XLiveBaseGetNatType({:08X})", arg1);
      xe::store_and_swap<uint32_t>(buffer + 0, xeXOnlineGetNatType());
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      XELOGD("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", arg1, arg2);

      XONLINE_SERVICE_INFO* service_info =
          reinterpret_cast<XONLINE_SERVICE_INFO*>(
              memory_->TranslateVirtual(arg2));
      memset(service_info, 0, sizeof(XONLINE_SERVICE_INFO));
      service_info->id = arg1;
      service_info->ip.s_addr = htonl(INADDR_LOOPBACK);
      return X_ERROR_SUCCESS;
    }
    case 0x00058019: {
      struct argument_item {
        xe::be<uint32_t> unk_00;  // Always set to 4
        xe::be<uint32_t> unk_04;
        xe::be<uint64_t> data;
      };

      // Called from XPresenceCreateEnumerator
      struct message_data {
        argument_item user_index;
        argument_item num_peers;
        argument_item peer_xuids_ptr;
        argument_item starting_index;
        argument_item max_peers;
        argument_item buffer_length_ptr;      // output
        argument_item enumerator_handle_ptr;  // output
      }* data =
          reinterpret_cast<message_data*>(memory_->TranslateVirtual(arg2));

      auto num_peers = *reinterpret_cast<xe::be<uint32_t>*>(
          memory_->TranslateVirtual((uint32_t)data->num_peers.data));
      auto max_peers = *reinterpret_cast<xe::be<uint32_t>*>(
          memory_->TranslateVirtual((uint32_t)data->max_peers.data));
      auto starting_index = *reinterpret_cast<xe::be<uint32_t>*>(
          memory_->TranslateVirtual((uint32_t)data->starting_index.data));

      assert_true(max_peers <= 100);
      assert_true(starting_index < num_peers);

      auto return_count = std::min(num_peers - starting_index, max_peers.get());
      auto e = make_object<XStaticEnumerator<XONLINE_PRESENCE>>(kernel_state_,
                                                                return_count);

      auto user_index = *reinterpret_cast<xe::be<uint32_t>*>(
          memory_->TranslateVirtual((uint32_t)data->user_index.data));
      auto result = e->Initialize(user_index, 0xFE, 0x5801A, 0x5801B, 0);
      if (XFAILED(result)) {
        return result;
      }

      auto xuids = reinterpret_cast<const xe::be<uint64_t>*>(
          memory_->TranslateVirtual((uint32_t)data->peer_xuids_ptr.data));
      for (auto i = starting_index; i < e->items_per_enumerate(); i++) {
        auto item = e->AppendItem();
        std::memset(item, 0, sizeof(item));
        item->xuid = xuids[i];
      }

      *reinterpret_cast<xe::be<uint32_t>*>(
          memory_->TranslateVirtual((uint32_t)data->buffer_length_ptr.data)) =
          uint32_t(e->items_per_enumerate() * e->item_size());
      *reinterpret_cast<xe::be<uint32_t>*>(memory_->TranslateVirtual(
          (uint32_t)data->enumerator_handle_ptr.data)) = e->handle();

      XELOGD("XLiveBase(0x00058019)({:08X}, {:08X})", arg1, arg2);
      return X_E_SUCCESS;
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      XELOGD("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented", arg1,
             arg2);
      return X_E_FAIL;
    }
    case 0x00058023: {
      XELOGD(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          arg1, arg2);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      XELOGD("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", arg1, arg2);
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, arg1, arg2);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
