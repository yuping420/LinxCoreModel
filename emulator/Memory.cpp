#include "Memory.h"

#include <cassert>
#include <cstring>
#include <iostream>

#include "ELF.h"
#include "utils/error.h"
using namespace std;
#define MEM_DEBUG 0

namespace JCore {

constexpr static uint64_t MEM_ALIGN = 8;

static int mem_create_bank(void *arg, uint64_t base, uint64_t size);
static int mem_load(void *arg, uint64_t addr, uint8_t data);
static bool gAllowTextStore = false;

bool is_text_region(uint64_t addr, uint64_t size);
extern std::map<uint64_t, uint64_t> text_region;

bool is_text_region(uint64_t addr, uint64_t size)
{
    for (auto it = text_region.begin(); it != text_region.end(); it++) {
        if (((it->first <= addr) && (it->second > addr)) &&
            ((it->first <= (addr + size)) && (it->second >= (addr + size)))) {
            return true;
        }
    }
    return false;
}

static uint64_t gen_mask(uint64_t begin_loc, int size)
{
    if (begin_loc == 0 && size == 0) {
        return 0;
    }
    if (begin_loc > 7 || size == 0 || int(size + begin_loc) > 8) {
        assert(0 && "Misalign aaccelss fails!");
    }
    return (~0ULL) >> (64 - (size << 3)) << (begin_loc << 3);
}

uint64_t SoftMemory::HostLoad(uint64_t address, int width, bool signedLoad)
{
    uint64_t data = 0;
    switch (width) {
        case 8:
            data = *((uint64_t*)address);
            break;
        case 4:
            data = *((uint32_t*)address);
            if (signedLoad) {
                if (data & (1 << 31)) {
                    data |= 0xFFFFFFFF00000000;
                }
            }
            break;
        case 2:
            data = *((uint16_t*)address);
            if (signedLoad) {
                if (data & (1 << 15)) {
                    data |= 0xFFFFFFFFFFFF0000;
                }
            }
            break;
        case 1:
            data = *((uint8_t*)address);
            if (signedLoad) {
                if (data & (1 << 7)) {
                    data |= 0xFFFFFFFFFFFFFF00;
                }
            }
            break;
        default:
            break;
    }
    return data;
}

void SoftMemory::HostStore(uint64_t address, uint64_t data, int width)
{
    switch (width) {
        case 8:
            *((uint64_t*)address) = data;
            break;
        case 4:
            *((uint32_t*)address) = static_cast<uint32_t>(data);
            break;
        case 2:
            *((uint16_t*)address) = static_cast<uint16_t>(data);
            break;
        case 1:
            *((uint8_t*)address) = static_cast<uint8_t>(data);
            break;
        default:
            break;
    }
}

struct addr SoftMemory::LoadElf(JsonElement& jsonElement, std::map<uint64_t, uint64_t>& sysregs, uint64_t spTop,
                                int argc, const std::vector<std::string> &argv)
{
    struct addr addr_ret;
    addr_ret.sp_addr = spTop;
    addr_ret.start_addr = 0;
    addr_ret.local_mem_addr = 0;
    addr_ret.end_addr = static_cast<uint64_t>(-1);
    std::pair<cb_mem_create, cb_mem_load> memVerify = {mem_create_bank, mem_load};
    gAllowTextStore = true;
    bool elfLoadRes = elf_load(jsonElement.filename, mem_create_bank, mem_load,
                               this, &addr_ret.start_addr, &addr_ret.sp_addr, argc, argv);
    gAllowTextStore = false;
    if (elfLoadRes) {
        return addr_ret;
    } else {
        addr_ret.sp_addr = static_cast<uint64_t>(-1);
        TxtArgs rets(&e_rets);
        rets.bpcTrace = &this->bpcTrace;
        rets.gregs = &this->traceRegs;
        rets.eregs = &this->ecallRegs;
        rets.ldata = &this->ldata;
        jsonElement.startAddr = &addr_ret.start_addr;
        jsonElement.endAddr = &addr_ret.end_addr;
        jsonElement.execBlockCnt = &execBlockCnt;
        jsonElement.arg = this;
        gAllowTextStore = true;
        int res = TxtLoadTxt(memVerify, &jsonElement, sysregs, rets);
        gAllowTextStore = false;
        if (res == 0) {
            fprintf(stderr, "ERROR: Either ELF binary or Json file open fail%s\n", jsonElement.filename);
            exit(-1);
        }
        return addr_ret;
    }
    return addr_ret;
}

uint64_t SoftMemoryBank::DataLoad(uint64_t address, int width)
{
    auto offset = address % MEM_ALIGN;
    uint64_t data = 0;
    auto handleWidth = 0uL;
    if (offset + width > MEM_ALIGN) {
        handleWidth = MEM_ALIGN - offset;
        auto mask = gen_mask(offset, handleWidth);
        data = ((mem[address / MEM_ALIGN] & mask) >> (MEM_ALIGN * offset));
        address += handleWidth;
        width -= handleWidth;
        offset = 0;
    }
    auto mask = gen_mask(offset, width);
    data |= (((mem[address / MEM_ALIGN] & mask) << (8 * handleWidth)) >> (8 * offset));
    return data;
}

void SoftMemoryBank::DataStore(uint64_t address, uint64_t data, int width)
{
    auto offset = address % MEM_ALIGN;
    if (offset + width > MEM_ALIGN) {
        auto handleWidth = MEM_ALIGN - offset;
        auto mask = gen_mask(offset, handleWidth);
        mem[address / MEM_ALIGN] = (mem[address / MEM_ALIGN] & ~mask) | ((data << (MEM_ALIGN * offset)) & mask);
        address += handleWidth;
        width -= handleWidth;
        data >>= (8 * handleWidth);
        offset = 0;
    }
    auto mask = gen_mask(offset, width);
    mem[address / MEM_ALIGN] = (mem[address / MEM_ALIGN] & ~mask) | ((data << (8 * offset)) & mask);
}

uint64_t SoftMemoryBank::Load(uint64_t address, int width, bool signedLoad)
{
    uint64_t data = 0;
    address = address - align_base;
    data = DataLoad(address, width);
    switch (width) {
        case 8:
            break;
        case 4:
            if (signedLoad) {
                if (data & (1 << 31)) {
                    data |= 0xFFFFFFFF00000000;
                }
            }
            break;
        case 2:
            if (signedLoad) {
                if (data & (1 << 15)) {
                    data |= 0xFFFFFFFFFFFF0000;
                }
            }
            break;
        case 1:
            if (signedLoad) {
                if (data & (1 << 7)) {
                    data |= 0xFFFFFFFFFFFFFF00;
                }
            }
            break;
        default:
            break;
    }
    if (MEM_DEBUG)
        cout << "*** load: addr: " << hex << address +align_base << " data: " << data << " width:" << width << endl;
    return data;
}

void SoftMemoryBank::Store(uint64_t address, uint64_t data, int width)
{
    if (MEM_DEBUG)
        cout << "*** store: addr: " << hex << address << " data: " << data << " width:" << width << endl;
    // TODO: ported from 32-bit arch. Need to update for 64-bit arch.
    address = address - align_base;
    DataStore(address, data, width);
}

bool SoftMemoryBank::InRange(uint64_t address)
{
    return (address >= base) && (address < (base + size));
}

// -----------------------------------------------------------------
// mem_create: Create memory region
// -----------------------------------------------------------------
static int mem_create_bank(void *arg, uint64_t base, uint64_t size)
{
    SoftMemory *mem = (SoftMemory *)arg;
    return mem->CreateMemoryBank(base, size);
}
// -----------------------------------------------------------------
// mem_load: Load byte into memory
// -----------------------------------------------------------------
static int mem_load(void *arg, uint64_t addr, uint8_t data)
{
    SoftMemory *mem = (SoftMemory *)arg;
    return mem->Store(addr, data, 1);
}

bool SoftMemory::CreateMemoryBank(uint64_t baseAddr, uint64_t len)
{
    auto appendBank = [this](uint64_t alignVaddr) {
        if (addrToBankIndex.find(alignVaddr)==addrToBankIndex.end()) {
            addrToBankIndex[alignVaddr] = banks.size();
            banks.push_back(SoftMemoryBank(alignVaddr, SECTION_INTERVAL, alignVaddr));
        }
    };
    auto updateBaseAddr = [](uint64_t &baseAddr, uint64_t &len, uint64_t bytes) {
        len = len > bytes ? len - bytes : 0;
        baseAddr += SECTION_INTERVAL;
    };
    auto offset = baseAddr % SECTION_INTERVAL;
    baseAddr = baseAddr - offset;
    appendBank(baseAddr);
    updateBaseAddr(baseAddr, len, SECTION_INTERVAL - offset);
    while (len > 0) {
        appendBank(baseAddr);
        updateBaseAddr(baseAddr, len, SECTION_INTERVAL);
    }

    return true;
}

auto SoftMemory::GetBank(uint64_t address) ->decltype(addrToBankIndex.begin())
{
    auto bankIndex = addrToBankIndex.end();
    bankIndex = addrToBankIndex.find(address - address % SECTION_INTERVAL);
    return bankIndex;
}

bool SoftMemory::LookupBank(uint64_t address)
{
    bool findbank = true;
    if (GetBank(address) == addrToBankIndex.end()) {
        findbank = false;
    }
    return findbank;
}

bool SoftMemory::Store(uint64_t address, uint64_t data, int width)
{
    // std::cout<<__FUNCTION__<<"addr 0x"<<hex<<address<<", data:"<<data<<", width:"<<dec<<width<<std::endl;
    ASSERT(gAllowTextStore || !is_text_region(address, width))
        << "SoftMemory runtime store targets ELF text"
        << " addr=0x" << std::hex << address
        << " width=" << std::dec << width
        << " data=0x" << std::hex << data;

    auto storeToBank = [this](uint64_t address, uint64_t data, int width) {
        auto bankIndex = GetBank(address);
        if (bankIndex==addrToBankIndex.end()) {
            return false;
        }
        auto &bank = banks[bankIndex->second];
        bank.Store(address, data, width);
        return true;
    };
    auto offset = address % SECTION_INTERVAL;
    auto handleWidth = 0uL;
    if (offset + width > SECTION_INTERVAL) {
        handleWidth = SECTION_INTERVAL - offset;
        auto mask = gen_mask(0, handleWidth);
        if (!storeToBank(address, data & mask, handleWidth)) {
            return false;
        }
    }
    if (!storeToBank(address + handleWidth, data >> (handleWidth * MEM_ALIGN), width - handleWidth)) {
        return false;
    }
    return true;
}

uint64_t SoftMemory::Load(uint64_t address, int width, bool signedLoad)
{
    auto loadFromBank = [this](uint64_t address, int width, bool signedLoad)->pair<bool, uint64_t> {
        auto bankIndex = GetBank(address);
        if (bankIndex==addrToBankIndex.end()) {
            return {false, 0};
        }
        auto &bank = banks[bankIndex->second];
        auto data = bank.Load(address, width, signedLoad);
        return {true, data};
    };
    auto handleSigned = [](uint64_t &data, int width, bool signedLoad) {
        switch (width) {
            case 8:
                break;
            case 4:
                if (signedLoad) {
                    if (data & (1 << 31)) {
                        data |= 0xFFFFFFFF00000000;
                    }
                }
                break;
            case 2:
                if (signedLoad) {
                    if (data & (1 << 15)) {
                        data |= 0xFFFFFFFFFFFF0000;
                    }
                }
                break;
            case 1:
                if (signedLoad) {
                    if (data & (1 << 7)) {
                        data |= 0xFFFFFFFFFFFFFF00;
                    }
                }
                break;
            default:
                break;
        }
    };
    auto offset = address % SECTION_INTERVAL;
    uint64_t data = 0;
    if (offset + width > SECTION_INTERVAL) {
        auto handleWidth = SECTION_INTERVAL - offset;
        auto ret = loadFromBank(address, handleWidth, 0);
        if (!ret.first) {
            return 0;
        }
        data = ret.second;
        ret = loadFromBank(address + handleWidth, width - handleWidth, 0);
        if (!ret.first) {
            return 0;
        }
        data |= ret.second << (handleWidth * MEM_ALIGN);
        handleSigned(data, width, signedLoad);
    } else {
        auto ret = loadFromBank(address, width, signedLoad);
        if (!ret.first) {
            return 0;
        }
        data = ret.second;
    }
    return data;
}

} // namespace JCore
