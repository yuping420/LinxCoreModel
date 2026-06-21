#include "SoftCore.h"

namespace JCore {

void SoftCore::AccumulateBlockInfo(MInstFuncPtr inst, BlockFuncPtr &currentBlock)
{
    auto &threadState = threadStatus[inst->threadId];
    if (OpcodeManager::Inst().GetOpcodeGroup(inst->opcode) == InstGroup::BLOCK_SPLIT) {
        if (currentBlock == nullptr) {
            currentBlock = std::make_shared<BlockFunc>();
            currentBlock->threadId = inst->threadId;
            currentBlock->execSeq = threadState.blockId;
            currentBlock->localArchStatus.InitScalarGeneralReg(config.scalarMaxIndexDistance,
                                                               config.scalarMaxOutputNum);
        } else {
            if (currentBlock->simt && currentBlock->bTextOffset != 0) {
                ProcessSIMTEnd(inst, currentBlock);
            } else {
                currentBlock->SetBlockIsComplete();
                HandleNextBSTART(inst, currentBlock);
            }
            return;
        }
    }
    switch (inst->opcode) {
        case Opcode::OP_BSTOP:
            currentBlock->SetBlockIsComplete();
            break;
        case Opcode::OP_BSTART_DIRECT:
        case Opcode::OP_BSTART_COND:
        case Opcode::OP_BSTART: {
            currentBlock->startMinst = inst;
            currentBlock->HandleBSTART(*inst);
            if (IsBlockTypeNeedVReg(currentBlock->blockType)) {
                currentBlock->simt = true;
            }
            break;
        }
        case Opcode::OP_B_IOR:
            currentBlock->HandleBIOR(*inst);
            break;
        case Opcode::OP_B_IOT: {
            auto &tileReg = threadState.archStatus.tileReg;
            for (size_t i = 1; i < inst->srcs.size(); i++) {
                const auto &entry = tileReg.LookUp(inst->srcs[i]->type,
                                            inst->srcs[i]->value + currentBlock->GetDstTileCount(inst->srcs[i]->type));
                inst->srcs[i]->baseAddr = entry.sAddr_;
                inst->srcs[i]->size = entry.realSize_;
                inst->srcs[i]->tileInfo = entry.tileInfo;
            }
            uint64_t existDsts = currentBlock->dstTile.size();
            for (size_t j = 0; j < inst->dsts.size(); j++) {
                auto &entry = tileReg.AllocTile(inst->dsts[j]->type, inst->dsts[j]->size);
                inst->dsts[j]->baseAddr = entry.sAddr_;
                TileInfoPtr tileInfo = currentBlock->GetDstTileInfo(existDsts + j);
                entry.tileInfo = tileInfo;
                inst->dsts[j]->tileInfo = tileInfo;
            }
            currentBlock->HandleBIOT(*inst);
            break;
        }
        case Opcode::OP_B_DIM:
        case Opcode::OP_B_DIMI:
            currentBlock->HandleBDIM(*inst);
            break;
        case Opcode::OP_B_TEXT:
            currentBlock->HandleBTEXT(*inst);
            if (verbose) {
                LOG_INFO << "BTextPC:0x" << std::hex << currentBlock->bTextPC;
            }
            if (currentBlock->simt) {
                PrepareSIMT(currentBlock);
            }
            break;
        case Opcode::OP_B_CATR:
            currentBlock->HandleBCATR(*inst);
            break;
        case Opcode::OP_B_DATR:
            currentBlock->HandleBDATR(*inst);
            break;
        case Opcode::OP_MCOPY:
            currentBlock->HandleMCOPY(*inst);
            break;
        case Opcode::OP_MSET:
            currentBlock->HandleMSET(*inst);
            break;
        case Opcode::OP_FENTRY:
            currentBlock->HandleFENTRY(*inst);
            break;
        case Opcode::OP_FEXIT:
            currentBlock->HandleFEXIT(*inst);
            break;
        case Opcode::OP_FRET_RA:
            currentBlock->HandleFRETRA(*inst);
            break;
        case Opcode::OP_FRET_STK:
            currentBlock->HandleFRETSTK(*inst);
            break;
        case Opcode::OP_B_HINT_PREFETCH:
        case Opcode::OP_B_HINT_TRACE:
            currentBlock->HandleBHINT(*inst);
            break;
        case Opcode::OP_B_IOD:
        default:
            ASSERT(false && "Unsupport B.PARAMETER");
    }
}

void SoftCore::HandleNextBSTART(MInstFuncPtr inst, BlockFuncPtr block)
{
    if (inst->opcode == Opcode::OP_BSTOP) {
        return;
    }
    inst->check = false;
    switch (block->branchType) {
        case BranchType::BLK_BR_DIRECT:
        case BranchType::BLK_BR_CALL:
        case BranchType::BLK_BR_IND:
        case BranchType::BLK_BR_ICALL:
        case BranchType::BLK_BR_RET:
            inst->opcode = Opcode::OP_BSTOP;
            inst->srcs.clear();
            inst->dsts.clear();
            break;
        case BranchType::BLK_BR_COND: {
            if (block->barg.taken) {
                inst->opcode = Opcode::OP_BSTOP;
                inst->srcs.clear();
                inst->dsts.clear();
            } else {
                inst->skipCurrentMinst = true;
            }
            break;
        }
        default:
            inst->skipCurrentMinst = true;
            break;
    }
}

void SoftCore::PrepareSIMT(BlockFuncPtr &currentBlock)
{
    currentBlock->simtFirst = true;
    currentBlock->GetGroupNum(config.laneNum);
    currentBlock->completedBodyIters = 0;
    currentBlock->completedGroup = 0;
    if (verbose) {
        LOG_INFO << "Total Block Body Iteration number:" << std::dec << currentBlock->totalBodyIters;
        LOG_INFO << "Total Group Number:" << std::dec << currentBlock->totalGroupNum;
        LOG_INFO << "Split Group Mode:" << (currentBlock->IsReduceDimension() ? "Reduce" : "Multi") << " Dimension";
    }
}

void SoftCore::InitSIMTRegInfo(BlockFuncPtr &currentBlock)
{
    currentBlock->execLane = currentBlock->GetCurrentGroupIters(currentBlock->completedBodyIters, config.laneNum);
    currentBlock->localArchStatus.InitPredMask(config.laneNum, currentBlock->execLane);
    currentBlock->localArchStatus.InitVectorGeneralReg(config.laneNum, config.vectorMaxIndexDistance,
                                                       config.vectorMaxOutputNum, currentBlock->vregMode);
    if (verbose) {
        std::stringstream oss;
        for (size_t i = 0; i < currentBlock->localArchStatus.predMask.size(); i++) {
            if ((i % __CHAR_BIT__) == 0) {
                oss << " ";
            }
            oss << currentBlock->localArchStatus.predMask[currentBlock->localArchStatus.predMask.size() - 1 - i].Dump();
        }
        LOG_INFO << "\nGroup:" << std::dec << currentBlock->completedGroup << " Mask:" << oss.str();
    }
}

void SoftCore::ProcessSIMTEnd(MInstFuncPtr inst, BlockFuncPtr &currentBlock)
{
    (void)inst;
    currentBlock->completedBodyIters += currentBlock->execLane;
    currentBlock->completedGroup++;
    if (currentBlock->completedBodyIters == currentBlock->totalBodyIters) {
        currentBlock->bIsComplete = true;
    } else {
        currentBlock->simtFirst = true;
    }
}
}
