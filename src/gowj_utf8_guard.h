// gowj - ReXGlue Recompiled Project
//
// Non-throwing UTF-8 decoder for rexruntime.
//
// GoJ hands the runtime raw guest bytes for save/profile content names, and
// some of those bytes are not valid UTF-8. Every rex::string helper that looks
// at a guest path decodes through one utf8cpp instantiation,
// utf8::next<std::_String_view_iterator<char>>, which *throws*
// utf8::invalid_utf8 on a bad lead byte, bad continuation byte, overlong form
// or surrogate. The throw has no host-side catcher, so it reaches the thread
// entry, turns into terminate() -> ucrtbase!abort -> __fastfail(7) and the
// process dies with 0xC0000409 and no [FATAL] line.
//
// Measured throw sites :
//   ContentManager::OpenContent -> utf8_hash_fnv1a_case -> next -> throw
//   NtCreateFile -> VirtualFileSystem::OpenFile -> ResolvePath
//                -> ResolveSymbolicLink -> utf8_starts_with_case
//                -> next -> throw
// Both chains die in the same decoder: 94 call sites inside rexruntime go
// through it, so replacing it once removes the whole throwing surface. Patching
// individual helpers instead (an earlier revision of this file replaced
// utf8_hash_fnv1a[_case] and utf8_equal[_case][_z] with byte-wise versions) is
// both more work and less correct - those replacements diverge from upstream
// comparison semantics, and the originals are exact once their decoder cannot
// throw.
//
// The replacement decodes valid UTF-8 identically to utf8cpp. For a malformed
// sequence it returns the lead byte as a one-byte codepoint and advances one
// byte, which keeps every caller's "advance or stop" loop terminating and keeps
// stray bytes in the stream instead of rewriting them. Invalid sequences are
// logged (rate limited) rather than fatalled: a PC port has no business
// aborting because a 2009 title stored a filename in a legacy codepage.

#pragma once

namespace rex::glue {

// Must run before any guest code executes. Idempotent.
void InstallUtf8Guard();

}  // namespace rex::glue
