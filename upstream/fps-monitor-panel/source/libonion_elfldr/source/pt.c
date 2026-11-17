/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <onion/log.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include <onion/pt.h>


/*
 * Private stack window for PT_SETREGS "calls".
 *
 * Hijacking a live ShellUI thread mid-frame (often SwapBuffers / Mono JIT)
 * must not reuse that frame's RSP:
 *   - x86-64 SysV red zone is [rsp-128, rsp); callee pushes would corrupt it
 *   - ABI requires rsp % 16 == 8 at function entry (as if a CALL just ran)
 *
 * Subtract 0x108 from the 16-byte-aligned interrupted rsp:
 *   0x108 > 128 (clears red zone) and is 8 mod 16 (correct entry alignment).
 * bak_reg is always restored after the remote work completes.
 */
static uintptr_t
pt_private_entry_rsp(uintptr_t interrupted_rsp) {
  return (interrupted_rsp & ~(uintptr_t)0xf) - (uintptr_t)0x108;
}

/*
 * A tracee may only be detached after its interrupted register context has
 * been restored.  Keep this state local to the single-target injector.
 */
static pid_t g_traced_pid = -1;
static int g_tracee_safe_to_resume = 1;

/* If waitpid did not establish the stop state, GETREGS is our stop-state probe. */
static int
pt_restore_saved_regs(pid_t pid, const struct reg *saved, int known_stopped) {
  struct reg probe;

  if (!known_stopped && pt_getregs(pid, &probe) != 0) {
    return -1;
  }
  if (pt_setregs(pid, saved) != 0) {
    return -1;
  }

  g_tracee_safe_to_resume = 1;
  return 0;
}


static int
sys_ptrace(int request, pid_t pid, caddr_t addr, int data) {
  /*
   * Do NOT flip ucred authid around every ptrace syscall.
   *
   * Caller elevates once with set_ucred_to_ptrace() (PTRACE_AUTHID) for the
   * inject window, then restores. No per-call authid flip here.
   */
  return (int)syscall(SYS_ptrace, request, pid, addr, data);
}


intptr_t
pt_resolve(pid_t pid, const char* nid) {
  intptr_t addr;

  if((addr=kernel_dynlib_resolve(pid, 0x1, nid))) {
    return addr;
  }

  return kernel_dynlib_resolve(pid, 0x2001, nid);
}


int
pt_attach(pid_t pid) {
  int status = 0;

  if (g_traced_pid != -1) {
    errno = EBUSY;
    return -1;
  }

  if (sys_ptrace(PT_ATTACH, pid, 0, 0) == -1) {
    return -1;
  }

  /*
   * Record ownership immediately after PT_ATTACH succeeds, not after
   * waitpid(). If waiting fails, the trace relationship may still exist.
   * No registers have been changed yet, so a detach preserves the original
   * execution context.
   */
  g_traced_pid = pid;
  g_tracee_safe_to_resume = 1;

  pid_t waited;
  do {
    waited = waitpid(pid, &status, WUNTRACED);
  } while (waited == -1 && errno == EINTR);

  if (waited == -1) {
    const int wait_errno = errno;
    if (pt_detach(pid, 0) != 0) {
      LOG_ERROR("pt_attach: waitpid failed and cleanup detach failed pid=%d "
                "wait_errno=%d detach_errno=%d",
                (int)pid, wait_errno, errno);
    }
    errno = wait_errno;
    return -1;
  }

  /* PT_ATTACH must leave the traced process stopped before callers touch
   * registers or its address space. */
  if (!WIFSTOPPED(status)) {
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      /* The target exited; the kernel has already released the trace. */
      g_traced_pid = -1;
      g_tracee_safe_to_resume = 1;
    } else if (pt_detach(pid, 0) != 0) {
      LOG_ERROR("pt_attach: unexpected wait status and cleanup detach failed "
                "pid=%d status=0x%x detach_errno=%d",
                (int)pid, status, errno);
    }
    errno = ESRCH;
    return -1;
  }

  return 0;
}

/* True while this helper still owns a ptrace relationship with pid. */
int
pt_is_attached(pid_t pid) {
  return g_traced_pid == pid;
}

int
pt_detach(pid_t pid, int sig) {
  if (g_traced_pid == pid && !g_tracee_safe_to_resume) {
    LOG_ERROR("pt_detach: refusing to resume pid=%d; original registers "
              "could not be restored", (int)pid);
    errno = EBUSY;
    return -1;
  }
  if(sys_ptrace(PT_DETACH, pid, 0, sig) == -1) {
    return -1;
  }

  if (g_traced_pid == pid) {
    g_traced_pid = -1;
    g_tracee_safe_to_resume = 1;
  }
  return 0;
}

