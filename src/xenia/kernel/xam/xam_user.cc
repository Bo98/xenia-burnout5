/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xdbf/xdbf.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"


DECLARE_int32(user_language);

namespace xe {
namespace kernel {
namespace xam {
namespace xdbf {

struct X_PROFILEENUMRESULT {
  xe::be<uint64_t> xuid_offline;  // E0.....
  X_XAMACCOUNTINFO account;
  xe::be<uint32_t> device_id;
};
static_assert_size(X_PROFILEENUMRESULT, 0x188);

dword_result_t XamProfileCreateEnumerator_entry(dword_t device_id,
                                                lpdword_t handle_out) {
  assert_not_null(handle_out);

  auto e = new XStaticEnumerator<X_PROFILEENUMRESULT>(kernel_state(), 1);

  e->Initialize(0xFF, 0xFE, 0x23001, 0x23003, 0);

  for (uint32_t i = 0; i < 4; i++) {
    if (!kernel_state()->IsUserSignedIn(i)) continue;

    const auto& user_profile = kernel_state()->user_profile(i);

    X_PROFILEENUMRESULT* profile = (X_PROFILEENUMRESULT*)e->AppendItem();
    memset(profile, 0, sizeof(X_PROFILEENUMRESULT));
    profile->xuid_offline = user_profile->xuid_offline();
    profile->device_id = 0xF00D0000;

    auto tag = to_utf16(user_profile->name());
    xe::copy_and_swap<char16_t>(profile->account.gamertag, tag.c_str(),
                                tag.length());
    profile->account.xuid_online = user_profile->xuid_online();
  }

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamProfileCreateEnumerator, kUserProfiles, kImplemented);

dword_result_t XamProfileEnumerate_entry(dword_t handle, dword_t flags,
                                         lpvoid_t buffer,
                                         pointer_t<XAM_OVERLAPPED> overlapped) {
  assert_true(flags == 0);

  auto e = kernel_state()->object_table()->LookupObject<XEnumerator>(handle);
  if (!e) {
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediateEx(
          overlapped, X_ERROR_INVALID_HANDLE, X_ERROR_INVALID_HANDLE, 0);
      return X_ERROR_IO_PENDING;
    } else {
      return X_ERROR_INVALID_HANDLE;
    }
  }

  buffer.Zero(sizeof(X_PROFILEENUMRESULT));

  X_RESULT result =
      e->WriteItems(buffer.guest_address(), buffer.as<uint8_t*>(), nullptr);

