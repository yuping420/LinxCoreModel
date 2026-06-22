#include "SoftCore.h"

namespace JCore {
void SoftCore::GetSrcData(MInstFuncPtr inst, BlockFuncPtr &currentBlock, uint64_t lane)
{
    for (auto &src : inst->srcs) {
        GetSrcOperandData(inst, currentBlock, lane, src);
    }
    InstGroup grp = OpcodeManager::Inst().GetOpcodeGroup(inst->opcode);
    switch (grp) {
        case InstGroup::REDUCE: {
            if (inst->dsts[DST0_IDX]->type == OperandType::OPD_RO) {
                GetSrcOperandData(inst, currentBlock, lane, inst->dsts[DST0_IDX]);
                inst->reduceInfo->data = inst->dsts[DST0_IDX]->data;
                inst->reduceInfo->first = false;
            }
            break;
        }
        case InstGroup::BLOCK_IO: {
            if (inst->opcode == Opcode::OP_B_IOR && !inst->dsts.empty()) {
                GetSrcOperandData(inst, currentBlock, lane, inst->dsts[DST0_IDX]);
            }
            break;
        }
        case InstGroup::BRANCH: {
            if (inst->opcode == Opcode::OP_B_Z || inst->opcode == Opcode::OP_B_NZ) {
                inst->brInfo->predMask = currentBlock->localArchStatus.GetLaneMask();
            }
            break;
        }
        default:
            break;
    }
}

void SoftCore::SetDstData(MInstFuncPtr inst, BlockFuncPtr &currentBlock, uint64_t lane)
{
    for (auto &dst : inst->dsts) {
        SetDstOperandData(inst, currentBlock, lane, dst);
    }
    InstGroup grp = OpcodeManager::Inst().GetOpcodeGroup(inst->opcode);
    switch (grp) {
        case InstGroup::SETC:
            currentBlock->barg.taken = inst->setcInfo->setcTaken;
            break;
        case InstGroup::SSR: {
            if (inst->opcode == Opcode::OP_SETC_TGT) {
                currentBlock->barg.target = inst->setcInfo->setcTarget;
            } else if (inst->ssrInfo->vld) {
                SetSystemReg(inst->ssrInfo->ssrId, inst->ssrInfo->data, inst->threadId);
            }
            break;
        }
        case InstGroup::EXECUTE_CONTROL: {
            if (inst->opcode == Opcode::OP_ACRC) {
                // 当前只支持用户态
                ASSERT(inst->srcs[0]->data < static_cast<uint64_t>(ACRC_REQUEST_TYPE::SCT_INVAL));
                currentBlock->acrcReqType = static_cast<ACRC_REQUEST_TYPE>(inst->srcs[0]->data);
            }
        }
        default:
            break;
    }
}

void SoftCore::GetSrcOperandData(MInstFuncPtr inst, BlockFuncPtr &currentBlock, uint64_t lane, OperandPtr src)
{
    auto &threadState = threadStatus[inst->threadId];
    switch (src->type) {
        case OperandType::OPD_ZERO:
            src->data = 0;
            break;
        case OperandType::OPD_GREG:
            src->data = threadState.archStatus.gpr[src->value];
            break;
        case OperandType::OPD_TLINK:
        case OperandType::OPD_ULINK:
            src->data = currentBlock->localArchStatus.scalarGeneralReg[src->type].BackToFrontIndex(src->value);
            break;
        case OperandType::OPD_VTLINK:
        case OperandType::OPD_VULINK:
        case OperandType::OPD_VMLINK:
        case OperandType::OPD_VNLINK:
            src->data = currentBlock->localArchStatus.vectorGeneralReg[lane][src->type].BackToFrontIndex(src->value);
            break;
        case OperandType::OPD_TILE_TLINK:
        case OperandType::OPD_TILE_ULINK:
        case OperandType::OPD_TILE_MLINK:
        case OperandType::OPD_TILE_NLINK:
        case OperandType::OPD_TILE_ACC:
        case OperandType::OPD_TILE_STACK:
        case OperandType::OPD_TILE_DLINK:
            // tileReg will be processed by HandleBIOT.
            break;
        case OperandType::OPD_TA_REG:
        case OperandType::OPD_TB_REG:
        case OperandType::OPD_TC_REG:
        case OperandType::OPD_TD_REG:
        case OperandType::OPD_TE_REG:
        case OperandType::OPD_TF_REG:
        case OperandType::OPD_TG_REG:
        case OperandType::OPD_TH_REG: {
            uint64_t idx = static_cast<uint64_t>(src->type) - static_cast<uint64_t>(OperandType::OPD_TA_REG);
            src->data = currentBlock->srcTile[idx]->baseAddr;
            break;
        }
        case OperandType::OPD_TO_REG:
        case OperandType::OPD_TO1_REG:
        case OperandType::OPD_TO2_REG:
        case OperandType::OPD_TO3_REG:
        case OperandType::OPD_TO_GPR_REG: {
            uint64_t idx = static_cast<uint64_t>(src->type) - static_cast<uint64_t>(OperandType::OPD_TO_REG);
            if (idx >= currentBlock->dstTile.size()) {
                idx = 0;
            }
            src->data = currentBlock->dstTile[idx]->baseAddr;
            break;
        }
        case OperandType::OPD_RI: {
            ASSERT(src->value < currentBlock->srcData.size());
            src->data = currentBlock->srcData[src->value];
            break;
        }
        case OperandType::OPD_RO:
            ASSERT(src->value < currentBlock->dstData.size());
            src->data = currentBlock->dstData[src->value];
            break;
        case OperandType::OPD_PREDMASK:
            src->data = 0;
            for (size_t i = 0 ; i < 64; i++) {
                if (currentBlock->localArchStatus.predMask[i].GetMask()) {
                    src->data |= (1ULL << i);
                }
            }
            break;
        case OperandType::OPD_SIMM:
            src->data = static_cast<int64_t>(src->value);
            break;
        case OperandType::OPD_UIMM:
            src->data = src->value;
            break;
        case OperandType::OPD_SYS:
            src->data = threadState.archStatus.sysreg[static_cast<SystemReg>(src->value)];
            if (inst->opcode == Opcode::OP_LSRGET) {
                switch (static_cast<LocalStatusRegID>(src->value)) {
                    case LocalStatusRegID::LSR_BPC:
                        src->data = currentBlock->barg.GetBPC();
                        break;
                    case LocalStatusRegID::LSR_BPCN:
                        src->data = currentBlock->barg.GetTarget();
                        break;
                    case LocalStatusRegID::LSR_OTHERS:
                        src->data = currentBlock->barg.GetLSROthers();
                        break;
                    default:
                        ASSERT(false && "Error Get:reserved local status reg id");
                }
            }
            break;
        case OperandType::OPD_CARG:
        case OperandType::OPD_LB0:
            src->data = currentBlock->lb0;
            break;
        case OperandType::OPD_LB1:
            src->data = currentBlock->lb1;
            break;
        case OperandType::OPD_LB2:
            src->data = currentBlock->lb2;
            break;
        case OperandType::OPD_LC0:
            src->data = currentBlock->GetLC0(currentBlock->completedGroup, config.laneNum, lane);
            break;
        case OperandType::OPD_LC1:
            src->data = currentBlock->GetLC1(currentBlock->completedGroup, config.laneNum, lane);
            break;
        case OperandType::OPD_LC2:
            src->data = currentBlock->GetLC2(currentBlock->completedGroup, config.laneNum, lane);
            break;
        default:
            break;
    }
    if (src->type != OperandType::OPD_PREDMASK) {
        switch (src->width) {
            case OperandWidth::OPDW_W: {
                src->data &= MASK_BIT32;
            } break;
            case OperandWidth::OPDW_H: {
                src->data &= MASK_BIT16;
            } break;
            case OperandWidth::OPDW_B: {
                src->data &= MASK_BIT8;
            } break;
            default:
                break;
        }
        OperandCvtVal(src);
    }
}

void SoftCore::SetDstOperandData(MInstFuncPtr inst, BlockFuncPtr &currentBlock, uint64_t lane, OperandPtr dst)
{
    ASSERT(inst != nullptr && "SetDstOperandData requires an instruction");
    ASSERT(dst != nullptr) << "SetDstOperandData received null destination"
        << " pc=0x" << std::hex << inst->pc
        << " opcode=" << GetOpcodeName(inst->opcode) << "\n"
        << inst->Dump();
    bool needsCurrentBlock =
        dst->type == OperandType::OPD_TLINK || dst->type == OperandType::OPD_ULINK ||
        dst->type == OperandType::OPD_VTLINK || dst->type == OperandType::OPD_VULINK ||
        dst->type == OperandType::OPD_VMLINK || dst->type == OperandType::OPD_VNLINK ||
        dst->type == OperandType::OPD_RO || dst->type == OperandType::OPD_PREDMASK;
    ASSERT(!needsCurrentBlock || currentBlock != nullptr)
        << "SetDstOperandData destination requires an active block"
        << " pc=0x" << std::hex << inst->pc
        << " opcode=" << GetOpcodeName(inst->opcode)
        << " dstType=" << GetOperandType(dst->type)
        << " dstValue=" << std::dec << dst->value << "\n"
        << inst->Dump();

    auto &threadState = threadStatus[inst->threadId];
    switch (dst->width) {
        case OperandWidth::OPDW_W: {
            dst->data &= MASK_BIT32;
        } break;
        case OperandWidth::OPDW_H: {
            dst->data &= MASK_BIT16;
        } break;
        case OperandWidth::OPDW_B: {
            dst->data &= MASK_BIT8;
        } break;
        default:
            break;
    }
    switch (dst->type) {
        case OperandType::OPD_ZERO:
            // Readonly;
            break;
        case OperandType::OPD_GREG:
            threadState.archStatus.gpr[dst->value] = dst->data;
            break;
        case OperandType::OPD_TLINK:
        case OperandType::OPD_ULINK:
            currentBlock->localArchStatus.scalarGeneralReg[dst->type].WriteDropOldest(dst->data);
            break;
        case OperandType::OPD_VTLINK:
        case OperandType::OPD_VULINK:
        case OperandType::OPD_VMLINK:
        case OperandType::OPD_VNLINK:
            currentBlock->localArchStatus.vectorGeneralReg[lane][dst->type].WriteDropOldest(dst->data);
            break;
        case OperandType::OPD_TILE_TLINK:
        case OperandType::OPD_TILE_ULINK:
        case OperandType::OPD_TILE_MLINK:
        case OperandType::OPD_TILE_NLINK:
        case OperandType::OPD_TILE_ACC:
        case OperandType::OPD_TILE_STACK:
        case OperandType::OPD_TILE_DLINK:
            // tileReg will be processed by HandleBIOT.
            break;
        case OperandType::OPD_TA_REG:
        case OperandType::OPD_TB_REG:
        case OperandType::OPD_TC_REG:
        case OperandType::OPD_TD_REG:
        case OperandType::OPD_TE_REG:
        case OperandType::OPD_TF_REG:
        case OperandType::OPD_TG_REG:
        case OperandType::OPD_TH_REG:
        case OperandType::OPD_TO_REG:
        case OperandType::OPD_TO1_REG:
        case OperandType::OPD_TO2_REG:
        case OperandType::OPD_TO3_REG:
        case OperandType::OPD_TO_GPR_REG:
        case OperandType::OPD_RI:
        case OperandType::OPD_SIMM:
        case OperandType::OPD_UIMM:
            // ReadOnly
            break;
        case OperandType::OPD_RO:
            ASSERT(dst->value < currentBlock->barg.dstATag.size());
            ASSERT(dst->value < currentBlock->dstData.size())
                << "RO destination index exceeds block destination data"
                << " pc=0x" << std::hex << inst->pc
                << " opcode=" << GetOpcodeName(inst->opcode)
                << " dstValue=" << std::dec << dst->value
                << " dstATagSize=" << currentBlock->barg.dstATag.size()
                << " dstDataSize=" << currentBlock->dstData.size() << "\n"
                << inst->Dump() << "\n"
                << currentBlock->Dump();
            currentBlock->dstData[dst->value] = dst->data;
            break;
        case OperandType::OPD_PREDMASK:
            if (inst->codeLen == EncodeLen::ENL_V) {
                currentBlock->localArchStatus.predMask[lane].SetMask(dst->data != 0ULL);
            } else {
                for (int i = 0; i < 64; ++i) {
                    currentBlock->localArchStatus.predMask[i].SetMask(((dst->data >> i) & 1ULL) != 0ULL);
                }
            }
            break;
        case OperandType::OPD_SYS:
            threadState.archStatus.sysreg[static_cast<SystemReg>(dst->value)] = dst->data;
            break;
        case OperandType::OPD_CARG:
        case OperandType::OPD_LB0:
        case OperandType::OPD_LB1:
        case OperandType::OPD_LB2:
        case OperandType::OPD_LC0:
        case OperandType::OPD_LC1:
        case OperandType::OPD_LC2:
            // ReadOnly;
            break;
        default:
            break;
    }
}
}
