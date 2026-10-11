// gowj - ReXGlue Recompiled Project
//
// XamContent* import shim.
//
// Measured: ContentManager::OpenContent receives a 10-byte "path"
// that is live guest UE3 object data (floats + 0x7018xxxx pointers), never a
// filename. rexruntime decodes content keys as UTF-8 and throws on a bad lead
// byte, which terminates the process (see gowj_utf8_guard.h).
//
// Two interception attempts failed before this one, both measured:
//  - PPCFuncMappings: 5 of 60372 slots rewritten, zero calls. Generated guest
//    code calls imports as `__imp__XamContentCreateEx(ctx, base)` directly.
//  - `&__imp__Xam...` in this TU is NOT gowj.exe's IAT slot: link.exe hands out
//    an 8-byte `jmp [rip+d]` address thunk for an address-taken import, so
//    writing through it overwrote code, not a pointer (seen as orig=0xcccc...).
//
// rexruntime exports the real call target: `__imp__XamContentCreateEx` is a
// 16-byte slot holding `jmp <HostToGuestFunction<...>_body>`, and every path
// (gowj's IAT, the mapping table, indirect dispatch) funnels through it. The
// shim rewrites that slot to jump to a wrapper and calls the saved body
// pointer, so no trampoline and no fixups are needed.
//
// Target list is exactly the 8 content APIs the guest imports
// (gowj/generated/default/gowj_funcs.h); hooking unimported ones just burns
// export rewrites. Each target records which PPC register carries the
// XCONTENT_DATA pointer, derived from the generated call sites, e.g.
// gowj_recomp.116.cpp:27771-27833 (sub_82DBACF8, lr=0x82DBAD60) reads
// lwz 0(r5)/lwz 4(r5) as device_id/content_type while r4 holds "savedrive0".
//
// Set GOWJ_CONTENT_PROBE=0 to skip installation; it is pure observation
// (no ctx or guest-memory writes) but it does patch 8 export slots.

#include "gowj_content_probe.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

// Set GOWJ_CONTENT_FIX=0 to disable the stable-descriptor copy (leaving the shim
// as pure observation). Default ON: this is the save-corruption fix, not a probe.
bool ContentFixEnabled() {
  static const bool enabled = [] {
    char gate[8]{};
    if (GetEnvironmentVariableA("GOWJ_CONTENT_FIX", gate, sizeof(gate)) &&
        std::strcmp(gate, "0") == 0) {
      return false;
    }
    return true;
  }();
  return enabled;
}

// The 8 APIs are listed with kContentDataSize snapshots each, so keep this in
// sync with the wrapper table below.
constexpr size_t kMaxTargets = 8;
constexpr size_t kThunkSize = 16;

// XCONTENT_DATA / XCONTENT_AGGREGATE_DATA field offsets (0x134 / 0x148).
constexpr size_t kContentDataSize = 0x134;
constexpr size_t kFileNameOffset = 0x108;
constexpr size_t kFileNameSize = 42;

struct Target {
  const char* export_name;
  // Register index into regs[] holding the content-data pointer, or -1.
  int data_reg;
  PPCFunc* body;  // original target, resolved at install time
};

// clang-format off
Target kTargets[kMaxTargets] = {
    {"__imp__XamContentCreateEx", 2, nullptr},          // r5
    {"__imp__XamContentDelete", 1, nullptr},            // r4
    {"__imp__XamContentGetCreator", 1, nullptr},        // r4
    {"__imp__XamContentGetDeviceData", 1, nullptr},     // r4
    {"__imp__XamContentGetDeviceState", -1, nullptr},   // scalar device id only
    {"__imp__XamContentCreateEnumerator", -1, nullptr},
    {"__imp__XamContentGetLicenseMask", -1, nullptr},
    {"__imp__XamContentClose", -1, nullptr},            // handle only
};
// clang-format on

bool Readable(const void* p, size_t len) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
    return false;
  }
  DWORD prot = mbi.Protect & 0xFF;
  if (prot == PAGE_NOACCESS || prot == PAGE_GUARD) {
    return false;
  }
  uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return reinterpret_cast<uintptr_t>(p) + len <= end;
}

