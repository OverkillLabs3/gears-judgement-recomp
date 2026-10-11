// gowj - ReXGlue Recompiled Project
//
// Guest XamInput* entry-point serializer, and the instrument that proves the
// race it fixes.
//
// Measured, two independent cdb captures of the same death:
//   rexruntime!std::vector<rex::input::DeviceInfo>::clear  <-
//   rexruntime!rex::input::InputSystem::RefreshDevices+0x970 <-
//   rexruntime!rex::input::InputSystem::GetState+0x40 <-
//   rexruntime!rex::kernel::xam::XamInputGetState_entry <-  guest XThread
// Once it faults as `ud2` inside clear() with rcx pointing at
// 0xBAADF00D-prefilled (never-written) LFH commit, once as a clean register set
// at clear+0xe3 - i.e. the vector's own bookkeeping, not the caller's arguments.
// That is heap corruption in the runtime's device list, and it is the process
// death this project has been calling "crashes at the title / at mission start"
// (0xC0000374 / 0xC000001D / 0xC0000005 with no [FATAL] line).
//
// RefreshDevices is reached from *every* XamInputGetState call, and the guest
// polls input from several concurrent XThreads (KBMRATE logs alternating t-ids,
// ~125 polls/s). InputSystem has no lock: `devices_` is a plain
// std::vector<DeviceInfo> that GetState rebuilds in place. Two threads in
// RefreshDevices is a textbook concurrent resize/free, and the runtime's own
// allocator is what ends up walking garbage.
//
// Fix: hold one recursive lock across the whole call for all three input APIs
// the guest imports, so no two threads are ever inside InputSystem at once.
// A shared lock, not per-API: GetCapabilities touches the same vector as
// GetState. The overlap counters are incremented BEFORE the lock is taken, so
// the same build both measures the racing population and removes it - and
// GOWJ_XAM_INPUT_LOCK=0 turns the measurement back into the bug on demand.
//
// Mechanism is the proven export-thunk rewrite (see gowj_content_probe.cpp):
// rexruntime's `__imp__Xam...` slot is a 16-byte `jmp <body>`, and every caller
// (gowj's IAT, the PPC func-mapping table, indirect dispatch) funnels through
// it, so pointing the slot at a wrapper and calling the saved body needs no
// trampoline and no fixups.

#include "gowj_xam_input_guard.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

