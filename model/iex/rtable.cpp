#include "iex/rtable.h"

#include "core/Core.h"
#include "iex/iex.h"

namespace JCore {

static std::vector<PTInfo>                      *g_cptagReady = nullptr;
static std::vector<PTInfo>                      *g_nptagReady = nullptr;

void ReadyState::Work() {
    setState();
}

void ReadyState::ReleasePtag(uint32_t ptag, PLpvInfo &lpvInfo)
{
    SetRegReadyTable(OperandType::OPD_GREG, ptag, false, lpvInfo);
    if (top->id == SCALAR_IEX) {
        (*next.ptagReady)[ptag]->global = false;
        (*next.ptagReady)[ptag]->retired = false;
        (*next.ptagReady)[ptag]->data_vld = false;
        (*next.ptagReady)[ptag]->inst_retired = false;
    } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        ASSERT(ptag < next.lptagReady.size());
        next.lptagReady[ptag]->global = false;
        next.lptagReady[ptag]->retired = false;
        next.lptagReady[ptag]->data_vld = false;
        next.lptagReady[ptag]->inst_retired = false;
    }
}

void ReadyState::setState()
{
    PLpvInfo lpvInfo;
    // ptag release, unset ready table
    rtable_release_q->Work();
    while (!rtable_release_q->Empty()) {
        uint32_t ptag = rtable_release_q->Read();
        if (top->id == SCALAR_IEX && ptag >= next.ptagReady->size()) {
            continue;
        }
        ReleasePtag(ptag, lpvInfo);
    }
    // block commit, retire for the whole block
    block_rtable_retire_q->Work();
    while (!block_rtable_retire_q->Empty()) {
        setBlockRetire(block_rtable_retire_q->Read());
    }
}

void ReadyState::Xfer()
{
    dataWrite();
    wr_inst_q.clear();
    next.MoveLpv();
    if (top->id == SCALAR_IEX) {
        for (uint32_t i = 0; i < next.ptagReady->size(); i++) {
            if ((*(next.ptagReady))[i]->lpvInfo) {
                (*(next.ptagReady))[i]->lpvInfo->Move();
            }
        }
    }
    if (top->core->IsVectorIex(top->machineType) || (top->id == MEM_IEX)) {
        vrfRtableTagFreeQ->Work();
        while (!vrfRtableTagFreeQ->Empty()) {
            PLpvInfo lpvInfo;
            SetRegReadyTable(OperandType::OPD_VTLINK, vrfRtableTagFreeQ->Read(), false, lpvInfo);
        }
        vrfRtableTagRetireQ->Work();
        while (!vrfRtableTagRetireQ->Empty()) {
            next.vrfReady[vrfRtableTagRetireQ->Read()]->retired = true;
        }
        for (uint32_t i = 0; i < next.lptagReady.size(); i++) {
            if (next.lptagReady[i]->lpvInfo) {
                next.lptagReady[i]->lpvInfo->Move();
            }
        }
    }
    current = next;
}

static void DataWriteToRtable(POperandPtr &dst, std::vector<PTInfo> &rdyTb)
{
    if (dst->type != OperandType::OPD_GREG) {
        return;
    }

    PTInfo newTb = std::make_shared<TableInfo>(*(rdyTb[dst->ptag]));
    newTb->data_vld = true;
    newTb->data = dst->data;
    ASSERT(dst->ptag < rdyTb.size());
    rdyTb[dst->ptag] = newTb;
}

void ReadyState::dataWrite()
{
    iex_rt_wr_q->Work();
    while (!iex_rt_wr_q->Empty()) {
        auto req = iex_rt_wr_q->Read();
        if (!req.vld) continue;
        if (top->id == SCALAR_IEX) {
            for (auto pdst : req.dsts) {
                DataWriteToRtable(pdst, *next.ptagReady);
            }
        } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
            for (auto pdst : req.dsts) {
                DataWriteToRtable(pdst, *next.ptagReady);
            }
        }
    }
}

static bool FlushMatch(FlushBus &flushReq, PTInfo &table)
{
    if (!table->ready || table->retired) {
        return false;
    }

    if (table->inst_retired && table->bid == flushReq.req.bid && flushReq.req.fetchTPCVld && !flushReq.baseOnBid) {
        return false;
    }

    if (flushReq.baseOnPE) {
        return ((flushReq.req.peID == table->peid) &&
            LessEqual(flushReq.req.bid, flushReq.req.rid, table->bid, table->rid));
    }

    return flushReq.baseOnBid ? LessEqual(flushReq.req.bid, table->bid) :
        LessEqual(flushReq.req.bid, flushReq.req.rid, table->bid, table->rid);
}