// Render the file_name_raw, device id and content type of a candidate
// XCONTENT_DATA so a real content request is recognisable at a glance.
bool DescribeContentData(std::string& out, const uint8_t* host, uint32_t guest_ptr) {
  if (!guest_ptr || !Readable(host + guest_ptr, kFileNameOffset + kFileNameSize)) {
    return false;
  }
  const uint8_t* s = host + guest_ptr;
  uint32_t device_id = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(s));
  uint32_t content_type = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(s + 4));
  std::string name;
  bool printable = true;
  for (size_t i = 0; i < kFileNameSize; ++i) {
    uint8_t c = s[kFileNameOffset + i];
    if (c == 0) {
      break;
    }
    printable = printable && c >= 0x20 && c < 0x7F;
    name += static_cast<char>(printable ? c : '.');
  }
  fmt::format_to(std::back_inserter(out), " data[@{:08x}]{{dev={:08x} type={} name='{}'{}}}",
                 guest_ptr, device_id, content_type, name,
                 printable ? "" : " NONASCII");
  return true;
}

// Stale-argument trace. A content create arrives valid at call time but
// ContentManager::OpenContent is later handed bytes from the same storage after
// the guest recycled it (measured: create at r5=0x460bce54 with
// display_name="Play...", 178ms later OpenContent reads 0x7018DAB4 ==
// a recycled object's XCONTENT_DATA::file_name_raw at +0x108). Snapshot the
// struct at call time and re-read it on every later hooked call; a DIFF whose
// new bytes match what the runtime consumed proves the deferred-overlapped
// worker re-reads a PPC argument slot late. Observation only - nothing here is
// written back into guest memory.
struct Snapshot {
  LARGE_INTEGER qpc;
  uint32_t lr;
  uint32_t data_ptr;
  uint8_t raw[kContentDataSize];
  bool used;
  bool reported;
};

constexpr size_t kRingSize = 64;
Snapshot g_ring[kRingSize];
std::atomic<size_t> g_ring_pos{0};
std::atomic<int> g_diff_budget{16};
LARGE_INTEGER g_qpc_freq{};

double ElapsedMs(LARGE_INTEGER from, LARGE_INTEGER now) {
  return g_qpc_freq.QuadPart
             ? static_cast<double>(now.QuadPart - from.QuadPart) * 1000.0 /
                   static_cast<double>(g_qpc_freq.QuadPart)
             : 0.0;
}

