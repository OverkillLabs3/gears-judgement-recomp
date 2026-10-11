#include "gowj_crash_watch.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

// Written with plain Win32 file handles, never through the logger: the thread
// that dies in a guest AV frequently owns the logging mutex (measured
// 09:25, where the log stops exactly at the last line before
// the exit code), so an REXLOG call from here can deadlock and lose the report.
constexpr char kLogName[] = "gowj_crash.log";

void AppendLog(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  // Next to the program the player runs (GOWJ_HOME, set by the single-file launcher), else
  // next to this exe.
  char path[MAX_PATH];
  const DWORD room = DWORD(sizeof(path) - sizeof(kLogName) - 1);
  DWORD n = GetEnvironmentVariableA("GOWJ_HOME", path, room);
  if (n && n < room) {
    if (path[n - 1] != '\\') {
      path[n++] = '\\';
    }
    memcpy(path + n, kLogName, sizeof(kLogName));
  } else {
    n = GetModuleFileNameA(nullptr, path, sizeof(path) - sizeof(kLogName));
    if (!n) {
      return;
    }
    char* slash = strrchr(path, '\\');
    if (!slash) {
      return;
    }
    memcpy(slash + 1, kLogName, sizeof(kLogName));
  }

  HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  SetFilePointer(h, 0, nullptr, FILE_END);
  DWORD written;
  WriteFile(h, buf, static_cast<DWORD>(strlen(buf)), &written, nullptr);
  CloseHandle(h);
}

uintptr_t g_image_base = 0;
uintptr_t g_image_end = 0;

// PPCFuncMappings is codegen's own { guest address, host entry point } table,
// terminated by { 0, nullptr } and sorted by GUEST address. Host addresses are
// not sorted, so attributing a RIP means a linear scan - fine for a crash path,
// and it is the only mapping available: the shipped exe carries no symbol table.
struct GuestFrame {
  uint32_t guest = 0;
  uintptr_t host = 0;
};

bool FindGuest(uintptr_t addr, GuestFrame* out) {
  if (addr < g_image_base || addr >= g_image_end) {
    return false;
  }
  GuestFrame best;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    uintptr_t host = reinterpret_cast<uintptr_t>(m->host);
    if (host <= addr && (!best.host || host > best.host)) {
      best.guest = static_cast<uint32_t>(m->guest);
      best.host = host;
    }
  }
  if (!best.host) {
    return false;
  }
  *out = best;
  return true;
}

bool Readable(const void* p, size_t len) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
    return false;
  }
  DWORD prot = mbi.Protect & 0xFF;
  if (prot == PAGE_NOACCESS || prot == PAGE_GUARD) {
    return false;
  }
  return reinterpret_cast<uintptr_t>(p) + len <=
         reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// A small minidump (stacks, registers, loaded modules; no 4 GB guest heap) beside the text report,