static void FlushPRdyTable(PTInfo &ptag, FlushBus &flushReq)
{
    if (!FlushMatch(flushReq, ptag)) {
        return;
    }

    PTInfo newPRTInfo = std::make_shared<TableInfo>(*ptag);
    newPRTInfo->ready = false;
    newPRTInfo->retired = false;
    newPRTInfo->inst_retired = false;
    newPRTInfo->data_vld = false;
    if (newPRTInfo->lpvInfo) {
        newPRTInfo->lpvInfo->Reset();
    }
    ptag = newPRTInfo;
}

void ReadyState::flush(FlushBus flushReq)
{
    if (!flushReq.req.vld)
        return;

    // status bypass
    setState();
    if (top->id == SCALAR_IEX) {
        for (uint32_t i = 0; i < next.ptagReady->size(); ++i) {
            FlushPRdyTable((*next.ptagReady)[i], flushReq);
        }
    }

    if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        for (uint32_t i = 0; i < next.lptagReady.size(); ++i) {
            FlushPRdyTable(next.lptagReady[i], flushReq);
        }
    }

    auto req_flush = [&flushReq] (RFReqBus bus) -> bool {
        if (flushReq.baseOnPE) {
            return ((flushReq.req.peID == bus.peid) && LessEqual(flushReq.req.bid, flushReq.req.rid, bus.bid, bus.rid));
        }
        return flushReq.baseOnBid ? LessEqual(flushReq.req.bid, bus.bid) :
                LessEqual(flushReq.req.bid, flushReq.req.rid, bus.bid, bus.rid);
    };
    iex_rt_wr_q->FlushIf(req_flush);
}

void ReadyState::Build(uint32_t peCount)
{
    // 仅用于StackRename 的
    uint32_t robDepth = top->core->bctrl->scalarPE->configs.speROBDepth;
    current.sim = sim;
    current.Build(top->core->configs, peCount, top->id, robDepth);
    next.sim = sim;
    next.Build(top->core->configs, peCount, top->id, robDepth);
    StackConfig sconfigs;
    sconfigs.overrideDefaultConfig(GetSim()->getCfgs());
    uint64_t sreg_count = sconfigs.sp_preg_count;

    auto initRTable = [](vector<PTInfo> &rtable, uint64_t size) {
        rtable.resize(size);
        for (auto &e : rtable) {
            e = std::make_shared<TableInfo>();
        }
    };

    initRTable(current.sptagReady, sreg_count);
    initRTable(next.sptagReady, sreg_count);

    if (!g_cptagReady) {
        g_cptagReady = new vector<PTInfo>(top->core->configs.ggpr_count);
        g_nptagReady = new vector<PTInfo>(top->core->configs.ggpr_count);
        for (auto &e : *g_cptagReady) {
            e = std::make_shared<TableInfo>();
        }
        for (auto &e : *g_nptagReady) {
            e = std::make_shared<TableInfo>();
        }
    }

    next.ptagReady = g_nptagReady;
    current.ptagReady = g_cptagReady;

    uint64_t lPtagSize = 0;
    if (top->core->IsVectorIex(top->machineType)) {
        lPtagSize = top->core->configs.vec_lpreg_count;
    } else if (top->id == MEM_IEX) {
        lPtagSize = top->core->configs.mtc_lpreg_count;
    }
    next.lptagReady.resize(lPtagSize);
    for (auto &e : next.lptagReady) {
        e = std::make_shared<TableInfo>();
    }
    current.lptagReady.resize(lPtagSize);
    for (auto &e : current.lptagReady) {
        e = std::make_shared<TableInfo>();
    }

    for (uint32_t i = 0; i < peCount; i++) {
        rf_ct_q.emplace_back(nullptr);
    }
}

