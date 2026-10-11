// gowj - ReXGlue Recompiled Project
//
// Save-file I/O trace. Every Nt* file call whose path is under a save root
// (SGn_0:, savedriveN:) is logged with its disposition, offsets, byte counts,
// status and the first bytes moved, so "the checkpoint reads back corrupted"
// can be attributed to a write, a read, a truncate or a delete instead of
// guessed. Observation only: nothing in guest memory or ctx is changed.
// GOWJ_SAVE_IO_PROBE=0 skips installation.

#include "gowj_save_io_probe.h"
#include "gowj_content_probe.h"
#include "gowj_checkpoint_probe.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <cstdint>
#include <utility>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ppc.h>

namespace {

enum Api { kCreate, kOpen, kRead, kWrite, kSetInfo, kClose, kQueryInfo, kQueryFull,
           kQueryDir, kWriteGather, kFlush, kSign, kVerify, kOverlapped, kReadProfile,
           kWriteProfile, kCount };

struct Target {
  const char* export_name;
  PPCFunc* body;
};

Target kTargets[kCount] = {
    {"__imp__NtCreateFile", nullptr},          {"__imp__NtOpenFile", nullptr},
    {"__imp__NtReadFile", nullptr},            {"__imp__NtWriteFile", nullptr},
    {"__imp__NtSetInformationFile", nullptr},  {"__imp__NtClose", nullptr},
    {"__imp__NtQueryInformationFile", nullptr}, {"__imp__NtQueryFullAttributesFile", nullptr},
    {"__imp__NtQueryDirectoryFile", nullptr},  {"__imp__NtWriteFileGather", nullptr},
    {"__imp__NtFlushBuffersFile", nullptr},
    {"__imp__XeKeysConsolePrivateKeySign", nullptr},
    {"__imp__XeKeysConsoleSignatureVerification", nullptr},
    {"__imp__XamGetOverlappedResult", nullptr},
    {"__imp__XamUserReadProfileSettings", nullptr},
    {"__imp__XamUserWriteProfileSettings", nullptr},
};

std::mutex g_mu;
std::unordered_map<uint32_t, std::string> g_handles;  // guest handle -> save path
std::unordered_map<uint32_t, uint64_t> g_written;     // guest handle -> write end
std::unordered_map<uint32_t, uint64_t> g_write_hash;  // guest handle -> buffer hash

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL;
  return h;
}

// Hash of the host file as it is on disk right now, first `n` bytes.
uint64_t FileHash(const std::filesystem::path& p, size_t n) {
  HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                         FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return 0;
  std::string buf(n, '\0');
  DWORD got = 0;
  ReadFile(f, buf.data(), static_cast<DWORD>(n), &got, nullptr);
  CloseHandle(f);
  return got == n ? Fnv(reinterpret_cast<const uint8_t*>(buf.data()), n) : 1;
}

// Checkpoint write-back fix. The guest rewrites GearsCheckpointN.sav in place
// (FILE_OPEN_IF, one write at offset 0, close, never an end-of-file set) right
// after XamContentDelete - which on hardware empties the package first and here
// fails with access denied. Measured: a 164747-byte checkpoint written
// over a 183593-byte one left the file at 183593, i.e. 18846 bytes of the older
// checkpoint trailing the new one, which the title reports as corrupted. On
// close, trim the host file to where the guest's writes ended.
void TrimToWritten(const std::string& guest_path, uint64_t end) {
  const size_t sep = guest_path.find(":\\");
  if (sep == std::string::npos || guest_path.find("Checkpoint") == std::string::npos) {
    return;
  }
  const std::filesystem::path dir = rex::glue::CheckpointDir();
  if (dir.empty()) return;
  const std::filesystem::path host = dir / guest_path.substr(sep + 2);
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(host, ec);
  if (ec || size <= end) return;
  std::filesystem::resize_file(host, end, ec);
  REXLOG_WARN("SAVEIO trimmed {} {} -> {} bytes ({})", host.string(), size, end,
              ec ? ec.message() : "ok");
}

bool Readable(const void* p, size_t len) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  const DWORD prot = mbi.Protect & 0xFF;
  if (prot == PAGE_NOACCESS || prot == PAGE_GUARD) return false;
  return reinterpret_cast<uintptr_t>(p) + len <=
         reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

