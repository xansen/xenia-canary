/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_X64_BACKEND_H_
#define XENIA_CPU_BACKEND_X64_X64_BACKEND_H_

#include <memory>

#include "xenia/base/bit_map.h"
#include "xenia/base/cvar.h"
#include "xenia/cpu/backend/backend.h"

#if XE_PLATFORM_WIN32 == 1
// we use KUSER_SHARED's systemtime field, which is at a fixed address and
// obviously windows specific, to get the start/end time for a function using
// rdtsc would be too slow and skew the results by consuming extra cpu time, so
// we have lower time precision but better overall accuracy
#define XE_X64_PROFILER_AVAILABLE 1
#endif

DECLARE_int64(x64_extension_mask);
DECLARE_int64(max_stackpoints);
DECLARE_bool(enable_host_guest_stack_synchronization);
namespace xe {
class Exception;
}  // namespace xe
namespace xe {
namespace cpu {
namespace backend {
namespace x64 {
// mapping of guest function addresses to total nanoseconds taken in the func
using GuestProfilerData = std::map<uint32_t, uint64_t>;

class X64CodeCache;

typedef void* (*HostToGuestThunk)(void* target, void* arg0, void* arg1);
typedef void* (*GuestToHostThunk)(void* target, void* arg0, void* arg1);
typedef void (*ResolveFunctionThunk)();

/*
    place guest trampolines in the memory range that the HV normally occupies.
    This way guests can call in via the indirection table and we don't have to
   clobber/reuse an existing memory range The xboxkrnl range is already used by
   export trampolines (see kernel/kernel_module.cc)
*/
static constexpr uint32_t GUEST_TRAMPOLINE_BASE = 0x80000000;
static constexpr uint32_t GUEST_TRAMPOLINE_END = 0x80040000;

static constexpr uint32_t GUEST_TRAMPOLINE_MIN_LEN = 8;

static constexpr uint32_t MAX_GUEST_TRAMPOLINES =
    (GUEST_TRAMPOLINE_END - GUEST_TRAMPOLINE_BASE) / GUEST_TRAMPOLINE_MIN_LEN;

#define RESERVE_BLOCK_SHIFT 16

#define RESERVE_NUM_ENTRIES \
  ((1024ULL * 1024ULL * 1024ULL * 4ULL) >> RESERVE_BLOCK_SHIFT)
// https://codalogic.com/blog/2022/12/06/Exploring-PowerPCs-read-modify-write-operations
struct ReserveHelper {
  uint64_t blocks[RESERVE_NUM_ENTRIES / 64];

  ReserveHelper() { memset(blocks, 0, sizeof(blocks)); }
};

struct X64BackendStackpoint {
  uint64_t host_stack_;
  unsigned guest_stack_;
  // pad to 16 bytes so we never end up having a 64 bit load/store for
  // host_stack_ straddling two lines. Consider this field reserved for future
  // use
  unsigned guest_return_address_;
};
enum : uint32_t {
  kX64BackendMXCSRModeBit = 0,
  kX64BackendHasReserveBit = 1,
  kX64BackendNJMOn =
      2,  // non-java mode bit is currently set. for use in software fp routines
  kX64BackendNonIEEEMode =
      3,  // non-ieee mode is currently enabled for scalar fpu.
};
// located prior to the ctx register
// some things it would be nice to have be per-emulator instance instead of per
// context (somehow placing a global X64BackendCtx prior to membase, so we can
// negatively index the membase reg)
struct X64BackendContext {
  union {
    __m128 helper_scratch_xmms[4];
    uint64_t helper_scratch_u64s[8];
    uint32_t helper_scratch_u32s[16];
  };
  ReserveHelper* reserve_helper_;
  uint64_t cached_reserve_value_;
  // guest_tick_count is used if inline_loadclock is used
  uint64_t* guest_tick_count;
  // records mapping of host_stack to guest_stack
  X64BackendStackpoint* stackpoints;
  uint64_t cached_reserve_offset;
  uint32_t cached_reserve_bit;
  unsigned int current_stackpoint_depth;
  unsigned int mxcsr_fpu;  // currently, the way we implement rounding mode
                           // affects both vmx and the fpu
  unsigned int mxcsr_vmx;
  // bit 0 = 0 if mxcsr is fpu, else it is vmx
  // bit 1 = got reserve
  unsigned int flags;
  unsigned int Ox1000;  // constant 0x1000 so we can shrink each tail emitted
                        // add of it by... 2 bytes lol
};
constexpr unsigned int DEFAULT_VMX_MXCSR =
    0x8000 |                   // flush to zero
    0x0040 | (_MM_MASK_MASK);  // default rounding mode for vmx

constexpr unsigned int DEFAULT_FPU_MXCSR = 0x1F80;
extern const uint32_t mxcsr_table[8];
class X64Backend : public Backend {
 public:
  static const uint32_t kForceReturnAddress = 0x9FFF0000u;