namespace {

constexpr size_t kMaxTargets = 3;
constexpr size_t kThunkSize = 16;
// Long enough that a legitimate slow call never trips it, short enough that a
// deadlock cannot stall the title: on timeout the call proceeds unlocked, which
// is exactly today's behaviour, and the event is logged.
constexpr int kLockWaitMs = 500;

struct Target {
  const char* export_name;
  PPCFunc* body;  // original target, resolved at install time
};

// Exactly the input APIs the guest imports
// (gowj/generated/default/gowj_funcs.h).
Target kTargets[kMaxTargets] = {
    {"__imp__XamInputGetState", nullptr},
    {"__imp__XamInputSetState", nullptr},
    {"__imp__XamInputGetCapabilities", nullptr},
};

std::recursive_timed_mutex g_call_lock;
bool g_lock_enabled = true;

std::atomic<uint32_t> g_inside{0};
std::atomic<uint32_t> g_last_arrival{0};
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_overlaps{0};
std::atomic<uint64_t> g_timeouts{0};
std::atomic<uint32_t> g_thread_count{0};
std::atomic<uint32_t> g_thread_ids[16]{};

// Cost of the serialization, because a lock that stops a crash and adds frame
// jitter has only traded one defect for another. Hold = time inside the runtime's
// call (RefreshDevices re-enumerates every driver on each one, so this is not
// free); wait = time this call spent blocked behind another thread.
LARGE_INTEGER g_qpc_freq{};
std::atomic<uint64_t> g_hold_sum_us{0};
std::atomic<uint64_t> g_hold_max_us{0};
std::atomic<uint64_t> g_wait_sum_us{0};
std::atomic<uint64_t> g_wait_max_us{0};
std::atomic<uint64_t> g_slow_wait{0};

uint64_t ElapsedUs(LARGE_INTEGER from, LARGE_INTEGER now) {
  if (g_qpc_freq.QuadPart == 0) {
    return 0;
  }
  return static_cast<uint64_t>((now.QuadPart - from.QuadPart) * 1000000 /
                               g_qpc_freq.QuadPart);
}

void NoteHoldUs(uint64_t us) {
  g_hold_sum_us.fetch_add(us, std::memory_order_relaxed);
  uint64_t prev = g_hold_max_us.load(std::memory_order_relaxed);
  while (us > prev &&
         !g_hold_max_us.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
  }
}

void NoteWaitUs(uint64_t us) {
  g_wait_sum_us.fetch_add(us, std::memory_order_relaxed);
  uint64_t prev = g_wait_max_us.load(std::memory_order_relaxed);
  while (us > prev &&
         !g_wait_max_us.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
  }
  // One frame is 16.7 ms: a wait of even 1 ms on the thread that is pumping
  // input for the guest's own frame is already visible as a hitch.
  if (us > 1000) {
    g_slow_wait.fetch_add(1, std::memory_order_relaxed);
  }
}

void NoteThread(uint32_t tid) {
  for (auto& slot : g_thread_ids) {
    uint32_t seen = slot.load(std::memory_order_relaxed);
    if (seen == tid) {
      return;
    }
    if (seen == 0 && slot.compare_exchange_strong(seen, tid)) {
      g_thread_count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

template <size_t I>
void GuardWrap(PPCContext& ctx, uint8_t* base) {
  const uint32_t tid = GetCurrentThreadId();
  NoteThread(tid);

  // Pre-lock: counts threads that entered while another was still inside the
  // API, i.e. the population the serialized version no longer overlaps.
  const uint32_t already_inside = g_inside.fetch_add(1, std::memory_order_acq_rel);
  const uint32_t last_arrival = g_last_arrival.exchange(tid, std::memory_order_acq_rel);
  const uint64_t call = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (already_inside) {
    const uint64_t overlap = g_overlaps.fetch_add(1, std::memory_order_relaxed) + 1;
    static std::atomic<int> budget{12};
    if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_WARN("XAMINPUTGUARD {} OVERLAP #{}: tid {:X} entered with {} call(s) "
                  "already inside (last arrival tid {:X}, calls={}, threads={}, "
                  "lock={})",
                  kTargets[I].export_name, overlap, tid, already_inside, last_arrival,
                  call, g_thread_count.load(std::memory_order_relaxed), g_lock_enabled);
    }
  }
  if (call % 2048 == 0) {
    const uint64_t window = 2048;
    REXLOG_WARN(
        "XAMINPUTGUARD state calls={} overlaps={} threads={} timeouts={} lock={} "
        "hold_us avg={:.2f} max={} wait_us avg={:.2f} max={} slow_waits(>1ms)={}",
        call, g_overlaps.load(std::memory_order_relaxed),
        g_thread_count.load(std::memory_order_relaxed),
        g_timeouts.load(std::memory_order_relaxed), g_lock_enabled,
        g_hold_sum_us.exchange(0, std::memory_order_relaxed) / double(window),
        g_hold_max_us.exchange(0, std::memory_order_relaxed),
        g_wait_sum_us.exchange(0, std::memory_order_relaxed) / double(window),
        g_wait_max_us.exchange(0, std::memory_order_relaxed),
        g_slow_wait.exchange(0, std::memory_order_relaxed));
  }

  LARGE_INTEGER arrive;
  QueryPerformanceCounter(&arrive);
  bool held = false;
  if (g_lock_enabled) {
    held = g_call_lock.try_lock_for(std::chrono::milliseconds(kLockWaitMs));
    LARGE_INTEGER owned;
    QueryPerformanceCounter(&owned);
    NoteWaitUs(ElapsedUs(arrive, owned));
    arrive = owned;
    if (!held) {
      const uint64_t timeout = g_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
      static std::atomic<int> budget{8};
      if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        REXLOG_WARN("XAMINPUTGUARD {} LOCKTIMEOUT #{} after {}ms (inside={}) - "
                    "calling unlocked",
                    kTargets[I].export_name, timeout, kLockWaitMs, already_inside + 1);
      }
    }
  }

  struct Scope {
    bool held;
    ~Scope() {
      if (held) {
        g_call_lock.unlock();
      }
      g_inside.fetch_sub(1, std::memory_order_acq_rel);
    }
  } scope{held};

  kTargets[I].body(ctx, base);

  LARGE_INTEGER done;
  QueryPerformanceCounter(&done);
  NoteHoldUs(ElapsedUs(arrive, done));
}

PPCFunc* const* Wrappers() {
  static PPCFunc* const w[kMaxTargets] = {
      &GuardWrap<0>, &GuardWrap<1>, &GuardWrap<2>,
  };
  return w;
}

bool Readable(const void* p, size_t len) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
    return false;
  }
  uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return reinterpret_cast<uintptr_t>(p) + len <= end;
}

