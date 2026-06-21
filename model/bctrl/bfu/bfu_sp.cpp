#include "bctrl/bfu/bfu_sp.h"

#include <cassert>
#include <cstdint>

#include "ISACommon/OpcodeManager.h"
#include "bctrl/bfu/bfu.h"
#include "bctrl/bfu/bfu_common.h"
#include "bctrl/bfu/bfu_logger.h"
#include "bctrl/bfu/bfu_utils.h"
#include "../common/utils/log.h"

namespace JCore {

namespace NS_CORE {

namespace {

bool IsStdFallDescriptor(SimInst const& inst)
{
    if (!inst || inst->opcode != Opcode::OP_BSTART || inst->srcs.size() <= SRC1_IDX) {
        return false;
    }
    return inst->codeLen == EncodeLen::ENL_W &&
           inst->srcs[SRC0_IDX]->data == static_cast<uint64_t>(BlockType::BLK_TYPE_STD) &&
           inst->srcs[SRC1_IDX]->data == static_cast<uint64_t>(BranchType::BLK_BR_FALL);
}

bool IsInlineFixedTargetDescriptor(SimInst const& openHeader, SimInst const& inst)
{
    // LLVM emits a 32-bit in-body STD/FALL descriptor in direct-boot finisher blocks.
    // A compressed C.BSTART.STD after a direct header is still a real block boundary.
    return openHeader && openHeader->GetBranchType() == BranchType::BLK_BR_DIRECT &&
           IsStdFallDescriptor(inst);
}

} // namespace

void StaticPredictor::Predict(PtrFB const& fb) {
    ASSERT(fb->stid < globalDec.size());
    PreDcodeInfo &predec = fb->global ? globalDec[fb->stid] : localDec[fb->pipe_id];
    bool first_after_redirect = fb->first_after_redirect;
    if (fb->global) {
        if (predec.taken) {
            predec.Reset();
        }
    }
    predec.fbid_global = fb->fbid;
    predec.fbid_local =  fb->fbid_local;

    bool hasDirectOrCall = false;
    bool callSetInFB = false;
    for (pos_t pos = utils->CalcPosInFB(fb->va); pos < BFU_BANDWIDTH; ++pos) {
        if (!fb->bin[pos].vld) {
            break;
        }
        addr_t va = utils->CalcPC(fb->va, pos);
        uint64_t bin = ((uint64_t)(fb->bin[pos].bin) << (8 * predec.binWidth)) | predec.preBin;
        // Instructions more than 2 byte, concat
        if (IsConcat(bin, predec.binWidth + (uint64_t)MIN_BUNDLE_SIZE)) {
            predec.preBin = bin;
            predec.preVa = predec.binWidth == 0 ? va : predec.preVa;
            predec.binWidth += MIN_BUNDLE_SIZE;
            fb->machineInst[pos] = std::make_shared<SimInstInfo>();
            fb->machineInst[pos]->bfuInfo = std::make_shared<BFUMachineInfo>(fb->fbid, va, nullptr, fb->create_time,
                bfu->GetSim()->getCycles(), fb->f1_time, fb->bhc_fetch_time, bfu->GetSim()->getCycles());
            fb->machineInst[pos]->pc = va;
            fb->machineInst[pos]->bfuInfo->global = fb->global;
            fb->sp_info.attr[pos] = BranchType::BLK_BR_FALL;
            fb->sp_info.tgt[pos] = utils->NextBlockPC(va);
            fb->machineInst[pos]->bfuInfo->concat = true;
            fb->machineInst[pos]->bfuInfo->vld = false;
            if (cfg->debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F3) << "SP concat fbid=" << dec << fb->fbid
                    << ", fbid_local=" << fb->fbid_local << hex << ", pc=0x" << va
                    << dec << ", pos=" << pos << hex << ", bin=0x" << fb->bin[pos].bin;
            }
            logger->debug("SP", F3, "concat fbid=%d, fbid_local=%d, hid=%d, pc=0x%x, pos=%d, bin=0x%x\n",
                fb->fbid, fb->fbid_local, fb->machineInst[pos]->bfuInfo->hid, va, pos, fb->bin[pos].bin);
            continue;
        }
        SimInst minst = std::make_shared<SimInstInfo>();
        SPInfoPtr spInfo = std::make_shared<SPMInstInfo>();
        minst->DecodeBin(bin, predec.binWidth + MIN_BUNDLE_SIZE);
        minst->binary = bin;
        minst->pc = (predec.binWidth == 0) ? va : predec.preVa;
        minst->stid = fb->stid;
        if (OpcodeInInstGroup(minst->opcode, InstGroup::BLOCK_SPLIT) &&
            IsInlineFixedTargetDescriptor(predec.machineInst, minst)) {
            spInfo->isInst = true;
            fb->hid = predec.machineInst->bfuInfo->hid;
            fb->sp_info.attr[pos] = BranchType::BLK_BR_FALL;
            fb->sp_info.tgt[pos] = utils->NextBlockPC(va);
            fb->machineInst[pos] = minst;
            fb->machineInst[pos]->bfuInfo = std::make_shared<BFUMachineInfo>(*predec.machineInst->bfuInfo);
            fb->machineInst[pos]->bfuInfo->spInfo = spInfo;
            fb->machineInst[pos]->bfuInfo->vld = false;
            fb->machineInst[pos]->bfuInfo->resolved = true;
            fb->machineInst[pos]->bfuInfo->global = fb->global;
            fb->machineInst[pos]->bfuInfo->fbid = fb->fbid;
            fb->machineInst[pos]->bfuInfo->fbid_local = fb->fbid_local;
            predec.preBin = 0;
            predec.binWidth = 0;
            continue;
        }
        if (OpcodeInInstGroup(minst->opcode, InstGroup::BLOCK_SPLIT) && minst->opcode != Opcode::OP_BSTOP) {
            BlockCommandPtr cmd = std::make_shared<BlockCommand>();
            cmd->AccumulateBlockInfo(minst);
            minst->bcmd = cmd;
            spInfo->isInst = false;
            addr_t va = utils->CalcPC(fb->va, pos);
            minst->bfuInfo = std::make_shared<BFUMachineInfo>(fb->fbid, va, spInfo, fb->create_time, bfu->GetSim()->getCycles(),
                    fb->f1_time, fb->bhc_fetch_time, bfu->GetSim()->getCycles());
            fb->machineInst[pos] = minst;
            fb->machineInst[pos]->bfuInfo->global = fb->global;
            fb->sp_info.attr[pos] = minst->GetBranchType();
            fb->sp_info.tgt[pos] = utils->CalcStaticTarget(minst);
            fb->machineInst[pos]->bfuInfo->fetch_end = IsTemplate(bin, predec.binWidth + MIN_BUNDLE_SIZE);
            if (predec.machineInst && !predec.machineInst->bfuInfo->spInfo->bsizeVld) {
                utils->SetBsize(predec.machineInst, utils->NextBlockPC(predec.machineInst->GetBundlePosPC()), fb->machineInst[pos]->pc);
            }
            predec.Reset();
            predec.machineInst = fb->machineInst[pos];
        } else if (predec.machineInst == nullptr) {
            // Eceptional route
            // 切片里没访问的内存是 0x0，可能后面还有能够解码的 bin，所以继续往后
            if (bin == 0x0) {
                continue;
            }
            predec.Reset();
            break;
        } else {
            spInfo->isInst = true;
            fb->hid = predec.machineInst->bfuInfo->hid;
            fb->sp_info.attr[pos] = BranchType::BLK_BR_FALL;
            // Get information from stash minst
            fb->machineInst[pos] = minst;
            fb->machineInst[pos]->bfuInfo = std::make_shared<BFUMachineInfo>(*predec.machineInst->bfuInfo);
            fb->machineInst[pos]->bfuInfo->resolved = true;
            fb->machineInst[pos]->bfuInfo->spInfo = spInfo;
            // Get call/return target for ras
            if (minst->IsSetRet()  && !hasDirectOrCall) {
                fb->sp_info.add_pc_vld = true;
                fb->sp_info.pos = pos;
                fb->sp_info.return_tgt = minst->GetSetRetDst();
                hasDirectOrCall = true;
                if (predec.machineInst != nullptr) {
                    if (utils->CheckEqual(predec.machineInst->bfuInfo->fbid, predec.machineInst->bfuInfo->fbid_local, fb->fbid, fb->fbid_local)) {
                        pos_t pos_bstart = utils->CalcPosInFB(predec.machineInst->GetBundlePosPC(), fb->va);
                        fb->sp_info.attr[pos_bstart] = utils->IsIndirect(fb->sp_info.attr[pos_bstart]) ?
                            BranchType::BLK_BR_ICALL : BranchType::BLK_BR_CALL;
                        predec.machineInst->bcmd->branchType = utils->IsIndirect(predec.machineInst->GetBranchType()) ?
                            BranchType::BLK_BR_ICALL : BranchType::BLK_BR_CALL;
                        callSetInFB = true;
                    }
                }
            }
        }
        if (cfg->debug_enable && callSetInFB) {
            callSetInFB = false;
            pos_t posStart = utils->CalcPosInFB(predec.machineInst->GetBundlePosPC(), fb->va);
            LOG_INFO_M(Unit::BFU, Stage::F3) << std::dec << "SP set target in fb fbid=" << predec.machineInst->bfuInfo->fbid
                << ", fbid_local=" << predec.machineInst->bfuInfo->fbid_local << ", hid=" << predec.machineInst->bfuInfo->hid
                << ", bstart_pos=" << posStart << ", sp_type=" << GetBlockBranchTypeName(fb->sp_info.attr[posStart]).c_str()
                << ", type=" << GetBlockBranchTypeName(predec.machineInst->GetBranchType()).c_str()
                << ", set_tgt_pos=" << fb->sp_info.pos << std::hex << ", bstart_pc=0x" << predec.machineInst->pc
                << ", bstart_bundle_pc=0x" << predec.machineInst->GetBundlePosPC() << ", return_tgt=0x" << fb->sp_info.return_tgt;
        }
        fb->machineInst[pos]->bfuInfo->global = fb->global;
        fb->machineInst[pos]->bfuInfo->fbid = fb->fbid;
        fb->machineInst[pos]->bfuInfo->fbid_local = fb->fbid_local;
        if (!minst->bfuInfo->spInfo->isInst && first_after_redirect) {
            fb->machineInst[pos]->bfuInfo->first_after_redirect = true;
        }

        if (fb->machineInst[pos]->opcode == Opcode::OP_BSTOP) {
            fb->machineInst[pos]->isLastInBlock = true;
            fb->machineInst[pos]->bfuInfo->spInfo->isLast = true;
            fb->sp_info.attr[pos] = BranchType::BLK_BR_FALL;
            fb->sp_info.tgt[pos] = utils->NextBlockPC(va);
            if (predec.machineInst && !predec.machineInst->bfuInfo->spInfo->bsizeVld) {
                utils->SetBsize(predec.machineInst, utils->NextBlockPC(predec.machineInst->GetBundlePosPC()),
                    utils->NextBlockPC(fb->machineInst[pos]->GetBundlePosPC()));
            }
            // bend, then remove the instruction minst
            predec.Reset();
        }
        // For nuke before/after branch
        if (OpcodeIsSetc(minst->opcode)) {
            predec.afterBranch = true;
        }
        fb->machineInst[pos]->bfuInfo->spInfo->afterBranch = predec.afterBranch;
        if (cfg->debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::F3) << "SP fbid=" << dec << fb->fbid << ", fbid_local=" << fb->fbid_local
                << ", hid=" << fb->machineInst[pos]->bfuInfo->hid << hex << ", pc=0x" << fb->machineInst[pos]->GetBundlePosPC()
                << ", type=" << GetBlockBranchTypeName(fb->machineInst[pos]->GetBranchType()).c_str()
                << ", sp_type=" << GetBlockBranchTypeName(fb->sp_info.attr[pos]).c_str() << dec << ", pos=" << pos
                << hex << ", tgt=0x" << fb->sp_info.tgt[pos] << ", bin=0x" << fb->bin[pos].bin
                << ", whole_bin=0x" << bin << ", inst=" << fb->machineInst[pos]->bfuInfo->spInfo->isInst
                << ", bstop=" << fb->machineInst[pos]->bfuInfo->spInfo->isLast;
        }
        logger->debug("SP", F3, "fbid=%d, fbid_local=%d, hid=%d, pc=0x%x, type=%s, sp_type=%d, pos=%d, tgt=0x%x, bin=0x%x, whole_bin=0x%x, inst=%d, last=%d\n",
            fb->fbid, fb->fbid_local, fb->machineInst[pos]->bfuInfo->hid, fb->machineInst[pos]->GetBundlePosPC(),
            GetBlockBranchTypeName(fb->machineInst[pos]->GetBranchType()).c_str(), fb->sp_info.attr[pos],
            pos, fb->sp_info.tgt[pos], fb->bin[pos].bin, bin, fb->machineInst[pos]->bfuInfo->spInfo->isInst, fb->machineInst[pos]->bfuInfo->spInfo->isLast);
        predec.preBin = 0;
        predec.binWidth = 0;
    }
    predec.taken = fb->ubtb_info.taken;
    fb->dec_info = predec;
    if (fb->first_after_nuke) {
        for (pos_t pos = 0; pos < BFU_BANDWIDTH; pos++) {
            if (!fb->machineInst[pos]) {
                continue;
            }
            fb->machineInst[pos]->bfuInfo->first_after_nuke = true;
        }
    }
}

