#include "ELF.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <map>
#include <stack>
#include <string>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#endif

#if __has_include(<sys/auxv.h>)
#include <sys/auxv.h>
#define LINXCOREMODEL_HAS_HOST_AUXV 1
#else
#define LINXCOREMODEL_HAS_HOST_AUXV 0
#ifndef AT_NULL
#define AT_NULL 0
#endif
#ifndef AT_PHDR
#define AT_PHDR 3
#endif
#ifndef AT_PHENT
#define AT_PHENT 4
#endif
#ifndef AT_PHNUM
#define AT_PHNUM 5
#endif
#ifndef AT_PAGESZ
#define AT_PAGESZ 6
#endif
#ifndef AT_BASE
#define AT_BASE 7
#endif
#ifndef AT_FLAGS
#define AT_FLAGS 8
#endif
#ifndef AT_ENTRY
#define AT_ENTRY 9
#endif
#ifndef AT_UID
#define AT_UID 11
#endif
#ifndef AT_EUID
#define AT_EUID 12
#endif
#ifndef AT_GID
#define AT_GID 13
#endif
#ifndef AT_EGID
#define AT_EGID 14
#endif
#ifndef AT_CLKTCK
#define AT_CLKTCK 17
#endif
#ifndef AT_HWCAP
#define AT_HWCAP 16
#endif
#ifndef AT_SECURE
#define AT_SECURE 23
#endif
#ifndef AT_RANDOM
#define AT_RANDOM 25
#endif
#ifndef AT_EXECFN
#define AT_EXECFN 31
#endif
#endif

#include "SoftCore.h"

