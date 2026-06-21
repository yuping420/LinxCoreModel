#pragma once

#ifndef __ECALL_H__
#define __ECALL_H__

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#if __has_include(<sys/sysinfo.h>)
#include <sys/sysinfo.h>
#define LINXCOREMODEL_HAS_HOST_SYSINFO 1
#else
#define LINXCOREMODEL_HAS_HOST_SYSINFO 0
struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
};

static inline int sysinfo(struct sysinfo* info)
{
    if (info == nullptr) {
        return -1;
    }
    std::memset(info, 0, sizeof(*info));
    info->mem_unit = 1;
    return 0;
}
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#if __has_include(<sched.h>)
#include <sched.h>
#endif
#if __has_include(<linux/unistd.h>)
#include <linux/unistd.h>
#endif
#include <signal.h>

#include "Memory.h"

static constexpr uint64_t BUFFER_SIZE_MAX = 0x8000;
#define PAGE_SIZE 4096 // 4KB 页大小

namespace JCore {
// todo: fix it
extern uint64_t g_programBreak;
extern uint64_t g_mmapAddr;

#define SYSCALL_DEBUG 0

std::unordered_map<int, std::string> syscall_name = {
    {0x1d,      "ioctl"},
    {0x32,      "fchdir"},
    {0x38,      "openat"},
    {0x39,      "close"},
    {0x3e,      "lseek"},
    {0x3f,      "read"},
    {0x40,      "write"},
    {0x41,      "readv"},
    {0x42,      "writev"},
    {0x4e,      "readlinkat"},
    {0x50,      "fstat"},
    {0x5d,      "exit"},
    {0x5e,      "exit_group"},
    {0x60,      "set_tid_address"},
    {0x65,      "nanosleep"},
    {0x71,      "clock_gettime"},
    {0x7b,      "sched_getaffinity"},
    {0x87,      "rt_sigprocmask"},
    {0xa0,      "uname"},
    {0xb2,      "gettid"},
    {0xb3,      "sysinfo"},
    {0xd6,      "brk"},
    {0xd7,      "munmap"},
    {0xde,      "mmap"},
    {0xe2,      "mprotect"},
    {0xe9,      "madvise"},
    {0x105,     "prlimit64"},
};

enum class SyscallRequest
{
    io_setup = 0,
    io_destroy = 1,
    io_submit = 2,
    io_cancel = 3,
    io_getevents = 4,
    setxattr = 5,
    lsetxattr = 6,
    fsetxattr = 7,
    getxattr = 8,
    lgetxattr = 9,
    fgetxattr = 10,
    listxattr = 11,
    llistxattr = 12,
    flistxattr = 13,
    removexattr = 14,
    lremovexattr = 15,
    fremovexattr = 16,
    getcwd = 17,
    lookup_dcookie = 18,
    eventfd2 = 19,
    epoll_create1 = 20,
    epoll_ctl = 21,
    epoll_pwait = 22,
    dup = 23,
    dup3 = 24,
    fcntl = 25,
    inotify_init1 = 26,
    inotify_add_watch = 27,
    inotify_rm_watch = 28,
    ioctl = 29,
    ioprio_set = 30,
    ioprio_get = 31,
    flock = 32,
    mknodat = 33,
    mkdirat = 34,
    unlinkat = 35,
    symlinkat = 36,
    linkat = 37,
    umount2 = 39,
    mount = 40,
    pivot_root = 41,
    nfsservctl = 42,
    statfs = 43,
    fstatfs = 44,
    truncate = 45,
    ftruncate = 46,
    fallocate = 47,
    faaccelssat = 48,
    chdir = 49,
    fchdir = 50,
    chroot = 51,
    fchmod = 52,
    fchmodat = 53,
    fchownat = 54,
    fchown = 55,
    openat = 56,
    close = 57,
    vhangup = 58,
    pipe2 = 59,
    quotactl = 60,
    getdents64 = 61,
    lseek = 62,
    read = 63,
    write = 64,
    readv = 65,
    writev = 66,
    pread64 = 67,
    pwrite64 = 68,
    preadv = 69,
    pwritev = 70,
    sendfile = 71,
    pselect6 = 72,
    ppoll = 73,
    signalfd4 = 74,
    vmsplice = 75,
    splice = 76,
    tee = 77,
    readlinkat = 78,
    newfstatat = 79,
    fstat = 80,
    sync = 81,
    fsync = 82,
    fdatasync = 83,
    sync_file_range = 84,
    timerfd_create = 85,
    timerfd_settime = 86,
    timerfd_gettime = 87,
    utimensat = 88,
    acct = 89,
    capget = 90,
    capset = 91,
    personality = 92,
    exit = 93,
    exit_group = 94,
    waitid = 95,
    set_tid_address = 96,
    unshare = 97,
    futex = 98,
    set_robust_list = 99,
    get_robust_list = 100,
    nanosleep = 101,
    getitimer = 102,
    setitimer = 103,
    kexec_load = 104,
    init_module = 105,
    delete_module = 106,
    timer_create = 107,
    timer_gettime = 108,
    timer_getoverrun = 109,
    timer_settime = 110,
    timer_delete = 111,
    clock_settime = 112,
    clock_gettime = 113,
    clock_getres = 114,
    clock_nanosleep = 115,
    syslog = 116,
    ptrace = 117,
    sched_setparam = 118,
    sched_setscheduler = 119,
    sched_getscheduler = 120,
    sched_getparam = 121,
    sched_setaffinity = 122,
    sched_getaffinity = 123,
    sched_yield = 124,
    sched_get_priority_max = 125,
    sched_get_priority_min = 126,
    sched_rr_get_interval = 127,
    restart_syscall = 128,
    kill = 129,
    tkill = 130,
    tgkill = 131,
    sigaltstack = 132,
    rt_sigsuspend = 133,
    rt_sigaction = 134,
    rt_sigprocmask = 135,
    rt_sigpending = 136,
    rt_sigtimedwait = 137,
    rt_sigqueueinfo = 138,
    rt_sigreturn = 139,
    setpriority = 140,
    getpriority = 141,
    reboot = 142,
    setregid = 143,
    setgid = 144,
    setreuid = 145,
    setuid = 146,
    setresuid = 147,
    getresuid = 148,
    setresgid = 149,
    getresgid = 150,
    setfsuid = 151,
    setfsgid = 152,
    times = 153,
    setpgid = 154,
    getpgid = 155,
    getsid = 156,
    setsid = 157,
    getgroups = 158,
    setgroups = 159,
    uname = 160,
    sethostname = 161,
    setdomainname = 162,
    getrlimit = 163,
    setrlimit = 164,
    getrusage = 165,
    umask = 166,
    prctl = 167,
    getcpu = 168,
    gettimeofday = 169,
    settimeofday = 170,
    adjtimex = 171,
    getpid = 172,
    getppid = 173,
    getuid = 174,
    geteuid = 175,
    getgid = 176,
    getegid = 177,
    gettid = 178,
    sysinfo = 179,
    mq_open = 180,
    mq_unlink = 181,
    mq_timedsend = 182,
    mq_timedreceive = 183,
    mq_notify = 184,
    mq_getsetattr = 185,
    msgget = 186,
    msgctl = 187,
    msgrcv = 188,
    msgsnd = 189,
    semget = 190,
    semctl = 191,
    semtimedop = 192,
    semop = 193,
    shmget = 194,
    shmctl = 195,
    shmat = 196,
    shmdt = 197,
    socket = 198,
    socketpair = 199,
    bind = 200,
    listen = 201,
    aaccelpt = 202,
    connect = 203,
    getsockname = 204,
    getpeername = 205,
    sendto = 206,
    recvfrom = 207,
    setsockopt = 208,
    getsockopt = 209,
    shutdown = 210,
    sendmsg = 211,
    recvmsg = 212,
    readahead = 213,
    brk = 214,
    munmap = 215,
    mremap = 216,
    add_key = 217,
    request_key = 218,
    keyctl = 219,
    clone = 220,
    execve = 221,
    mmap = 222,
    fadvise64 = 223,
    swapon = 224,
    swapoff = 225,
    mprotect = 226,
    msync = 227,
    mlock = 228,
    munlock = 229,
    mlockall = 230,
    munlockall = 231,
    mincore = 232,
    madvise = 233,
    remap_file_pages = 234,
    mbind = 235,
    get_mempolicy = 236,
    set_mempolicy = 237,
    migrate_pages = 238,
    move_pages = 239,
    rt_tgsigqueueinfo = 240,
    perf_event_open = 241,
    aaccelpt4 = 242,
    recvmmsg = 243,
    arch_specific_syscall = 244,
    wait4 = 260,
    prlimit64 = 261,
    fanotify_init = 262,
    fanotify_mark = 263,
    name_to_handle_at = 264,
    open_by_handle_at = 265,
    clock_adjtime = 266,
    syncfs = 267,
    setns = 268,
    sendmmsg = 269,
    process_vm_readv = 270,
    process_vm_writev = 271,
    kcmp = 272,
    finit_module = 273,
    sched_setattr = 274,
    sched_getattr = 275,
    renameat2 = 276,
    seccomp = 277,
    getrandom = 278,
    memfd_create = 279,
    bpf = 280,
    execveat = 281,
    userfaultfd = 282,
    membarrier = 283,
    mlock2 = 284,
    copy_file_range = 285,
    preadv2 = 286,
    pwritev2 = 287,
    pkey_mprotect = 288,
    pkey_alloc = 289,
    pkey_free = 290,
    statx = 291,
    io_pgetevents = 292,
    rseq = 293,
    kexec_file_load = 294,
    pidfd_send_signal = 424,
    io_uring_setup = 425,
    io_uring_enter = 426,
    io_uring_register = 427,
    open_tree = 428,
    move_mount = 429,
    fsopen = 430,
    fsconfig = 431,
    fsmount = 432,
    fspick = 433,
    pidfd_open = 434,
    clone3 = 435,
    close_range = 436,
    openat2 = 437,
    pidfd_getfd = 438,
    faaccelssat2 = 439,
    process_madvise = 440,
    epoll_pwait2 = 441,
    mount_setattr = 442,
    landlock_create_ruleset = 444,
    landlock_add_rule = 445,
    landlock_restrict_self = 446,
    syscalls = 447,