void StaticPredictor::PredictLB(PtrFB const& fb)
{
    auto& sp_info = fb->sp_info;
    for (pos_t pos=0; pos < BFU_BANDWIDTH; pos++) {
        if (fb->machineInst[pos] == nullptr) {
            continue;
        }
        sp_info.attr[pos] = fb->machineInst[pos]->GetBranchType();
        sp_info.tgt[pos] = utils->CalcStaticTarget(fb->machineInst[pos]);
    }
}

static bool Is16Prefix(uint64_t bin)
{
    return GetBits(bin, FIELD_16_TYPE_END, FIELD_16_TYPE_BEGIN) == PREFIX_TYPE;
}

static bool Is32Prefix(uint64_t bin)
{
    return GetBits(bin, FIELD_32_TYPE_END, FIELD_32_TYPE_BEGIN) == PREFIX_TYPE;
}

static bool Is16bitInsn(uint64_t bin)
{
    return (bin & 0x1) == 0;
}

// v0.50 16bit can only concat with a 32bit, and 32bit can only concat with a 32bit
bool StaticPredictor::IsConcat(uint64_t bin, uint64_t nByte)
{
    if (Is16bitInsn(bin)) {
        if (nByte == WIDTH_16) {
            return Is16Prefix(bin);
        } else {
            return nByte < WIDTH_48;
        }
    } else {
        if (nByte == WIDTH_32) {
            return Is32Prefix(bin);
        }
        return nByte < WIDTH_64;
    }
}