void ReadyTable::Build(CoreConfig const &configs, uint32_t peCount, ExecEngineTyp type, uint32_t robDepth)
{
    uint64_t localRegT = type == SCALAR_IEX ? configs.local_reg_t : configs.simt_local_reg_t;
    uint64_t localRegU = type == SCALAR_IEX ? configs.local_reg_u : configs.simt_local_reg_u;
    uint64_t localRegP = type == SCALAR_IEX ? 0 : configs.simt_local_reg_p;

    if (type == MEM_IEX) {
        localRegT = type == SCALAR_IEX ? configs.local_reg_t : configs.mtc_local_reg_t;
        localRegU = type == SCALAR_IEX ? configs.local_reg_u : configs.mtc_local_reg_u;
    }

    auto initRTable = [type, &configs](std::vector<std::vector<std::vector<PTInfo>>> &rtable, uint32_t size,
                         uint32_t depth) {
        rtable.resize(size);
        for (uint32_t i = 0; i < size; i++) {
            uint32_t threadCnt = (type == SCALAR_IEX) ? configs.scalar_smt_thread : configs.threadCount;
            rtable[i].resize(threadCnt);
            for (auto &tb: rtable[i]) {
                tb.resize(depth);
                for (auto &e : tb) {
                    e = std::make_shared<TableInfo>();
                }
             }
        }
    };

    initRTable(tRegReady, peCount, localRegT);
    initRTable(uRegReady, peCount, localRegU);
    vrfReady.resize(configs.vrf_preg_count);
    for (auto &e : vrfReady) {
        e = make_shared<TableInfo>();
    }
    initRTable(simtMaskRegReady, peCount, localRegP);

    /* init robReady */
    robReady.resize(peCount);
    for (auto &tb : robReady) {
        tb.resize(robDepth);
        for (auto &e : tb) {
            e = std::make_shared<TableInfo>();
        }
    }
}

static void ResetPtagRdy(const std::vector<PTInfo>& ptagTrady)
{
    for (auto &ptag : ptagTrady) {
        if (!ptag) {
            continue;
        }

        ptag->ready = false;
        ptag->retired = true;
        ptag->inst_retired = false;
        ptag->data_vld = false;
        if (ptag->lpvInfo) {
            ptag->lpvInfo->Reset();
        }
    }
}

void ReadyState::Reset()
{
    current.Reset();
    next.Reset();
}

void ReadyTable::Reset()
{
    auto resetLocalRT = [](std::vector<std::vector<std::vector<PTInfo>>> &tb) {
        for (uint32_t peId = 0; peId < tb.size(); peId++) {
            for (uint32_t tid = 0; tid < tb[peId].size(); tid++) {
                for (uint32_t entryId = 0; entryId < tb[peId][tid].size(); entryId++) {
                    tb[peId][tid][entryId]->ready = false;
                    tb[peId][tid][entryId]->retired = false;
                    if (tb[peId][tid][entryId]->lpvInfo) {
                        tb[peId][tid][entryId]->lpvInfo->Reset();
                    }
                }
            }
        }
    };
    resetLocalRT(tRegReady);
    resetLocalRT(uRegReady);
    for (auto &e : vrfReady) {
        e->ready = false;
        e->retired = false;
        if (e->lpvInfo) {
            e->lpvInfo->Reset();
        }
    }
    resetLocalRT(simtMaskRegReady);

    if (ptagReady) {
        ResetPtagRdy(*ptagReady);
        for (uint32_t i = 0; i < static_cast<uint32_t>(GPR::GPR_COUNT) * sim->core->configs.scalar_smt_thread; i++) {
            (*ptagReady)[i]->ready = true;
            (*ptagReady)[i]->data_vld = true;
            (*ptagReady)[i]->data = 0;
        }
    }
    ResetPtagRdy(lptagReady);

    for (auto &tag : sptagReady) {
        tag->ready = false;
        tag->retired = false;
        if (tag->lpvInfo) {
            tag->lpvInfo->Reset();
        }
    }
    for (uint32_t i = 0; i < robReady.size(); i++) {
        for (uint32_t j = 0; j < robReady[i].size(); j++) {
            robReady[i][j]->ready = false;
            robReady[i][j]->retired = false;
            if (robReady[i][j]->lpvInfo) {
                robReady[i][j]->lpvInfo->Reset();
            }
        }
    }
}

static void MoveOnedimensionRTLpv(vector<PTInfo> &tb)
{
    for (auto &e : tb) {
        if (e->lpvInfo) {
            e->lpvInfo->Move();
            if (!e->lpvInfo->notEmpty) {
                e->lpvInfo = nullptr;
            }
        }
    }
}

static void MoveRTLpv(const std::vector<std::vector<PTInfo>>& tb)
{
    for (uint32_t i = 0; i < tb.size(); i++) {
        for (uint32_t j = 0; j < tb[i].size(); j++) {
            if (tb[i][j]->lpvInfo) {
                tb[i][j]->lpvInfo->Move();
                if (!tb[i][j]->lpvInfo->notEmpty) {
                    tb[i][j]->lpvInfo = nullptr;
                }
            }
        }
    }
}

static void MoveLocalRTLpv(const std::vector<std::vector<std::vector<PTInfo>>>& tb)
{
    for (uint32_t peId = 0; peId < tb.size(); peId++) {
        for (uint32_t tid = 0; tid < tb[peId].size(); tid++) {
            for (uint32_t entryId = 0; entryId < tb[peId][tid].size(); entryId++) {
                if (tb[peId][tid][entryId]->lpvInfo) {
                    tb[peId][tid][entryId]->lpvInfo->Move();
                    if (!tb[peId][tid][entryId]->lpvInfo->notEmpty) {
                        tb[peId][tid][entryId]->lpvInfo = nullptr;
                    }
                }
            }
        }
    }
}