    xprintf = 3939
};

class AddressType
{
    uint64_t _v;

public:
    constexpr AddressType(uint64_t v) : _v(v) {}
    AddressType(void *v) : _v(reinterpret_cast<uint64_t>(v)) {}
    AddressType &operator=(uint64_t v) {
        _v = v;
        return *this;
    }
    AddressType &operator=(void *v) {
        _v = reinterpret_cast<uint64_t>(v);
        return *this;
    }
    constexpr operator uint64_t() const { return _v; }
    operator void *() const { return reinterpret_cast<void *>(_v); }
    operator char *() const { return reinterpret_cast<char *>(_v); }
};

template <typename MemoryType>
class Aaccelssor;

template <>
class Aaccelssor<SoftMemory>
{
    SoftMemory *mem;
    const int FD_INVALID_VAL = -1;
    const uint64_t INVALID_NULL = 0;

    struct storeInfo {
        uint64_t addr;
        uint64_t data;
        int len;

        storeInfo(uint64_t a, uint64_t d, int l) : addr(a), data(d), len(l) {}
    };

    #define ROUND_DOWN(n, d) ((n) & -(0 ? (n) : (d)))
    #define ROUND_UP(n, d) ROUND_DOWN((n) + (d) - 1, (d))
    #define HOST_PAGE_ALIGN(addr) ROUND_UP((addr), 0x1000)
public:
    Aaccelssor(SoftMemory *m) : mem(m) {}

