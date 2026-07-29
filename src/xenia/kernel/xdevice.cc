/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xdevice.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {

XDevice::XDevice(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XDevice::~XDevice() = default;

void XDevice::Initialize(uint32_t device_extension_size) {
  auto native_ptr = this->CreateNative(
      sizeof(X_DEVICE_OBJECT) + xe::align<uint32_t>(device_extension_size, 8));
  auto native = reinterpret_cast<X_DEVICE_OBJECT*>(native_ptr);
  native->size = sizeof(X_DEVICE_OBJECT) + device_extension_size;
}

void XDevice::InitializeNative(void* native_ptr) {
  // No host side right now
}

bool XDevice::Save(ByteStream* stream) {
  XELOGD("XDevice {:08X}", handle());
  return SaveObject(stream);
}

object_ref<XDevice> XDevice::Restore(KernelState* kernel_state,
                                     ByteStream* stream) {
  auto evt = new XDevice(nullptr);
  evt->kernel_state_ = kernel_state;
  evt->RestoreObject(stream);
  return object_ref<XDevice>(evt);
}

}  // namespace kernel
}  // namespace xe