/*
 * Return 0 for a normal single-step SIGTRAP, -2 when the tracee is known to
 * remain stopped but the step failed/unexpectedly stopped, and -1 when its
 * execution state could not be established.
 */
int
pt_step(int pid) {
  int status = 0;
  pid_t waited;

  if (sys_ptrace(PT_STEP, pid, (caddr_t)1, 0) == -1) {
    return -2; /* PT_STEP was rejected; the tracee remains stopped. */
  }

  do {
    waited = waitpid(pid, &status, 0);
  } while (waited == -1 && errno == EINTR);

  if (waited == -1) {
    return -1;
  }
  if (!WIFSTOPPED(status)) {
    errno = ESRCH;
    return -1;
  }
  return WSTOPSIG(status) == SIGTRAP ? 0 : -2;
}


int
pt_continue(pid_t pid, int sig) {
  if(sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, sig) == -1) {
    return -1;
  }

  return 0;
}


int
pt_getint(pid_t pid, intptr_t addr) {
  return sys_ptrace(PT_READ_D, pid, (caddr_t)addr, 0);
}


int
pt_setint(pid_t pid, intptr_t addr, int val) {
  return sys_ptrace(PT_WRITE_D, pid, (caddr_t)addr, val);
}


int
pt_getregs(pid_t pid, struct reg *r) {
  return sys_ptrace(PT_GETREGS, pid, (caddr_t)r, 0);
}


int
pt_setregs(pid_t pid, const struct reg *r) {
  return sys_ptrace(PT_SETREGS, pid, (caddr_t)r, 0);
}


int
pt_copyin(pid_t pid, const void* buf, intptr_t addr, size_t len) {
  struct ptrace_io_desc iod = {
    .piod_op = PIOD_WRITE_D,
    .piod_offs = (void*)addr,
    .piod_addr = (void*)buf,
    .piod_len = len};
  return sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
}


int
pt_setchar(pid_t pid, intptr_t addr, char val) {
  return pt_copyin(pid, &val, addr, sizeof(val));
}


int
pt_setshort(pid_t pid, intptr_t addr, short val) {
  return pt_copyin(pid, &val, addr, sizeof(val));
}


int
pt_setlong(pid_t pid, intptr_t addr, long val) {
  return pt_copyin(pid, &val, addr, sizeof(val));
}


int
pt_copyout(pid_t pid, intptr_t addr, void* buf, size_t len) {
  struct ptrace_io_desc iod = {
    .piod_op = PIOD_READ_D,
    .piod_offs = (void*)addr,
    .piod_addr = buf,
    .piod_len = len};
  return sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
}


char
pt_getchar(pid_t pid, intptr_t addr) {
  char val = 0;

  pt_copyout(pid, addr, &val, sizeof(val));

  return val;
}


short
pt_getshort(pid_t pid, intptr_t addr) {
  short val = 0;

  pt_copyout(pid, addr, &val, sizeof(val));

  return val;
}


long
pt_getlong(pid_t pid, intptr_t addr) {
  long val = 0;

  pt_copyout(pid, addr, &val, sizeof(val));

  return val;
}