void ReadyTable::MoveLpv() {
    MoveLocalRTLpv(tRegReady);
    MoveLocalRTLpv(uRegReady);
    MoveOnedimensionRTLpv(vrfReady);
    MoveLocalRTLpv(simtMaskRegReady);
    MoveRTLpv(robReady);
}

void ReadyState::checkReady(SimInst &inst)
{
    if (!inst) {
        return;
    }

    std::string dispInfo;

    if ((inst->opcode == Opcode::OP_TLD) || (inst->opcode == Opcode::OP_TSD)) {
        return;
    }

    for (size_t i = 0; i < inst->psrcs_.size(); ++i) {
        auto psrc = inst->psrcs_[i];
        if (psrc->type == OperandType::STACK_POINTER) {
            psrc->ready = current.robReady[inst->peID][inst->rid.val]->ready;
            if (psrc->ready) {
                inst = std::make_shared<SimInstInfo>(*inst); // New ptr, not write in rob
                inst->stack_renamed = true;
                SetLpv(current.robReady[inst->peID][inst->rid.val]->lpvInfo, inst->psrcs_[i]->lpvInfo, true);
            }
        }
        CheckReadySrc(inst, i, dispInfo);
    }

    // inputRegMask (b.ior) , rename get ptag at D1/D2
    // if (inst->inputRegMask > 0) {
    //     checkReadySrc(inst, Location::INREG, dispInfo);
    // }

    // checkLoadReady(inst, dispInfo);

    for (size_t i = 0; i < inst->pdsts_.size(); ++i) {
        CheckDst(inst, i);
    }

    checkRslv(inst, dispInfo);
    LOG_INFO_M(top->machineType, Stage::S1) << "inst dispatched to issue queue" << dispInfo << " " << inst->Dump();
}

static void SetPtagRslv(std::vector<PTInfo> &ptagRdy, uint32_t ptag, uint64_t dataOut)
{
    PTInfo newTb = std::make_shared<TableInfo>(*ptagRdy[ptag]);
    newTb->data_vld = true;
    newTb->data = dataOut;
    ASSERT(ptag < ptagRdy.size());
    ptagRdy[ptag] = newTb;
}

void ReadyState::checkRslv(SimInst &inst, std::string &dispInfo)
{
    if (!top->core->configs.reno_dynamic_enable || inst->iexType != SCALAR_IEX || inst->opcode == Opcode::OP_B_IOT) {
        return;
    }
    if (OpcodeIsLoad(inst->opcode) || OpcodeIsStore(inst->opcode) || inst->opcode == Opcode::OP_SSRSET ||
        OpcodeIsBDIM(inst->opcode) || (top->core->configs.enable_cmd_isq && OpcodeIsCMD(inst->opcode))) {
        return;
    }
    for (auto pdst : inst->pdsts_) {
        if (pdst->type == OperandType::OPD_TLINK || pdst->type == OperandType::OPD_ULINK) {
            return;
        }
    }

    if (inst->RangedDataReady()) {
        inst->Execute();
        inst->ResetLpv();
        auto checkLocalRegRdy = [](std::vector<std::vector<std::vector<PTInfo>>> &tb, uint32_t peid, uint32_t tid,
                                   uint32_t tag) {
            PTInfo newTb = std::make_shared<TableInfo>(*tb[peid][tid][tag]);
            newTb->ready = true;
            tb[peid][tid][tag] = newTb;
        };
        auto checkTagReady = [](vector<PTInfo> &tb, uint32_t tag) {
            PTInfo newEntry = std::make_shared<TableInfo>(*tb[tag]);
            newEntry->ready = true;
            tb[tag] = newEntry;
        };
        for (auto pdst : inst->pdsts_) {
            switch (pdst->type) {
                case OperandType::OPD_GREG:
                    if (top->id == SCALAR_IEX) {
                        SetPtagRslv(*next.ptagReady, pdst->ptag, pdst->data);
                    } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
                        SetPtagRslv(next.lptagReady, pdst->ptag, pdst->data);
                    }
                    dispInfo += " dst0 data:0d" + std::to_string(pdst->data);
                    break;
                case OperandType::OPD_TLINK:
                    checkLocalRegRdy(next.tRegReady, inst->peID, inst->tid, pdst->ptag);
                    break;
                case OperandType::OPD_ULINK:
                    checkLocalRegRdy(next.uRegReady, inst->peID, inst->tid, pdst->ptag);
                    break;
                case OperandType::OPD_VTLINK:
                case OperandType::OPD_VULINK:
                case OperandType::OPD_VMLINK:
                case OperandType::OPD_VNLINK:
                    checkTagReady(next.vrfReady, pdst->ptag);
                    break;
                case OperandType::OPD_PREDMASK:
                    checkLocalRegRdy(next.simtMaskRegReady, inst->peID, inst->tid, pdst->ptag);
                    break;
                default:
                    break;
            }
            if (pdst->type == OperandType::OPD_GREG) {
                PLpvInfo tmpLpvInfo;
                GetSim()->core->iex[SCALAR_IEX]->rtable.SetRegReadyTable(OperandType::OPD_GREG, pdst->ptag, true, tmpLpvInfo);
            }
        }
        RFReqBus req = inst->GenRFReqBus(false);
        rf_wr_q->Write(req);
        isq_wake_q->Write(inst);
        wr_inst_q.push_back(inst);
        PEResolveBus rslv = inst->GenRslvBus();
        rslv_array[inst->peID][inst->tid]->Write(rslv);
        if (inst->backToCodeTemplate) {
            rf_ct_q[inst->peID]->Write(inst);
        }
        top->brRlsvQ.push_back(inst);
        top->innerBranchResolve(inst);
        dispInfo += " optimized";
        inst = std::shared_ptr<SimInstInfo>(nullptr);
        top->stats->s1_optimized_inst++;
        top->stats->ope_s1_inst_cnt++;
    }
}