// Compare each stored snapshot against live guest memory now.
void SweepStaleArgs(const uint8_t* host) {
  if (g_diff_budget.load(std::memory_order_relaxed) <= 0) {
    return;
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  for (size_t i = 0; i < kRingSize; ++i) {
    Snapshot& s = g_ring[i];
    if (!s.used || s.reported) {
      continue;
    }
    if (!Readable(host + s.data_ptr, kContentDataSize)) {
      continue;
    }
    const uint8_t* live = host + s.data_ptr;
    size_t first_diff = kContentDataSize;
    for (size_t b = 0; b < kContentDataSize; ++b) {
      if (live[b] != s.raw[b]) {
        first_diff = b;
        break;
      }
    }
    if (first_diff == kContentDataSize) {
      continue;
    }
    if (g_diff_budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
      return;
    }
    s.reported = true;
    std::string was, now_s;
    for (size_t b = first_diff; b < kContentDataSize && (b - first_diff) < 24; ++b) {
      fmt::format_to(std::back_inserter(was), "{:02x}", s.raw[b]);
      fmt::format_to(std::back_inserter(now_s), "{:02x}", live[b]);
    }
    REXLOG_WARN(
        "CONTENTPROBE STALEARG @{:08x} +{:#x} changed {}ms after lr={:08x}: "
        "was={} now={} (file_name_raw then='{}')",
        s.data_ptr, first_diff, static_cast<int>(ElapsedMs(s.qpc, now)), s.lr, was, now_s,
        reinterpret_cast<const char*>(s.raw + kFileNameOffset));
  }
}

void LogArgs(const char* name, int data_reg, PPCContext& ctx, uint8_t* base) {
  static std::atomic<int> budget{64};
  const bool log = budget.fetch_sub(1, std::memory_order_relaxed) > 0;
  const uint32_t regs[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                            ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};

  if (data_reg >= 0 && regs[data_reg]) {
    const uint32_t ptr = regs[data_reg];
    if (Readable(base + ptr, kContentDataSize)) {
      LARGE_INTEGER qpc;
      QueryPerformanceCounter(&qpc);
      const size_t i = g_ring_pos.fetch_add(1, std::memory_order_relaxed) % kRingSize;
      Snapshot& s = g_ring[i];
      s.qpc = qpc;
      s.lr = static_cast<uint32_t>(ctx.lr);
      s.data_ptr = ptr;
      s.used = true;
      s.reported = false;
      std::memcpy(s.raw, base + ptr, kContentDataSize);
    }
  }
  SweepStaleArgs(base);

  if (!log) {
    return;
  }
  std::string line;
  fmt::format_to(std::back_inserter(line), " lr={:08x}", static_cast<uint32_t>(ctx.lr));
  for (size_t i = 0; i < 8; ++i) {
    fmt::format_to(std::back_inserter(line), " r{}={:08x}", i + 3, regs[i]);
    if (regs[i]) {
      const uint8_t* host = base + regs[i];
      if (Readable(host, 16)) {
        std::string hex, ascii;
        for (size_t k = 0; k < 16; ++k) {
          fmt::format_to(std::back_inserter(hex), "{:02x}", host[k]);
          uint8_t c = host[k];
          ascii += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
        }
        fmt::format_to(std::back_inserter(line), " ->[{} \"{}\"]", hex, ascii);
      }
    }
  }
  // 9th argument onwards is stack-passed at 0x54(r1) in the PPC64-style frame.
  if (Readable(base + ctx.r1.u32 + 0x54, 8)) {
    const uint32_t s0 = __builtin_bswap32(
        *reinterpret_cast<const uint32_t*>(base + ctx.r1.u32 + 0x54));
    fmt::format_to(std::back_inserter(line), " stack0x54={:08x}", s0);
  }
  if (data_reg >= 0) {
    DescribeContentData(line, base, regs[data_reg]);
  }
  REXLOG_WARN("CONTENTPROBE {}{}", name, line);
}

// The guest opens GearsCheckpoint under a fresh root alias every time (SG0_0,
// SG1_0, ... measured past SG9_0 within 5 minutes of play), and on hardware all
// of them name the same package. An alias with no device made every later
// NtCreateFile fail 0xc000000f, which the guest reports as a corrupted save.
std::mutex g_mount_mu;
std::set<std::string> g_mounted;
std::filesystem::path g_checkpoint_dir;

std::unordered_map<std::string, uint32_t> g_root_flags;  // "SG3_0:" -> CreateEx flags

void EnsureCheckpointRoot(uint8_t* base, uint32_t root_ptr, uint32_t data_ptr,
                          uint32_t flags) {
  if (!root_ptr || !data_ptr || !Readable(base + root_ptr, 16) ||
      !Readable(base + data_ptr, kContentDataSize)) {
    return;
  }
  const char* name = reinterpret_cast<const char*>(base + data_ptr + kFileNameOffset);
  if (strnlen(name, kFileNameSize) != 15 || std::memcmp(name, "GearsCheckpoint", 15)) {
    return;
  }
  std::string root;
  for (size_t i = 0; i < 16; ++i) {
    const char c = static_cast<char>(base[root_ptr + i]);
    if (c == 0) break;
    if (c < 0x20 || c >= 0x7F || c == ':' || c == '\\') return;
    root += c;
  }
  if (root.empty()) return;
  root += ':';

  std::lock_guard lock(g_mount_mu);
  g_root_flags[root] = flags;
  if (g_checkpoint_dir.empty() || !g_mounted.insert(root).second) return;
  auto* rt = rex::Runtime::instance();
  if (!rt || !rt->file_system()) return;
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(
      root, g_checkpoint_dir, /*read_only=*/false, /*allow_share_delete=*/true);
  const bool init = device->Initialize();
  const bool ok = rt->file_system()->RegisterDevice(std::move(device));
  REXLOG_WARN("CONTENTFIX mounted checkpoint alias {} -> {} init={} mount={}", root,
              g_checkpoint_dir.string(), init, ok);
}

// XamContentCreateEx for the GearsCheckpoint package, served here instead of by
// the runtime. Measured 07:29: the runtime's content manager never
// releases a package (its XamContentClose fails ERROR_FILE_NOT_FOUND on every
// call), so by the title's third read-only open in the boot slot scan the open
// fails, the title falls back to OPEN_ALWAYS, that completes ERROR_ALREADY_EXISTS
// (0xB7 in the XOVERLAPPED) and the slot is reported corrupted - with every
// checkpoint file intact and byte-exact on disk. The files themselves are served
// by the host directory mounted on the alias, so all the package needs is
// hardware open/close semantics:
//   OPEN_EXISTING of a package with no files -> ERROR_PATH_NOT_FOUND
//   otherwise                                 -> success, opened/created
// published through the XOVERLAPPED the title polls (hEvent 0 in every call).
bool ServeCheckpointCreate(PPCContext& ctx, uint8_t* base) {
  const uint32_t root_ptr = ctx.r4.u32, data = ctx.r5.u32, flags = ctx.r6.u32;
  if (!data || !Readable(base + data, kContentDataSize)) return false;
  const char* name = reinterpret_cast<const char*>(base + data + kFileNameOffset);
  if (strnlen(name, kFileNameSize) != 15 || std::memcmp(name, "GearsCheckpoint", 15)) {
    return false;
  }
  uint32_t ovl = 0;
  if (Readable(base + ctx.r1.u32 + 0x54, 4)) {
    std::memcpy(&ovl, base + ctx.r1.u32 + 0x54, 4);
    ovl = __builtin_bswap32(ovl);
  }
  if (ovl && !Readable(base + ovl, 20)) return false;
  if (ovl) {
    uint32_t hevent;
    std::memcpy(&hevent, base + ovl + 12, 4);
    if (hevent) return false;  // would need a kernel event signal; leave to runtime
  }
  EnsureCheckpointRoot(base, root_ptr, data, flags);
  std::filesystem::path dir;
  {
    std::lock_guard lock(g_mount_mu);
    dir = g_checkpoint_dir;
  }
  if (dir.empty()) return false;
  std::error_code ec;
  bool has_files = false;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code fec;
    if (it->is_regular_file(fec) && it->file_size(fec) > 0) {
      has_files = true;
      break;
    }
  }
  const uint32_t mode = flags & 0xF;  // 1 new, 2 always-new, 3 existing, 4 always
  uint32_t result = 0, disposition = has_files ? 2 : 1;  // 2 opened, 1 created
  if (mode == 3 && !has_files) result = 3;              // ERROR_PATH_NOT_FOUND
  if (mode == 1 && has_files) result = 183;             // ERROR_ALREADY_EXISTS
  if (ctx.r7.u32 && Readable(base + ctx.r7.u32, 4) && result == 0) {
    const uint32_t d = __builtin_bswap32(disposition);
    std::memcpy(base + ctx.r7.u32, &d, 4);
  }
  if (ovl) {
    const uint32_t low = __builtin_bswap32(result);
    const uint32_t high = __builtin_bswap32(result == 0 ? disposition : 0);
    std::memcpy(base + ovl + 4, &high, 4);
    std::memcpy(base + ovl, &low, 4);
    ctx.r3.u64 = 997;  // ERROR_IO_PENDING; the completion is already in ovl
  } else {
    ctx.r3.u64 = result;
  }
  static std::atomic<int> budget{64};
  if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
    REXLOG_WARN("CONTENTFIX served GearsCheckpoint create flags={:08x} files={} -> "
                "result={} disposition={} ovl={:08x}",
                flags, has_files, result, disposition, ovl);
  }
  return true;
}