uint32_t Be32(const uint8_t* base, uint32_t addr) {
  uint32_t v = 0;
  if (addr && Readable(base + addr, 4)) std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

uint16_t Be16(const uint8_t* base, uint32_t addr) {
  uint16_t v = 0;
  if (addr && Readable(base + addr, 2)) std::memcpy(&v, base + addr, 2);
  return __builtin_bswap16(v);
}

uint64_t Be64(const uint8_t* base, uint32_t addr) {
  return (uint64_t(Be32(base, addr)) << 32) | Be32(base, addr + 4);
}

// X_OBJECT_ATTRIBUTES { u32 root_directory; u32 name (X_ANSI_STRING*); u32 attributes }
// X_ANSI_STRING { u16 length; u16 maximum_length; u32 buffer }
std::string PathOf(const uint8_t* base, uint32_t obj_attr) {
  const uint32_t name = Be32(base, obj_attr + 4);
  if (!name) return {};
  const uint16_t len = Be16(base, name);
  const uint32_t buf = Be32(base, name + 4);
  if (!buf || !len || len > 512 || !Readable(base + buf, len)) return {};
  return std::string(reinterpret_cast<const char*>(base + buf), len);
}

bool IsSavePath(const std::string& p) {
  if (p.size() >= 2 && p[0] == 'S' && p[1] == 'G') return true;
  if (p.rfind("savedrive", 0) == 0) return true;
  return p.find("Checkpoint") != std::string::npos;
}

std::string Hex(const uint8_t* base, uint32_t addr, uint32_t len) {
  std::string out;
  const uint32_t n = len < 16 ? len : 16;
  if (!addr || !Readable(base + addr, n)) return "?";
  for (uint32_t i = 0; i < n; ++i) fmt::format_to(std::back_inserter(out), "{:02x}", base[addr + i]);
  return out;
}

std::string Tracked(uint32_t handle) {
  std::lock_guard lock(g_mu);
  auto it = g_handles.find(handle);
  return it == g_handles.end() ? std::string() : it->second;
}

template <int I>
void Wrap(PPCContext& ctx, uint8_t* base) {
  rex::glue::InstallCheckpointProbe();
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32,
                 r7 = ctx.r7.u32, r8 = ctx.r8.u32, r9 = ctx.r9.u32, r10 = ctx.r10.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  std::string path;
  if constexpr (I == kCreate || I == kOpen) {
    path = PathOf(base, r5);
  } else if constexpr (I == kQueryFull) {
    path = PathOf(base, r3);
  } else {
    path = Tracked(r3);
  }
  const bool save = !path.empty() && IsSavePath(path);

  if constexpr (I == kCreate || I == kOpen) {
    // A checkpoint package opened OPEN_EXISTING is read-only on hardware, so the
    // title's slot scan (FILE_OPEN_IF on each GearsCheckpointN.sav) reports a
    // missing slot as absent. On a writable host dir the same call created a
    // 0-byte slot that the next read called corrupted.
    const size_t sep = path.find(":\\");
    if (save && sep != std::string::npos && path.find("Checkpoint") != std::string::npos &&
        rex::glue::CheckpointRootReadOnly(path.substr(0, sep + 1))) {
      const std::filesystem::path dir = rex::glue::CheckpointDir();
      std::error_code ec;
      if (!dir.empty() && !std::filesystem::exists(dir / path.substr(sep + 2), ec)) {
        constexpr uint32_t kNotFound = 0xC0000034;  // STATUS_OBJECT_NAME_NOT_FOUND
        ctx.r3.u64 = kNotFound;
        REXLOG_WARN("SAVEIO absent slot '{}' in read-only package -> {:08x} lr={:08x}",
                    path, kNotFound, lr);
        return;
      }
    }
  }

  uint64_t pre_hash = 0;
  if constexpr (I == kWrite) {
    if (save && r9 && Readable(base + r8, r9)) pre_hash = Fnv(base + r8, r9);
  }
  kTargets[I].body(ctx, base);

  if constexpr (I == kWriteProfile) {
    // XamUserWriteProfileSettings(title_id, user_index, count, XUSER_PROFILE_SETTING*,
    // overlapped). Setting: +0x10 id, +0x18 data type byte, +0x20 cb, +0x24 ptr.
    std::string ids;
    for (uint32_t i = 0; i < r5 && i < 16; ++i) {
      const uint32_t s = r6 + i * 0x28;
      const uint32_t cb = Be32(base, s + 0x20), ptr = Be32(base, s + 0x24);
      fmt::format_to(std::back_inserter(ids), " [{:08x} type={} cb={} data={}]",
                     Be32(base, s + 0x10), base[s + 0x18], cb,
                     base[s + 0x18] == 6 ? Hex(base, ptr, cb) : Hex(base, s + 0x20, 8));
    }
    REXLOG_WARN("SAVEIO WriteProfileSettings title={:08x} user={} n={} ->{:08x} lr={:08x}{}",
                r3, r4, r5, ctx.r3.u32, lr, ids);
    return;
  }
  if constexpr (I == kReadProfile) {
    // XamUserReadProfileSettings(title_id, user_index, num_xuids, xuids, count,
    // setting_ids*, size*, results*, overlapped)
    std::string ids;
    for (uint32_t i = 0; i < r7 && i < 16; ++i) {
      fmt::format_to(std::back_inserter(ids), " {:08x}", Be32(base, r8 + 4 * i));
    }
    // XUSER_READ_PROFILE_SETTING_RESULT { u32 count; u32 settings* } in results*.
    const uint32_t count = r10 ? Be32(base, r10) : 0;
    const uint32_t arr = r10 ? Be32(base, r10 + 4) : 0;
    for (uint32_t i = 0; i < count && i < 16 && arr; ++i) {
      const uint32_t s = arr + i * 0x28;
      const uint32_t cb = Be32(base, s + 0x20), ptr = Be32(base, s + 0x24);
      fmt::format_to(std::back_inserter(ids), " [got {:08x} type={} cb={} data={}]",
                     Be32(base, s + 0x10), base[s + 0x18], cb,
                     base[s + 0x18] == 6 ? Hex(base, ptr, cb) : Hex(base, s + 0x20, 8));
    }
    REXLOG_WARN("SAVEIO ReadProfileSettings title={:08x} user={} n={} size={} ->{:08x} "
                "lr={:08x} ids:{}",
                r3, r4, r7, r9 ? Be32(base, r9) : 0, ctx.r3.u32, lr, ids);
    return;
  }
  if constexpr (I == kOverlapped) {
    // XamGetOverlappedResult(XOVERLAPPED*, u32* result, BOOL wait) -> win32 error.
    // XOVERLAPPED: InternalLow(+0) = win32 status, InternalHigh(+4) = result,
    // InternalContext(+8), hEvent(+12), completion routine(+16).
    const uint32_t ret = ctx.r3.u32;
    if (ret != 996) {  // ERROR_IO_INCOMPLETE: still pending, not interesting
      static std::atomic<int> budget{400};
      if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        REXLOG_WARN("SAVEIO overlapped ovl={:08x} -> ret={} low={:08x} high={:08x} "
                    "result_out={:08x} wait={} lr={:08x}",
                    r3, ret, Be32(base, r3), Be32(base, r3 + 4),
                    r4 ? Be32(base, r4) : 0, r5, lr);
      }
    }
    return;
  }
  if constexpr (I == kSign) {
    // XeKeysConsolePrivateKeySign(hash[20], XE_CONSOLE_SIGNATURE* out) -> BOOL.
    // Measured: the runtime returns FALSE and leaves a zero signature.
    // The title signs every checkpoint on save and re-signs on load to compare,
    // so a failing signer makes every save it ever wrote read as "corrupted".
    // A console's key is fixed, so its signature is a pure function of the hash:
    // derive one deterministically and report success.
    // XE_CONSOLE_SIGNATURE = XE_CONSOLE_CERTIFICATE (0x1A8) + signature[0x80].
    constexpr uint32_t kCertSize = 0x1A8, kSigSize = 0x80;
    const uint32_t before = ctx.r3.u32;
    if (before == 0 && r3 && r4 && Readable(base + r3, 20) &&
        Readable(base + r4, kCertSize + kSigSize)) {
      const uint8_t* hash = base + r3;
      uint8_t* sig = base + r4 + kCertSize;
      uint32_t x = 0x811C9DC5;
      for (uint32_t i = 0; i < kSigSize; ++i) {
        x = (x ^ hash[i % 20]) * 0x01000193u;
        sig[i] = static_cast<uint8_t>(x >> 24);
      }
      ctx.r3.u64 = 1;
    }
    static std::atomic<int> budget{16};
    if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_WARN("SAVEIO XeKeysConsolePrivateKeySign hash={} runtime={:08x} -> {:08x} "
                  "sig={} lr={:08x}",
                  Hex(base, r3, 16), before, ctx.r3.u32, Hex(base, r4 + kCertSize, 16), lr);
    }
    return;
  }
  if constexpr (I == kVerify) {
    // XeKeysConsoleSignatureVerification(hash[20], XE_CONSOLE_SIGNATURE*, s32* result)
    // A title signs its saves with the console key and verifies them on load; a
    // real console always verifies its own signature. A runtime that cannot
    // confirm the signature it just produced makes every save "corrupted".
    const uint32_t ret = ctx.r3.u32;
    const uint32_t cmp = r5 ? Be32(base, r5) : 0xFFFFFFFF;
    REXLOG_WARN("SAVEIO XeKeysConsoleSignatureVerification hash={} sig={} -> {:08x} "
                "result={:08x} lr={:08x}",
                Hex(base, r3, 16), Hex(base, r4, 16), ret, cmp, lr);
    if (ret == 0) {
      ctx.r3.u64 = 1;
      REXLOG_WARN("SAVEIO signature verification forced to success");
    }
    return;
  }
  if (!save) return;
  // A missing file inside an existing directory is STATUS_OBJECT_NAME_NOT_FOUND
  // on the console; the runtime answers STATUS_NO_SUCH_FILE, which the title's
  // slot scan treats as an unreadable (corrupted) slot rather than an empty one.
  if constexpr (I == kCreate || I == kOpen || I == kQueryFull) {
    if (ctx.r3.u32 == 0xC000000F && path.find(":\\") != std::string::npos &&
        path.find('\\', path.find(":\\") + 2) == std::string::npos) {
      ctx.r3.u64 = 0xC0000034;
    }
  }
  const uint32_t status = ctx.r3.u32;

  if constexpr (I == kCreate) {
    // NtCreateFile(handle_out, access, obj_attr, iosb, alloc_size, attrs, share, disposition)
    const uint32_t handle = Be32(base, r3);
    const uint32_t action = Be32(base, r6 + 4);
    if (status == 0 && handle) {
      std::lock_guard lock(g_mu);
      g_handles[handle] = path;
    }
    REXLOG_WARN("SAVEIO create '{}' access={:08x} disp={} share={} -> st={:08x} h={:08x} "
                "action={} lr={:08x}",
                path, r4, r10, r9, status, handle, action, lr);
  } else if constexpr (I == kOpen) {
    const uint32_t handle = Be32(base, r3);
    if (status == 0 && handle) {
      std::lock_guard lock(g_mu);
      g_handles[handle] = path;
    }
    REXLOG_WARN("SAVEIO open '{}' access={:08x} share={} opts={:08x} -> st={:08x} h={:08x} "
                "lr={:08x}",
                path, r4, r7, r8, status, handle, lr);
  } else if constexpr (I == kRead || I == kWrite) {
    // Nt{Read,Write}File(handle, event, apc, apc_ctx, iosb, buffer, length, offset*)
    const uint64_t off = r10 ? Be64(base, r10) : ~0ULL;
    if (I == kWrite && off != ~0ULL && (status == 0 || status == 0x103)) {
      std::lock_guard lock(g_mu);
      uint64_t& end = g_written[r3];
      end = std::max<uint64_t>(end, off + Be32(base, r7 + 4));
      if (off == 0 && pre_hash) g_write_hash[r3] = pre_hash;
    }
    REXLOG_WARN("SAVEIO {} '{}' h={:08x} off={} len={} -> st={:08x} iosb={:08x}/{} "
                "data={} lr={:08x}",
                I == kRead ? "read" : "write", path, r3, static_cast<int64_t>(off), r9,
                status, Be32(base, r7), Be32(base, r7 + 4), Hex(base, r8, r9), lr);
  } else if constexpr (I == kWriteGather) {
    REXLOG_WARN("SAVEIO writegather '{}' h={:08x} len={} -> st={:08x} iosb={:08x}/{} "
                "lr={:08x}",
                path, r3, r9, status, Be32(base, r7), Be32(base, r7 + 4), lr);
  } else if constexpr (I == kSetInfo) {
    // NtSetInformationFile(handle, iosb, info, length, class)
    REXLOG_WARN("SAVEIO setinfo '{}' h={:08x} class={} len={} info={} -> st={:08x} "
                "lr={:08x}",
                path, r3, r7, r6, Hex(base, r5, r6), status, lr);
  } else if constexpr (I == kQueryInfo) {
    REXLOG_WARN("SAVEIO queryinfo '{}' h={:08x} class={} -> st={:08x} info={} lr={:08x}",
                path, r3, r7, status, Hex(base, r5, r6), lr);
  } else if constexpr (I == kQueryFull) {
    // NtQueryFullAttributesFile(obj_attr, X_FILE_NETWORK_OPEN_INFORMATION*): end_of_file @ +0x28
    REXLOG_WARN("SAVEIO queryfull '{}' -> st={:08x} eof={} lr={:08x}", path, status,
                static_cast<int64_t>(Be64(base, r4 + 0x28)), lr);
  } else if constexpr (I == kQueryDir) {
    REXLOG_WARN("SAVEIO querydir '{}' h={:08x} -> st={:08x} lr={:08x}", path, r3, status,
                lr);
  } else if constexpr (I == kFlush) {
    REXLOG_WARN("SAVEIO flush '{}' h={:08x} -> st={:08x}", path, r3, status);
  } else if constexpr (I == kClose) {
    uint64_t written = 0, want_hash = 0;
    bool had_write = false;
    {
      std::lock_guard lock(g_mu);
      g_handles.erase(r3);
      if (auto it = g_written.find(r3); it != g_written.end()) {
        written = it->second;
        had_write = true;
        g_written.erase(it);
      }
      if (auto it = g_write_hash.find(r3); it != g_write_hash.end()) {
        want_hash = it->second;
        g_write_hash.erase(it);
      }
    }
    if (had_write && written > 0) {
      TrimToWritten(path, written);
      const size_t sep = path.find(":\\");
      const std::filesystem::path dir = rex::glue::CheckpointDir();
      if (want_hash && sep != std::string::npos && !dir.empty()) {
        const uint64_t disk = FileHash(dir / path.substr(sep + 2), written);
        REXLOG_WARN("SAVEIO verify '{}' {} bytes: buffer-at-write={:016x} disk-at-close="
                    "{:016x} {}",
                    path, written, want_hash, disk,
                    disk == want_hash ? "MATCH" : "MISMATCH (torn write)");
      }
    }
    REXLOG_WARN("SAVEIO close '{}' h={:08x} -> st={:08x} lr={:08x}", path, r3, status, lr);
  }
  (void)r8;
}

