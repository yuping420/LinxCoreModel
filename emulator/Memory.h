#ifndef _MEMORY_
#define _MEMORY_

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace JCore {

bool is_text_region(uint64_t addr, uint64_t size);

const int GPR_NUM = 24;

struct mem_data {
    bool has_load;
    uint64_t data;
    uint64_t mask;
};

struct addr {
    uint64_t start_addr;
    uint64_t sp_addr;
    uint64_t local_mem_addr;
    uint64_t end_addr;
};

typedef struct EcallRet {
    uint64_t *ecall_rets = nullptr;
    size_t size = 0;

    EcallRet& operator=(const EcallRet& rightOp)
    {
        if (this != &rightOp) {
            if (ecall_rets) {
                delete ecall_rets;
                ecall_rets = nullptr;
            }
            size = rightOp.size;
            ecall_rets = new uint64_t[size];
            for (uint64_t i = 0; i < size; i++) {
                ecall_rets[i] = rightOp.ecall_rets[i];
            }
        }
        return *this;
    }

    EcallRet(const EcallRet& rightOp)
    {
        *this = rightOp;
    }

    EcallRet() = default;
} EcallRet;

struct TGPR {
    uint64_t gpr[GPR_NUM];
};
struct LData {
    uint64_t addr;
    uint64_t data;
};

typedef struct TxtArgs {
    EcallRet *e_rets;
    std::vector<uint64_t> *bpcTrace;
    std::vector<TGPR> *gregs;
    std::vector<TGPR> *eregs;
    std::vector<std::vector<LData>> *ldata;
    TxtArgs(EcallRet *rets):e_rets(rets) {}
} TxtArgs;

static uint64_t NUM_8 = 8;

/* Emulation for A Bank of Memory */
class SoftMemoryBank {
private:
    std::vector<uint64_t> mem;

public:
    uint64_t    base;
    uint64_t    size;
    uint64_t    align_base;

    SoftMemoryBank(uint64_t base, uint64_t size, uint64_t align_base)
    : mem(size / NUM_8, 0), base(base), size(size), align_base(align_base) {}

    uint64_t Load(uint64_t address, int width, bool signedLoad);

    void Store(uint64_t address, uint64_t data, int width);

    bool InRange(uint64_t address);

    uint64_t DataLoad(uint64_t address, int width);

    void DataStore(uint64_t address, uint64_t data, int width);
};

class SoftMemory;
struct JsonElement {
    const char*                         filename;
    uint64_t*                           startAddr;
    uint64_t*                           regs;
    uint64_t*                           endAddr;
    uint64_t*                           execBlockCnt;
    SoftMemory*                         arg;
} ;
using JsonElement = struct JsonElement;

template <class T>
class SimQueue;
class SoftMemory {
private:
    /* \brief Soft memory is organized as banked memory */
    std::vector<SoftMemoryBank> banks;
    std::unordered_map<uint64_t, uint64_t> addrToBankIndex;
    constexpr static uint64_t SECTION_INTERVAL = 1024;
    EcallRet e_rets;
    std::vector<uint64_t> bpcTrace;

    auto GetBank(uint64_t address) ->decltype(addrToBankIndex.begin());
    void RecieveTTransLdReq();
    void RecieveTTransStReq();

public:
    bool verbose = false;
    std::vector<TGPR>  traceRegs;
    std::vector<TGPR>  ecallRegs;
    std::vector<std::vector<LData>> ldata;
    uint64_t execBlockCnt = -1;
    bool gfrunning = false;

    SoftMemory() {};
    void Build(uint64_t addr, uint32_t size);
    void Work();

   TGPR & InitEcall(uint64_t eidx) {
        return ecallRegs[eidx];
   }
    void Reset(void);

    virtual uint64_t Load(uint64_t address, int width, bool signedLoad);

    virtual bool Store(uint64_t address, uint64_t data, int width);

    /* Create a memory region */
    bool CreateMemoryBank(uint64_t baseAddr, uint64_t len);

    struct addr LoadElf(JsonElement& jsonElement, std::map<uint64_t, uint64_t>& sysregs, uint64_t spTop = 0,
                        int argc = 0, const std::vector<std::string> &argv = {});

    /* Load from Host Memory */
    virtual uint64_t HostLoad(uint64_t address, int width, bool signedLoad);

    /* Store to Host Memory */
    virtual void HostStore(uint64_t address, uint64_t data, int width);

    uint64_t GetEcallRet(uint32_t idx) {
        if (idx >= e_rets.size) {
            return (uint64_t)-1;
        }
        return e_rets.ecall_rets[idx];
    }

    virtual bool LookupBank(uint64_t address);
    virtual ~SoftMemory(){
        if (e_rets.ecall_rets) {
            delete[] e_rets.ecall_rets;
            e_rets.ecall_rets = nullptr;
            e_rets.size = 0;
        }
        addrToBankIndex.clear();
    }
};

} // namespace JCore

#endif