void StaticPredictor::Flush(seq_t fbid_g, seq_t fbid_l, bool global_flush, bool local_flush, uint32_t stid)
{
    ASSERT(stid < globalDec.size());
    globalDec[stid].Flush(fbid_g, fbid_l, global_flush, local_flush);
    for (auto &lane : localDec) {
        lane.Flush(fbid_g, fbid_l, global_flush, local_flush, stid);
    }
}

void StaticPredictor::ResetPipe(bool global, uint32_t pipe_id, uint32_t stid) {
    if (global) {
        ASSERT(stid < globalDec.size());
        globalDec[stid].Reset();
    } else {
        localDec[pipe_id].Reset(stid);
    }
}

void StaticPredictor::Reset(uint32_t stid)
{
    ASSERT(stid < globalDec.size());
    globalDec[stid].Reset();
    ResetLocal(stid);
}

void StaticPredictor::ResetLocal(uint32_t stid) {
    for (auto &lane : localDec) {
        lane.Reset(stid);
    }
}

bool StaticPredictor::IsTemplate(uint64_t &bin, uint64_t binWidth)
{
    if (binWidth != WIDTH_32) {
        return false;
    }
    uint64_t opcode = GetBits(bin, 6, 4);
    uint64_t type = GetBits(bin, 3, 1);
    // MCOPY, MSET, FENTRY, FEXIT, FRET.RA, FRET.STK
    return (opcode == 0b011 || opcode == 0b100) && type == 0b000;
}

