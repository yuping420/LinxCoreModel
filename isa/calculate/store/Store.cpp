#include "Store.h"

namespace JCore {
namespace Calculate {
namespace Store {
/*
 * Uniformly placing the aaccelss addresses on dsts[DST0_IDX].
 * After the memory returns the data, it overwrites the addresses.
 * load PR and load PO type operation: ISA definition requires that the address be placed in dsts[DST1_IDX],
 * so both dsts[DST0_IDX] and dsts[DST1_IDX] need to be written
 */

static bool CalcVectorStoreAddr(MInst &inst)
{
    if (inst.accMemInfo->continuous) {
        if (inst.srcs.size() != SRC4_IDX) {
            return false;
        }
        inst.accMemInfo->accMemAddr = inst.srcs[SRC1_IDX]->data + inst.srcs[SRC2_IDX]->data + inst.srcs[SRC3_IDX]->data;
    } else {
        if (inst.srcs.size() != SRC3_IDX) {
            return false;
        }
        inst.accMemInfo->accMemAddr = inst.srcs[SRC1_IDX]->data + inst.srcs[SRC2_IDX]->data;
    }
    return true;
}

static bool CalcStoreAddr(MInst &inst)
{
    if (inst.codeLen == EncodeLen::ENL_V) {
        return CalcVectorStoreAddr(inst);
    }
    if (inst.srcs.size() != SRC3_IDX) {
        return false;
    }
    inst.accMemInfo->accMemAddr = inst.srcs[SRC1_IDX]->data + inst.srcs[SRC2_IDX]->data;
    return true;
}

static bool CalcStorePCRAddr(MInst &inst)
{
    if (inst.srcs.size() != SRC2_IDX) {
        return false;
    }
    inst.accMemInfo->accMemAddr = inst.pc + inst.srcs[SRC1_IDX]->data;
    return true;
}

static bool CalcStorePairAddr(MInst &inst)
{
    if (inst.srcs.size() != SRC4_IDX) {
        return false;
    }
    inst.accMemInfo->accMemAddr = inst.srcs[SRC2_IDX]->data + inst.srcs[SRC3_IDX]->data;
    return true;
}

static bool CalcStorePRAddr(MInst &inst)
{
    if (inst.srcs.size() != SRC3_IDX || inst.dsts.size() != DST1_IDX) {
        return false;
    }
    inst.accMemInfo->accMemAddr = inst.srcs[SRC1_IDX]->data + inst.srcs[SRC2_IDX]->data;
    inst.dsts[DST0_IDX]->data = inst.srcs[SRC1_IDX]->data + inst.srcs[SRC2_IDX]->data;
    return true;
}

static bool CalcStorePOAddr(MInst &inst)
{
    if (inst.srcs.size() != SRC3_IDX || inst.dsts.size() != DST1_IDX) {
        return false;
    }
    inst.accMemInfo->accMemAddr = inst.srcs[SRC1_IDX]->data;
    inst.dsts[DST0_IDX]->data = inst.srcs[SRC1_IDX]->data + inst.srcs[SRC2_IDX]->data;
    return true;
}

Handler Lookup(Opcode op)
{
    static const std::unordered_map<Opcode, Handler> STORE_TABLE = std::unordered_map<Opcode, Handler> {
        // Group: Store Register Offset
        {Opcode::OP_SB, &CalcStoreAddr},
        {Opcode::OP_SH, &CalcStoreAddr},
        {Opcode::OP_SW, &CalcStoreAddr},
        {Opcode::OP_SD, &CalcStoreAddr},
        {Opcode::OP_SH_U, &CalcStoreAddr},
        {Opcode::OP_SW_U, &CalcStoreAddr},
        {Opcode::OP_SD_U, &CalcStoreAddr},
        // Group: Store Immediate Offset
        {Opcode::OP_SBI, &CalcStoreAddr},
        {Opcode::OP_SHI, &CalcStoreAddr},
        {Opcode::OP_SWI, &CalcStoreAddr},
        {Opcode::OP_SDI, &CalcStoreAddr},
        {Opcode::OP_SHI_U, &CalcStoreAddr},
        {Opcode::OP_SWI_U, &CalcStoreAddr},
        {Opcode::OP_SDI_U, &CalcStoreAddr},
        // Group: Store Symbol-PC-Relative
        {Opcode::OP_SB_PCR, &CalcStorePCRAddr},
        {Opcode::OP_SH_PCR, &CalcStorePCRAddr},
        {Opcode::OP_SW_PCR, &CalcStorePCRAddr},
        {Opcode::OP_SD_PCR, &CalcStorePCRAddr},
        // 48 Bits Group: Store Pair
        {Opcode::OP_SBP, &CalcStorePairAddr},
        {Opcode::OP_SHP, &CalcStorePairAddr},
        {Opcode::OP_SWP, &CalcStorePairAddr},
        {Opcode::OP_SDP, &CalcStorePairAddr},
        {Opcode::OP_SHP_U, &CalcStorePairAddr},
        {Opcode::OP_SWP_U, &CalcStorePairAddr},
        {Opcode::OP_SDP_U, &CalcStorePairAddr},
        {Opcode::OP_SBIP, &CalcStorePairAddr},
        {Opcode::OP_SHIP, &CalcStorePairAddr},
        {Opcode::OP_SWIP, &CalcStorePairAddr},
        {Opcode::OP_SDIP, &CalcStorePairAddr},
        {Opcode::OP_SHIP_U, &CalcStorePairAddr},
        {Opcode::OP_SWIP_U, &CalcStorePairAddr},
        {Opcode::OP_SDIP_U, &CalcStorePairAddr},
        // 48 Bits Group: Store Pre-index
        {Opcode::OP_SB_PR, &CalcStorePRAddr},
        {Opcode::OP_SH_PR, &CalcStorePRAddr},
        {Opcode::OP_SW_PR, &CalcStorePRAddr},
        {Opcode::OP_SD_PR, &CalcStorePRAddr},
        {Opcode::OP_SH_UPR, &CalcStorePRAddr},
        {Opcode::OP_SW_UPR, &CalcStorePRAddr},
        {Opcode::OP_SD_UPR, &CalcStorePRAddr},
        {Opcode::OP_SBI_PR, &CalcStorePRAddr},
        {Opcode::OP_SHI_PR, &CalcStorePRAddr},
        {Opcode::OP_SWI_PR, &CalcStorePRAddr},
        {Opcode::OP_SDI_PR, &CalcStorePRAddr},
        {Opcode::OP_SHI_UPR, &CalcStorePRAddr},
        {Opcode::OP_SWI_UPR, &CalcStorePRAddr},
        {Opcode::OP_SWI_UPR, &CalcStorePRAddr},
        // 48 Bits Group: Store Post-index
        {Opcode::OP_SB_PO, &CalcStorePOAddr},
        {Opcode::OP_SH_PO, &CalcStorePOAddr},
        {Opcode::OP_SW_PO, &CalcStorePOAddr},
        {Opcode::OP_SD_PO, &CalcStorePOAddr},
        {Opcode::OP_SH_UPO, &CalcStorePOAddr},
        {Opcode::OP_SW_UPO, &CalcStorePOAddr},
        {Opcode::OP_SD_UPO, &CalcStorePOAddr},
        {Opcode::OP_SBI_PO, &CalcStorePOAddr},
        {Opcode::OP_SHI_PO, &CalcStorePOAddr},
        {Opcode::OP_SWI_PO, &CalcStorePOAddr},
        {Opcode::OP_SDI_PO, &CalcStorePOAddr},
        {Opcode::OP_SHI_UPO, &CalcStorePOAddr},
        {Opcode::OP_SWI_UPO, &CalcStorePOAddr},
        {Opcode::OP_SDI_UPO, &CalcStorePOAddr},
        // Group: Store Bridge
        {Opcode::OP_SB_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SH_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SW_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SD_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SH_U_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SW_U_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SD_U_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SBI_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SHI_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SWI_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SDI_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SHI_U_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SWI_U_BRG, &CalcVectorStoreAddr},
        {Opcode::OP_SDI_U_BRG, &CalcVectorStoreAddr},
    };
    auto it = STORE_TABLE.find(op);
    return (it == STORE_TABLE.end()) ? 0 : it->second;
}

bool CalculateStore(MInst &inst)
{
    Handler fn = Lookup(inst.opcode);
    return fn ? fn(inst) : false;
}

} // namespace Store
} // namespace Calculate
} // namespace JCore