    /* The store operation is not performed. Only the store information is recorded. */
    std::vector<storeInfo> storeInfoVector;
    template <typename DataType>
    void Store(AddressType va, DataType const &dat) {
        if (va == 0 || sizeof(DataType) < 1)
            return;
        int len = sizeof(DataType);
        char* p = (char*)&dat;
        for (int i = 0; i < len; ++i) {
            storeInfoVector.emplace_back(va + i, (uint8_t)p[i], 1);
        }
    }
    template <typename DataType>
    void Load(AddressType va, DataType &dat) {
        if (va == 0 || sizeof(DataType) < 1)
            return;
        dat = mem->Load(va, sizeof(DataType), false);
    }
    void Store(AddressType va, AddressType ha, size_t len) {
        if (va == 0 || len < 0)
            return;
        char *p = ha;
        for (size_t i = 0; i < len; ++i) {
            storeInfoVector.emplace_back(va + i, (uint8_t)p[i], 1);
        }
    }
    void Load(AddressType va, AddressType ha, size_t len) {
        if (va == 0 || len < 0)
            return;
        char *p = ha;
        for (size_t i = 0; i < len; ++i)
            p[i] = mem->Load(va + i, 1, false);
    }

    void MergeStoreInfo() {
        size_t storeInfoVectorSize = storeInfoVector.size();
        if (storeInfoVectorSize == 0) {
            return;
        }

        std::sort(storeInfoVector.begin(), storeInfoVector.end(), [](storeInfo &a, storeInfo &b){
            return (a.addr <= b.addr);
        });

        std::unordered_map<uint64_t, storeInfo> storeInfoMap;
        for (size_t i = 0; i < storeInfoVectorSize; i++) {
            storeInfoMap.insert(std::make_pair(storeInfoVector[i].addr, storeInfoVector[i]));
        }

        uint64_t start_addr = 0;
        uint64_t end_addr = 0;
        int len = 8;

        std::vector<storeInfo> new_storeInfoVector;
        for (size_t i = 0; i < storeInfoVectorSize; i++) {
            auto si = storeInfoVector[i];
            if (si.addr >= start_addr && si.addr < end_addr) {
                continue;
            } else {
                start_addr = si.addr;
                end_addr = start_addr + 8;
            }
            /* merge */
            uint64_t data;
            char* p = (char*)&data;
            for (int j = 0; j < len; j++) {
                uint64_t curr_addr = start_addr + j;
                uint8_t tmp = 0;
                if (storeInfoMap.count(curr_addr)) {
                    auto it = storeInfoMap.find(curr_addr);
                    tmp = it->second.data;
                } else {
                    Load(curr_addr, tmp);
                }

                p[j] = tmp;
            }
            new_storeInfoVector.emplace_back(start_addr, data, 8);
        }
        storeInfoVector = new_storeInfoVector;
    }