// so a crash that only happens once in a few dozen launches can still be analysed afterwards.
// dbghelp is loaded on demand; the five newest dumps are kept.
void WriteMiniDump(EXCEPTION_POINTERS* ep) {
  char dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("GOWJ_HOME", dir, DWORD(sizeof(dir) - 32));
  if (!n || n >= sizeof(dir) - 32) {
    n = GetModuleFileNameA(nullptr, dir, DWORD(sizeof(dir) - 32));
    while (n && dir[n - 1] != '\\' && dir[n - 1] != '/') --n;
    if (!n) return;
    dir[n] = 0;
  } else if (dir[n - 1] != '\\') {
    dir[n++] = '\\';
    dir[n] = 0;
  }
  char crash_dir[MAX_PATH];
  _snprintf(crash_dir, sizeof(crash_dir), "%scrash", dir);
  CreateDirectoryA(crash_dir, nullptr);

  // keep only the newest four before writing a fifth
  {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s\\gowj_*.dmp", crash_dir);
    struct Item { FILETIME t; char name[MAX_PATH]; };
    Item items[64];
    int count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (count < 64) { items[count].t = fd.ftLastWriteTime; _snprintf(items[count].name, MAX_PATH, "%s\\%s", crash_dir, fd.cFileName); ++count; }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
    while (count >= 5) {
      int oldest = 0;
      for (int i = 1; i < count; ++i)
        if (CompareFileTime(&items[i].t, &items[oldest].t) < 0) oldest = i;
      DeleteFileA(items[oldest].name);
      items[oldest] = items[--count];
    }
  }

  SYSTEMTIME st;
  GetLocalTime(&st);
  char path[MAX_PATH];
  _snprintf(path, sizeof(path), "%s\\gowj_%04d%02d%02d_%02d%02d%02d.dmp", crash_dir, st.wYear, st.wMonth,
            st.wDay, st.wHour, st.wMinute, st.wSecond);
  HMODULE dbg = LoadLibraryA("dbghelp.dll");
  if (!dbg) return;
  struct Info { DWORD tid; EXCEPTION_POINTERS* ep; BOOL client; };
  using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD, void*, void*, void*);
  auto write = reinterpret_cast<Fn>(GetProcAddress(dbg, "MiniDumpWriteDump"));
  if (!write) return;
  // The guest's 4 GB reservation and its custom thread stacks make some dump flag sets fail with
  // ERROR_NOACCESS (0x800703E6), so smaller sets are tried in turn until one is written.
  // 0x400 MiniDumpWithoutOptionalData, 0x1000 WithThreadInfo, 0x0 Normal.
  const DWORD kTypes[] = {0x0 | 0x1000, 0x0, 0x400, 0x400 | 0x1000};
  for (DWORD type : kTypes) {
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    Info info{GetCurrentThreadId(), ep, FALSE};
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &info, nullptr, nullptr);
    const DWORD err = ok ? 0 : GetLastError();
    CloseHandle(f);
    AppendLog("  minidump type 0x%lx %s: %s (error 0x%lx)\n", type, ok ? "written" : "FAILED", path, err);
    if (ok) return;
  }
  DeleteFileA(path);
}


// ---------------------------------------------------------------------------------------------
// Overread guard. The console's memory is flat: code that reads a few bytes past the end of a
// buffer (an optimised strlen or memchr, a sprintf %s on a string at the end of a heap block) just
// reads whatever the next page holds. Here the guest lives in a 4 GB reservation that is committed
// per allocation, so the same overread lands on an uncommitted page and the process dies with an
// access violation (measured: a one-byte read at guest 0xBF5D0000, exactly one past the end of the
// region ending at 0xBF5CFFFF, in about one launch in six). The next page is committed as zeros and
// the read retried, which is what the console would have returned. Only READS of RESERVED pages are
// handled; the first 64 KB (null pointers), writes and everything the runtime protects on purpose
// are left alone, so real bugs still crash.
// ---------------------------------------------------------------------------------------------
uintptr_t g_guest_base = 0;
volatile LONG g_overreads = 0;