void StaticPredictor::Build(uint32_t laneN, uint32_t threadCount) {
    localDec.resize(laneN);
    // TODO: use scalar thread count
    globalDec.resize(threadCount);
    for (auto& dec : globalDec) {
        dec.global = true;
    }
}

void StaticPredictor::SetHeaderStash(NS_CORE::PtrMachineInst h, SPInfoPtr spInfo, bool global, uint32_t pipe_id,
    uint32_t stid) {
    if (global) {
        ASSERT(stid < globalDec.size());
        globalDec[stid].fbid_global = h->bfuInfo->fbid;
        globalDec[stid].fbid_local = h->bfuInfo->fbid_local;
        globalDec[stid].machineInst = h;
        globalDec[stid].machineInst->bfuInfo->spInfo = spInfo;
        // globalDec.minst = minst;
    } else {
        localDec[pipe_id].fbid_global = h->bfuInfo->fbid;
        localDec[pipe_id].fbid_local = h->bfuInfo->fbid_local;
        localDec[pipe_id].machineInst = std::make_shared<SimInstInfo>(*h);
        localDec[pipe_id].machineInst->bfuInfo = std::make_shared<BFUMachineInfo>(*(h->bfuInfo));
        localDec[pipe_id].machineInst->bfuInfo->spInfo = spInfo;
        // localDec[pipe_id].minst = minst;
    }
}

void StaticPredictor::RecoverGlobalDec(PtrFB fb) {
    ASSERT(fb->stid < globalDec.size());
    globalDec[fb->stid] = fb->dec_info;
}

void StaticPredictor::GlobalToLocal(uint32_t pipe_id, uint32_t stid) {
    ASSERT(stid < globalDec.size());
    localDec[pipe_id] = globalDec[stid];
}
}

} // namespace JCore