// bool getRefSrc(SimInst &inst, Location type) {
//     switch (type) {
//         case Location::SRC0: return inst->ref.gsInfo.src0_vld;
//         case Location::SRC1: return inst->ref.gsInfo.src1_vld;
//         case Location::SRC2: return inst->ref.gsInfo.src2_vld;
//         case Location::SRC3: return inst->ref.gsInfo.src3_vld;
//         default: return false;
//     }
//     return false;
// }

void ReadyState::GetSrcData(POperandPtr src, bool scalar)
{
    if (!top->core->configs.reno_dynamic_enable || !scalar) {
        return;
    }

    PTInfo ptag;
    if (top->id == SCALAR_IEX) {
        ptag = (*current.ptagReady)[src->ptag];
    } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        ptag = current.lptagReady[src->ptag];
    } else {
        ASSERT(0 && "not support");
    }
    if (ptag->data_vld) {
        src->dataVld = true;
        src->data = ptag->data;
    }
    // bypass
    for (auto &inst : wr_inst_q) {
        for (auto pdst : inst->pdsts_) {
            if (pdst->DataRanged() && pdst->ptag == src->ptag && pdst->type == src->type) {
                src->dataVld = true;
                src->data = pdst->data;
            }
        }
    }
}

void ReadyState::CheckReadySrc(SimInst &inst, size_t idx, std::string &dispInfo)
{
    POperandPtr operand = inst->psrcs_[idx];
    if (inst->SrcTypeContain(OperandType::STACK_POINTER)) {
        return;
    }
    // if (GetSim()->perfectGetSet && getRefSrc(inst, idx)) {
    //     return;
    // }
    auto checkTagReady = [&idx, &inst](vector<PTInfo> &rt, POperandPtr &operand) {
        operand->ready = operand->ready ? operand->ready : rt[operand->ptag]->ready;
        if (operand->ready) {
            inst = std::make_shared<SimInstInfo>(*inst);
            operand = inst->psrcs_[idx];
            if (operand == nullptr) {
                return false;
            }
            SetLpv(rt[operand->ptag]->lpvInfo, operand->lpvInfo, true);
        }
        return true;
    };
    if (operand->type == OperandType::OPD_GREG) {
        if (operand->renamed) {
            if (top->id == SCALAR_IEX && operand->ptag < current.ptagReady->size()) {
               operand->ready = operand->ready ? operand->ready : checkPtagReady(operand->ptag);
            } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
                operand->ready = operand->ready ? operand->ready : checkPtagReady(operand->ptag);
            }
            // if (!operand->ready) {
            //     auto dataInfo = top->core->bctrl->blockRenameUnit.getMapTableData(operand->ptag);
            //     operand->dataVld = dataInfo.data_vld;
            //     operand->data = dataInfo.data;
            //     if (dataInfo.data_vld) {
            //         operand->ready = true;
            //     }
            // }
        }
        if (operand->ready) {
            inst = std::make_shared<SimInstInfo>(*inst); // New ptr, not write in rob
            operand = inst->psrcs_[idx];
            if (operand == nullptr) {
                return;
            }
            if (top->id == SCALAR_IEX && operand->ptag < current.ptagReady->size()) {
                SetLpv((*current.ptagReady)[operand->ptag]->lpvInfo, operand->lpvInfo, true);
                GetSrcData(operand, inst->opcode != Opcode::OP_B_IOT);
            } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
                ASSERT(operand->ptag < current.lptagReady.size());
                SetLpv(current.lptagReady[operand->ptag]->lpvInfo, operand->lpvInfo, true);
                GetSrcData(operand, false);
            }
        }
        dispInfo += " src" + std::to_string(static_cast<int>(idx)) + "  R" + std::to_string(operand->value) + "-GP"
                    + std::to_string(operand->ptag) + "-ready:" + std::to_string(static_cast<int>(operand->ready)) + " ";
        if (operand->dataVld) {
            dispInfo += "get data:0d" + std::to_string(operand->data) + " ";
        }
    } else if (operand->type == OperandType::LS_MDB_DEPENDENCY) {
        if (top->iexmdb.isStRdy(operand->ptag, inst->addrWakeuped)) {
            operand->ready = true;
            inst->wait_store = -1;
            top->iexmdb.release(operand->ptag);
            inst->psrcs_.erase(inst->psrcs_.begin() + idx);
        } else {
            top->stats->total_intercept++;
            inst->intercept = true;
        }
        dispInfo += " src" + std::to_string(idx) + "  A" + std::to_string(operand->ptag) + "-ready:"
                    + std::to_string(static_cast<int>(operand->ready)) + " ";
    } else if (operand->type == OperandType::OPD_SYS) {
        SimInst ssrinst = std::make_shared<SimInstInfo>();
        bool ssrqEmpty = top->ssrsetOrderQ.Empty();
        if (!ssrqEmpty) {
            ssrinst = top->ssrsetOrderQ.Front();
        }
        operand->ready = !((!ssrqEmpty) && (LessEqual(ssrinst->bid, ssrinst->rid, inst->bid, inst->rid)));

        dispInfo += " src" + std::to_string(idx) + "  sys" + std::to_string(operand->ptag)
                    + "-ready:" + std::to_string(static_cast<int>(operand->ready)) + " ";
    } else {
        if (OperandTypeIsLoopReg(operand->type)) {
            operand->ready = true;
        } else {
            switch (operand->type) {
                case OperandType::OPD_TLINK:
                    checkTagReady(current.tRegReady[inst->peID][inst->tid], operand);
                    break;
                case OperandType::OPD_ULINK:
                    checkTagReady(current.uRegReady[inst->peID][inst->tid], operand);
                    break;
                case OperandType::OPD_VTLINK:
                case OperandType::OPD_VULINK:
                case OperandType::OPD_VMLINK:
                case OperandType::OPD_VNLINK:
                    checkTagReady(current.vrfReady, operand);
                    break;
                case OperandType::OPD_PREDMASK:
                    checkTagReady(current.simtMaskRegReady[inst->peID][inst->tid], operand);
                    break;
                default:
                    break;
            }
        }
        dispInfo += " src" + std::to_string(idx) + "  " + GetOperandType(operand->type) +
                    std::to_string(operand->ptag) + "-ready:" + std::to_string(static_cast<int>(operand->ready)) + " ";
    }
}