template <size_t I>
void ProbeWrap(PPCContext& ctx, uint8_t* base) {
  if (I == 0 && ContentFixEnabled() && ServeCheckpointCreate(ctx, base)) {
    return;
  }
  if (I == 0) {
    EnsureCheckpointRoot(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    static std::atomic<int> raw_budget{24};
    if (ctx.r5.u32 && Readable(base + ctx.r5.u32, kContentDataSize) &&
        raw_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      std::string hex;
      for (size_t b = 0; b < kContentDataSize; ++b) {
        const uint8_t c = base[ctx.r5.u32 + b];
        if (b < 8 || b >= kFileNameOffset || c) {
          fmt::format_to(std::back_inserter(hex), "{}{:02x}", b == 8 || b == kFileNameOffset ? "|" : "", c);
        }
      }
      REXLOG_WARN("CONTENTPROBE CreateEx raw flags={:08x} disp*={:08x} lic*={:08x} "
                  "cache={:08x} size={:08x} data={}",
                  ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, hex);
    }
  }
  // Save-corruption fix (measured). XamContentCreateEx returns
  // 0x3E5 = ERROR_IO_PENDING: the request is serviced by a deferred host worker
  // that re-reads the guest's XCONTENT_DATA descriptor LATER. The STALEARG lines
  // above prove that descriptor is mutated/recycled 1.4-8.6 s after the call, so
  // the worker builds a malformed header -> the slot reads "CORRUPTED". Copy the
  // struct into a SystemHeap guest buffer the guest cannot recycle and repoint
  // the argument register at it, so the worker's later read sees the snapshot
  // taken at call time.
  //
  // ONLY CreateEx (index 0) gets this. The other hooked calls take XCONTENT_DATA
  // as an input/output: the runtime writes the queried result (device total/free
  // space, creator XUID) BACK into the caller's struct. Redirecting those to a
  // private copy broke the write-back - the guest read its never-filled original
  // buffer, so GetDeviceData returned 0 free space and the title showed
  // "insufficient space" and hung loading the profile forever (measured the
  // session this shipped). CreateEx's struct is pure input (the result is the
  // out handle), so only it is safe to stabilize.
  const int data_reg = kTargets[I].data_reg;
  if (ContentFixEnabled() && I == 0 && data_reg >= 0) {
    uint32_t* arg = nullptr;
    switch (data_reg) {
      case 1: arg = &ctx.r4.u32; break;  // r4
      case 2: arg = &ctx.r5.u32; break;  // r5
      default: break;
    }
    if (arg && *arg && Readable(base + *arg, kContentDataSize)) {
      const uint32_t src = *arg;
      uint32_t stable = 0;
      if (auto* rt = rex::Runtime::instance(); rt && rt->memory()) {
        stable = rt->memory()->SystemHeapAlloc(static_cast<uint32_t>(kContentDataSize),
                                               0x20);
      }
      if (stable && Readable(base + stable, kContentDataSize)) {
        std::memcpy(base + stable, base + src, kContentDataSize);
        *arg = stable;
        static std::atomic<int> fix_budget{64};
        if (fix_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
          REXLOG_WARN("CONTENTFIX {} {:08x}->{:08x} (lr={:08x}) '{}'",
                      kTargets[I].export_name, src, stable,
                      static_cast<uint32_t>(ctx.lr),
                      reinterpret_cast<const char*>(base + stable + kFileNameOffset));
        }
      } else {
        REXLOG_WARN("CONTENTFIX {} SystemHeapAlloc failed, arg left as {:08x}",
                    kTargets[I].export_name, src);
      }
    }
  }
  LogArgs(kTargets[I].export_name, data_reg, ctx, base);
  const uint32_t creator_out = ctx.r5.u32, xuid_out = ctx.r6.u32;
  const uint32_t delete_data = ctx.r4.u32;
  bool deleting_checkpoints = false;
  if (I == 1 && delete_data && Readable(base + delete_data, kContentDataSize)) {
    const char* name =
        reinterpret_cast<const char*>(base + delete_data + kFileNameOffset);
    deleting_checkpoints = strnlen(name, kFileNameSize) == 15 &&
                           std::memcmp(name, "GearsCheckpoint", 15) == 0;
  }
  // XamContentClose(root_name, XOVERLAPPED*). The runtime completes an overlapped
  // close through a deferred worker that sleeps ~100 ms first. The title's slot
  // scan closes SG0_0 and immediately re-opens SG0_0 for the next slot, so the
  // late close tore down the NEW open and the title's following synchronous
  // close failed with ERROR_FILE_NOT_FOUND (measured: "XamContentClose ->
  // 00000002" right after every absent slot) - which the read task records as a
  // corrupted slot. Close synchronously instead and publish the completion in
  // the XOVERLAPPED the title polls (hEvent is 0 in every measured call).
  //
  // Measured further (07:25): the runtime's close fails with ERROR_FILE_NOT_FOUND
  // on EVERY call, synchronous or overlapped, even right after a successful read
  // of the package - it does not track the content opened by the deferred
  // create. On hardware closing an open package cannot fail. A slot whose data
  // parsed survives the failed close, but an empty slot has nothing else to go
  // on and is marked corrupted. So: close synchronously and report success.
  if (I == 7 && ContentFixEnabled()) {
    const uint32_t ovl = ctx.r4.u32;
    uint32_t hevent = 0;
    if (ovl && Readable(base + ovl, 20)) std::memcpy(&hevent, base + ovl + 12, 4);
    if (hevent == 0) {
      ctx.r4.u64 = 0;
      kTargets[I].body(ctx, base);
      const uint32_t err = ctx.r3.u32;
      if (ovl && Readable(base + ovl, 20)) {
        const uint32_t zero = 0;
        std::memcpy(base + ovl + 4, &zero, 4);
        std::memcpy(base + ovl, &zero, 4);  // InternalLow = ERROR_SUCCESS
        ctx.r3.u64 = 997;                    // ERROR_IO_PENDING: completion is in ovl
      } else {
        ctx.r3.u64 = 0;
      }
      static std::atomic<int> sync_budget{32};
      if (sync_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        REXLOG_WARN("CONTENTFIX XamContentClose runtime={:08x} -> success (ovl={:08x})",
                    err, ovl);
      }
      return;
    }
  }
  if (I == 1) {
    // Guest back-chain: 0(r1) is the caller's frame, and each prologue stores LR
    // at -8 of the frame it is about to leave, i.e. at (back - 8).
    std::string chain;
    uint32_t sp = ctx.r1.u32;
    for (int depth = 0; depth < 14 && sp && Readable(base + sp, 4); ++depth) {
      uint32_t back;
      std::memcpy(&back, base + sp, 4);
      back = __builtin_bswap32(back);
      if (!back || back <= sp || !Readable(base + back - 8, 4)) break;
      uint32_t lr;
      std::memcpy(&lr, base + back - 8, 4);
      fmt::format_to(std::back_inserter(chain), " {:08x}", __builtin_bswap32(lr));
      sp = back;
    }
    REXLOG_WARN("CONTENTPROBE XamContentDelete guest stack lr={:08x}:{}",
                static_cast<uint32_t>(ctx.lr), chain);
  }
  kTargets[I].body(ctx, base);
  // XamContentDelete of the checkpoint package is the title's "overwrite
  // corrupted saves" action. The runtime answers ERROR_ACCESS_DENIED every time
  // (measured in every session log), so the corrupted state could never be
  // cleared. Do what the console does - remove the package's files - but move
  // them into a backup folder rather than destroying them.
  // DISABLED: measured 06:09, the title calls this on its own ~1 s
  // after every boot slot scan, not on the player's Overwrite choice, so
  // honouring it removed the live checkpoint every boot.
  if (false && deleting_checkpoints && ctx.r3.u32 != 0) {
    std::filesystem::path dir;
    {
      std::lock_guard lock(g_mount_mu);
      dir = g_checkpoint_dir;
    }
    std::error_code ec;
    if (!dir.empty()) {
      const auto backup = dir.parent_path() /
                          ("_deleted_" + std::to_string(GetTickCount64()));
      size_t moved = 0;
      for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
           it.increment(ec)) {
        std::error_code fec;
        std::filesystem::create_directories(backup, fec);
        std::filesystem::rename(it->path(), backup / it->path().filename(), fec);
        moved += !fec;
      }
      REXLOG_WARN("CONTENTFIX XamContentDelete GearsCheckpoint: runtime={:08x}, moved {} "
                  "file(s) to {} -> success",
                  ctx.r3.u32, moved, backup.string());
      ctx.r3.u64 = 0;
    }
  }
  if (I == 2 && creator_out && Readable(base + creator_out, 4)) {
    uint32_t is_creator = 0;
    std::memcpy(&is_creator, base + creator_out, 4);
    uint64_t xuid = 0;
    if (xuid_out && Readable(base + xuid_out, 8)) std::memcpy(&xuid, base + xuid_out, 8);
    static std::atomic<int> creator_budget{64};
    if (creator_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_WARN("CONTENTPROBE GetCreator result is_creator={:08x} xuid={:016x}",
                  __builtin_bswap32(is_creator), __builtin_bswap64(xuid));
    }
  }
  // The X_STATUS the guest received (r3 after the call). "Saves corrupted /
  // belongs to another user" is the guest reacting to a content call that failed
  // ownership or header validation, and the only host-side evidence of which
  // call and which status is this return code - read-only, so it cannot change
  // behaviour. GetCreator returning a mismatched/zero XUID is the leading
  // hypothesis; this line proves or kills it.
  static std::atomic<int> ret_budget{64};
  if (ret_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
    REXLOG_WARN("CONTENTPROBE {} -> status={:08x}", kTargets[I].export_name,
                ctx.r3.u32);
  }
}

}  // namespace