namespace JCore {
using namespace std;
std::map<uint64_t, uint64_t> text_region;
extern std::vector<struct adderss_region> stub_region;
uint64_t g_programBreak = 0;
uint64_t g_mmapAddr = 0;
static int g_offsetByte = 8;
static int g_endOffsetByte = 16;
static int g_memOffsetBase = 8;
static int g_tregGprNum = 24;
static int g_instOffsetBase = 2;
static uint64_t g_endOffsetBase = static_cast<uint64_t>(-16);

static constexpr int ZERO = 0;
static constexpr uint64_t STACK_ALIGN_MASK = static_cast<uint64_t>(-16);
static constexpr uint64_t STACK_ALIGN_8BYTE = static_cast<uint64_t>(-8);
static uint64_t g_pageSize = 0x1000;
    struct PrepareAuxData {
        Elf64_Ehdr *elfHeader;
        std::string fileName;
        std::vector<string> spArgv;
        std::vector<std::string> spEnvp;
        uint64_t phdrVaddr;
        uint64_t ptrRandomBytes;
        uint64_t ptrFileString;
};

static constexpr bool ENABLE_LOG = false;
template <typename... Args>
static void Log(const char* format, Args... args)
{
    if (ENABLE_LOG) {
        printf(format, args...);
    }
}

static uint64_t HostAuxVal(uint64_t type)
{
#if LINXCOREMODEL_HAS_HOST_AUXV
    return static_cast<uint64_t>(getauxval(static_cast<unsigned long>(type)));
#else
    (void)type;
    return 0;
#endif
}

static char** HostEnviron()
{
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    extern char** environ;
    return environ;
#endif
}

static void WriteByte(uint64_t addr, uint8_t val, SoftMemory *arg)
{
    if (!(arg->Store(addr, val, 1))) {
        fprintf(stderr, "ERROR: Cannot write byte to 0x%08lx\n", addr);
        return;
    }
}

static void WriteString(const std::string &str, SoftMemory *arg, uint64_t *spAddr)
{
    *spAddr -= 1;
    WriteByte(*spAddr, '\0', arg);
    int len = str.length();
    *spAddr -= len;
    for (int i = 0; i < len; ++i) {
        WriteByte(*spAddr + i, str.at(i), arg);
    }
}

static void WriteUint64(uint64_t val, SoftMemory *arg, uint64_t *spAddr)
{
    constexpr int size = 8;
    *spAddr -= size;
    if (!(arg->Store(*spAddr, val, size))) {
        fprintf(stderr, "ERROR: Cannot write 8 bytes to 0x%08lx\n", *spAddr);
        return;
    }
}

static void WriteAuxTable(SoftMemory *arg, uint64_t *spAddr, PrepareAuxData &auxData)
{
    auto writeIdAndVal = [arg, spAddr](uint64_t id, uint64_t val) {
        WriteUint64(val, arg, spAddr);
        WriteUint64(id, arg, spAddr);
    };
    constexpr uint64_t ELF_HWCAP = 1;
    writeIdAndVal(AT_NULL, ZERO);
    writeIdAndVal(AT_EXECFN, auxData.ptrFileString);
    writeIdAndVal(AT_SECURE, HostAuxVal(AT_SECURE));
    writeIdAndVal(AT_RANDOM, auxData.ptrRandomBytes);
    writeIdAndVal(AT_CLKTCK, sysconf(_SC_CLK_TCK));
    writeIdAndVal(AT_HWCAP, ELF_HWCAP);
    writeIdAndVal(AT_EGID, getegid());
    writeIdAndVal(AT_GID, getgid());
    writeIdAndVal(AT_EUID, geteuid());
    writeIdAndVal(AT_UID, getuid());
    writeIdAndVal(AT_ENTRY, auxData.elfHeader->e_entry);
    writeIdAndVal(AT_FLAGS, ZERO);
    writeIdAndVal(AT_BASE, ZERO);
    writeIdAndVal(AT_PAGESZ, g_pageSize);
    writeIdAndVal(AT_PHNUM, auxData.elfHeader->e_phnum);
    writeIdAndVal(AT_PHENT, (sizeof(Elf64_Phdr)));
    writeIdAndVal(AT_PHDR, auxData.phdrVaddr);
}

static void WriteArgvEnvpTable(SoftMemory *arg, uint64_t *spAddr,
                               const std::vector<string> &strVec, uint64_t ptrString)
{
    WriteUint64(ZERO, arg, spAddr);
    std::stack<uint64_t> ptrTable;
    for (auto &str : strVec) {
        ptrTable.push(ptrString);
        ptrString += (str.length() + 1);
    }
    while (!ptrTable.empty()) {
        WriteUint64(ptrTable.top(), arg, spAddr);
        ptrTable.pop();
    }
}

static void InitSpField(SoftMemory *arg, uint64_t *spAddr, PrepareAuxData &auxData)
{
    constexpr int patternBytes = 8;
    constexpr int auxTableNeedEven = 2;
    *spAddr = *spAddr & STACK_ALIGN_8BYTE;
    WriteString(auxData.fileName, arg, spAddr);
    auxData.ptrFileString = *spAddr;
    Log("ptrFileString: 0x%lx\n", auxData.ptrFileString);
    for (auto it = auxData.spEnvp.rbegin(); it != auxData.spEnvp.rend(); ++it) {
        WriteString(*it, arg, spAddr);
    }
    auto ptrEnvpString = *spAddr;
    Log("ptrEnvpString: 0x%lx\n", ptrEnvpString);
    for (auto it = auxData.spArgv.rbegin(); it != auxData.spArgv.rend(); ++it) {
        WriteString(*it, arg, spAddr);
    }
    auto ptrArgvString = *spAddr;
    Log("ptrArgvString: 0x%lx\n", ptrArgvString);

    *spAddr = *spAddr & STACK_ALIGN_MASK;
    uint64_t randomBytes1 = static_cast<uint64_t>(std::rand()) << 32 | static_cast<uint64_t>(std::rand());
    uint64_t randomBytes2 = static_cast<uint64_t>(std::rand()) << 32 | static_cast<uint64_t>(std::rand());
    WriteUint64(randomBytes1, arg, spAddr);
    WriteUint64(randomBytes2, arg, spAddr);
    auxData.ptrRandomBytes = *spAddr;

    // To ensure that the stack is 16-byte aligned after the table is filled
    auto tableEnvpArgvSize = auxData.spArgv.size() + auxData.spEnvp.size() + 2 + 1;
    if (tableEnvpArgvSize % auxTableNeedEven != 0) {
        *spAddr -= patternBytes;
    }

    WriteAuxTable(arg, spAddr, auxData);
    Log("startAuxv: 0x%lx\n", *spAddr);
    WriteArgvEnvpTable(arg, spAddr, auxData.spEnvp, ptrEnvpString);
    Log("startEnvp: 0x%lx\n", *spAddr);
    WriteArgvEnvpTable(arg, spAddr, auxData.spArgv, ptrArgvString);
    Log("startArgv: 0x%lx\n", *spAddr);
    WriteUint64(auxData.spArgv.size(), arg, spAddr);
    Log("startArgc: 0x%lx\n", *spAddr);
}

static void InitArgv(std::vector<std::string> &spArgv, int argc, const std::vector<std::string> &argv)
{
    Log("argc number: %d\n", argc);
    for (auto &i : argv) {
        spArgv.emplace_back(i);
        Log("%s\n", i.c_str());
    }
}

static void InitEnvp(std::vector<std::string> &spEnvp)
{
    Log("%s\n", "envp: ");
    for (char** env = HostEnviron(); env != nullptr && *env != nullptr; ++env) {
        spEnvp.emplace_back(*env);
        Log("%s\n", *env);
    }
}

// -----------------------------------------------------------------
// elf_load
// -----------------------------------------------------------------
int elf_load(const char *filename, cb_mem_create fn_create, cb_mem_load fn_load, void *arg, uint64_t *start_addr,
    uint64_t *sp_addr, int argc, const std::vector<std::string> &argv)
{
    int fd = -1;
    Elf* e;
    Elf_Kind ek;
    Elf_Scn *scn;
    Elf_Data *data;
    Elf64_Shdr *shdr;
    Elf64_Ehdr *elfHeader;
    size_t shstrndx;
    uint64_t max_addr = 0;
    uint64_t sp_size = 0;
    uint64_t spTop = 0;
    uint64_t spBottom = 0;

    if (elf_version (EV_CURRENT) == EV_NONE) {
        return 0;
    }

    if ((fd = open (filename, O_RDONLY, 0)) < 0) {
        return 0;
    }

    if ((e = elf_begin (fd, ELF_C_READ, nullptr)) == nullptr) {
        return 0;
    }

    ek = elf_kind (e);
    if (ek != ELF_K_ELF) {
        return 0;
    }

    // Get section name header index
    if (elf_getshdrstrndx(e, &shstrndx) != 0) {
        return 0;
    }

    // Get entry point
    if (start_addr) {
        GElf_Ehdr _ehdr;
        GElf_Ehdr *ehdr = gelf_getehdr(e, &_ehdr);
        *start_addr = static_cast<uint64_t>(ehdr->e_entry);
    }

    int section_idx = 0;
    while ((scn = elf_getscn(e, section_idx)) != nullptr) {
        shdr = elf64_getshdr(scn);
        // Section which need allocating
        if ((shdr->sh_flags & SHF_ALLOC) && (shdr->sh_size > 0)) {
            data = elf_getdata(scn, nullptr);
            printf("Memory: 0x%lx - 0x%lx (Size=%ldKB) [%s]\n", shdr->sh_addr, shdr->sh_addr + shdr->sh_size - 1,
                shdr->sh_size / 1024, elf_strptr(e, shstrndx, shdr->sh_name));

            if (!fn_create(arg, shdr->sh_addr, shdr->sh_size)) {
                fprintf(stderr, "ERROR: Cannot allocate memory region\n");
                close (fd);
                return 0;
            }
            if (shdr->sh_addr + shdr->sh_size - 1 > max_addr) {
                max_addr = shdr->sh_addr + shdr->sh_size - 1;
            }

            if (shdr->sh_type == SHT_PROGBITS || shdr->sh_type == SHT_INIT_ARRAY || shdr->sh_type == SHT_FINI_ARRAY) {
                unsigned i;
                for (i = 0; i < shdr->sh_size; i++) {
                    if (!fn_load(arg, shdr->sh_addr + i, ((uint8_t*)data->d_buf)[i])) {
                        fprintf(stderr, "ERROR: Cannot write byte to 0x%08lx\n", shdr->sh_addr + i);
                        close (fd);
                        return 0;
                    }
                }
            }
        }

        section_idx++;
    }
    //gfrun stack space is fixed at 128M
    sp_size = 0x8001000;
    max_addr = (max_addr + 16) & STACK_ALIGN_MASK;

    if (*sp_addr) {
        *sp_addr = (*sp_addr + 16) & STACK_ALIGN_MASK;
        if (*sp_addr < (max_addr + sp_size)) {
            fprintf(stderr, "stack top addr requires more than virtual address space (0x%lx > 0x%lx)",
                    max_addr + sp_size, *sp_addr);
            return 0;
        }
        spBottom = *sp_addr - sp_size;
        spTop = *sp_addr;
    } else {
        spBottom = max_addr;
        spTop = max_addr + sp_size;
    }

    printf("Memory: 0x%lx - 0x%lx (Size=%ldKB) [%s]\n", spBottom, spTop, sp_size / 1024, "stack mem");
    if (!fn_create(arg, spBottom, sp_size)) {
        fprintf(stderr, "ERROR: Cannot allocate memory region\n");
        close (fd);
        return 0;
    }

    uint64_t heapSize = 0x8001000;
    uint64_t heapBottom = 0x4000802000; // qemu 默认的堆地址
    uint64_t heapTop = 0x4000802000 + 0x8001000;
    printf("Memory: 0x%lx - 0x%lx (Size=%ldKB) [%s]\n", heapBottom, heapTop, heapSize / 1024, "map mem");
    if (!fn_create(arg, heapBottom, heapSize)) {
        fprintf(stderr, "ERROR: Cannot allocate memory region\n");
        close (fd);
        return 0;
    }

    constexpr int eightBytes = 8;
    *sp_addr = spTop - eightBytes;

    const std::string fileName(filename, strlen(filename));
    std::vector<std::string> spArgv;
    std::vector<std::string> spEnvp;
    elfHeader = elf64_getehdr(e);

    uint64_t phdrVaddr = 0;
    uint64_t phdrOffset = 0;
    int phdrSize = 0;
    std::vector<uint8_t> phdrBuffer;
    Elf64_Phdr *phdr = elf64_getphdr(e);
    for (int i = 0; i < elfHeader->e_phnum; i++) {
        if (phdr[i].p_type == PT_PHDR) {
            phdrVaddr = phdr[i].p_vaddr;
            phdrOffset = phdr[i].p_offset;
            phdrSize = phdr[i].p_filesz;
            break;
        }
    }
    phdrBuffer.resize(phdrSize);
    auto byteRead = pread(fd, phdrBuffer.data(), phdrSize, phdrOffset);
    if (byteRead < 0) {
        fprintf(stderr, "pread failed\n");
        close(fd);
        return 0;
    }
    for (int i = 0; i < phdrSize; i++) {
        if (!fn_load(arg, phdrVaddr + i, phdrBuffer[i])) {
            fprintf(stderr, "ERROR: Cannot write byte to 0x%08lx\n", phdrVaddr + i);
            close (fd);
            return 0;
        }
    }

    InitArgv(spArgv, argc, argv);
    InitEnvp(spEnvp);
    SoftMemory *argMem = (SoftMemory *)arg;

    if (argMem->gfrunning) {
        // Only Init Sp Fields for GFRUN
        PrepareAuxData auxData;
        auxData.elfHeader = elfHeader;
        auxData.fileName = fileName;
        auxData.spArgv = std::move(spArgv);
        auxData.spEnvp = std::move(spEnvp);
        auxData.phdrVaddr = phdrVaddr;
        InitSpField(argMem, sp_addr, auxData);
    } else {
        *sp_addr = (*sp_addr + 16) & STACK_ALIGN_MASK;
    }
    Log("startSp: 0x%lx\n", *sp_addr);
    g_programBreak = spBottom;
    if (g_programBreak % 4096 != 0) {
        g_programBreak = (g_programBreak + 4096 - 1) & ~(4096 - 1);
    }
    Log("brkStart: %lx\n", g_programBreak);
    g_mmapAddr = heapBottom;
    Log("mmapStart: %lx\n", g_mmapAddr);

    section_idx++;

    elf_end (e);
    close (fd);

    return 1;
}

// -----------------------------------------------------------------
// elf_load for checkpoint
// -----------------------------------------------------------------
int elf_load_ckpt(const char *filename, cb_mem_create fn_create,
    cb_mem_load fn_load, void *arg, uint64_t *start_addr, const char* func_name)
{
    int fd = -1;
    Elf* e;
    Elf_Kind ek;
    Elf_Scn *scn;
    Elf_Data *data;
    Elf64_Shdr *shdr;
    size_t shstrndx;
    struct adderss_region stub;

    if (elf_version (EV_CURRENT) == EV_NONE) {
        return 0;
    }

    if ((fd = open (filename, O_RDONLY, 0)) < 0) {
        return 0;
    }

    if ((e = elf_begin (fd, ELF_C_READ, nullptr)) == nullptr) {
        return 0;
    }

    ek = elf_kind (e);
    if (ek != ELF_K_ELF) {
        return 0;
    }

    // Get section name header index
    if (elf_getshdrstrndx(e, &shstrndx) != 0) {
        return 0;
    }

    int section_idx = 0;
    while ((scn = elf_getscn(e, section_idx)) != nullptr) {
        shdr = elf64_getshdr(scn);
        // Get specific entry point
        if (shdr->sh_type == SHT_SYMTAB) {
            data = elf_getdata(scn, nullptr);
            int count = shdr->sh_size / shdr->sh_entsize;
            char* name = nullptr;
            for (int ii = 0; ii < count; ii++) {
                GElf_Sym sym;
                gelf_getsym(data, ii, &sym);
                name = elf_strptr(e, shdr->sh_link, sym.st_name);
                if (!strcmp(name, func_name)) {
                    *start_addr = sym.st_value;
                    printf("addr %lx func_name %s\n", sym.st_value, name);
                }
            }
        }

        // Section which need allocating
        if ((shdr->sh_flags & SHF_ALLOC) && (shdr->sh_size > 0)) {
            data = elf_getdata(scn, nullptr);
            const char *sec_name =  elf_strptr(e, shstrndx, shdr->sh_name);
            if (strcmp(".undefined", sec_name) == 0) {
                stub.start_addr = shdr->sh_addr;
                stub.end_addr = shdr->sh_addr + shdr->sh_size - 1;
                stub_region.push_back(stub);
            }
            printf("Memory: 0x%lx - 0x%lx (Size=%ldKB) [%s]\n", shdr->sh_addr,
                shdr->sh_addr + shdr->sh_size - 1, shdr->sh_size / 1024, elf_strptr(e, shstrndx, shdr->sh_name));

            if (!fn_create(arg, shdr->sh_addr, shdr->sh_size)) {
                fprintf(stderr, "ERROR: Cannot allocate memory region\n");
                close (fd);
                return 0;
            }

            if (shdr->sh_type == SHT_PROGBITS) {
                unsigned i;
                for (i = 0; i < shdr->sh_size; i++) {
                    if (!fn_load(arg, shdr->sh_addr + i, ((uint8_t*)data->d_buf)[i])) {
                        fprintf(stderr, "ERROR: Cannot write byte to 0x%08lx\n", shdr->sh_addr + i);
                        close (fd);
                        return 0;
                    }
                }
            }
        }

        section_idx++;
    }
    section_idx++;

    elf_end(e);
    close(fd);

    return 1;
}

// -----------------------------------------------------------------
// txt_load
// -----------------------------------------------------------------
int inst_data_load(void *arg, cb_mem_create fn_create, cb_mem_load fn_load, uint64_t addr, uint64_t data)
{
    for (int k = 0; k < 8; k++) {
        if (!fn_load(arg, addr + k, (data >> (8 * k)) & 0xff)) {
            if (!fn_create(arg, addr, 1000)) {
                fprintf(stderr, "ERROR: Cannot allocate memory region\n");
                return -1;
            }
            if (!fn_load(arg, addr + k, (data >> (8 * k)) & 0xff)) {
                fprintf(stderr, "ERROR: Cannot write byte to 0x%08lx\n", addr + k);
                return -1;
            }
        }
    }
    return 0;
}

int wrapper_block_load(void *arg, cb_mem_create fn_create, cb_mem_load fn_load, uint64_t addr)
{
    uint64_t inst_data = 0x0000000000002006;  // block for end simulation
    int ret = inst_data_load(arg, fn_create, fn_load, addr, inst_data);
    if (ret == -1) {
        assert(0);
    }
    return 0;
}

int txt_load(const char *filename, cb_mem_create fn_create, cb_mem_load fn_load,
    void *arg, uint64_t *start_addr, uint64_t* regs, std::map<uint64_t,
    uint64_t>& sysreg, int imemEn, uint64_t* end_addr, uint64_t *execBlockCnt)
{
    std::ifstream is;
    is.open(filename, std::ios::binary);
    if (is.fail()) {
        printf("Input File cannot open!\n");
        return 0;
    }
    uint64_t asize = 0;
    uint64_t addr = 0;
    uint64_t size = 0;
    uint64_t data = 0;
    nlohmann::json in_trace = nlohmann::json::parse(is);
    for (auto root = in_trace.begin(); root != in_trace.end(); root++) {
        if (imemEn) {
            asize = (*root)["ISection"].size();
            for (uint64_t i = 0; i < asize; i++) {
                addr = ((*root)["ISection"][i]["begin"]);
                size = ((*root)["ISection"][i]["end"]);
                size = size - addr;
                if (!fn_create(arg, addr, size)) {
                    fprintf(stderr, "ERROR: Cannot allocate memory region\n");
                    is.close();
                    return 0;
                }
            }
            asize = (*root)["Inst"].size();
            for (uint64_t i = 0; i < asize; i++) {
                addr = (*root)["Inst"][i]["pc"];
                data = (*root)["Inst"][i]["opcode"];
                for (int k = 0; k < 2; k++) {
                    if (!fn_load(arg, addr + k, (data >> (8 * k)) & 0xff)) {
                        fprintf(stderr, "ERROR: Cannot write byte to 0x%08lx\n", addr + k);
                        is.close();
                        return 0;
                    }
                }
            }
            if (root->find("ExecBlockCnt") != root->end()) {
                *execBlockCnt = (*root)["ExecBlockCnt"];
            }
	    // save wrapper block
        addr = (addr + 16) & (uint64_t)(-16);
	    *end_addr = addr;
        } else {
            asize = (*root)["DSection"].size();
            for (uint64_t i = 0; i < asize; i++) {
                addr = ((*root)["DSection"][i]["begin"]);
                size = ((*root)["DSection"][i]["end"]);
                size = size - addr;
                if (!fn_create(arg, addr, size)) {
                    fprintf(stderr, "ERROR: Cannot allocate dmemory region\n");
                    is.close();
                    return 0;
                }
            }
            asize = (*root)["memAcc"].size();
            for (uint64_t i = 0; i < asize; i++) {
                addr = (*root)["memAcc"][i]["addr"];
                data = (*root)["memAcc"][i]["data"];
                for (int k = 0; k < 8; k++) {
                    if (!fn_load(arg, addr + k, (data >> (8 * k)) & 0xff)) {
                        fprintf(stderr, "ERROR: Cannot write byte to 0x%08lx\n", addr + k);
                        is.close();
                        return 0;
                    }
                }
            }
            *start_addr = (*root)["StartPc"];
            asize = (*root)["Reg"].size();
            for (uint64_t i = 0; i < asize; i++) {
                regs[i] = (*root)["Reg"][i];
            }
            if (root->find("SysReg") != root->end()) {
                asize = (*root)["SysReg"].size();
                for (uint64_t i = 0; i < asize; i++) {
                    uint64_t regID = (*root)["SysReg"][i]["id"];
                    uint64_t regVal = (*root)["SysReg"][i]["value"];
                    sysreg[regID] = regVal;
                }
            }
            if (root->find("ExecBlockCnt") != root->end()) {
                *execBlockCnt = (*root)["ExecBlockCnt"];
            }
        }
    }
    is.close();
    return 1;
}

int load_txt(const char *filename, TxtArgs &txt_a) {
    std::ifstream is;
    EcallRet * rets = txt_a.e_rets;
    is.open(filename, std::ios::binary);
    if (is.fail()) {
        printf("Input File cannot open!\n");
        return 0;
    }
    nlohmann::json in_trace = nlohmann::json::parse(is);
    for (auto root = in_trace.begin(); root != in_trace.end(); root++) {
        if (!rets->size && root->find("Rets") != root->end()) {
            rets->size = (*root)["Rets"].size();
            rets->ecall_rets = new uint64_t[rets->size];
            for (uint64_t i = 0; i < rets->size; i++) {
                rets->ecall_rets[i] = (*root)["Rets"][i];
            }
        }
        if (root->find("bpc_trace") != root->end()) {
            size_t bsize =  (*root)["bpc_trace"].size();
            if (txt_a.bpcTrace->size() > 0) {
                continue; // initialized
            }
            for (uint64_t i = 0; i < bsize; i++) {
                txt_a.bpcTrace->emplace_back((*root)["bpc_trace"][i]);
            }
        }
        if (root->find("trace_regs") != root->end()) {
            if (txt_a.gregs->size() > 0) {
                continue; // initialized
            }
            size_t bsize =  (*root)["trace_regs"].size();
            for (uint64_t i = 0; i < bsize; i++) {
                TGPR treg;
                for (int j = 0; j < g_tregGprNum; j++) {
                    treg.gpr[j] = (*root)["trace_regs"][i][j];
                }
                txt_a.gregs->emplace_back(treg);
            }
        }
        if (root->find("ecall_regs") != root->end()) {
            if (txt_a.eregs->size() > 0) {
                continue; // initialized
            }
            size_t bsize =  (*root)["ecall_regs"].size();
            for (uint64_t i = 0; i < bsize; i++) {
                TGPR treg;
                for (int j = 0; j < g_tregGprNum; j++) {
                    treg.gpr[j] = (*root)["ecall_regs"][i][j];
                }
                txt_a.eregs->emplace_back(treg);
            }
        }
        if (root->find("ecall_mem") != root->end()) {
            if (txt_a.ldata->size() > 0) {
                continue;
            }
            size_t bsize =  (*root)["ecall_mem"].size();
            for (uint64_t i = 0; i < bsize; i++) {
                LData mdat;
                std::vector<LData> *ldat = new std::vector<LData>();
                for (size_t j = 0; j < (*root)["ecall_mem"][i].size(); j++) {
                    mdat.addr = (*root)["ecall_mem"][i][j]["addr"];
                    mdat.data = (*root)["ecall_mem"][i][j]["data"];
                    ldat->emplace_back(mdat);
                }
                txt_a.ldata->emplace_back(*ldat);
            }
        }
    }
    is.close();
    return 1;
}

// Get data from memory
static bool FetchDFromMem(std::pair<cb_mem_create, cb_mem_load>& memVerify, JsonElement* jsonElement,
                          uint64_t addrBegin, uint64_t addrData)
{
    for (int k = 0; k < g_memOffsetBase; k++) {
        bool dataRes = memVerify.second(jsonElement->arg, addrBegin + k, (addrData >> (g_offsetByte * k)) & 0xff);
        if (!dataRes) {
            fprintf(stderr, "ERROR: Cannot write byte to 0x%08lx\n", addrBegin + k);
            return false;
        }
    }
    return true;
}

// Get instructions from memory
static bool FetchIFromMem(std::pair<cb_mem_create, cb_mem_load>& memVerify, JsonElement* jsonElement,
                          uint64_t addrBegin, uint64_t addrData)
{
    for (int k = 0; k < g_instOffsetBase; k++) {
        bool instRes = memVerify.second(jsonElement->arg, addrBegin + k, (addrData >> (g_offsetByte * k)) & 0xff);
        if (!instRes) {
            fprintf(stderr, "ERROR: Cannot write byte to 0x%08lx\n", addrBegin + k);
            return false;
        }
    }
    return true;
}

static bool RecordCkptMemData(rapidjson::Value& root, std::pair<cb_mem_create, cb_mem_load>& memVerify,
                              JsonElement* jsonElement)
{
    uint64_t addrBegin = 0;
    uint64_t addrEnd = 0;
    uint64_t addrSize = 0;
    uint64_t addrData = 0;

    if (!root.HasMember("DSection") && !root.HasMember("memAcc") && !root.HasMember("ISection")) {
        return true;
    }
    if (root.HasMember("DSection") && root["DSection"].IsArray()) {
        rapidjson::Value& dSections = root["DSection"];
        addrBegin = addrEnd = addrSize = 0;
        for (rapidjson::SizeType i = 0; i < dSections.Size(); i++) {
            rapidjson::Value& section = dSections[i];
            addrBegin = section["begin"].GetUint64();
            addrEnd = section["end"].GetUint64();
            addrSize = addrEnd - addrBegin;
            if (!memVerify.first(jsonElement->arg, addrBegin, addrSize)) {
                fprintf(stderr, "ERROR: Cannot allocate Dmemory region\n");
                return false;
            }
        }
    }
    if (root.HasMember("memAcc") && root["memAcc"].IsArray()) {
        rapidjson::Value& memAcc = root["memAcc"];
        addrBegin = addrData = 0;
        for (rapidjson::SizeType i = 0; i < memAcc.Size(); i++) {
            rapidjson::Value& acc = memAcc[i];
            addrBegin = acc["addr"].GetUint64();
            addrData = acc["data"].GetUint64();
            bool dataRes = FetchDFromMem(memVerify, jsonElement, addrBegin, addrData);
            if (!dataRes) {
                return false;
            }
        }
    }
    if (root.HasMember("ISection") && root["ISection"].IsArray()) {
        rapidjson::Value& iSection = root["ISection"];
        addrBegin = addrEnd = addrSize = 0;
        for (rapidjson::SizeType i = 0; i < iSection.Size(); i++) {
            rapidjson::Value& section = iSection[i];
            addrBegin = section["begin"].GetUint64();
            addrEnd = section["end"].GetUint64();
            addrSize = addrEnd - addrBegin;
            if (!memVerify.first(jsonElement->arg, addrBegin, addrSize)) {
                fprintf(stderr, "ERROR: Cannot allocate Imemory region\n");
                return false;
            }
        }
    }
    return true;
}

static bool RecordCkptInstData(rapidjson::Value& root, std::pair<cb_mem_create, cb_mem_load>& memVerify,
                               JsonElement* jsonElement)
{
    uint64_t addrBegin = 0;
    uint64_t addrData = 0;

    if (root.HasMember("Inst") && root["Inst"].IsArray()) {
        rapidjson::Value& inst = root["Inst"];
        addrBegin = addrData = 0;
        for (rapidjson::SizeType i = 0; i < inst.Size(); i++) {
            rapidjson::Value& instVal = inst[i];
            addrBegin = instVal["pc"].GetUint64();
            addrData = instVal["opcode"].GetUint64();
            bool instRes = FetchIFromMem(memVerify, jsonElement, addrBegin, addrData);
            if (!instRes) {
                return false;
            }
        }
        *jsonElement->endAddr = (addrBegin + g_endOffsetByte) & g_endOffsetBase;
    }
    return true;
}

static bool RecordCkptRegData(rapidjson::Value& root, JsonElement* jsonElement, std::map<uint64_t, uint64_t>& sysregs)
{
    if (root.HasMember("Reg") && root["Reg"].IsArray()) {
        rapidjson::Value& reg = root["Reg"];
        for (rapidjson::SizeType i = 0; i < reg.Size(); i++) {
            rapidjson::Value& regVal = reg[i];
            jsonElement->regs[i] = regVal.GetUint64();
        }
    }
    if (root.HasMember("SysReg") && root["SysReg"].IsArray()) {
        rapidjson::Value& sysReg = root["SysReg"];
        for (rapidjson::SizeType i = 0; i < sysReg.Size(); i++) {
            rapidjson::Value& sysRegVal = sysReg[i];
            uint64_t regID = sysRegVal["id"].GetUint64();
            uint64_t regVal = sysRegVal["value"].GetUint64();
            sysregs[regID] = regVal;
        }
    }
    return true;
}

static bool RecordCkptExeInfo(rapidjson::Value& root, JsonElement* jsonElement)
{
    if (root.HasMember("StartPc") && root["StartPc"].IsInt()) {
        rapidjson::Value& startPc = root["StartPc"];
        *jsonElement->startAddr = startPc.GetUint64();
    }
    if (root.HasMember("ExecBlockCnt") && root["ExecBlockCnt"].IsInt()) {
        rapidjson::Value& execBlockCnt = root["ExecBlockCnt"];
        *jsonElement->execBlockCnt = execBlockCnt.GetUint64();
    }
    return true;
}

static bool RecordCkptTraceData(rapidjson::Value& root, EcallRet* rets, TxtArgs& txtArgs)
{
    if (!rets->size && root.HasMember("Rets") && root["Rets"].IsArray()) {
        rapidjson::Value& retsVal = root["Rets"];
        rets->size = static_cast<unsigned long>(retsVal.Size());
        rets->ecall_rets = new uint64_t[rets->size];
        for (uint64_t i = 0; i < rets->size; i++) {
            rapidjson::Value& retsVali = retsVal[i];
            rets->ecall_rets[i] = retsVali.GetUint64();
        }
    }
    if (root.HasMember("bpc_trace") && root["bpc_trace"].IsArray()) {
        if (txtArgs.bpcTrace->size() > 0) {
            return false;
        }
        rapidjson::Value& bpcTrace = root["bpc_trace"];
        for (rapidjson::SizeType i = 0; i < bpcTrace.Size(); i++) {
            rapidjson::Value& bpcTraceVal = bpcTrace[i];
            txtArgs.bpcTrace->emplace_back(bpcTraceVal.GetUint64());
        }
    }
    if (root.HasMember("trace_regs") && root["trace_regs"].IsArray()) {
        if (txtArgs.gregs->size() > 0) {
            return false;
        }
        rapidjson::Value& traceRegs = root["trace_regs"];
        for (rapidjson::SizeType i = 0; i < traceRegs.Size(); i++) {
            TGPR treg;
            rapidjson::Value& traceRegsVal = traceRegs[i];
            for (int j = 0; j < g_tregGprNum; j++) {
                treg.gpr[j] = traceRegsVal[j].GetUint64();
            }
            txtArgs.gregs->emplace_back(treg);
        }
    }
    return true;
}

static bool RecordCkptEcallData(rapidjson::Value& root, TxtArgs& txtArgs)
{
    if (root.HasMember("ecall_regs") && root["ecall_regs"].IsArray()) {
        if (txtArgs.eregs->size() > 0) {
            return false;
        }
        rapidjson::Value& ecallRegs = root["ecall_regs"];
        for (rapidjson::SizeType i = 0; i < ecallRegs.Size(); i++) {
            TGPR treg;
            rapidjson::Value& ecallRegsi = ecallRegs[i];
            for (rapidjson::SizeType j = 0; j < static_cast<rapidjson::SizeType>(g_tregGprNum); j++) {
                rapidjson::Value& ecallRegsj = ecallRegsi[j];
                treg.gpr[j] = ecallRegsj.GetUint64();
            }
            txtArgs.eregs->emplace_back(treg);
        }
    }
    if (root.HasMember("ecall_mem") && root["ecall_mem"].IsArray()) {
        if (txtArgs.ldata->size() > 0) {
            return false;
        }
        rapidjson::Value& ecallMem = root["ecall_mem"];
        for (rapidjson::SizeType i = 0; i < ecallMem.Size(); i++) {
            LData mdat;
            std::vector<LData> *ldat = new std::vector<LData>();
            rapidjson::Value& ecallMemVal = ecallMem[i];
            for (rapidjson::SizeType j = 0; j < ecallMemVal.Size(); j++) {
                rapidjson::Value& ecallMemValj = ecallMemVal[j];
                mdat.addr = ecallMemValj["addr"].GetUint64();
                mdat.data = ecallMemValj["data"].GetUint64();
                ldat->emplace_back(mdat);
            }
            txtArgs.ldata->emplace_back(*ldat);
        }
    }
    return true;
}

static bool RecordCkptData(rapidjson::Document& doc, std::pair<cb_mem_create, cb_mem_load>& memVerify,
                           JsonElement* jsonElement, std::map<uint64_t, uint64_t>& sysregs)
{
    rapidjson::Value& root = doc[0];
    assert(root.IsObject() && "The second layer of ckpt is an object!");

    if (!RecordCkptMemData(root, memVerify, jsonElement)) {
        return false;
    }
    if (!RecordCkptInstData(root, memVerify, jsonElement)) {
        return false;
    }
    if (!RecordCkptRegData(root, jsonElement, sysregs)) {
        return false;
    }
    if (!RecordCkptExeInfo(root, jsonElement)) {
        return false;
    }
    return true;
}

static bool TxtLoadTxtArgsByRjson(rapidjson::Document& doc, EcallRet* rets, TxtArgs& txtArgs)
{
    rapidjson::Value& root = doc[0];
    assert(root.IsObject() && "The second layer of ckpt is an object!");

    if (!RecordCkptTraceData(root, rets, txtArgs)) {
        return false;
    }
    if (!RecordCkptEcallData(root, txtArgs)) {
        return false;
    }
    return true;
}

// txt_load and load_txt merge execution
int TxtLoadTxt(std::pair<cb_mem_create, cb_mem_load>& memVerify, JsonElement* jsonElement,
               std::map<uint64_t, uint64_t>& sysregs, TxtArgs& txtArgs)
{
    std::ifstream ifs(jsonElement->filename);
    if (!ifs.is_open()) {
        std::cerr << "Input File cannot open!" << std::endl;
        return 0;
    }
    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document doc;
    // parse JSON data
    doc.ParseStream(isw);
    if (doc.HasParseError()) {
        std::cerr << "ParseError!" << std::endl;
        return 0;
    }

    assert(doc.IsArray() && "The outermost layer of ckpt is an array!");
    assert(doc[0].IsObject() && "The second layer of ckpt is an object!");
    // parse the JSON object
    if (!RecordCkptData(doc, memVerify, jsonElement, sysregs)) {
        ifs.close();
        return 0;
    }
    EcallRet* rets = txtArgs.e_rets;
    TxtLoadTxtArgsByRjson(doc, rets, txtArgs);
    ifs.close();
    return 1;
}

} // namespace JCore