static void CheckPtag(std::vector<PTInfo> &ptagRdy, SimInst &inst, POperandPtr &dst)
{
    if (dst->ptag >= ptagRdy.size()) {
        cout << "error inst: " << inst << endl;
        ASSERT(0);
    }
    PTInfo newTb = std::make_shared<TableInfo>(*ptagRdy[dst->ptag]);
    newTb->bid = inst->bid;
    newTb->rid = inst->rid;
    newTb->peid = inst->peID;
    newTb->retired = false;
    newTb->global = dst->type == OperandType::OPD_GREG;
    ptagRdy[dst->ptag] = newTb;
}

void ReadyState::CheckDst(SimInst &inst, uint32_t idx)
{
    auto &dst = inst->pdsts_[idx];
    if (dst->type == OperandType::OPD_GREG) {
        if (top->id == SCALAR_IEX) {
            CheckPtag(*next.ptagReady, inst, dst);
        } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
            CheckPtag(next.lptagReady, inst, dst);
        }
    } else {
        auto setRTable = [inst](std::vector<std::vector<std::vector<PTInfo>>> &rt, uint32_t tag) {
            PTInfo newTb = std::make_shared<TableInfo>(*rt[inst->peID][inst->tid][tag]);
            ASSERT(tag < rt[inst->peID][inst->tid].size());
            newTb->bid = inst->bid;
            newTb->rid = inst->rid;
            newTb->peid = inst->peID;
            rt[inst->peID][inst->tid][tag] = newTb;
        };
        auto setTagTable = [inst](vector<PTInfo> &rt, uint32_t tag) {
            PTInfo newEntry = make_shared<TableInfo>(*rt[tag]);
            newEntry->bid = inst->bid;
            newEntry->rid = inst->rid;
            newEntry->peid = inst->peID;
            rt[tag] = newEntry;
        };
        switch (inst->pdsts_[idx]->type) {
            case OperandType::OPD_TLINK:
                setRTable(next.tRegReady, inst->pdsts_[idx]->ptag);
                break;
            case OperandType::OPD_ULINK:
                setRTable(next.uRegReady, inst->pdsts_[idx]->ptag);
                break;
            case OperandType::OPD_VTLINK:
            case OperandType::OPD_VULINK:
            case OperandType::OPD_VMLINK:
            case OperandType::OPD_VNLINK:
                setTagTable(next.vrfReady, inst->pdsts_[idx]->ptag);
                break;
            case OperandType::OPD_PREDMASK:
                setRTable(next.simtMaskRegReady, inst->pdsts_[idx]->ptag);
                break;
            default:
                break;
        }
    }
}