    int Set_tid_address(int *tptr) {
#if defined(SYS_set_tid_address)
        return syscall(SYS_set_tid_address, tptr);
#else
        (void)tptr;
        return 0;
#endif
    }
    int Sys_prlimit64(pid_t pid, int resource, const struct rlimit *new_limit,
                        struct rlimit *old_limit) {
#if defined(SYS_prlimit64)
        return syscall(SYS_prlimit64, pid, resource, new_limit, old_limit);
#else
        (void)pid;
        if (new_limit != nullptr) {
            return setrlimit(resource, new_limit);
        }
        if (old_limit != nullptr) {
            return getrlimit(resource, old_limit);
        }
        return 0;
#endif
    }

    int Sys_write(int fd, const void* buffer, size_t count)
    {
        return static_cast<int>(write(fd, buffer, count));
    }

    int Sys_writev(int fd, const struct iovec *iov, int iovcnt) {
        return static_cast<int>(writev(fd, iov, iovcnt));
    }

    int Sys_ioctl(int fd, int request, void *data) {
        return ioctl(fd, static_cast<unsigned long>(request), data);
    }

    int Sys_brk(uint64_t new_brk) {
        if (new_brk == INVALID_NULL) {
            // returns the currect program break
            return g_programBreak;
        } else {
            // change currect brk
            g_programBreak = new_brk;
        }
        return g_programBreak;
    }

    uint64_t target_mmap (uint64_t start, uint64_t len, int prot,
    int flags, int fd, off_t offset) {
        if (fd == FD_INVALID_VAL) {
            if (start == INVALID_NULL) {
                if (g_mmapAddr % PAGE_SIZE != 0) {
                    g_mmapAddr = (g_mmapAddr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                }

                g_mmapAddr += len;
                return (g_mmapAddr - len);
            } else {
                return start;
            }
        } else {
            std::cout << "mmapp donot support fd!" << std::endl;
            abort();
        }

        return -1;
    }
};

#define xA0 2  /* gpr[10-17] are syscall arguments */
#define xA1 3
#define xA2 4
#define xA3 5
#define xA4 6
#define xA5 7
#define xA6 8
#define xA7 9
#define xX1 21

template <typename RegisterType, typename MemoryType, typename AaccelssorType = Aaccelssor<MemoryType>>
class EcallAgent
{
public:
    using SyscallHandler = void (EcallAgent<RegisterType, MemoryType, AaccelssorType>::*)(void);
    AaccelssorType aaccelssor;
    EcallAgent(EcallAgent const &) = delete;
    EcallAgent(EcallAgent &&) = delete;
    EcallAgent(RegisterType* gpr, uint64_t xbCid, MemoryType &memory)
        : aaccelssor(&memory),
          returnValue(0),
          requestNumber(static_cast<SyscallRequest>(gpr[xX1]) /* A7 register */),
          arguments{gpr[xA0], gpr[xA1], gpr[xA2], gpr[xA3], gpr[xA4], gpr[xA5]},
          p_ret(&(gpr[xA0])),
          memory(&memory) {
        ((void)(&_Static_Constructor));
        auto it = HandlerTable.find(requestNumber);
        if (it == HandlerTable.end()) {
            returnValue = -1;
            std::cerr << std::hex << "\nAt " << __FILE__ << ' ' << __LINE__
                      << ' ' << __func__ << ":\nBad Syscall Request: syscall("
                      << static_cast<uint64_t>(requestNumber) << ", "
                      << arguments[0] << ", " << arguments[1] << ", "
                      << arguments[2] << ", " << arguments[3] << ", "
                      << arguments[4] << ", " << arguments[5] << ");\n";
            abort();
        }
        if (SYSCALL_DEBUG) {
            std::cerr << std::hex << "Syscall Request: syscall(" << static_cast<uint64_t>(requestNumber) << " "
                        << syscall_name[static_cast<uint64_t>(requestNumber)] << ", "
                        << arguments[0] << ", " << arguments[1] << ", "
                        << arguments[2] << ", " << arguments[3] << ", "
                        << arguments[4] << ", " << arguments[5] << ");\n";
        }
        auto h = it->second;
        (this->*h)();
        aaccelssor.MergeStoreInfo();
    }

