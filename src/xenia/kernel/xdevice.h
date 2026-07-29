/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XDEVICE_H_
#define XENIA_KERNEL_XDEVICE_H_

#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

struct X_KDEVICE_QUEUE {
  xe::be<uint16_t> type;
  xe::be<uint8_t> size;
  xe::be<uint8_t> busy;
  X_KSPINLOCK lock;
  X_LIST_ENTRY device_list_head;
};
static_assert_size(X_KDEVICE_QUEUE, 0x10);

struct X_DEVICE_OBJECT {
  xe::be<int16_t> type;
  xe::be<uint16_t> size;
  xe::be<int32_t> reference_count;
  xe::be<uint32_t> driver_object_ptr;
  xe::be<uint32_t> mounted_device_ptr;
  xe::be<uint32_t> irp_ptr;
  xe::be<uint32_t> flags;
  xe::be<uint32_t> device_extension_ptr;
  xe::be<uint8_t> device_type;
  xe::be<uint8_t> start_flags;
  xe::be<uint8_t> stack_size;
  xe::be<uint8_t> deleted;
  xe::be<uint32_t> sector_size;
  xe::be<uint32_t> alignment_requirement;
  X_KDEVICE_QUEUE device_queue;
  X_KEVENT lock;
  xe::be<uint32_t> unknown_48;
  xe::be<uint32_t> start_key;
};
static_assert_size(X_DEVICE_OBJECT, 0x50);

class XDevice : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Device;

  explicit XDevice(KernelState* kernel_state);
  ~XDevice() override;

  void Initialize(uint32_t device_extension_size);
  void InitializeNative(void* native_ptr);

  bool Save(ByteStream* stream) override;
  static object_ref<XDevice> Restore(KernelState* kernel_state,
                                     ByteStream* stream);
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XDEVICE_H_