long
pt_call(pid_t pid, intptr_t addr, uint64_t arg1, uint64_t arg2,
        uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
  struct reg jmp_reg;
  struct reg bak_reg;
  uintptr_t entry_rsp;

  /* Never redirect the tracee to a null or invalid remote function address. */
  if (addr <= 0) {
    errno = EINVAL;
    return -1;
  }

  if(pt_getregs(pid, &bak_reg)) {
    return -1;
  }

  memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));
  jmp_reg.r_rip = addr;
  entry_rsp = pt_private_entry_rsp((uintptr_t)bak_reg.r_rsp);
  jmp_reg.r_rsp = entry_rsp;

  jmp_reg.r_rdi = arg1;
  jmp_reg.r_rsi = arg2;
  jmp_reg.r_rdx = arg3;
  jmp_reg.r_rcx = arg4;
  jmp_reg.r_r8  = arg5;
  jmp_reg.r_r9  = arg6;

  if(pt_setregs(pid, &jmp_reg)) {
    return -1;
  }
  g_tracee_safe_to_resume = 0;

  while((uintptr_t)jmp_reg.r_rsp <= entry_rsp) {
    const int step_rc = pt_step(pid);
    if (step_rc != 0) {
      if (pt_restore_saved_regs(pid, &bak_reg, step_rc == -2) != 0) {
        LOG_ERROR("pt_call: could not restore interrupted registers pid=%d",
                  (int)pid);
      }
      return -1;
    }
    if(pt_getregs(pid, &jmp_reg)) {
      if (pt_restore_saved_regs(pid, &bak_reg, 1) != 0) {
        LOG_ERROR("pt_call: register read/restore failed pid=%d", (int)pid);
      }
      return -1;
    }
  }

  if (pt_restore_saved_regs(pid, &bak_reg, 1) != 0) {
    LOG_ERROR("pt_call: failed to restore interrupted registers pid=%d",
              (int)pid);
    return -1;
  }
  return jmp_reg.r_rax;
}
long
pt_call2(pid_t pid, intptr_t addr, uint64_t arg1, uintptr_t expected_rip)
{
  struct reg jmp_reg;
  struct reg bak_reg;
  struct reg stopped_reg;

  if(pt_getregs(pid, &bak_reg)) {
    return -1;
  }

  memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));
  jmp_reg.r_rip = addr;
  jmp_reg.r_rsp = pt_private_entry_rsp((uintptr_t)bak_reg.r_rsp);
  jmp_reg.r_rdi = arg1;

  if(pt_setregs(pid, &jmp_reg)) {
    return -1;
  }
  g_tracee_safe_to_resume = 0;

  if (pt_continue(pid, 0) != 0) {
    if (pt_restore_saved_regs(pid, &bak_reg, 1) != 0) {
      LOG_ERROR("pt_call2: continue failed and register restore failed pid=%d",
                (int)pid);
    }
    return -1;
  }

  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited == -1 && errno == EINTR);

  if (waited == -1) {
    /* Recover only if GETREGS proves the tracee is stopped. */
    if (pt_restore_saved_regs(pid, &bak_reg, 0) != 0) {
      LOG_ERROR("pt_call2: waitpid failed; tracee state unknown pid=%d errno=%d",
                (int)pid, errno);
    }
    return -1;
  }

  if (!WIFSTOPPED(status)) {
    LOG_ERROR("pt_call2: target exited before stager stop pid=%d status=0x%x",
              (int)pid, status);
    errno = ESRCH;
    return -1;
  }

  const int stop_signal = WSTOPSIG(status);
  if (pt_getregs(pid, &stopped_reg) != 0) {
    LOG_ERROR("pt_call2: could not read stopped registers pid=%d", (int)pid);
    if (pt_restore_saved_regs(pid, &bak_reg, 1) != 0) {
      LOG_ERROR("pt_call2: register restore also failed pid=%d", (int)pid);
    }
    return -1;
  }

  const uintptr_t stopped_rip = (uintptr_t)stopped_reg.r_rip;
  if (stop_signal != SIGTRAP || stopped_rip != expected_rip) {
    LOG_ERROR("pt_call2: unexpected stop pid=%d signal=%d rip=%#lx expected=%#lx",
              (int)pid, stop_signal, (unsigned long)stopped_rip,
              (unsigned long)expected_rip);
    if (pt_restore_saved_regs(pid, &bak_reg, 1) != 0) {
      LOG_ERROR("pt_call2: failed to restore registers after unexpected stop pid=%d",
                (int)pid);
    }
    errno = EPROTO;
    return -1;
  }

  /* Capture the actual stager result before restoring the interrupted context. */
  const long stager_result = (long)stopped_reg.r_rax;
  if (pt_restore_saved_regs(pid, &bak_reg, 1) != 0) {
    LOG_ERROR("pt_call2: failed to restore interrupted registers pid=%d",
              (int)pid);
    return -1;
  }

  return stager_result;
}