    uint64_t operator()(void) const
    {
        if (SYSCALL_DEBUG) {
            std::cerr << "returnValue: " << std::hex << returnValue << std::endl;
        }
        return returnValue;
    }

private:
    static const std::unordered_map<SyscallRequest, SyscallHandler> HandlerTable;
    static const std::nullptr_t _Static_Constructor;
    static timespec DummyTime;

    uint64_t returnValue;
    SyscallRequest requestNumber;
    uint64_t arguments[6];
    uint64_t *p_ret;
    MemoryType *memory;

#define HANDLER(x) inline void do_##x()
#define min(a, b) ((a) < (b) ? (a) : (b))
#define store(...) aaccelssor.Store(__VA_ARGS__)
#define load(...) aaccelssor.Load(__VA_ARGS__)
#define set_tid_address(...) aaccelssor.Set_tid_address(__VA_ARGS__)
#define target_to_host_resource(...) aaccelssor.Target_to_host_resource(__VA_ARGS__)
#define sys_prlimit64(...) aaccelssor.Sys_prlimit64(__VA_ARGS__)
#define sys_ioctl(...) aaccelssor.Sys_ioctl(__VA_ARGS__)
#define sys_write(...) aaccelssor.Sys_write(__VA_ARGS__)
#define sys_writev(...) aaccelssor.Sys_writev(__VA_ARGS__)

#define sys_brk(...)  aaccelssor.Sys_brk(__VA_ARGS__)
#define t_mmap(...) aaccelssor.target_mmap(__VA_ARGS__)

    const uint64_t INVALID_VAL = -1;
    HANDLER(set_tid_address)
    {
        returnValue = set_tid_address((int *)arguments[0]);
        if (returnValue == INVALID_VAL) {
            std::cout << "ecall warnning: sys_set_tid_address function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
        }
    }

    HANDLER(mmap)
    {
        returnValue = t_mmap(arguments[0], arguments[1], arguments[2], arguments[3], arguments[4],
                            arguments[5]);
        if (returnValue == INVALID_VAL) {
            std::cout << "ecall warnning: sys_mmap function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
        }

        return;
    }

    HANDLER(mprotect)
    {
        if (arguments[1] == 0) {
            returnValue = -1;
        } else {
            returnValue = 0;
        }
        return;
    }

    HANDLER(openat)
    {
        int i = 0;
        std::string pathname = "";

        while (true) {
            char c = 0;
            aaccelssor.Load(arguments[1] + i, c);

            if (c == '\0') {
                break;
            }

            pathname += c;
            i++;
        }
        const char* c_pathname = pathname.c_str();

        if (SYSCALL_DEBUG) {
        std::cout << "SYSCALL: openat" << std::endl;
        std::cout << "path=[" << pathname << "]";
        std::cout << ", dirfd=[" << arguments[0] << "]";
        std::cout << ", flags=0x" << std::hex << arguments[2];
        std::cout << ", mode=0" << std::oct << arguments[3] << std::dec << std::endl;
        }
        /* int openat(int dirfd, const char *pathname, int flags, mode_t mode); */
#if defined(__APPLE__)
        int fd = open(c_pathname, static_cast<int>(arguments[2]), static_cast<mode_t>(arguments[3]));
#else
        int fd = openat(static_cast<int>(arguments[0]), c_pathname,
                        static_cast<int>(arguments[2]), static_cast<mode_t>(arguments[3]));
#endif
        if (fd == -1) {
            std::cout << "ecall warnning: sys_openat function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = fd;
        }
    }

