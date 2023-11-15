/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MEMORY_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MEMORY_H_

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {


namespace xboxkrnl {

uint32_t xeMmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t region_size,
                                      uint32_t protect_bits,
                                      uint32_t min_addr_range,
                                      uint32_t max_addr_range,
                                      uint32_t alignment);
uint32_t xeAllocatePoolTypeWithTag(PPCContext* context, uint32_t size,
                                   uint32_t tag, uint32_t zero);

void xeFreePool(PPCContext* context, uint32_t base_address);

uint32_t xeMmCreateKernelStack(uint32_t size, uint32_t r4);
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MEMORY_H_
