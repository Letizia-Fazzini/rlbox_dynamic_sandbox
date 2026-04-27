#pragma once

// Seccomp-bpf syscall filter applied per-worker in the process backend.
//
// Intended call site: worker_main, after the wire job header + payload have
// been read but before do_ffi_call.  The worker is single-threaded by
// construction at that point (no TSYNC needed), and any setup syscalls the
// shim itself needs have already happened.
//
// Modes (selected at build time via RLBOX_SECCOMP_MODE):
//   0 (Off)     -- no-op.
//   1 (Audit)   -- terminal action is SECCOMP_RET_LOG.  Denied syscalls are
//                  logged via dmesg/audit and *allowed* through, so workloads
//                  run unimpeded while the allowlist is being shaken out.
//                  Requires the kernel to have "log" enabled in
//                  /proc/sys/kernel/seccomp/actions_logged.
//   2 (Enforce) -- terminal action is SECCOMP_RET_KILL_PROCESS.  Denied
//                  syscalls kill the worker via SIGSYS; the host's
//                  dispatch_to_worker path already handles worker death.
//
// The arch gate matches the build's host ABI (AUDIT_ARCH_X86_64 in 64-bit
// builds, AUDIT_ARCH_I386 in -m32 builds) and kills anything else, closing
// the multiarch backdoor that would otherwise bypass an arch-specific
// allowlist.  The allowlist below covers what zlib + libjpeg workers issue
// today, with a small margin for timing/signal/memory paths that future
// workloads may use.  Tightening (mmap-flag filter, futex PI deny, tgkill/
// kill pid restriction) is intentionally deferred.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RLBOX_SECCOMP_MODE
#  define RLBOX_SECCOMP_MODE 0
#endif

namespace rlbox::seccomp {

enum class Mode : int
{
  Off = 0,
  Audit = 1,
  Enforce = 2,
};

inline constexpr Mode compile_mode()
{
  return static_cast<Mode>(RLBOX_SECCOMP_MODE);
}

namespace detail {

inline sock_filter bpf_stmt(unsigned short code, std::uint32_t k)
{
  return sock_filter{ code, 0, 0, k };
}

inline sock_filter bpf_jump(unsigned short code,
                            std::uint32_t k,
                            std::uint8_t jt,
                            std::uint8_t jf)
{
  return sock_filter{ code, jt, jf, k };
}

inline bool install_filter(std::uint32_t terminal_action)
{
  static constexpr std::size_t k_max = 96;
  sock_filter prog[k_max];
  std::size_t n = 0;
  auto push = [&](sock_filter f) {
    if (n < k_max) {
      prog[n++] = f;
    }
  };

  // (1) Arch gate: kill if the running ABI doesn't match what we built for.
  //     Closes the multiarch entry-point backdoor that would bypass an
  //     allowlist whose syscall numbers belong to a different ABI.
#if defined(__x86_64__)
  constexpr std::uint32_t k_expected_arch = AUDIT_ARCH_X86_64;
#elif defined(__i386__)
  constexpr std::uint32_t k_expected_arch = AUDIT_ARCH_I386;
#else
#  error "rlbox seccomp filter: unsupported host architecture"
#endif
  push(bpf_stmt(BPF_LD | BPF_W | BPF_ABS,
                offsetof(struct seccomp_data, arch)));
  push(bpf_jump(BPF_JMP | BPF_JEQ | BPF_K, k_expected_arch, 1, 0));
  push(bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));

  // (2) Load syscall nr; on x86_64 also reject x32 syscalls (top bit set in
  //     __X32_SYSCALL_BIT).  i386 has no x32, so the gate is x86_64-only.
  push(bpf_stmt(BPF_LD | BPF_W | BPF_ABS,
                offsetof(struct seccomp_data, nr)));
#if defined(__x86_64__)
  push(bpf_jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
  push(bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
#endif

  auto allow_eq = [&](unsigned nr) {
    push(bpf_jump(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
    push(bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  };

  // (3) Allowlist.  Empirically zlib + libjpeg workers issue only `write`
  //     and `exit_group` after the filter point; the rest of this list is
  //     headroom for memory/signal/timing syscalls future workloads may need.
  allow_eq(SYS_read);
  allow_eq(SYS_write);
  allow_eq(SYS_writev);
  allow_eq(SYS_close);
  allow_eq(SYS_mmap);
  allow_eq(SYS_mprotect);
  allow_eq(SYS_munmap);
  allow_eq(SYS_madvise);
  allow_eq(SYS_brk);
  allow_eq(SYS_futex);
  allow_eq(SYS_rt_sigaction);
  allow_eq(SYS_rt_sigprocmask);
  allow_eq(SYS_rt_sigreturn);
  allow_eq(SYS_sigaltstack);
  allow_eq(SYS_exit);
  allow_eq(SYS_exit_group);
  allow_eq(SYS_getpid);
  allow_eq(SYS_gettid);
  allow_eq(SYS_clock_gettime);
  allow_eq(SYS_clock_nanosleep);
  allow_eq(SYS_nanosleep);
  allow_eq(SYS_getrandom);
  allow_eq(SYS_restart_syscall);
  allow_eq(SYS_sched_yield);

  // (4) Default: terminal action.
  push(bpf_stmt(BPF_RET | BPF_K, terminal_action));

  sock_fprog fprog = {
    static_cast<unsigned short>(n),
    prog,
  };
  if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0u, &fprog) != 0) {
    std::fprintf(stderr,
                 "[rlbox] seccomp install failed: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

}  // namespace detail

// Apply the configured seccomp filter to the calling thread.  Returns true
// on success; on failure the caller should _exit(2) so the host sees the
// worker die rather than running unfiltered code.
//
// Caller must already be in single-thread state for this thread; no TSYNC.
inline bool apply_filter(Mode mode)
{
  if (mode == Mode::Off) {
    return true;
  }
  std::uint32_t terminal = (mode == Mode::Audit)
                             ? SECCOMP_RET_LOG
                             : SECCOMP_RET_KILL_PROCESS;
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    std::fprintf(stderr,
                 "[rlbox] PR_SET_NO_NEW_PRIVS failed: %s\n",
                 std::strerror(errno));
    return false;
  }
  return detail::install_filter(terminal);
}

}  // namespace rlbox::seccomp