    HANDLER(lseek)
    {
        int ret = static_cast<int>(lseek(arguments[0], arguments[1], arguments[2]));
        if (ret == -1) {
            std::cout << "ecall warnning: sys_lseek function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }
    }

    HANDLER(readv)
    {
        // ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
        int fd = arguments[0];
        uint64_t iov_addr = arguments[1];
        int iovcnt = arguments[2];
        struct iovec* p_iov = (struct iovec*)malloc(sizeof(struct iovec) * iovcnt);

        for (int i = 0; i < iovcnt; i++) {
            uint64_t len = 0;
            aaccelssor.Load(iov_addr + sizeof(struct iovec) * i + 8, len);

            p_iov[i].iov_base = (void*)malloc(len);
            p_iov[i].iov_len = len;
        }

        int ret = static_cast<int>(readv(fd, p_iov, iovcnt));

        // write iov to guest

        for (int i = 0; i < iovcnt; i++) {
            uint64_t base = 0;
            aaccelssor.Load(iov_addr + sizeof(struct iovec) * i, base);
            aaccelssor.Store(base, p_iov[i].iov_base, p_iov[i].iov_len);
        }

        if (ret == -1) {
            std::cout << "ecall warnning: sys_readv function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }

        /* free mem */
        for (int i = 0; i < iovcnt; i++) {
            free(p_iov[i].iov_base);
        }
        free(p_iov);
    }

    HANDLER(close)
    {
        int ret = close(arguments[0]);
        if (SYSCALL_DEBUG) {
            std::cout << "SYSCALL: close" << std::endl;
        }
        if (ret == -1) {
            std::cout << "ecall warnning: close function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }
    }

struct target_rlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};
struct host_rlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

struct linux_dirent64 {
    unsigned long d_ino;
    unsigned int d_off;
    unsigned short d_reclen;
    char d_name[];
};

struct sigaction {
    void (*a)(int);
    void (*b)(int, siginfo_t*, void*);
    sigset_t sa_mask;
    int sa_flags;
    void (*c)(void);
};

    HANDLER(prlimit64)
    {
        /* args: pid, resource number, ptr to new rlimit, ptr to old rlimit */
        struct host_rlimit64 rold;
        int resource = arguments[1];// target_to_host_resource(arg2);
        int ret = 0;

        ret = sys_prlimit64(arguments[0], resource, (struct rlimit *)arguments[2], (struct rlimit *)(arguments[3] ? &rold : 0));
        if (SYSCALL_DEBUG) {
            std::cout << "SYSCALL: sys_prlimit64" << std::endl;
        }
        if (ret == -1) {
            std::cout << "ecall warnning: sys_prlimit64 function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            /* write to gfrun */
            aaccelssor.Store(arguments[3], rold.rlim_cur);
            aaccelssor.Store(arguments[3] + 8, rold.rlim_max);

            returnValue = ret;
        }
        return;
    }

    HANDLER(brk)
    {
        int ret = 0;
        ret = sys_brk(arguments[0]);
        if (ret == -1) {
            std::cout << "ecall warnning: sys_brk function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }
    }

    HANDLER(munmap)
    {
        returnValue = 0;
        return;
    }

    HANDLER(madvise)
    {
        returnValue = 0;
        return;
    }

    HANDLER(exit_group)
    {
        returnValue = arguments[0];
    }

    HANDLER(exit)
    {
        returnValue = arguments[0];
    }

    HANDLER(write)
    {
        int ret = 0;
        int count = arguments[2];
        void *buf = malloc(count);
        aaccelssor.Load(arguments[1], buf, count);
        ret = sys_write(arguments[0], buf, count);
        if (ret == -1) {
            std::cout << "ecall warnning: sys_write function failed! errno: " << errno << " " << strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }
    }

    HANDLER(writev)
    {
        int ret;
        /* 需要申请的空间 */
        int iovcnt = arguments[2];
        int iov_size = iovcnt * sizeof(struct iovec);

        /* 申请 host 空间 */
        struct iovec *iovec_array = (struct iovec *)malloc(iov_size);

        /* 从 guest空间拷贝数据到host空间 */
        for (int i = 0; i < iovcnt; i++) {
            struct iovec *p = iovec_array + i;
            uint64_t iov_base = 0;
            uint64_t iov_len = 0;

            aaccelssor.Load(arguments[1] + i * 16, iov_base);
            aaccelssor.Load(arguments[1] + i * 16 + 8, iov_len);

            p->iov_base = (void*)iov_base;
            p->iov_len = iov_len;

            char* str_p = (char*)malloc(p->iov_len);

            for (uint64_t i = 0; i < p->iov_len; i++) {
                char tmp = 0;
                aaccelssor.Load((uint64_t)p->iov_base + i, tmp);
                *(str_p + i) = tmp;
            }
            p->iov_base = str_p;
        }

        ret = sys_writev(arguments[0], iovec_array, iovcnt);
        if (SYSCALL_DEBUG) {
            std::cout << "SYSCALL: writev" << std::endl;
        }
        /* todo: 释放内存 */
        if (ret == -1) {
            std::cout << "ecall warnning: sys_writev function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }

        /* free mem */
        for (int i = 0; i < iovcnt; i++) {
            free(iovec_array[i].iov_base);
        }
        free(iovec_array);
    }

    HANDLER(ioctl)
    {
        struct winsize ws;
        int ret = 0;
        ret = sys_ioctl(arguments[0], arguments[1], (void *)&ws);
        if (SYSCALL_DEBUG) {
            std::cout << "SYSCALL: ioctl" << std::endl;
        }
        if (arguments[0] == 1) {
            ret = 0;
        }

        if (ret == -1) {
            std::cout << "ecall warnning: sys_ioctl function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            /* write to gfrun */
            aaccelssor.Store(arguments[2], ws.ws_row);
            aaccelssor.Store(arguments[2] + 2, ws.ws_col);
            aaccelssor.Store(arguments[2] + 4, ws.ws_xpixel);
            aaccelssor.Store(arguments[2] + 6, ws.ws_ypixel);

            returnValue = ret;
        }
    }

    HANDLER(nanosleep)
    {
        if (arguments[0] == 0) {
            returnValue = -1;
            return;
        }
        struct timespec req = {0, 0};
        size_t off = 0;
        load(arguments[0], req.tv_sec);
        off += sizeof(req.tv_sec);
        load(arguments[0] + off, req.tv_nsec);
        uint64_t ns = DummyTime.tv_nsec + req.tv_nsec;
        DummyTime.tv_nsec = ns % 1000000000;
        DummyTime.tv_sec += ns / 1000000000;
        if (arguments[1] == 0)
            return;
        char buf[64]{0};
        store(arguments[1], buf, sizeof(struct timespec));
    }

    HANDLER(clock_gettime)
    {
        // int clock_gettime(clockid_t clockid, struct timespec *tp);
        clockid_t clockid = static_cast<clockid_t>(arguments[0]);
        struct timespec ts;

        int ret = clock_gettime(clockid, &ts);
        aaccelssor.Store(arguments[1], &ts, sizeof(struct timespec));

        if (ret == -1) {
            std::cout << "ecall warnning: clock_gettime function failed! errno: " << errno << " " << strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }
    }

    HANDLER(rt_sigprocmask)
    {
        returnValue = 0;
    }

    HANDLER(uname)
    {
        static char const NODENAME[64] = "EcallAgent";
        static char const MACHINE[64] = "linx64";
        if (arguments[0] == 0) {
            returnValue = -1;
            return;
        }
        struct utsname un;
        uname(&un);
        strcpy(un.nodename, NODENAME);
        strcpy(un.machine, MACHINE);
        store(arguments[0], &un, sizeof(un));
    }

    HANDLER(sysinfo)
    {
        if (arguments[0] == 0) {
            returnValue = -1;
            return;
        }
        struct sysinfo si;
        sysinfo(&si);
        store(arguments[0], &si, sizeof(struct sysinfo));
    }

    HANDLER(readlinkat)
    {
        // TODO: handle polybench test
        returnValue = -1;
        return;
    }

    HANDLER(read)
    {
        // ssize_t read(int fd, void *buf, size_t count);
        int fd = arguments[0];
        uint64_t buf = arguments[1];
        int count = arguments[2];

        void* buf_tmp = (void*)malloc(count);

        int ret = static_cast<int>(read(fd, buf_tmp, count));
        if (SYSCALL_DEBUG) {
            std::cout << "SYSCALL: read" << std::endl;
        }
        // write buf_tmp to guest

        aaccelssor.Store(buf, buf_tmp, count);

        if (ret == -1) {
            std::cout << "ecall warnning: sys_read function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }

        return;
    }

    HANDLER(sched_getaffinity)
    {
#if defined(__linux__)
        // int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
        pid_t pid = arguments[0];
        size_t cpusetsize = arguments[1];
        uint64_t mask = arguments[2];

        cpu_set_t* mask_tmp = (cpu_set_t*)malloc(sizeof(cpu_set_t));

        int ret = syscall(SYS_sched_getaffinity, pid, cpusetsize, mask_tmp);

        // write mask_tmp to guest

        aaccelssor.Store(mask, mask_tmp, sizeof(cpu_set_t));

        if (ret == -1) {
            std::cout << "ecall warnning: sys_sched_getaffinity function failed! errno: " << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }

        return;
#else
        (void)arguments;
        returnValue = 0;
        return;
#endif
    }

    HANDLER(gettid)
    {
#if defined(__linux__) && defined(SYS_gettid)
        int ret = syscall(SYS_gettid);
#else
        int ret = static_cast<int>(getpid());
#endif
        returnValue = ret;
    }

    HANDLER(fchdir)
    {
        // function prototype: int fchdir(int fd);
        int fd = arguments[0];
        int ret = fchdir(fd);
        if (ret == -1) {
            std::cout << "ecall warnning: sys_fchdir function failed! errno: "
                      << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }
        return;
    }

    HANDLER(fstat)
    {
        // function prototype: int fstat(int fd, struct stat* buf);
        int fd = arguments[0];
        struct stat* buf = (struct stat*)malloc(sizeof(struct stat));
        aaccelssor.Load(arguments[1], buf, sizeof(struct stat));
        int ret = fstat(fd, buf);
        aaccelssor.Store(arguments[1], buf, sizeof(struct stat));
        if (ret == -1) {
            std::cout << "ecall warnning: sys_fstat function failed! errno: "
                      << errno << " "<< strerror(errno)<< std::endl;
            returnValue = -1;
        } else {
            returnValue = ret;
        }
        return;
    }

#undef load
#undef store
#undef min
#undef HANDLER
};

template <typename RegisterType, typename MemoryType, typename AaccelssorType>
const std::unordered_map<SyscallRequest, typename EcallAgent<RegisterType, MemoryType, AaccelssorType>::SyscallHandler>
    EcallAgent<RegisterType, MemoryType, AaccelssorType>::HandlerTable = {
#define REGISTER_HANDLER(x)                                                            \
    {                                                                                  \
        SyscallRequest::x, &EcallAgent<RegisterType, MemoryType, AaccelssorType>::do_##x \
    }
        REGISTER_HANDLER(madvise),
        REGISTER_HANDLER(rt_sigprocmask),
        REGISTER_HANDLER(openat),
        REGISTER_HANDLER(lseek),
        REGISTER_HANDLER(readv),
        REGISTER_HANDLER(close),
        REGISTER_HANDLER(mprotect),
        REGISTER_HANDLER(mmap),
        REGISTER_HANDLER(munmap),
        REGISTER_HANDLER(brk),
        REGISTER_HANDLER(prlimit64),
        REGISTER_HANDLER(ioctl),
        REGISTER_HANDLER(writev),
        REGISTER_HANDLER(exit_group),
        REGISTER_HANDLER(exit),
        REGISTER_HANDLER(set_tid_address),
        REGISTER_HANDLER(clock_gettime),
        REGISTER_HANDLER(sysinfo),
        REGISTER_HANDLER(nanosleep),
        REGISTER_HANDLER(uname),
        REGISTER_HANDLER(readlinkat),
        REGISTER_HANDLER(read),
        REGISTER_HANDLER(sched_getaffinity),
        REGISTER_HANDLER(gettid),
        REGISTER_HANDLER(fchdir),
        REGISTER_HANDLER(fstat),
        REGISTER_HANDLER(write),
#undef REGISTER_HANDLER
};

template <typename RegisterType, typename MemoryType, typename AaccelssorType>
timespec EcallAgent<RegisterType, MemoryType, AaccelssorType>::DummyTime;

template <typename RegisterType, typename MemoryType, typename AaccelssorType>
const std::nullptr_t EcallAgent<RegisterType, MemoryType, AaccelssorType>::_Static_Constructor = []() {
    clock_gettime(CLOCK_REALTIME, &DummyTime);
    return nullptr;
}();

} // namespace JCore

#endif // __ECALL_H__
