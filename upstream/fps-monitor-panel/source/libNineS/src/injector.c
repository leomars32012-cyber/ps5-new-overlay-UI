#include <onion/log.h>
#include <errno.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/injector.h"
#include "ps5/klog.h"
#include <ps5/kernel.h>

int attached = false;
intptr_t remote_malloc = 0;
intptr_t remote_pthread_create = 0;
SCEFunctions sce_functions = {0};

/*
 * Scoped PTRACE_AUTHID for the inject_elf() window (not DEBUG_AUTHID).
 * Elevate once via set_ucred_to_ptrace(); restore on every exit. No per-call
 * flip inside sys_ptrace (see libonion_elfldr pt.c).
 */
static int read_self_authid(uint64_t *authid_out)
{
    if (!authid_out) {
        return -1;
    }

    *authid_out = kernel_get_ucred_authid(getpid());
    return (*authid_out != 0) ? 0 : -1;
}

static int write_self_authid(uint64_t authid)
{
    return kernel_set_ucred_authid(getpid(), authid);
}


/*
 * Keep the bootstrap in one executable section. Separate compiler-generated
 * "$1"/"$2" function sections do not guarantee contiguous ordered placement.
 *
 * This x86-64 SysV stub saves r12, aligns the private stack before calling
 * pthread_create, then traps. stager_breakpoint is the expected post-INT3 RIP.
 */
_Static_assert(sizeof(void *) == 8, "PS5 stager requires x86-64 pointers");
_Static_assert(offsetof(SCEFunctions, elf_main) == 8,
               "Unexpected SCEFunctions.elf_main offset");
_Static_assert(offsetof(SCEFunctions, payload_args) == 16,
               "Unexpected SCEFunctions.payload_args offset");
_Static_assert(offsetof(SCEFunctions, pthread_create_ptr) == 24,
               "Unexpected SCEFunctions.pthread_create_ptr offset");

__asm__(
    ".section .stager_shellcode,\"ax\",@progbits\n"
    ".p2align 4\n"
    ".globl stager\n"
    ".type stager,@function\n"
    "stager:\n"
    "pushq %r12\n"
    "movq %rdi, %r12\n"
    "subq $16, %rsp\n"
    "leaq 8(%rsp), %rdi\n"
    "xorq %rsi, %rsi\n"
    "movq 8(%r12), %rdx\n"
    "movq 16(%r12), %rcx\n"
    "callq *24(%r12)\n"
    "int3\n"
    ".globl stager_breakpoint\n"
    "stager_breakpoint:\n"
    "addq $16, %rsp\n"
    "popq %r12\n"
    "retq\n"
    ".globl stager_end\n"
    "stager_end:\n"
    ".size stager, stager_end-stager\n"
    ".previous\n");

extern const unsigned char stager_breakpoint[];
extern const unsigned char stager_end[];

/* Return zero if the linked bootstrap boundaries are invalid. */
uint32_t get_shellcode_size(void)
{
    const uintptr_t start_addr = (uintptr_t)stager;
    const uintptr_t breakpoint_addr = (uintptr_t)stager_breakpoint;
    const uintptr_t end_addr = (uintptr_t)stager_end;

    if (end_addr <= start_addr || breakpoint_addr <= start_addr ||
        breakpoint_addr >= end_addr || end_addr - start_addr > 0x1000) {
        return 0;
    }

    return (uint32_t)(end_addr - start_addr);
}


// Init all remote function pointers needed for injection
//
bool init_remote_function_pointers(pid_t pid)
{
    if (!attached)
    {
        if (pt_attach(pid) < 0)
        {
            LOG_ERROR("Error attaching PID %d! aborting...", pid);
            return false;
        }
    }

    char nid[12] = {0};
    //
    // Injector/loader specifics
    //
    nid_encode("malloc", nid);
    remote_malloc = pt_resolve(pid, nid);
    nid_encode("pthread_create", nid);
    remote_pthread_create = pt_resolve(pid, nid);

    //
    // Shellcode function pointers
    //
    nid_encode("sceKernelDebugOutText", nid);
    sce_functions.sceKernelDebugOutText = (void*) pt_resolve(pid, nid);
    sce_functions.pthread_create_ptr = (void*) remote_pthread_create;

    //
    // A failed resolve returns 0. The stager calls pthread_create_ptr inside
    // the target, so a null there crashes ShellUI: refuse to inject. malloc is
    // only used by load_remote_library/create_remote_thread, which this
    // payload never calls, and the stager does not call sceKernelDebugOutText.
    //
    if (!remote_pthread_create)
    {
        LOG_ERROR("init_remote_function_pointers: pthread_create not found "
                  "in pid=%d; not injecting", (int)pid);
        return false;
    }
    if (!remote_malloc || !sce_functions.sceKernelDebugOutText)
    {
        LOG_WARN("init_remote_function_pointers: optional symbols missing "
                 "pid=%d malloc=%#lx debugout=%#lx", (int)pid,
                 (unsigned long)remote_malloc,
                 (unsigned long)(uintptr_t)sce_functions.sceKernelDebugOutText);
    }
    return true;
}