namespace rex::glue {

void SetCheckpointDir(const std::filesystem::path& dir) {
  std::lock_guard lock(g_mount_mu);
  g_checkpoint_dir = dir;
}

bool CheckpointRootReadOnly(const std::string& root) {
  std::lock_guard lock(g_mount_mu);
  auto it = g_root_flags.find(root);
  // XCONTENTFLAG_OPENEXISTING = 3 in the low nibble: the package is opened for
  // reading, and on hardware nothing can be created inside it.
  return it != g_root_flags.end() && (it->second & 0xF) == 3;
}

std::filesystem::path CheckpointDir() {
  std::lock_guard lock(g_mount_mu);
  return g_checkpoint_dir;
}

void NoteMountedRoot(const std::string& root) {
  std::lock_guard lock(g_mount_mu);
  g_mounted.insert(root);
}

}  // namespace rex::glue

namespace {

PPCFunc* const* Wrappers() {
  static PPCFunc* const w[kMaxTargets] = {
      &ProbeWrap<0>, &ProbeWrap<1>, &ProbeWrap<2>, &ProbeWrap<3>,
      &ProbeWrap<4>, &ProbeWrap<5>, &ProbeWrap<6>, &ProbeWrap<7>,
  };
  return w;
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

void InstallContentProbe() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;

  char gate[8]{};
  if (GetEnvironmentVariableA("GOWJ_CONTENT_PROBE", gate, sizeof(gate)) &&
      std::strcmp(gate, "0") == 0) {
    REXLOG_INFO("CONTENTPROBE disabled via GOWJ_CONTENT_PROBE=0");
    return;
  }
  QueryPerformanceFrequency(&g_qpc_freq);

  HMODULE mod = GetModuleHandleA("rexruntime.dll");
  if (!mod) {
    REXLOG_WARN("CONTENTPROBE rexruntime.dll not loaded, shim not installed");
    return;
  }

  size_t hits = 0;
  for (size_t i = 0; i < kMaxTargets; ++i) {
    uint8_t* thunk =
        reinterpret_cast<uint8_t*>(GetProcAddress(mod, kTargets[i].export_name));
    if (!thunk) {
      REXLOG_WARN("CONTENTPROBE {} not exported", kTargets[i].export_name);
      continue;
    }
    PPCFunc* body = ResolveThunkBody(thunk);
    if (!body) {
      REXLOG_WARN("CONTENTPROBE {} thunk {:#x} is not a jmp thunk", kTargets[i].export_name,
                  reinterpret_cast<uintptr_t>(thunk));
      continue;
    }
    kTargets[i].body = body;

    // FF 25 00000000 imm64 -> jmp qword ptr [rip+0]; 14 bytes in a 16 byte slot.
    DWORD old = 0;
    if (!VirtualProtect(thunk, kThunkSize, PAGE_EXECUTE_READWRITE, &old)) {
      REXLOG_WARN("CONTENTPROBE {} unprotect failed (gle={})", kTargets[i].export_name,
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
    REXLOG_INFO("CONTENTPROBE {} thunk {:#x} -> wrapper {:#x} (body {:#x})",
                kTargets[i].export_name, reinterpret_cast<uintptr_t>(thunk),
                reinterpret_cast<uintptr_t>(replacement),
                reinterpret_cast<uintptr_t>(body));
  }
  REXLOG_INFO("CONTENTPROBE hooked {}/{} xam content entry points", hits, kMaxTargets);
}

}  // namespace rex::glue