template <size_t... Is>
constexpr auto MakeWrappers(std::index_sequence<Is...>) {
  return std::array<PPCFunc*, sizeof...(Is)>{&Wrap<static_cast<int>(Is)>...};
}

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

void InstallSaveIoProbe() {
  char gate[8]{};
  if (GetEnvironmentVariableA("GOWJ_SAVE_IO_PROBE", gate, sizeof(gate)) &&
      std::strcmp(gate, "0") == 0) {
    return;
  }
  HMODULE mod = GetModuleHandleA("rexruntime.dll");
  if (!mod) return;
  static const auto wrappers = MakeWrappers(std::make_index_sequence<kCount>{});
  size_t hits = 0;
  for (size_t i = 0; i < kCount; ++i) {
    auto* thunk = reinterpret_cast<uint8_t*>(GetProcAddress(mod, kTargets[i].export_name));
    if (!thunk) continue;
    PPCFunc* body = ResolveThunkBody(thunk);
    if (!body) {
      REXLOG_WARN("SAVEIO {} is not a jmp thunk", kTargets[i].export_name);
      continue;
    }
    kTargets[i].body = body;
    DWORD old = 0;
    if (!VirtualProtect(thunk, 16, PAGE_EXECUTE_READWRITE, &old)) continue;
    thunk[0] = 0xFF;
    thunk[1] = 0x25;
    std::memset(thunk + 2, 0, 4);
    void* replacement = reinterpret_cast<void*>(wrappers[i]);
    std::memcpy(thunk + 6, &replacement, 8);
    DWORD tmp = 0;
    VirtualProtect(thunk, 16, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), thunk, 16);
    ++hits;
  }
  REXLOG_INFO("SAVEIO hooked {}/{} file entry points", hits, static_cast<size_t>(kCount));
}

}  // namespace rex::glue