int inject_elf(struct proc* proc, void* elf)
{
    int status = true;
    uint64_t original_authid = 0;
    int ptrace_authid_changed = 0;
    uint64_t sce_ptr_mem = 0;
    uint64_t bootstrap = 0;
    int release_stager_mappings = 1;
    long stager_result = -1;
    uint64_t shellcode_size = get_shellcode_size();

    LOG_DEBUG("[+] Elevating for ptrace (PTRACE_AUTHID)...[+]");

    if (proc == NULL || elf == NULL)
    {
        LOG_ERROR("[-] inject_elf: invalid args proc=%p elf=%p",
                    (void*)proc, elf);
        status = false;
        goto exit;
    }

    if (shellcode_size == 0)
    {
        LOG_ERROR("[-] Invalid stager boundaries; refusing injection");
        status = false;
        goto exit;
    }

    /*
     * Never attempt ptrace without a recoverable privilege state. If the
     * original Auth-ID cannot be read, abort before changing credentials.
     */
    if (read_self_authid(&original_authid) != 0)
    {
        LOG_ERROR("[-] inject: authid backup failed; refusing injection");
        status = false;
        goto exit;
    }

    ptrace_authid_changed = 1;
    LOG_DEBUG("[+] inject: saved original authid=0x%lx",
              (unsigned long)original_authid);

    /* Scoped PTRACE_AUTHID for attach/load only (not DEBUG_AUTHID). */
    set_ucred_to_ptrace();

    /*
     * Successful PT_ATTACH is the operational check that privilege
     * elevation was sufficient. pt_attach tracks ownership immediately
     * after PT_ATTACH, including its waitpid failure path.
     */
    if (pt_attach(proc->pid) < 0)
    {
        const int attach_errno = errno;
        if (pt_is_attached(proc->pid))
        {
            attached = true;
            if (pt_detach(proc->pid, 0) == 0)
            {
                attached = false;
            }
            else
            {
                LOG_ERROR("[-] attach cleanup detach failed pid=%d errno=%d",
                          proc->pid, errno);
            }
        }
        errno = attach_errno;
        LOG_ERROR("Error attaching into PID: %d errno=%d", proc->pid, errno);
        status = false;
        goto exit;
    }

    LOG_DEBUG("[+] Attached to %d! [+]", proc->pid);
    attached = true;

    if (!init_remote_function_pointers(proc->pid))
    {
        status = false;
        goto detach;
    }

    LOG_DEBUG("[+] Loading ELF on %d...[+]", proc->pid);
    /* Crash window: remote mmap/mprotect via pt_syscall while ShellUI is stopped. */
    intptr_t entry = elfldr_load(proc->pid, (uint8_t*) elf);

    if (entry <= 0)
    {
        LOG_ERROR("[-] Failed to load ELF! [-]");
        status = false;
        goto detach;
    }
    LOG_DEBUG("[+] elfldr_load ok entry=%#lx", (unsigned long)entry);

    LOG_DEBUG("[+] Allocating payload args on %d...[+]", proc->pid);
    intptr_t args = elfldr_payload_args(proc->pid);
    LOG_DEBUG("[+] ELF entrypoint: %#02lx [+]\n[+] Payload Args: %#02lx [+]", entry, args);
    if (args <= 0)
    {
        LOG_ERROR("[-] Failed to allocate payload args! [-]");
        status = false;
        goto detach;
    }

    //
    // Copy shellcode thread parameters & boot code
    //
    sce_functions.elf_main = (void*) entry;
    sce_functions.payload_args = (void*) args;

    bootstrap = pt_mmap(proc->pid, 0, shellcode_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    /* MAP_FAILED is (void*)-1; also reject NULL. (kylin-core) */
    if (!bootstrap || bootstrap == (uint64_t)-1)
    {
        LOG_ERROR("Unable to allocate bootstrap code, injection aborted!");
        status = false;
        goto detach;
    }

    //
    // Make it executable
    //
    if (kernel_mprotect(proc->pid, bootstrap, shellcode_size, PROT_EXEC|PROT_WRITE|PROT_READ) != 0)
    {
        LOG_ERROR("[-] bootstrap mprotect failed pid=%d addr=%#lx",
                    proc->pid, (unsigned long)bootstrap);
        status = false;
        goto detach;
    }
    if (pt_copyin(proc->pid, stager, bootstrap, shellcode_size) != 0)
    {
        LOG_ERROR("[-] bootstrap copyin failed pid=%d addr=%#lx",
                    proc->pid, (unsigned long)bootstrap);
        status = false;
        goto detach;
    }

    LOG_DEBUG("[+] Bootstrap code allocated at %#02lx [+]", bootstrap);
    //
    // Write the sce functions data
    //
    sce_ptr_mem = pt_mmap(proc->pid, 0, sizeof(sce_functions), PROT_READ|PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (!sce_ptr_mem || sce_ptr_mem == (uint64_t)-1)
    {
        LOG_ERROR("[-] sce_functions mmap failed pid=%d", proc->pid);
        status = false;
        goto detach;
    }
    if (pt_copyin(proc->pid, &sce_functions, sce_ptr_mem, sizeof(SCEFunctions)) != 0)
    {
        LOG_ERROR("[-] sce_functions copyin failed pid=%d", proc->pid);
        status = false;
        goto detach;
    }

    LOG_DEBUG("[+] Triggering entrypoint... [+]");
    //
    // Call until hit a breakpoint (stager ends with int3; pt_call2 waits)
    //
    const uintptr_t breakpoint_offset =
        (uintptr_t)stager_breakpoint - (uintptr_t)stager;
    const uintptr_t expected_breakpoint = (uintptr_t)bootstrap + breakpoint_offset;
    /* A negative result means execution/stop state is uncertain; preserve
     * both mappings rather than risk unmapping memory still in use. */
    release_stager_mappings = 0;
    stager_result =
        pt_call2(proc->pid, bootstrap, sce_ptr_mem, expected_breakpoint);
    if (stager_result >= 0)
    {
        /* pthread_create has returned, so the bootstrap and parameter struct
         * are no longer needed by the newly created payload thread. */
        release_stager_mappings = 1;
    }
    if (stager_result != 0)
    {
        LOG_ERROR("[-] stager failed or stopped unexpectedly pid=%d result=%ld errno=%d",
                  proc->pid, stager_result, errno);
        status = false;
        goto detach;
    }

detach:
    if (release_stager_mappings)
    {
        if (sce_ptr_mem > 0 && sce_ptr_mem != (uint64_t)-1 &&
            pt_munmap(proc->pid, (intptr_t)sce_ptr_mem, sizeof(sce_functions)) != 0)
        {
            LOG_ERROR("[-] failed to release sce_functions mapping pid=%d addr=%#lx",
                      proc->pid, (unsigned long)sce_ptr_mem);
        }
        if (bootstrap > 0 && bootstrap != (uint64_t)-1 &&
            pt_munmap(proc->pid, (intptr_t)bootstrap, shellcode_size) != 0)
        {
            LOG_ERROR("[-] failed to release bootstrap mapping pid=%d addr=%#lx",
                      proc->pid, (unsigned long)bootstrap);
        }
    }

    if (pt_detach(proc->pid, 0) == 0)
    {
        attached = false;
    }
    else
    {
        LOG_ERROR("[-] pt_detach failed pid=%d errno=%d", proc->pid, errno);
    }

    LOG_DEBUG("[+] ELF injection finished! [+]");
    LOG_DEBUG("[+] Detached [+]");
exit:
    if (ptrace_authid_changed)
    {
        if (write_self_authid(original_authid) == 0)
        {
            LOG_DEBUG("[+] inject: restored authid=0x%lx",
                        (unsigned long)original_authid);
        }
        else
        {
            LOG_ERROR("[-] inject: authid restore failed authid=0x%lx",
                        (unsigned long)original_authid);
        }
    }
    return status;
}

//
// We can't stuck sceshellui for too long or the system will kill it's process, so we will load the library in a separated thread
//
module_info_t* load_remote_library(pid_t pid, const char* library_path, const char* library_name)
{
    if (!attached)
    {
        if (pt_attach(pid) < 0)
        {
            LOG_ERROR("load_remote_library: Failed to attach PID %d", pid);
            return NULL;
        }
    }

    // intptr_t library_str = pt_call(pid, 0, 0x100, PROT_WRITE | PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    intptr_t library_str = pt_call(pid, remote_malloc, strlen(library_name) + 1, 0, 0, 0, 0, 0);
    mdbg_copyin(pid, library_path, library_str, strlen(library_path) + 1);

    //
    // Run the module loading in a separated thread
    //
    intptr_t sce_kernel_load_start_module = pt_resolve(pid, nid_sce_kernel_load_start_module);
    create_remote_thread(pid, sce_kernel_load_start_module, library_str);

    LOG_DEBUG("sce_kernel_load_start_module: %#02lx", sce_kernel_load_start_module);
    //
    // Now we detach, sleep a little and attach again
    //
    pt_detach(pid, 0);

    int retries = 0;
    int max_retries = 100;
    module_info_t* module = NULL;

    while (retries <= max_retries)
    {
        module = get_module_info(pid, library_name);
        if (!module)
        {
            usleep(500);
        } else
        {
            break;
        }
        retries++;
    }

    if (!module)
    {
        LOG_ERROR("Unable to load %s into PID %d!", library_name, pid);
    }

    pt_attach(pid);

    return module;
}


int create_remote_thread(pid_t pid, uintptr_t target_address, uintptr_t parameters)
{
    if (!attached)
    {
        if (pt_attach(pid) < 0)
        {
            LOG_ERROR("Unable to attach into the remote process!");
            return false;
        }
    }

    intptr_t pthread = pt_call(pid, remote_malloc, sizeof(pthread_t), 0, 0, 0, 0, 0);
    if (!pthread)
    {
        LOG_ERROR("Unable to allocate memory for pthread pointer!");
        return false;
    }

    //
    // We don't have to wait (join), otherwise we would block the whole target
    //
    return pt_call(pid, remote_pthread_create, pthread, 0, target_address, parameters, 0, 0);
}
