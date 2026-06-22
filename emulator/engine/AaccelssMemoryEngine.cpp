#include "SoftCore.h"

namespace JCore {
namespace {

void AssertNotTextStore(const char *kind, MInstFuncPtr inst, uint64_t addr, uint64_t data, uint64_t size)
{
    if (!is_text_region(addr, size)) {
        return;
    }

    ASSERT(false) << kind << " targets ELF text"
                  << " pc=0x" << std::hex << inst->pc
                  << " addr=0x" << addr
                  << " size=" << std::dec << size
                  << " data=0x" << std::hex << data
                  << "\n" << inst->Dump();
}

} // namespace

void SoftCore::AaccelssMemoryLoad(MInstFuncPtr inst)
{
    auto &threadState = threadStatus[inst->threadId];
    bool aaccelssTileReg = (inst->codeLen == EncodeLen::ENL_V) && (inst->accMemInfo->local);
    uint64_t addr = inst->accMemInfo->accMemAddr;
    uint64_t loadSize = GetLoadStoreBytes(inst->opcode);
    bool signedLoad = IsSignedLoad(inst->opcode);
    bool loadPair = IsLoadStorePair(inst->opcode);
    if (aaccelssTileReg) {
        inst->dsts[DST0_IDX]->data = threadState.archStatus.tileReg.Load(addr, 0, loadSize, signedLoad);
        if (loadPair) {
            inst->dsts[DST1_IDX]->data = threadState.archStatus.tileReg.Load(addr + loadSize, 0, loadSize, signedLoad);
        }
    } else {
        inst->dsts[DST0_IDX]->data = memory->Load(addr, loadSize, signedLoad);
        if (loadPair) {
            inst->dsts[DST1_IDX]->data = memory->Load(addr + loadSize, loadSize, signedLoad);
        }
    }
}

void SoftCore::AaccelssMemoryStore(MInstFuncPtr inst)
{
    auto &threadState = threadStatus[inst->threadId];
    bool aaccelssTileReg = (inst->codeLen == EncodeLen::ENL_V) && (inst->accMemInfo->local);
    uint64_t addr = inst->accMemInfo->accMemAddr;
    uint64_t storeSize = GetLoadStoreBytes(inst->opcode);
    bool storePair = IsLoadStorePair(inst->opcode);
    if (aaccelssTileReg) {
        threadState.archStatus.tileReg.Store(addr, 0, storeSize, inst->srcs[SRC0_IDX]->data);
        if (storePair) {
            threadState.archStatus.tileReg.Store(addr + storeSize, 0, storeSize, inst->srcs[SRC1_IDX]->data);
        }
    } else {
        AssertNotTextStore("SoftCore store", inst, addr, inst->srcs[SRC0_IDX]->data, storeSize);
        memory->Store(addr, inst->srcs[SRC0_IDX]->data, storeSize);
        if (storePair) {
            AssertNotTextStore("SoftCore store-pair", inst, addr + storeSize, inst->srcs[SRC1_IDX]->data, storeSize);
            memory->Store(addr + storeSize, inst->srcs[SRC1_IDX]->data, storeSize);
        }
    }
}

void SoftCore::AtomicAaccelssMemory(MInstFuncPtr inst)
{
    uint64_t addr = inst->accMemInfo->accMemAddr;
    uint64_t lsSize = GetLoadStoreBytes(inst->opcode);
    bool lsPair = IsLoadStorePair(inst->opcode);
    bool writeDst = !inst->dsts.empty();
    bool signedLoad = IsSignedLoad(inst->opcode);

    /* atomic load memory data */
    inst->atomicInfo->loadData = memory->Load(addr, lsSize, signedLoad);
    if (writeDst) {
        inst->dsts[DST0_IDX]->data = inst->atomicInfo->loadData;
    }
    if (lsPair) {
        inst->atomicInfo->loadData1 = memory->Load(addr + lsSize, lsSize, signedLoad);
        if (writeDst) {
            inst->dsts[DST0_IDX]->data = inst->atomicInfo->loadData1;
        }
    }
    inst->atomicInfo->loadDone = true;

    /* Perform the corresponding operation */
    bool ret = JCore::Calculate::MInstCalculator::Inst().CalculateMinst(*inst);
    if (!ret) {
        ASSERT(false) << "Atomic Calculate error:\n" << inst->Dump();
    }

    /* Write back data to memory */
    AssertNotTextStore("SoftCore atomic store", inst, addr, inst->atomicInfo->writeBackData, lsSize);
    memory->Store(addr, inst->atomicInfo->writeBackData, lsSize);
    if (lsPair) {
        AssertNotTextStore("SoftCore atomic store-pair", inst, addr + lsSize, inst->atomicInfo->writeBackData, lsSize);
        memory->Store(addr + lsSize, inst->atomicInfo->writeBackData, lsSize);
    }
}

void SoftCore::CacheMaintainAaccelssMemory(MInstFuncPtr inst) const
{
    if (!OpcodeIsDCZVA(inst->opcode)) {
        return;
    }
    // For now, only dc.zva will be here
    uint64_t addr = inst->accMemInfo->accMemAddr;
    uint64_t storeSize = GetLoadStoreBytes(inst->opcode);
    uint64_t data = 0;
    /* memory 层的接口不支持一次 64B 所以需要拆分成多个 8B 的请求 */
    ASSERT(storeSize % NUM8 == 0);
    for (size_t i = 0; i < storeSize / NUM8; ++i) {
        AssertNotTextStore("SoftCore cache-zero store", inst, addr, data, NUM8);
        memory->Store(addr, data, NUM8);
        addr += NUM8;
    }
}
}