LONG CALLBACK OverreadHandler(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 ||
      rec->ExceptionInformation[0] != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (!g_guest_base) {
    auto* rt = rex::Runtime::instance();
    auto* mem = rt ? rt->memory() : nullptr;
    if (mem) g_guest_base = reinterpret_cast<uintptr_t>(mem->virtual_membase());
    if (!g_guest_base) return EXCEPTION_CONTINUE_SEARCH;
  }
  const uintptr_t addr = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
  if (addr < g_guest_base + 0x10000 || addr >= g_guest_base + 0x100000000ull) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
      mbi.Protect == PAGE_NOACCESS && (addr & 0xFFFF) < 0x40 && addr == reinterpret_cast<uintptr_t>(mbi.BaseAddress)) {
    // A read just past the end of a buffer into the no-access page that follows it (measured:
    // guest 0xBF5D0000, a 64 KB no-access mapping directly after a writable one, read at its first
    // byte by a string routine in the Bink movie path). Only the first page of such a region is
    // opened for reading, and only when the page before it is writable data.
    MEMORY_BASIC_INFORMATION prev{};
    if (VirtualQuery(reinterpret_cast<void*>(addr - 1), &prev, sizeof(prev)) && prev.State == MEM_COMMIT &&
        (prev.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE))) {
      DWORD old_protect = 0;
      void* page = mbi.BaseAddress;
      const SIZE_T len = mbi.RegionSize < 0x10000 ? mbi.RegionSize : 0x10000;
      if (VirtualProtect(page, len, PAGE_READONLY, &old_protect)) {
        const LONG n = InterlockedIncrement(&g_overreads);
        if (n <= 20) {
          AppendLog("overread guard #%ld: read of guest 0x%08llX opened (no-access page after a buffer), rip=%p\n", n,
                    static_cast<unsigned long long>(addr - g_guest_base), reinterpret_cast<void*>(ep->ContextRecord->Rip));
        }
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
  }
  mbi = MEMORY_BASIC_INFORMATION{};
  if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_RESERVE) {
    static LONG logged = 0;
    if (InterlockedIncrement(&logged) <= 6) {
      AppendLog("overread guard: not handled, guest 0x%08llX state=0x%lx protect=0x%lx alloc_protect=0x%lx type=0x%lx region=%p size=0x%llx\n",
                static_cast<unsigned long long>(addr - g_guest_base), mbi.State, mbi.Protect, mbi.AllocationProtect,
                mbi.Type, mbi.BaseAddress, static_cast<unsigned long long>(mbi.RegionSize));
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }
  void* page = reinterpret_cast<void*>(addr & ~static_cast<uintptr_t>(0xFFF));
  if (!VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const LONG n = InterlockedIncrement(&g_overreads);
  if (n <= 20) {
    AppendLog("overread guard #%ld: read of guest 0x%08llX (page committed as zeros), rip=%p\n", n,
              static_cast<unsigned long long>(addr - g_guest_base), reinterpret_cast<void*>(ep->ContextRecord->Rip));
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
  const CONTEXT* ctx = ep->ContextRecord;
  AppendLog("=== gowj crash watch: code=%08lx addr=%p rip=%p rsp=%p ===\n",
            rec->ExceptionCode, rec->ExceptionAddress,
            reinterpret_cast<void*>(ctx->Rip), reinterpret_cast<void*>(ctx->Rsp));

  AppendLog("  exception info: kind=%llu address=%p\n",
            rec->NumberParameters > 0 ? static_cast<unsigned long long>(rec->ExceptionInformation[0]) : 0ull,
            rec->NumberParameters > 1 ? reinterpret_cast<void*>(rec->ExceptionInformation[1]) : nullptr);
  AppendLog("  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx r8=%llx r9=%llx\n", ctx->Rax, ctx->Rbx,
            ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->R8, ctx->R9);
  AppendLog("  r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n", ctx->R10, ctx->R11, ctx->R12, ctx->R13,
            ctx->R14, ctx->R15);
  if (Readable(reinterpret_cast<const void*>(ctx->Rip), 16)) {
    const unsigned char* c = reinterpret_cast<const unsigned char*>(ctx->Rip);
    AppendLog("  code at rip: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", c[0], c[1], c[2], c[3],
              c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11]);
  } else {
    AppendLog("  rip is not readable (a jump or call to a bad address)\n");
  }

  GuestFrame f;
  if (FindGuest(reinterpret_cast<uintptr_t>(ctx->Rip), &f)) {
    AppendLog("  RIP in guest sub_%08X (host %p + 0x%llX)\n", f.guest,
              reinterpret_cast<void*>(f.host),
              static_cast<unsigned long long>(ctx->Rip - f.host));
  } else {
    // Name the owning module: "not inside any recompiled function" alone cannot
    // tell a rexruntime guest-memory helper (bad guest pointer) from ntdll
    // memcpy or a GPU driver, and those imply completely different fixes.
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(ctx->Rip), &mod)) {
      char name[MAX_PATH] = {0};
      GetModuleFileNameA(mod, name, sizeof(name) - 1);
      AppendLog("  RIP %p in module %s (+0x%llX)\n",
                reinterpret_cast<void*>(ctx->Rip), name,
                static_cast<unsigned long long>(
                    ctx->Rip - reinterpret_cast<uintptr_t>(mod)));
    } else {
      AppendLog("  RIP %p not inside any recompiled function or loaded module\n",
                reinterpret_cast<void*>(ctx->Rip));
    }
  }

  // Translate-time return addresses: the recompiled caller's entry point is
  // still on the host stack, so scanning it reconstructs the guest call chain
  // that led here - which is what distinguishes "this function is broken" from
  // "its caller passed it garbage".
  size_t reported = 0;
  uint32_t last_guest = 0;
  for (uintptr_t sp = ctx->Rsp; sp < ctx->Rsp + 0x20000; sp += 8) {
    if (!Readable(reinterpret_cast<const void*>(sp), 8)) {
      break;
    }
    uintptr_t value = *reinterpret_cast<const uintptr_t*>(sp);
    GuestFrame frame;
    if (!FindGuest(value, &frame) || frame.host == ctx->Rip) {
      continue;
    }
    if (frame.guest != last_guest) {
      last_guest = frame.guest;
      AppendLog("  chain[%zu] sub_%08X (host %p + 0x%llX)\n", reported,
                frame.guest, reinterpret_cast<void*>(frame.host),
                static_cast<unsigned long long>(value - frame.host));
      if (++reported >= 24) {
        break;
      }
    }
  }
  WriteMiniDump(ep);
  AppendLog("=== end of crash report ===\n");
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

