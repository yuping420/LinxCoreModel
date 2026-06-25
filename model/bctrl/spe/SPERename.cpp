#include "SPERename.h"
#include "core/Core.h"
#include "SPE.h"

namespace JCore {
using namespace std;
void SPERename::Work()
{
    Rename();
    ToDispatch();
}

void SPERename::Xfer()
{

}

void SPERename::Reset()
{

}

void SPERename::ReportStat()
{

}

SimSys* SPERename::GetSim()
{
    return top->sim;
}

void SPERename::Build()
{
    ggprRenameUnit.sim = sim;
    ggprRenameUnit.Build();

    sgprRenameUnit = vector<vector<vector<LocalRegMgr>>>(sim->core->configs.stdPeCount,
                     vector<vector<LocalRegMgr>>(sim->core->configs.scalar_smt_thread,
                     vector<LocalRegMgr>(SGPR_HAND_COUNT)));
    for (uint32_t pe = 0; pe < sgprRenameUnit.size(); ++pe) {
        for (uint32_t tid = 0; tid < sgprRenameUnit[pe].size(); ++tid) {
            auto &sgpr = sgprRenameUnit[pe][tid];
            auto &tsgpr = sgpr[SGPRType2Idx(OperandType::OPD_TLINK)];
            tsgpr.sim = sim;
            tsgpr.Init(sim->core->configs.local_reg_t, top->configs.speROBDepth, OperandType::OPD_TLINK, pe, 0);
            auto &usgpr = sgpr[SGPRType2Idx(OperandType::OPD_ULINK)];
            usgpr.sim = sim;
            usgpr.Init(sim->core->configs.local_reg_u, top->configs.speROBDepth, OperandType::OPD_ULINK, pe, 0);
        }
    }
}

void SPERename::Rename()
{
    if (dec_ren_q->Empty()) {
        return;
    }
    uint64_t width = top->configs.renameWidth;
    // 重命名宽度目前按照指令个数来。有需要也可以加operand 的个数。
    for (uint64_t i = 0; i < width && !dec_ren_q->Empty() && !CheckRenameStall(dec_ren_q->Front()); i++) {
        SimInst inst = dec_ren_q->Front();
        for (auto &psrc : inst->psrcs_) {
            RenameSrcPOperand(inst, psrc);
        }
        auto &sgprRename = sgprRenameUnit[inst->peID][inst->stid];
        inst->tSeq = sgprRename[SGPRType2Idx(OperandType::OPD_TLINK)].GetCurSeq();
        inst->uSeq = sgprRename[SGPRType2Idx(OperandType::OPD_ULINK)].GetCurSeq();
        for (auto &pdst : inst->pdsts_) {
            RenameDstPOperand(inst, pdst);
        }
        if (inst->isLastInBlock) {
            ggprRenameUnit.SetCheckPoint(inst->bid, inst->stid);
        }
        LOG_INFO_M(Unit::SPE, Stage::D2) << "Rename for " << inst->Dump();
        dec_ren_q->Read();
        d2_s1_q.push_back(inst);
    }
}

bool SPERename::CheckRenameStall(SimInst inst)
{
    uint32_t sgprTCount = 0;
    uint32_t sgprUCount = 0;
    uint32_t ggprCount = 0;
    for (auto pdst : inst->pdsts_) {
        if (pdst->type == OperandType::OPD_GREG) {
            ++ggprCount;
        }
        if (pdst->type == OperandType::OPD_TLINK) {
            ++sgprTCount;
        }
        if (pdst->type == OperandType::OPD_ULINK) {
            ++sgprUCount;
        }
    }
    ASSERT(sgprTCount <= 1);
    ASSERT(sgprUCount <= 1);
    auto &sgpr = sgprRenameUnit[inst->peID][inst->stid];
    if (sgprTCount > 0 && sgpr[SGPRType2Idx(OperandType::OPD_TLINK)].CheckStall(sgprTCount)) {
        return true;
    }
    if (sgprUCount > 0 && sgpr[SGPRType2Idx(OperandType::OPD_ULINK)].CheckStall(sgprUCount)) {
        return true;
    }
    if (ggprCount > 0 && ggprRenameUnit.CheckStall(ggprCount, inst->stid)) {
        return true;
    }
    return false;
}

void SPERename::ToDispatch()
{
    dec_ren_q->unsetStall();
    while (!d2_s1_q.empty() && !dec_ren_q->getStall()) {
        if (InsertToSIEXQ(d2_s1_q.front())) {
            d2_s1_q.pop_front();
        } else {
            dec_ren_q->setStall();
            break;
        }
    }
}

bool SPERename::RenameSrcPOperand(SimInst &inst, POperandPtr &operand)
{
    auto &sgprRename = sgprRenameUnit[inst->peID][inst->stid];
    RecordInfo sgprInfo;
    switch (operand->type) {
        case OperandType::OPD_GREG:
            ggprRenameUnit.RenameSrc(inst, operand);
            break;
        case OperandType::OPD_TLINK:
        case OperandType::OPD_ULINK:
            // rename-lookup
            sgprInfo = sgprRename[SGPRType2Idx(operand->type)].Dispatch(operand->value);
            operand->renamed = true;
            operand->ptag = sgprInfo.tag;
            break;
        case OperandType::OPD_ZERO:
        case OperandType::OPD_TILE_TLINK:
        case OperandType::OPD_TILE_ULINK:
        case OperandType::OPD_TILE_MLINK:
        case OperandType::OPD_TILE_NLINK:
        case OperandType::OPD_TILE_ACC:
        case OperandType::OPD_TILE_STACK:
        case OperandType::OPD_TILE_DLINK:
        case OperandType::OPD_SIMM:
        case OperandType::OPD_UIMM:
        case OperandType::OPD_SYS:
        case OperandType::OPD_CARG:
            // do noting
            break;
        case OperandType::OPD_VTLINK:
        case OperandType::OPD_VULINK:
        case OperandType::OPD_VMLINK:
        case OperandType::OPD_VNLINK:
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
        case OperandType::OPD_RO:
        case OperandType::OPD_PREDMASK:
        case OperandType::OPD_LB0:
        case OperandType::OPD_LB1:
        case OperandType::OPD_LB2:
        case OperandType::OPD_LC0:
        case OperandType::OPD_LC1:
        case OperandType::OPD_LC2:
            break;
        default:
            ASSERT(false && "SPE Unexpected src operandtype") << GetOperandType(operand->type);
    }
    return true;
}

bool SPERename::RenameDstPOperand(SimInst &inst, POperandPtr &operand)
{
    auto &sgprRename = sgprRenameUnit[inst->peID][inst->stid];
    switch (operand->type) {
        case OperandType::OPD_GREG:
            ggprRenameUnit.RenameDst(inst, operand);
            break;
        case OperandType::OPD_TLINK:
        case OperandType::OPD_ULINK:
            // rename-alloc
            operand->renamed = true;
            operand->ptag = sgprRename[SGPRType2Idx(operand->type)].Alloc(inst->bid, inst->rid, 1);
            break;
        case OperandType::OPD_TILE_TLINK:
        case OperandType::OPD_TILE_ULINK:
        case OperandType::OPD_TILE_MLINK:
        case OperandType::OPD_TILE_NLINK:
        case OperandType::OPD_TILE_ACC:
        case OperandType::OPD_TILE_STACK:
        case OperandType::OPD_TILE_DLINK:
        case OperandType::OPD_SIMM:
        case OperandType::OPD_UIMM:
        case OperandType::OPD_SYS:
            // do noting
            break;
        case OperandType::OPD_ZERO:
        case OperandType::OPD_CARG:
        case OperandType::OPD_VTLINK:
        case OperandType::OPD_VULINK:
        case OperandType::OPD_VMLINK:
        case OperandType::OPD_VNLINK:
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
        case OperandType::OPD_RO:
        case OperandType::OPD_PREDMASK:
        case OperandType::OPD_LB0:
        case OperandType::OPD_LB1:
        case OperandType::OPD_LB2:
        case OperandType::OPD_LC0:
        case OperandType::OPD_LC1:
        case OperandType::OPD_LC2:
            break;
        default:
            ASSERT(false && "SPE Unexpected src operandtype") << GetOperandType(operand->type);
    }
    return true;
}

bool SPERename::InsertToSIEXQ(SimInst &inst)
{
    bool suaccelss = false;
    InstGroup grp = OpcodeManager::Inst().GetOpcodeGroup(inst->opcode);
    switch (grp) {
        case InstGroup::BLOCK_SPLIT:
        case InstGroup::BLOCK_OFFSET:
        case InstGroup::BLOCK_IO:
        case InstGroup::BLOCK_ATTR:
        case InstGroup::BLOCK_HINT:
        case InstGroup::BLOCK_ARGUMENT:
            suaccelss = InsertToCmdIEX(inst);
            break;
        case InstGroup::SETC:
            suaccelss = InsertToBruIEX(inst);
            break;
        case InstGroup::LOAD:
            suaccelss = InsertToLoadIEX(inst);
            break;
        case InstGroup::STORE:
        case InstGroup::CACHE_MAINTAIN:
            suaccelss = InsertToStoreIEX(inst);
            break;
        case InstGroup::ARITHMETIC:
        case InstGroup::ARITHMETIC_FP:
        case InstGroup::COMPARE:
        case InstGroup::COMPARE_FP:
        case InstGroup::PC:
        case InstGroup::IMMEDIATE:
        case InstGroup::MOVE:
        case InstGroup::MULTICYCLE:
        case InstGroup::BIT:
        case InstGroup::GQM:
        case InstGroup::COMPOUND:
        case InstGroup::PREFETCH:
        case InstGroup::ATOMIC:
        case InstGroup::EXECUTE_CONTROL:
        case InstGroup::EXTEND:
        case InstGroup::GMO:
        case InstGroup::MAX_MIN:
        case InstGroup::CONVERT:
        case InstGroup::REDUCE:
        case InstGroup::SHUFFLE:
        case InstGroup::CT_CUSTOM:
            suaccelss = InsertToAluIEX(inst);
            break;
        case InstGroup::SSR: {
            if (inst->opcode == Opcode::OP_SETC_TGT) {
                suaccelss = InsertToBruIEX(inst);
            } else {
                suaccelss = InsertToAluIEX(inst);
            }
            break;
        }
        case InstGroup::BRANCH:
            ASSERT(false && "SPE Unsupport");
            break;
        default:
            break;
    }
    return suaccelss;
}

bool SPERename::InsertToCmdIEX(SimInst inst)
{
    auto &dispatchQ = top->pe_iex_cmd_array[inst->peID];
    if (!dispatchQ->getStall()) {
        dispatchQ->Write(inst);
        LOG_INFO_M(Unit::SPE, Stage::D2) << "Send to cmd IEX " << inst->Dump();
        return true;
    }
    return false;
}

bool SPERename::InsertToAluIEX(SimInst inst)
{
    auto &dispatchQ = top->pe_iex_alu_array[inst->peID];
    if (dispatchQ->getStall()) {
        return false;
    }
    dispatchQ->Write(inst);
    LOG_INFO_M(Unit::SPE, Stage::D2) << "Send to alu IEX " << inst->Dump();
    return true;
}

void HandleSta(SimInst &sta)
{
    // 目前在解码阶段，已经将全部store 类指令的 srcD 放在了src0
    // 设置成 sta 0 [addr]
    // 避免后续 Execute 的时候拿错地址
    size_t copySrcBegin = 1;
    if (!OpcodeIsStorePCR(sta->opcode)) {
        sta->psrcs_[0] = std::make_shared<PhysicalOperand>(*std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
        sta->srcs[0] = sta->psrcs_[0];
    } else {
        // PCR stores use src0 as the PC-relative address immediate and src1 as data.
        copySrcBegin = 0;
    }
    // 深拷贝，后续 wakeup 的时候避免movelpv出问题
    sta->accMemInfo = std::make_shared<AaccelssMemInfo>(*sta->accMemInfo);
    ASSERT(sta->psrcs_.size() == sta->srcs.size() && "PSrc is not equal to src, may need to check decode");
    // 这样复制是因为 SimInst 继承自 MInst，但是同事
    for (size_t i = copySrcBegin; i < sta->psrcs_.size(); ++i) {
        auto &psrc = sta->psrcs_[i];
        auto &src = sta->srcs[i];
        auto srcPtr = std::make_shared<Operand>(*src);
        auto psrcPtr = std::make_shared<PhysicalOperand>(*srcPtr);
        psrcPtr->renamed = psrc->renamed;
        psrcPtr->ptag = psrc->ptag;
        psrc = psrcPtr;
        src = psrcPtr;
    }
    // if (OpcodeIsStoreReg(sta->opcode)) {
    //     sta->psrcs_[SRC0_IDX]->ptag_vld = false;
    //     if (sta->ciscForm.vld) {
    //         if (sta->ciscForm.form == SRCFusionForm::SRC_0_2) {
    //             sta->src0.vld = false;
    //             sta->src0.ptag_vld = false;
    //             sta->ciscForm.vld = false;
    //         } else if (sta->ciscForm.form == SRCFusionForm::SRC_1_2) {
    //             sta->src1.vld = false;
    //             sta->src1.ptag_vld = false;
    //             sta->ciscForm.vld = false;
    //         }
    //     }
    // } else {
    //     sta->src0.vld = false;
    //     sta->src0.ptag_vld = false;
    //     if (sta->ciscForm.vld) {
    //         if (sta->ciscForm.form == SRCFusionForm::SRC_0_1) {
    //             sta->src1.vld = false;
    //             sta->src1.ptag_vld = false;
    //             sta->ciscForm.vld = false;
    //         } else if (sta->ciscForm.form == SRCFusionForm::SRC_0_2) {
    //             sta->src2.vld = false;
    //             sta->src2.ptag_vld = false;
    //             sta->ciscForm.vld = false;
    //         }
    //     }
    // }
}

bool SPERename::InsertToStoreIEX(SimInst inst)
{
    auto &staDispatchQ = top->pe_iex_sta_array[inst->peID];
    auto &stdDispatchQ = top->pe_iex_std_array[inst->peID];
    const bool splitStore = (inst->storeSplit || inst->stack_type == StackInstType::STACK_SET) &&
                            !IsLoadStorePair(inst->opcode);
    if (splitStore) {
        if (staDispatchQ->getStall() || stdDispatchQ->getStall()) {
            return false;
        }
        SimInst sta = std::make_shared<SimInstInfo>(*inst);
        SimInst std = std::make_shared<SimInstInfo>(*inst);
        HandleSta(sta);
        sta->stack_type = StackInstType::NORMAL;  // only std do stack rename
        sta->type = ST_ADDR;
        std->type = ST_DATA;
        // stack rename
        // if (std->stack_type == StackInstType::STACK_SET) {
        //     POperandPtr dst = std::make_shared<PhysicalOperand>();
        //     dst->type = OperandType::STACK_POINTER;
        //     std->pdsts_.emplace_back(dst);
        // }
        sim->core->iex[SCALAR_IEX]->stats->slots += 2;
        staDispatchQ->Write(sta);
        LOG_INFO_M(Unit::SPE, Stage::D2) << "Send to sta IEX " << sta->Dump();
        stdDispatchQ->Write(std);
        LOG_INFO_M(Unit::SPE, Stage::D2) << "Send to std IEX " << std->Dump();
    } else {
        if (staDispatchQ->getStall()) {
            return false;
        }
        staDispatchQ->Write(inst);
        LOG_INFO_M(Unit::SPE, Stage::D2) << "Send to sta IEX " << inst->Dump();
    }
    return true;
}

bool SPERename::InsertToLoadIEX(SimInst inst)
{
    auto &dispatchQ = top->pe_iex_lda_array[inst->peID];
    if (inst->stack_type == StackInstType::STACK_GET) {
        // stack rename 暂未适配
        // stall = top->pe_iex_alu_q->getStall();
        // if (inst->stack_check) {
        //     stall |= top->pe_iex_lda_q->getStall();
        // }
        // if (stall) {
        //     return;
        // }
        // SimInst getInst = std::make_shared<UInstInfo>(*inst);
        // getInst->src0.ptag_vld = false;
        // getInst->src0.ttag_vld = false;
        // getInst->src0.utag_vld = false;
        // getInst->ref.gsInfo.src0_vld = false;
        // getInst->src0.data_vld = false;
        // getInst->src0.rdy = false;
        // getInst->src0.sptag_vld = true;
        // getInst->src1.vld = false;
        // getInst->src2.vld = false;
        // getInst->src3.vld = false;
        // getInst->ref.Reset();
        // top->pe_iex_alu_q->Write(getInst);
        // if (inst->stack_check) {
        //     SimInst loadInst = std::make_shared<UInstInfo>(*inst);
        //     loadInst->stack_renamed = false;
        //     loadInst->stack_type = StackInstType::STACK_LOAD;
        //     loadInst->srcSP.vld = true;
        //     loadInst->src0.sptag_vld = false;
        //     top->pe_iex_lda_q->Write(loadInst);
        // }
    } else {
        if (dispatchQ->getStall()) {
            return false;
        }
        dispatchQ->Write(inst);
        LOG_INFO_M(Unit::SPE, Stage::D2) << "Send to lda IEX " << inst->Dump();
    }
    return true;
}

bool SPERename::InsertToBruIEX(SimInst inst)
{
    auto &dispatchQ = top->pe_iex_bru_array[inst->peID];
    if (dispatchQ->getStall()) {
        return false;
    }
    dispatchQ->Write(inst);
    LOG_INFO_M(Unit::SPE, Stage::D2) << "Send to bru IEX " << inst->Dump();
    return true;
}


void SPERename::Flush(FlushBus flushReq)
{
    ggprRenameUnit.Flush(flushReq);

    bool flushBegin = false;
    for (auto it = d2_s1_q.begin(); it != d2_s1_q.end();) {
        if ((*it)->stid != flushReq.req.stid) {
            continue;
        }
        if (LessEqual(flushReq.req.bid, (*it)->bid)) {
            flushBegin = true;
        }
        if (flushBegin) {
            it = d2_s1_q.erase(it);
        } else {
            ++it;
        }
    }

    for (auto &peSGPRRename : sgprRenameUnit) {
        for (auto &threadSGPRRename : peSGPRRename) {
            for (auto &handSGPRRename : threadSGPRRename) {
                handSGPRRename.flush(flushReq);
            }
        }
    }
}

void SPERename::ReportSGPRBlockCommit(ROBID bid, uint32_t stid)
{
    for (auto &peSGPRRename : sgprRenameUnit) {
        for (auto &threadSGPRRename : peSGPRRename[stid]) {
            threadSGPRRename.ReportBlockCommit(bid);
        }
    }
}

void SPERename::RepLocalRetired(OperandType type, uint32_t peid, ROBID &seq, bool isDealloc, uint32_t tid)
{
    sgprRenameUnit[peid][tid][SGPRType2Idx(type)].ReportRetired(seq.val, isDealloc);
}

}