long
pt_syscall(pid_t pid, int sysno, uint64_t arg1, uint64_t arg2,
           uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
  intptr_t addr = pt_resolve(pid, "HoLVWNanBBc");
  struct reg jmp_reg;
  struct reg bak_reg;
  uintptr_t entry_rsp;
  if(!addr) {
    return -1;
  }
  addr += 0xa;

  if(pt_getregs(pid, &bak_reg)) {
    return -1;
  }

  memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));
  jmp_reg.r_rip = addr;
  jmp_reg.r_rax = sysno;
  entry_rsp = pt_private_entry_rsp((uintptr_t)bak_reg.r_rsp);
  jmp_reg.r_rsp = entry_rsp;

  jmp_reg.r_rdi = arg1;
  jmp_reg.r_rsi = arg2;
  jmp_reg.r_rdx = arg3;
  jmp_reg.r_r10 = arg4;
  jmp_reg.r_r8  = arg5;
  jmp_reg.r_r9  = arg6;

  if(pt_setregs(pid, &jmp_reg)) {
    return -1;
  }
  g_tracee_safe_to_resume = 0;

  while((uintptr_t)jmp_reg.r_rsp <= entry_rsp) {
    const int step_rc = pt_step(pid);
    if (step_rc != 0) {
      if (pt_restore_saved_regs(pid, &bak_reg, step_rc == -2) != 0) {
        LOG_ERROR("pt_syscall: could not restore interrupted registers pid=%d",
                  (int)pid);
      }
      return -1;
    }
    if(pt_getregs(pid, &jmp_reg)) {
      if (pt_restore_saved_regs(pid, &bak_reg, 1) != 0) {
        LOG_ERROR("pt_syscall: register read/restore failed pid=%d", (int)pid);
      }
      return -1;
    }
  }

  if (pt_restore_saved_regs(pid, &bak_reg, 1) != 0) {
    LOG_ERROR("pt_syscall: failed to restore interrupted registers pid=%d",
              (int)pid);
    return -1;
  }
  return jmp_reg.r_rax;
}

intptr_t
pt_mmap(pid_t pid, intptr_t addr, size_t len, int prot, int flags,
	int fd, off_t off) {
  return pt_syscall(pid, SYS_mmap, addr, len, prot, flags, fd, off);
}


int
pt_msync(pid_t pid, intptr_t addr, size_t len, int flags) {
  return pt_syscall(pid, SYS_msync, addr, len, flags, 0, 0, 0);
}


int
pt_munmap(pid_t pid, intptr_t addr, size_t len) {
  return pt_syscall(pid, SYS_munmap, addr, len, 0, 0, 0, 0);
}


int
pt_mprotect(pid_t pid, intptr_t addr, size_t len, int prot) {
  return pt_syscall(pid, SYS_mprotect, addr, len, prot, 0, 0, 0);
}


int
pt_socket(pid_t pid, int domain, int type, int protocol) {
  return (int)pt_syscall(pid, SYS_socket, domain, type, protocol, 0, 0, 0);
}


int
pt_setsockopt(pid_t pid, int fd, int level, int optname, intptr_t optval,
	      socklen_t optlen) {
  return (int)pt_syscall(pid, SYS_setsockopt, fd, level, optname, optval,
			 optlen, 0);
}


int
pt_close(pid_t pid, int fd) {
  return (int)pt_syscall(pid, SYS_close, fd, 0, 0, 0, 0, 0);
}


int
pt_bind(pid_t pid, int sockfd, intptr_t addr, uint32_t addrlen) {
  return (int)pt_syscall(pid, SYS_bind, sockfd, addr, addrlen, 0, 0, 0);
}


ssize_t
pt_recvmsg(pid_t pid, int fd, intptr_t msg, int flags) {
  return (int)pt_syscall(pid, SYS_recvmsg, fd, msg, flags, 0, 0, 0);
}


int
pt_dup2(pid_t pid, int oldfd, int newfd) {
  return (int)pt_syscall(pid, SYS_dup2, oldfd, newfd, 0, 0, 0, 0);
}


int
pt_rdup(pid_t pid, pid_t other_pid, int fd) {
  return (int)pt_syscall(pid, 0x25b, other_pid, fd, 0, 0, 0, 0);
}


int
pt_pipe(pid_t pid, intptr_t pipefd) {
  intptr_t faddr = pt_resolve(pid, "-Jp7F+pXxNg");
  return (int)pt_call(pid, faddr, pipefd, 0, 0, 0, 0, 0);
}


int
pt_errno(pid_t pid) {
  intptr_t faddr = pt_resolve(pid, "9BcDykPmo1I");
  intptr_t addr = pt_call(pid, faddr, 0, 0, 0, 0, 0, 0);
  return pt_getint(pid, addr);
}


intptr_t
pt_sceKernelGetProcParam(pid_t pid) {
  intptr_t faddr = pt_resolve(pid, "959qrazPIrg");

  return pt_call(pid, faddr, 0, 0, 0, 0, 0, 0);
}
