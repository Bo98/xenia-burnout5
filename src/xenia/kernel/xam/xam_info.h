/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_INFO_H_
#define XENIA_KERNEL_XAM_XAM_INFO_H_

#include "xenia/base/byte_order.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

uint32_t xeXamAlloc(uint32_t flags, uint32_t size, xe::be<uint32_t>* out_ptr);
void xeXamFree(uint32_t ptr);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