void ReadyState::SetRegReadyTable(OperandType type, uint32_t ptag, bool ready,
                                  PLpvInfo lpvInfo, uint32_t peID, uint32_t tid)
{
    if (type == OperandType::OPD_TLINK) {
        ASSERT(ptag < next.tRegReady[peID][tid].size());
        PTInfo newTbT = std::make_shared<TableInfo>(*next.tRegReady[peID][tid][ptag]);
        newTbT->ready = ready;
        SetLpv(lpvInfo, newTbT->lpvInfo, true);
        next.tRegReady[peID][tid][ptag] = newTbT;
    } else if (type == OperandType::OPD_ULINK) {
        ASSERT(ptag < next.uRegReady[peID][tid].size());
        PTInfo newTbU = std::make_shared<TableInfo>(*next.uRegReady[peID][tid][ptag]);
        newTbU->ready = ready;
        SetLpv(lpvInfo, newTbU->lpvInfo, true);
        next.uRegReady[peID][tid][ptag] = newTbU;
    } else if (type == OperandType::OPD_VTLINK || type == OperandType::OPD_VULINK
            || type == OperandType::OPD_VMLINK || type == OperandType::OPD_VNLINK) {
        ASSERT(top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX);
        ASSERT(ptag < next.vrfReady.size());
        PTInfo newEntry = make_shared<TableInfo>(*next.vrfReady[ptag]);
        newEntry->ready = ready;
        SetLpv(lpvInfo, newEntry->lpvInfo, true);
        next.vrfReady[ptag] = newEntry;
    } else if (type == OperandType::OPD_PREDMASK) {
        ASSERT(top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX);
        ASSERT(ptag < next.simtMaskRegReady[peID][tid].size());
        PTInfo newTbP = std::make_shared<TableInfo>(*next.simtMaskRegReady[peID][tid][ptag]);
        newTbP->ready = ready;
        SetLpv(lpvInfo, newTbP->lpvInfo, true);
        next.simtMaskRegReady[peID][tid][ptag] = newTbP;
    } else if (type == OperandType::OPD_GREG) {
        if (top->id == SCALAR_IEX && ptag < next.ptagReady->size()) {
            ASSERT(ptag < (*next.ptagReady).size());
            PTInfo newTbG = std::make_shared<TableInfo>(*((*next.ptagReady)[ptag]));
            newTbG->ready = ready;
            SetLpv(lpvInfo, newTbG->lpvInfo, true);
            (*next.ptagReady)[ptag] = newTbG;
        } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
            ASSERT(ptag < next.lptagReady.size());
            PTInfo newTbG = std::make_shared<TableInfo>(*(next.lptagReady[ptag]));
            newTbG->ready = ready;
            SetLpv(lpvInfo, newTbG->lpvInfo, true);
            next.lptagReady[ptag] = newTbG;
        }
    } else if (type == JCore::OperandType::STACK_POINTER) {
        ASSERT(ptag < next.sptagReady.size());
        PTInfo newTbS = std::make_shared<TableInfo>(*next.sptagReady[ptag]);
        newTbS->ready = ready;
        next.sptagReady[ptag] = newTbS;
    }
}

void ReadyState::setROBReadyTable(uint32_t peid, uint32_t rid, PLpvInfo &lpvInfo, bool ready)
{
    if (peid >= GetSim()->GetCore()->configs.stdPeCount) {
        return;
    }
    PTInfo newTb = std::make_shared<TableInfo>(*next.robReady[peid][rid]);
    newTb->ready = ready;
    SetLpv(lpvInfo, newTb->lpvInfo, true);
    next.robReady[peid][rid] = newTb;
}