namespace rex::glue {

void InstallCrashWatch() {
  HMODULE self = GetModuleHandleA(nullptr);
  g_image_base = reinterpret_cast<uintptr_t>(self);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
      reinterpret_cast<uint8_t*>(self) +
      reinterpret_cast<IMAGE_DOS_HEADER*>(self)->e_lfanew);
  g_image_end = g_image_base + nt->OptionalHeader.SizeOfImage;
  SetUnhandledExceptionFilter(UnhandledFilter);
  if (char g[4]; !(GetEnvironmentVariableA("GOWJ_OVERREAD_GUARD", g, sizeof g) && g[0] == '0')) {
    AddVectoredExceptionHandler(1, OverreadHandler);
  }
#ifdef GOWJ_DEV_TOOLS
  if (char t[16]; GetEnvironmentVariableA("GOWJ_OVERREAD_TEST", t, sizeof t)) {
    // Test rigs: after N seconds read one byte from a reserved-but-uncommitted guest page, the way
    // an overreading guest function does. With the guard this must be survived and logged.
    static int osecs = std::atoi(t);
    CreateThread(nullptr, 0, [](void*) -> DWORD {
      Sleep(DWORD(osecs) * 1000);
      auto* rt = rex::Runtime::instance();
      const uintptr_t base = rt && rt->memory() ? reinterpret_cast<uintptr_t>(rt->memory()->virtual_membase()) : 0;
      uintptr_t p = base + 0x20000000ull;
      MEMORY_BASIC_INFORMATION mbi{};
      while (base && p < base + 0xE0000000ull && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_RESERVE) {
          volatile unsigned char v = *reinterpret_cast<volatile unsigned char*>(p);
          (void)v;
          AppendLog("overread test: read of guest 0x%08llX survived\n", static_cast<unsigned long long>(p - base));
          return 0;
        }
        p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      }
      AppendLog("overread test: no reserved page found\n");
      return 0;
    }, nullptr, 0, nullptr);
  }
  if (char t[16]; GetEnvironmentVariableA("GOWJ_CRASH_TEST", t, sizeof t)) {
    // Test rigs: crash on purpose after N seconds to check that the report and minidump are written.
    static int secs = std::atoi(t);
    CreateThread(nullptr, 0, [](void*) -> DWORD {
      Sleep(DWORD(secs) * 1000);
      *reinterpret_cast<volatile int*>(8) = 1;
      return 0;
    }, nullptr, 0, nullptr);
  }
#endif
  AppendLog("crash watch installed, image [%p - %p)\n",
            reinterpret_cast<void*>(g_image_base),
            reinterpret_cast<void*>(g_image_end));
}

}  // namespace rex::glue