  explicit X64Backend();
  ~X64Backend() override;

  X64CodeCache* code_cache() const { return code_cache_.get(); }
  uintptr_t emitter_data() const { return emitter_data_; }

  // Call a generated function, saving all stack parameters.
  HostToGuestThunk host_to_guest_thunk() const { return host_to_guest_thunk_; }
  // Function that guest code can call to transition into host code.
  GuestToHostThunk guest_to_host_thunk() const { return guest_to_host_thunk_; }
  // Function that thunks to the ResolveFunction in X64Emitter.
  ResolveFunctionThunk resolve_function_thunk() const {
    return resolve_function_thunk_;
  }

  void* synchronize_guest_and_host_stack_helper() const {
    return synchronize_guest_and_host_stack_helper_;
  }
  void* synchronize_guest_and_host_stack_helper_for_size(size_t sz) const {
    switch (sz) {
      case 1:
        return synchronize_guest_and_host_stack_helper_size8_;
      case 2:
        return synchronize_guest_and_host_stack_helper_size16_;
      default:
        return synchronize_guest_and_host_stack_helper_size32_;
    }
  }
  bool Initialize(Processor* processor) override;

  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high) override;

  std::unique_ptr<Assembler> CreateAssembler() override;

  std::unique_ptr<GuestFunction> CreateGuestFunction(Module* module,
                                                     uint32_t address) override;

  uint64_t CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                        uint64_t current_pc) override;

  void InstallBreakpoint(Breakpoint* breakpoint) override;
  void InstallBreakpoint(Breakpoint* breakpoint, Function* fn) override;
  void UninstallBreakpoint(Breakpoint* breakpoint) override;
  virtual void InitializeBackendContext(void* ctx) override;
  virtual void DeinitializeBackendContext(void* ctx) override;
  virtual void PrepareForReentry(void* ctx) override;
  X64BackendContext* BackendContextForGuestContext(void* ctx) {
    return reinterpret_cast<X64BackendContext*>(
        reinterpret_cast<intptr_t>(ctx) - sizeof(X64BackendContext));
  }
  virtual uint32_t CreateGuestTrampoline(GuestTrampolineProc proc,
                                         void* userdata1, void* userdata2,
                                         bool long_term) override;

  virtual void FreeGuestTrampoline(uint32_t trampoline_addr) override;
  virtual void SetGuestRoundingMode(void* ctx, unsigned int mode) override;
  virtual bool PopulatePseudoStacktrace(GuestPseudoStackTrace* st) override;
  void RecordMMIOExceptionForGuestInstruction(void* host_address);

  uint32_t LookupXMMConstantAddress32(unsigned index) {
    return static_cast<uint32_t>(emitter_data() + sizeof(vec128_t) * index);
  }
  void* LookupXMMConstantAddress(unsigned index) {
    return reinterpret_cast<void*>(emitter_data() + sizeof(vec128_t) * index);
  }
#if XE_X64_PROFILER_AVAILABLE == 1
  uint64_t* GetProfilerRecordForFunction(uint32_t guest_address);
#endif
 private:
  static bool ExceptionCallbackThunk(Exception* ex, void* data);
  bool ExceptionCallback(Exception* ex);

  uintptr_t capstone_handle_ = 0;

  std::unique_ptr<X64CodeCache> code_cache_;
  uintptr_t emitter_data_ = 0;

  HostToGuestThunk host_to_guest_thunk_;
  GuestToHostThunk guest_to_host_thunk_;
  ResolveFunctionThunk resolve_function_thunk_;
  void* synchronize_guest_and_host_stack_helper_ = nullptr;

  // loads stack sizes 1 byte, 2 bytes or 4 bytes
  void* synchronize_guest_and_host_stack_helper_size8_ = nullptr;
  void* synchronize_guest_and_host_stack_helper_size16_ = nullptr;
  void* synchronize_guest_and_host_stack_helper_size32_ = nullptr;

 public:
  void* try_acquire_reservation_helper_ = nullptr;
  void* reserved_store_32_helper = nullptr;
  void* reserved_store_64_helper = nullptr;
  void* vrsqrtefp_vector_helper = nullptr;
  void* vrsqrtefp_scalar_helper = nullptr;
  void* frsqrtefp_helper = nullptr;

 private:
#if XE_X64_PROFILER_AVAILABLE == 1
  GuestProfilerData profiler_data_;
#endif

  alignas(64) ReserveHelper reserve_helper_;
  // allocates 8-byte aligned addresses in a normally not executable guest
  // address
  // range that will be used to dispatch to host code
  BitMap guest_trampoline_address_bitmap_;
  uint8_t* guest_trampoline_memory_;
};

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_X64_X64_BACKEND_H_