static void SetBlockPtagRetire(std::vector<PTInfo> &ptagRdy, ROBID &bid)
{
    for (uint32_t i = 0; i < ptagRdy.size(); i++) {
        auto &ptag = ptagRdy[i];
        if (ptag->ready && ptag->bid == bid && ptag->global) {
            PTInfo newTb = std::make_shared<TableInfo>(*ptag);
            newTb->retired = true;
            ptag = newTb;
        }
    }
}

void ReadyState::setBlockRetire(ROBID bid)
{
    if (top->id == SCALAR_IEX) {
        SetBlockPtagRetire((*next.ptagReady), bid);
    } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        SetBlockPtagRetire(next.lptagReady, bid);
    }
}

static void MarkedPtagRetire(std::vector<PTInfo> &curPtagRdy, std::vector<PTInfo> &nxtPtagRdy, uint32_t ptag)
{
    if (ptag >= curPtagRdy.size())
        return;
    if (curPtagRdy[ptag]->global) {
        nxtPtagRdy[ptag]->inst_retired = true;
        return;
    }
    PTInfo newTb = std::make_shared<TableInfo>(*(nxtPtagRdy[ptag]));
    newTb->retired = true;
    nxtPtagRdy[ptag] = newTb;
}

void ReadyState::setPtagRetire(uint32_t ptag)
{
    if (top->id == SCALAR_IEX) {
        MarkedPtagRetire(*current.ptagReady, *next.ptagReady, ptag);
    } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        MarkedPtagRetire(current.lptagReady, next.lptagReady, ptag);
    }
}

bool ReadyState::checkPtagReady(uint32_t ptag)
{
    if (top->id == SCALAR_IEX) {
        ASSERT(ptag < current.ptagReady->size());
        return (*current.ptagReady)[ptag]->ready;
    } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        ASSERT(ptag < current.lptagReady.size());
        return current.lptagReady[ptag]->ready;
    }

    ASSERT(0 && "not support");
    return false;
}

PLpvInfo ReadyState::checkPtagLpvInfo(uint32_t ptag)
{
    if (top->id == SCALAR_IEX) {
        ASSERT(ptag < next.ptagReady->size());
        return (*next.ptagReady)[ptag]->lpvInfo;
    } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        ASSERT(ptag < next.lptagReady.size());
        return next.lptagReady[ptag]->lpvInfo;
    }

    PLpvInfo lpvInfo;
    ASSERT(0 && "not support");
    return lpvInfo;
}

static void ResetPtag(std::vector<PTInfo> &ptagRdy, uint32_t ptag, IDBus bus, bool global, bool ready)
{
    PTInfo newTb = std::make_shared<TableInfo>(*(ptagRdy[ptag]));
    newTb->ready = ready;
    newTb->bid = bus.bid;
    newTb->rid = bus.rid;
    newTb->peid = bus.peID;
    newTb->global = global;
    newTb->data_vld = false;
    newTb->retired = false;
    newTb->inst_retired = false;
    ptagRdy[ptag] = newTb;
}

void ReadyState::resetPtagID(uint32_t ptag, IDBus bus, bool global)
{
    if (!bus.vld)
        return;
    bool ready = false;
    if (top->id == SCALAR_IEX) {
        ResetPtag(*next.ptagReady, ptag, bus, global, ready);
    } else if (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        ResetPtag(next.lptagReady, ptag, bus, global, ready);
    }
}

void ReadyState::checkLoadReady(SimInst &inst, std::string &dispInfo)
{
    // if (OpcodeIsLoad(inst->opcode) && GetSim()->perfectLoadStore && inst->ref.lsInfo.id_vld) {
    //     inst->addr_rdy = GetSim()->checkRefLoadReady(inst->ref.lsInfo);
    //     if (inst->addr_rdy) {
    //         dispInfo += " address ready ";
    //     } else {
    //         dispInfo += " load depend id: ";
    //         for (auto id : inst->ref.lsInfo.depend_id) {
    //             dispInfo += "-" + std::to_string(id);
    //         }
    //     }
    // }
}

void ReadyState::InitGGPRRtable(uint64_t ptag, uint64_t data)
{
    (*current.ptagReady)[ptag]->ready = true;
    (*current.ptagReady)[ptag]->data_vld = true;
    (*current.ptagReady)[ptag]->data = data;

    (*next.ptagReady)[ptag]->ready = true;
    (*next.ptagReady)[ptag]->data_vld = true;
    (*next.ptagReady)[ptag]->data = data;
}

} // namespace JCore