// Follow rexruntime's `jmp <body>` export thunk to the body it dispatches to.
PPCFunc* ResolveThunkBody(uint8_t* thunk) {
  if (thunk[0] == 0xE9) {
    int32_t rel = 0;
    std::memcpy(&rel, thunk + 1, 4);
    return reinterpret_cast<PPCFunc*>(thunk + 5 + rel);
  }
  if (thunk[0] == 0xFF && thunk[1] == 0x25) {
    int32_t rel = 0;
    std::memcpy(&rel, thunk + 2, 4);
    uint8_t* cell = thunk + 6 + rel;
    if (Readable(cell, 8)) {
      void* target = nullptr;
      std::memcpy(&target, cell, 8);
      return reinterpret_cast<PPCFunc*>(target);
    }
  }
  return nullptr;
}

}  // namespace

namespace rex::glue {

void InstallXamInputGuard() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;

  // ElapsedUs divides by this, so a zero frequency would be a fault inside the
  // guard that fixes a fault - check it rather than trusting the call.
  if (!QueryPerformanceFrequency(&g_qpc_freq) || g_qpc_freq.QuadPart == 0) {
    g_qpc_freq.QuadPart = 0;
  }

  char gate[8]{};
  if (GetEnvironmentVariableA("GOWJ_XAM_INPUT_GUARD", gate, sizeof(gate)) &&
      std::strcmp(gate, "0") == 0) {
    REXLOG_INFO("XAMINPUTGUARD disabled via GOWJ_XAM_INPUT_GUARD=0");
    return;
  }
  char lock_gate[8]{};
  if (GetEnvironmentVariableA("GOWJ_XAM_INPUT_LOCK", lock_gate, sizeof(lock_gate)) &&
      std::strcmp(lock_gate, "0") == 0) {
    g_lock_enabled = false;
    REXLOG_WARN("XAMINPUTGUARD counting only (GOWJ_XAM_INPUT_LOCK=0)");
  }

  HMODULE mod = GetModuleHandleA("rexruntime.dll");
  if (!mod) {
    REXLOG_WARN("XAMINPUTGUARD rexruntime.dll not loaded, guard not installed");
    return;
  }