  // Return X_ERROR_NO_MORE_FILES in HRESULT form.
  X_HRESULT extended_result = result != 0 ? X_HRESULT_FROM_WIN32(result) : 0;
  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediateEx(
        overlapped, result, extended_result, result == X_ERROR_SUCCESS ? 1 : 0);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamProfileEnumerate, kUserProfiles, kImplemented);

X_HRESULT_result_t XamUserGetXUID_entry(dword_t user_index, dword_t type_mask,
                                        lpqword_t xuid_ptr) {
  assert_true(type_mask >= 1 && type_mask <= 7);
  if (!xuid_ptr) {
    return X_E_INVALIDARG;
  }
  uint32_t result = X_E_NO_SUCH_USER;
  uint64_t xuid = 0;
  if (user_index < 4) {
    if (kernel_state()->IsUserSignedIn(user_index)) {
      const auto& user_profile = kernel_state()->user_profile(user_index);
      auto type = user_profile->type() & type_mask;
      if (type & 2 && user_profile->signin_state() == 2) {
        // maybe online profile?
        xuid = user_profile->xuid_online();
        result = X_E_SUCCESS;
      } else if (type & (1 | 4)) {
        // maybe offline profile?
        xuid = user_profile->xuid_offline();
        result = X_E_SUCCESS;
      }
    }
  } else {
    result = X_E_INVALIDARG;
  }
  *xuid_ptr = xuid;
  return result;
}
DECLARE_XAM_EXPORT1(XamUserGetXUID, kUserProfiles, kImplemented);

dword_result_t XamUserGetSigninState_entry(dword_t user_index) {
  // Yield, as some games spam this.
  xe::threading::MaybeYield();
  uint32_t signin_state = 0;
  if (user_index < 4) {
    if (kernel_state()->IsUserSignedIn(user_index)) {
      const auto& user_profile = kernel_state()->user_profile(user_index);
      signin_state = user_profile->signin_state();
    }
  }
  return signin_state;
}
DECLARE_XAM_EXPORT2(XamUserGetSigninState, kUserProfiles, kImplemented,
                    kHighFrequency);

typedef struct {
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> flags;  // bit 0 = live enabled, bit 1 = guest
  xe::be<uint32_t> signin_state;
  xe::be<uint32_t> guest_number;
  xe::be<uint32_t> guest_parent_index;
  char name[16];
} X_USER_SIGNIN_INFO;
static_assert_size(X_USER_SIGNIN_INFO, 40);

X_HRESULT_result_t XamUserGetSigninInfo_entry(
    dword_t user_index, dword_t flags, pointer_t<X_USER_SIGNIN_INFO> info) {
  if (!info || flags < 0 || flags > 2) {
    return X_E_INVALIDARG;
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  if (user_index > 3 && user_index != 0xFF) {
    return X_E_NO_SUCH_USER;
  }

  kernel_state()->UpdateUsedUserProfiles();

  if (kernel_state()->IsUserSignedIn(user_index)) {
    const auto& user_profile = kernel_state()->user_profile(user_index);
    if (flags & 1) {
      if (user_profile->type() & 1) {
        info->xuid = user_profile->xuid_offline();
      } else {
        info->xuid = 0;
      }
    } else if (user_profile->type() & 2) {
      info->xuid = user_profile->xuid_online();
    } else {
      info->xuid = (flags == 0) ? user_profile->xuid_offline() : 0;
    }

    info->signin_state = user_profile->signin_state();

    uint32_t flags = 0;
    if (info->signin_state == 2) flags &= 1;
    if (user_profile->type() & 4) flags &= 2;
    info->flags = flags;

    xe::string_util::copy_truncating(info->name, user_profile->name(),
                                     xe::countof(info->name));
  } else {
    return X_E_NO_SUCH_USER;
  }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetSigninInfo, kUserProfiles, kImplemented);

dword_result_t XamUserGetName_entry(dword_t user_index, lpstring_t buffer,
                                    dword_t buffer_len) {
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (kernel_state()->IsUserSignedIn(user_index)) {
    const auto& user_profile = kernel_state()->user_profile(user_index);
    const auto& user_name = user_profile->name();
    xe::string_util::copy_truncating(
        buffer, user_name, std::min(buffer_len.value(), uint32_t(16)));
  } else {
    *buffer = 0;
    return X_ERROR_NO_SUCH_USER;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetName, kUserProfiles, kImplemented);

dword_result_t XamUserGetGamerTag_entry(dword_t user_index,
                                        lpu16string_t buffer,
                                        dword_t buffer_len) {
  if (user_index >= 4 && user_index != 0xFF) {
    return X_ERROR_NO_SUCH_USER;
  }

  if (!buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }

  if (!kernel_state()->IsUserSignedIn(user_index)) {
    return X_E_INVALIDARG;
  }

  const auto& user_profile = kernel_state()->user_profile(user_index);
  auto user_name = xe::to_utf16(user_profile->name());
  xe::string_util::copy_and_swap_truncating(
      buffer, user_name, std::min(buffer_len.value(), uint32_t(16)));
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetGamerTag, kUserProfiles, kImplemented);

typedef struct {
  xe::be<uint32_t> setting_count;
  xe::be<uint32_t> settings_ptr;
} X_USER_READ_PROFILE_SETTINGS;
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/xboxtools.cpp
uint32_t XamUserReadProfileSettingsEx(uint32_t title_id, uint32_t user_index,
                                      uint32_t xuid_count, be<uint64_t>* xuids,
                                      uint32_t setting_count,
                                      be<uint32_t>* setting_ids, uint32_t unk,
                                      be<uint32_t>* buffer_size_ptr,
                                      uint8_t* buffer,
                                      XAM_OVERLAPPED* overlapped) {
  if (!xuid_count) {
    assert_null(xuids);
  } else {
    assert_true(xuid_count == 1);
    assert_not_null(xuids);
    // TODO(gibbed): allow proper lookup of arbitrary XUIDs
    // TODO(gibbed): we assert here, but in case a title passes xuid_count > 1
    // until it's implemented for release builds...
    xuid_count = 1;
    if (kernel_state()->IsUserSignedIn(user_index)) {
      const auto& user_profile = kernel_state()->user_profile(user_index);
      assert_true(static_cast<uint64_t>(xuids[0]) ==
                  user_profile->xuid_online());
    }
  }
  assert_zero(unk);  // probably flags

  // must have at least 1 to 32 settings
  if (setting_count < 1 || setting_count > 32) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // buffer size pointer must be valid
  if (!buffer_size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // if buffer size is non-zero, buffer pointer must be valid
  auto buffer_size = static_cast<uint32_t>(*buffer_size_ptr);
  if (buffer_size && !buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t needed_header_size = 0;
  uint32_t needed_data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    needed_header_size += sizeof(X_USER_PROFILE_SETTING);
    UserProfile::Setting::Key setting_key;
    setting_key.value = static_cast<uint32_t>(setting_ids[i]);
    switch (static_cast<UserProfile::Setting::Type>(setting_key.type)) {
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::BINARY:
        needed_data_size += setting_key.size;
        break;
      default:
        break;
    }
  }
  if (xuids) {
    needed_header_size *= xuid_count;
    needed_data_size *= xuid_count;
  }
  needed_header_size += sizeof(X_USER_READ_PROFILE_SETTINGS);

  uint32_t needed_size = needed_header_size + needed_data_size;
  if (!buffer || buffer_size < needed_size) {
    if (!buffer_size) {
      *buffer_size_ptr = needed_size;
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  // Title ID = 0 means us.
  // 0xfffe07d1 = profile?
  if (!kernel_state()->IsUserSignedIn(user_index) && !xuids) {
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(
          kernel_state()->memory()->HostToGuestVirtual(overlapped),
          X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  auto user_profile = kernel_state()->user_profile(user_index);

  if (xuids) {
    uint64_t user_xuid = static_cast<uint64_t>(xuids[0]);
    if (!kernel_state()->IsUserSignedIn(user_xuid, true)) {
      if (overlapped) {
        kernel_state()->CompleteOverlappedImmediate(
            kernel_state()->memory()->HostToGuestVirtual(overlapped),
            X_ERROR_NO_SUCH_USER);
        return X_ERROR_IO_PENDING;
      }
      return X_ERROR_NO_SUCH_USER;
    }
    user_profile = kernel_state()->user_profile(user_xuid, true);
  }

  // First call asks for size (fill buffer_size_ptr).
  // Second call asks for buffer contents with that size.

  // TODO(gibbed): setting validity checking without needing a user profile
  // object.
  bool any_missing = false;
  for (uint32_t i = 0; i < setting_count; ++i) {
    auto setting_id = static_cast<uint32_t>(setting_ids[i]);
    auto setting = user_profile->GetSetting(setting_id);
    if (!setting) {
      any_missing = true;
      XELOGE(
          "xeXamUserReadProfileSettingsEx requested unimplemented setting "
          "{:08X}",
          setting_id);
    }
  }
  if (any_missing) {
    // TODO(benvanik): don't fail? most games don't even check!
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(
          kernel_state()->memory()->HostToGuestVirtual(overlapped),
          X_ERROR_INVALID_PARAMETER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_INVALID_PARAMETER;
  }

  auto out_header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer);
  auto out_setting = reinterpret_cast<X_USER_PROFILE_SETTING*>(&out_header[1]);
  out_header->setting_count = static_cast<uint32_t>(setting_count);
  out_header->settings_ptr =
      kernel_state()->memory()->HostToGuestVirtual(out_setting);

  UserProfile::SettingByteStream out_stream(
      kernel_state()->memory()->HostToGuestVirtual(buffer), buffer, buffer_size,
      needed_header_size);
  for (uint32_t n = 0; n < setting_count; ++n) {
    uint32_t setting_id = setting_ids[n];
    auto setting = user_profile->GetSetting(setting_id);

    std::memset(out_setting, 0, sizeof(X_USER_PROFILE_SETTING));
    out_setting->from = !setting || !setting->is_set   ? 0
                        : setting->is_title_specific() ? 2
                                                       : 1;
    if (xuids) {
      out_setting->xuid = user_profile->xuid_offline();
    } else {
      out_setting->xuid = -1;
      out_setting->user_index = static_cast<uint32_t>(user_index);
    }
    out_setting->setting_id = setting_id;

    if (setting) {
      out_setting->from = 1;
      out_setting->data.type = uint8_t(setting->type);
      if (setting->is_set) {
        if (setting->is_title_specific()) {
          out_setting->from = 2;
        }

        setting->Append(&out_setting->data, &out_stream);
      }
    }
    ++out_setting;
  }

  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediate(
        kernel_state()->memory()->HostToGuestVirtual(overlapped),
        X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

dword_result_t XamUserReadProfileSettings_entry(
    dword_t title_id, dword_t user_index, dword_t xuid_count, lpqword_t xuids,
    dword_t setting_count, lpdword_t setting_ids, lpdword_t buffer_size_ptr,
    lpvoid_t buffer_ptr, pointer_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids,
                                      setting_count, setting_ids, 0,
                                      buffer_size_ptr, buffer_ptr, overlapped);
}
DECLARE_XAM_EXPORT1(XamUserReadProfileSettings, kUserProfiles, kImplemented);

dword_result_t XamUserReadProfileSettingsEx_entry(
    dword_t title_id, dword_t user_index, dword_t xuid_count, lpqword_t xuids,
    dword_t setting_count, lpdword_t setting_ids, lpdword_t buffer_size_ptr,
    dword_t unk_2, lpvoid_t buffer_ptr, pointer_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids,
                                      setting_count, setting_ids, unk_2,
                                      buffer_size_ptr, buffer_ptr, overlapped);
}
DECLARE_XAM_EXPORT1(XamUserReadProfileSettingsEx, kUserProfiles, kImplemented);

dword_result_t XamUserWriteProfileSettings_entry(
    dword_t title_id, dword_t user_index, dword_t setting_count,
    pointer_t<X_USER_PROFILE_SETTING> settings,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!setting_count || !settings) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // Skip writing data about users with id != 0 they're not supported
  if (user_index > 0) {
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(
          kernel_state()->memory()->HostToGuestVirtual(overlapped),
          X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_SUCCESS;
  }
  // Update and save settings.
  const auto& user_profile = kernel_state()->user_profile(user_index);

  for (uint32_t n = 0; n < setting_count; ++n) {
    const X_USER_PROFILE_SETTING& setting = settings[n];

    auto setting_type =
        static_cast<UserProfile::Setting::Type>(setting.data.type);
    if (setting_type == UserProfile::Setting::Type::UNSET) {
      continue;
    }

    XELOGD(
        "XamUserWriteProfileSettings: setting index [{}]:"
        " from={} setting_id={:08X} data.type={}",
        n, (uint32_t)setting.from, (uint32_t)setting.setting_id,
        setting.data.type);

    switch (setting_type) {
      case UserProfile::Setting::Type::CONTENT:
      case UserProfile::Setting::Type::BINARY: {
        UserProfile::Setting::Key setting_key;
        setting_key.value = static_cast<uint32_t>(setting.setting_id);

        uint8_t* binary_ptr =
            kernel_state()->memory()->TranslateVirtual(setting.data.binary.ptr);

        size_t binary_size = setting.data.binary.size;
        if (setting_key.size < binary_size) {
          XELOGW(
              "XamUserWriteProfileSettings: binary size > key size. Shrinking "
              "binary size!");
          binary_size = setting_key.size;
        }
        std::vector<uint8_t> bytes;
        if (setting.data.binary.ptr) {
          // Copy provided data
          bytes.resize(binary_size);
          std::memcpy(bytes.data(), binary_ptr, binary_size);
        } else {
          // Data pointer was NULL, so just fill with zeroes
          bytes.resize(binary_size, 0);
        }
        user_profile->AddSetting(
            std::make_unique<xam::UserProfile::BinarySetting>(
                setting.setting_id, bytes));
      } break;
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::DOUBLE:
      case UserProfile::Setting::Type::FLOAT:
      case UserProfile::Setting::Type::INT32:
      case UserProfile::Setting::Type::INT64:
      case UserProfile::Setting::Type::DATETIME:
      default: {
        XELOGE("XamUserWriteProfileSettings: Unimplemented data type {}",
               setting_type);
      } break;
    };
  }

  user_profile->UpdateAllGpds();

  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserWriteProfileSettings, kUserProfiles, kImplemented);

dword_result_t XamUserCheckPrivilege_entry(dword_t user_index, dword_t type,
                                           lpdword_t out_value) {
  // checking all users?
  if (user_index != 0xFF) {
    if (user_index >= 4) {
      return X_ERROR_INVALID_PARAMETER;
    }

    if (!kernel_state()->IsUserSignedIn(user_index)) {
      return X_ERROR_NO_SUCH_USER;
    }
  }

  // If we deny everything, games should hopefully not try to do stuff.
  *out_value = 0;

  const auto& user_profile = kernel_state()->user_profile(user_index);
  if (user_profile->signin_state() == 2 && type == 254) {
    // We have enabled Live so let's allow multiplayer
    *out_value = 1;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCheckPrivilege, kUserProfiles, kStub);

dword_result_t XamUserContentRestrictionGetFlags_entry(dword_t user_index,
                                                       lpdword_t out_flags) {
  if (!kernel_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  // No restrictions?
  *out_flags = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserContentRestrictionGetFlags, kUserProfiles, kStub);

dword_result_t XamUserContentRestrictionGetRating_entry(dword_t user_index,
                                                        dword_t unk1,
                                                        lpdword_t out_unk2,
                                                        lpdword_t out_unk3) {
  if (!kernel_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  // Some games have special case paths for 3F that differ from the failure
  // path, so my guess is that's 'don't care'.
  *out_unk2 = 0x3F;
  *out_unk3 = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserContentRestrictionGetRating, kUserProfiles, kStub);

dword_result_t XamUserContentRestrictionCheckAccess_entry(
    dword_t user_index, dword_t unk1, dword_t unk2, dword_t unk3, dword_t unk4,
    lpdword_t out_unk5, dword_t overlapped_ptr) {
  *out_unk5 = 1;

  if (overlapped_ptr) {
    // TODO(benvanik): does this need the access arg on it?
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserContentRestrictionCheckAccess, kUserProfiles, kStub);

dword_result_t XamUserIsOnlineEnabled_entry(dword_t user_index) { return 1; }
DECLARE_XAM_EXPORT1(XamUserIsOnlineEnabled, kUserProfiles, kStub);

dword_result_t XamUserGetMembershipTier_entry(dword_t user_index) {
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (kernel_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }
  return 6 /* 6 appears to be Gold */;
}
DECLARE_XAM_EXPORT1(XamUserGetMembershipTier, kUserProfiles, kStub);

dword_result_t XamUserAreUsersFriends_entry(dword_t user_index, dword_t unk1,
                                            dword_t unk2, lpdword_t out_value,
                                            dword_t overlapped_ptr) {
  uint32_t are_friends = 0;
  X_RESULT result;

  if (user_index >= 4) {
    result = X_ERROR_INVALID_PARAMETER;
  } else {
    if (kernel_state()->IsUserSignedIn(user_index)) {
      const auto& user_profile = kernel_state()->user_profile(user_index);
      if (user_profile->signin_state() == 0) {
        result = X_ERROR_NOT_LOGGED_ON;
      } else {
        // No friends!
        are_friends = 0;
        result = X_ERROR_SUCCESS;
      }
    } else {
      // Only support user 0.
      result =
          X_ERROR_NO_SUCH_USER;  // if user is local -> X_ERROR_NOT_LOGGED_ON
    }
  }

  if (out_value) {
    assert_true(!overlapped_ptr);
    *out_value = result == X_ERROR_SUCCESS ? are_friends : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!out_value);
    kernel_state()->CompleteOverlappedImmediateEx(
        overlapped_ptr,
        result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
        X_HRESULT_FROM_WIN32(result),
        result == X_ERROR_SUCCESS ? are_friends : 0);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}
DECLARE_XAM_EXPORT1(XamUserAreUsersFriends, kUserProfiles, kStub);

dword_result_t XamShowSigninUI_entry(dword_t users_needed, dword_t unk_mask) {
  // XN_SYS_UI (on)
  kernel_state()->BroadcastNotification(kXNotificationIDSystemUI, 1);
  kernel_state()->UpdateUsedUserProfiles();
  // Mask values vary. Probably matching user types? Local/remote?
  // Games seem to sit and loop until we trigger this notification:
  uint32_t user_mask = 0;
  uint32_t active_users = 0;

  for (uint32_t i = 0; i < 4; i++) {
    if (kernel_state()->IsUserSignedIn(i)) {
      user_mask |= (1 << i);
      active_users++;
      if (active_users >= users_needed) break;
    }
  }

  // XN_SYS_SIGNINCHANGED (players)
  kernel_state()->BroadcastNotification(kXNotificationIDSystemSignInChanged,
                                        user_mask);

  // XN_SYS_UI (off)
  kernel_state()->BroadcastNotification(kXNotificationIDSystemUI, 0);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowSigninUI, kUserProfiles, kStub);

#pragma pack(push, 1)
struct X_ACHIEVEMENT_DETAILS {
  xe::be<uint32_t> id;
  xe::be<uint32_t> label_ptr;
  xe::be<uint32_t> description_ptr;
  xe::be<uint32_t> unachieved_ptr;
  xe::be<uint32_t> image_id;
  xe::be<uint32_t> gamerscore;
  xe::be<uint64_t> unlock_time;  // FILETIME
  xe::be<uint32_t> flags;

  static const size_t kStringBufferSize = 464;
};
static_assert_size(X_ACHIEVEMENT_DETAILS, 36);
#pragma pack(pop)

class XStaticAchievementEnumerator : public XEnumerator {
 public:
  struct AchievementDetails {
    uint32_t id;
    std::u16string label;
    std::u16string description;
    std::u16string unachieved;
    uint32_t image_id;
    uint32_t gamerscore;
    uint64_t unlock_time;
    uint32_t flags;
  };

  XStaticAchievementEnumerator(KernelState* kernel_state,
                               size_t items_per_enumerate, uint32_t flags)
      : XEnumerator(
            kernel_state, items_per_enumerate,
            sizeof(X_ACHIEVEMENT_DETAILS) +
                (!!(flags & 7) ? X_ACHIEVEMENT_DETAILS::kStringBufferSize : 0)),
        flags_(flags) {}

  void AppendItem(AchievementDetails item) {
    items_.push_back(std::move(item));
  }

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data,
                      uint32_t* written_count) override {
    size_t count =
        std::min(items_.size() - current_item_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    size_t size = count * item_size();

    auto details = reinterpret_cast<X_ACHIEVEMENT_DETAILS*>(buffer_data);
    size_t string_offset =
        items_per_enumerate() * sizeof(X_ACHIEVEMENT_DETAILS);
    auto string_buffer =
        StringBuffer{buffer_ptr + static_cast<uint32_t>(string_offset),
                     &buffer_data[string_offset],
                     count * X_ACHIEVEMENT_DETAILS::kStringBufferSize};
    for (size_t i = 0, o = current_item_; i < count; ++i, ++current_item_) {
      const auto& item = items_[current_item_];
      details[i].id = item.id;
      details[i].label_ptr =
          !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
      details[i].description_ptr =
          !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
      details[i].unachieved_ptr =
          !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
      details[i].image_id = item.image_id;
      details[i].gamerscore = item.gamerscore;
      details[i].unlock_time = item.unlock_time;
      details[i].flags = item.flags;
    }

    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }

    return X_ERROR_SUCCESS;
  }

 private:
  struct StringBuffer {
    uint32_t ptr;
    uint8_t* data;
    size_t remaining_bytes;
  };

  uint32_t AppendString(StringBuffer& sb, const std::u16string_view string) {
    size_t count = string.length() + 1;
    size_t size = count * sizeof(char16_t);
    if (size > sb.remaining_bytes) {
      assert_always();
      return 0;
    }
    auto ptr = sb.ptr;
    string_util::copy_and_swap_truncating(reinterpret_cast<char16_t*>(sb.data),
                                          string, count);
    sb.ptr += static_cast<uint32_t>(size);
    sb.data += size;
    sb.remaining_bytes -= size;
    return ptr;
  }

 private:
  uint32_t flags_;
  std::vector<AchievementDetails> items_;
  size_t current_item_ = 0;
};

dword_result_t XamUserCreateAchievementEnumerator_entry(
    dword_t title_id, dword_t user_index, dword_t xuid, dword_t flags,
    dword_t offset, dword_t count, lpdword_t buffer_size_ptr,
    lpdword_t handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t entry_size = sizeof(X_ACHIEVEMENT_DETAILS);
  if (flags & 7) {
    entry_size += X_ACHIEVEMENT_DETAILS::kStringBufferSize;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<uint32_t>(entry_size) * count;
  }

  if (!kernel_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // Copy achievements into the enumerator if game GPD is loaded
  auto* game_gpd =
      kernel_state()->user_profile(user_index)->GetTitleGpd(title_id);
  if (!game_gpd) {
    XELOGE(
        "XamUserCreateAchievementEnumerator failed to find GPD for title {:08X}!",
        title_id);
    return X_ERROR_SUCCESS;
  }

  auto e = object_ref<XStaticAchievementEnumerator>(
      new XStaticAchievementEnumerator(kernel_state(), count, flags));
  auto result =
      e->Initialize(user_index, game_gpd->GetTitleId(), 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) {
    return result;
  }

  const util::XdbfGameData db = kernel_state()->title_xdbf();

  if (db.is_valid()) {
    const XLanguage language =
        db.GetExistingLanguage(static_cast<XLanguage>(cvars::user_language));
    const std::vector<util::XdbfAchievementTableEntry> achievement_list =
        db.GetAchievements();

    for (const util::XdbfAchievementTableEntry& entry : achievement_list) {
      auto is_unlocked =
          kernel_state()->achievement_manager()->IsAchievementUnlocked(
              entry.id);
      auto unlock_time =
          kernel_state()->achievement_manager()->GetAchievementUnlockTime(
              entry.id);

      auto item = XStaticAchievementEnumerator::AchievementDetails{
          entry.id,
          xe::to_utf16(db.GetStringTableEntry(language, entry.label_id)),
          xe::to_utf16(db.GetStringTableEntry(language, entry.description_id)),
          xe::to_utf16(db.GetStringTableEntry(language, entry.unachieved_id)),
          entry.image_id,
          entry.gamerscore,
          {0},
          entry.flags};

      e->AppendItem(item);
    }
  }


  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCreateAchievementEnumerator, kUserProfiles,
                    kSketchy);

dword_result_t XamParseGamerTileKey_entry(lpdword_t key_ptr, lpdword_t out1_ptr,
                                          lpdword_t out2_ptr,
                                          lpdword_t out3_ptr) {
  *out1_ptr = 0xC0DE0001;
  *out2_ptr = 0xC0DE0002;
  *out3_ptr = 0xC0DE0003;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamParseGamerTileKey, kUserProfiles, kStub);

dword_result_t XamReadTileToTexture_entry(dword_t unknown, dword_t title_id,
                                          qword_t tile_id, dword_t user_index,
                                          lpvoid_t buffer_ptr, dword_t stride,
                                          dword_t height,
                                          dword_t overlapped_ptr) {
  // TODO(gibbed): unknown=0,2,3,9
  if (!tile_id) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t size = size_t(stride) * size_t(height);
  std::memset(buffer_ptr, 0xFF, size);

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamReadTileToTexture, kUserProfiles, kStub);

dword_result_t XamWriteGamerTile_entry(dword_t arg1, dword_t arg2, dword_t arg3,
                                       dword_t arg4, dword_t arg5,
                                       dword_t overlapped_ptr) {
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamWriteGamerTile, kUserProfiles, kStub);

dword_result_t XamSessionCreateHandle_entry(lpdword_t handle_ptr) {
  *handle_ptr = 0xCAFEDEAD;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSessionCreateHandle, kUserProfiles, kStub);

dword_result_t XamSessionRefObjByHandle_entry(dword_t handle,
                                              lpdword_t obj_ptr) {
  assert_true(handle == 0xCAFEDEAD);
  // TODO(PermaNull): Implement this properly,
  // For the time being returning 0xDEADF00D will prevent crashing.
  *obj_ptr = 0xDEADF00D;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSessionRefObjByHandle, kUserProfiles, kStub);

dword_result_t XamUserIsUnsafeProgrammingAllowed_entry(dword_t unk1, dword_t unk2,
                                                       lpdword_t unk3, dword_t unk4,
                                                       dword_t unk5, dword_t unk6) {
  if (!unk3 || unk1 != 255 && unk1 >= 4) {
    return 87;
  }
  *unk3 = 1;
  return 0;
}
DECLARE_XAM_EXPORT1(XamUserIsUnsafeProgrammingAllowed, kUserProfiles, kStub);

dword_result_t XamUserGetSubscriptionType_entry(dword_t user_index, dword_t unk2,
                                                dword_t unk3, dword_t unk4,
                                                dword_t unk5, dword_t unk6) {
  if (!unk2 || !unk3 || user_index > 4) {
    return 0x80070057;
  }

  return 0;
}
DECLARE_XAM_EXPORT1(XamUserGetSubscriptionType, kUserProfiles, kStub);

dword_result_t XamUserCreateTitlesPlayedEnumerator_entry(
    dword_t user_index, dword_t xuid, dword_t flags, dword_t offset,
    dword_t games_count, lpdword_t buffer_size_ptr, lpdword_t handle_ptr) {
  // + 128 bytes for the 64-char titlename
  const uint32_t kEntrySize = sizeof(xdbf::X_XDBF_GPD_TITLEPLAYED) + 128;

  if (buffer_size_ptr) {
    *buffer_size_ptr = kEntrySize * games_count;
  }

  std::vector<xdbf::TitlePlayed> titles;
  kernel_state()->user_profile(user_index)->GetDashboardGpd()->GetTitles(&titles);

  auto e = make_object<XStaticUntypedEnumerator>(
      kernel_state(), games_count, sizeof(xdbf::X_XDBF_GPD_TITLEPLAYED));

  *handle_ptr = e->handle();

  for (auto title : titles) {
    if (e->item_count() >= games_count) {
      break;
    }

    // For some reason dashboard gpd stores info about itself
    if (title.title_id == kDashboardID) 
      continue;

    // TODO: Look for better check to provide information about demo title
    // or system title
    if (!title.gamerscore_total || !title.achievements_possible) 
      continue;

    auto* details = (xdbf::X_XDBF_GPD_TITLEPLAYED*)e->AppendItem();
    details->title_id = title.title_id;
    details->achievements_possible = title.achievements_possible;
    details->achievements_earned = title.achievements_earned;
    details->gamerscore_total = title.gamerscore_total;
    details->gamerscore_earned = title.gamerscore_earned;
    details->reserved_achievement_count = title.reserved_achievement_count;
    details->all_avatar_awards = title.all_avatar_awards;
    details->male_avatar_awards = title.male_avatar_awards;
    details->female_avatar_awards = title.female_avatar_awards;
    details->reserved_flags = title.reserved_flags;
    details->last_played = title.last_played;

    // Ensure details->title_name has enough space
    std::u16string src = title.title_name;
    std::vector<wchar_t> converted(src.begin(), src.end());
    converted.push_back('\0');  // Ensure null-termination

    xe::copy_and_swap<wchar_t>((wchar_t*)details->title_name, converted.data(),
                               converted.size() * sizeof(wchar_t));
  }

  XELOGD("XamUserCreateTitlesPlayedEnumerator: added %d items to enumerator",
         e->item_count());

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCreateTitlesPlayedEnumerator, kUserProfiles,
                    kImplemented);

dword_result_t XamReadTile_entry(dword_t tile_type, dword_t game_id, qword_t item_id,
                           dword_t offset, lpdword_t output_ptr,
                           lpdword_t buffer_size_ptr, dword_t overlapped_ptr) {
  // Wrap function in a lambda func so we can use return to exit out when
  // needed, but still always be able to set the xoverlapped value
  // this way we don't need a bunch of if/else nesting to accomplish the same
  auto main_fn = [tile_type, game_id, item_id, offset, output_ptr,
                  buffer_size_ptr]() {
    uint64_t image_id = item_id;

    uint8_t* data = nullptr;
    size_t data_len = 0;
    std::unique_ptr<MappedMemory> mmap;

    if (!output_ptr || !buffer_size_ptr) {
      return X_ERROR_FILE_NOT_FOUND;
    }

    auto type = (XTileType)tile_type.value();
    if (kTileFileNames.count(type)) {
      // image_id = XUID of profile to retrieve from

      auto file_path = kernel_state()->user_profile(0)->profile_dir();
      file_path += kTileFileNames.at(type);

      mmap = MappedMemory::Open(file_path, MappedMemory::Mode::kRead);
      if (!mmap) {
        return X_ERROR_FILE_NOT_FOUND;
      }
      data = mmap->data();
      data_len = mmap->size();
    } else {
      auto gpd = kernel_state()->user_profile(0)->GetTitleGpd(game_id.value());

      if (!gpd) {
        return X_ERROR_FILE_NOT_FOUND;
      }

      auto entry = gpd->GetEntry(
          static_cast<uint16_t>(xdbf::GpdSection::kImage), image_id);

      if (!entry) {
        return X_ERROR_FILE_NOT_FOUND;
      }

      data = entry->data.data();
      data_len = entry->data.size();
    }

    if (!data || !data_len) {
      return X_ERROR_FILE_NOT_FOUND;
    }

    auto passed_size = *buffer_size_ptr;
    *buffer_size_ptr = (uint32_t)data_len;

    auto ret_val = X_ERROR_INVALID_PARAMETER;

    if (passed_size >= *buffer_size_ptr) {
      memcpy_s(output_ptr, *buffer_size_ptr, data, data_len);
      ret_val = X_ERROR_SUCCESS;
    }

    if (mmap) {
      mmap->Close();
    }

    return ret_val;
  };

  auto ret_val = main_fn();

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, ret_val);
    return X_ERROR_IO_PENDING;
  }
  return ret_val;
}
DECLARE_XAM_EXPORT1(XamReadTile, kUserProfiles, kSketchy);

dword_result_t XamReadTileEx_entry(dword_t tile_type, dword_t game_id,
                             qword_t item_id, dword_t offset, dword_t unk1,
                             dword_t unk2, lpdword_t output_ptr,
                             lpdword_t buffer_size_ptr) {
  return XamReadTile_entry(tile_type, game_id, item_id, offset, output_ptr,
                     buffer_size_ptr, 0);
}
DECLARE_XAM_EXPORT1(XamReadTileEx, kUserProfiles, kSketchy);

}  // namespace xdbf
}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(User);