  size_t hits = 0;
  for (size_t i = 0; i < kMaxTargets; ++i) {
    uint8_t* thunk =
        reinterpret_cast<uint8_t*>(GetProcAddress(mod, kTargets[i].export_name));
    if (!thunk) {
      REXLOG_WARN("XAMINPUTGUARD {} not exported", kTargets[i].export_name);
      continue;
    }
    PPCFunc* body = ResolveThunkBody(thunk);
    if (!body) {
      REXLOG_WARN("XAMINPUTGUARD {} thunk {:#x} is not a jmp thunk",
                  kTargets[i].export_name, reinterpret_cast<uintptr_t>(thunk));
      continue;
    }
    kTargets[i].body = body;

    // FF 25 00000000 imm64 -> jmp qword ptr [rip+0]; 14 bytes in a 16 byte slot.
    DWORD old = 0;
    if (!VirtualProtect(thunk, kThunkSize, PAGE_EXECUTE_READWRITE, &old)) {
      REXLOG_WARN("XAMINPUTGUARD {} unprotect failed (gle={})", kTargets[i].export_name,
                  GetLastError());
      continue;
    }
    thunk[0] = 0xFF;
    thunk[1] = 0x25;
    std::memset(thunk + 2, 0, 4);
    void* replacement = reinterpret_cast<void*>(Wrappers()[i]);
    std::memcpy(thunk + 6, &replacement, 8);
    DWORD tmp = 0;
    VirtualProtect(thunk, kThunkSize, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), thunk, kThunkSize);
    ++hits;
    REXLOG_INFO("XAMINPUTGUARD {} thunk {:#x} -> wrapper {:#x} (body {:#x})",
                kTargets[i].export_name, reinterpret_cast<uintptr_t>(thunk),
                reinterpret_cast<uintptr_t>(replacement),
                reinterpret_cast<uintptr_t>(body));
  }
  REXLOG_INFO("XAMINPUTGUARD hooked {}/{} guest input entry points (lock={})", hits,
              kMaxTargets, g_lock_enabled);
}

}  // namespace rex::glue

namespace {

// The game checks the disc in the drive at startup. This port does not mount a disc drive (the game's
// video decoder, which the disc movies need, crashes at mission start), so that check fails and the game
// asks the system to show its "Disc Read Error" box. The game carries on without it, so the box is only
// a startup popup that has to be dismissed. This replaces that one call with a parked thread and logs it once.
void DiscErrorNoop(PPCContext& ctx, uint8_t* base) {
  (void)base;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("DISCERR the game's 'Disc Read Error' box was suppressed (no disc drive is mounted by design)");
  }
  // The guest treats this call as one that never comes back (the runtime's own version shows the box and
  // then exits the process), and the title shuts itself down if it does return. So only the box is removed:
  // the calling thread is parked, which is the state the game runs in while the box is up, and every other
  // thread carries on.
  for (;;) Sleep(INFINITE);
}

}  // namespace

namespace rex::glue {

void InstallDiscErrorSilencer() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  char gate[8]{};
  if (GetEnvironmentVariableA("GOWJ_DISC_ERROR_UI", gate, sizeof(gate)) && std::strcmp(gate, "1") == 0) return;
  HMODULE mod = GetModuleHandleA("rexruntime.dll");
  if (!mod) return;
  uint8_t* thunk = reinterpret_cast<uint8_t*>(GetProcAddress(mod, "__imp__XamShowDirtyDiscErrorUI"));
  if (!thunk) {
    REXLOG_WARN("DISCERR __imp__XamShowDirtyDiscErrorUI not exported, box not suppressed");
    return;
  }
  DWORD old = 0;
  if (!VirtualProtect(thunk, kThunkSize, PAGE_EXECUTE_READWRITE, &old)) return;
  thunk[0] = 0xFF;
  thunk[1] = 0x25;
  std::memset(thunk + 2, 0, 4);
  void* replacement = reinterpret_cast<void*>(&DiscErrorNoop);
  std::memcpy(thunk + 6, &replacement, 8);
  DWORD tmp = 0;
  VirtualProtect(thunk, kThunkSize, old, &tmp);
  FlushInstructionCache(GetCurrentProcess(), thunk, kThunkSize);
  REXLOG_INFO("DISCERR disc error box silenced (thunk {:#x})", reinterpret_cast<uintptr_t>(thunk));
}

}  // namespace rex::glue
