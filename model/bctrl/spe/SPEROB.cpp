#include "SPEROB.h"

#include <cassert>

#include "core/Core.h"
#include "iex/iex.h"
#include "SPE.h"
#include "utils/obj_print.h"
#include "DFX/InstTracer.h"

namespace JCore {
using namespace std;

void SPEROB::Build()
{
    peID = speTop->peID;
    uint64_t robDepth = speTop->configs.speROBDepth;
    current.entry.clear();
    next.entry.clear();
    for (uint32_t i = 0; i < robDepth; i++) {
        current.entry.emplace_back();
        next.entry.emplace_back();
        current.entry[i].inst = nullptr;
        next.entry[i].inst = nullptr;
    }

    tcmap.Reset(OperandType::OPD_TLINK);
    ucmap.Reset(OperandType::OPD_ULINK);
    predcmap.Reset(OperandType::OPD_PREDMASK);

    VectorCoreConfig vcConfig;
    vcSidWindow.resize(vcConfig.ta_stq_size);
    for (uint64_t i = 0; i < vcSidWindow.size(); i++) {
        vcSidWindow[i] = false;
    }
    // PEConfig peConfig;
    // IncROBID(ldCanIssMaxSid, top->configs.vcSidMax);
    // IncROBID(oldestRetiredSid, top->configs.vcSidMax);
    // AddROBID(windowEndSid, vcConfig.ta_stq_size, peConfig.vcSidMax);
}

void SPEROB::Reset()
{
    VectorCoreConfig vcConfig;
    vcSidWindow.resize(vcConfig.ta_stq_size);
    for (uint64_t i = 0; i < vcSidWindow.size(); i++) {
        vcSidWindow[i] = false;
    }
    ldCanIssMaxSid.val = 0;
    ldCanIssMaxSid.wrap = false;
    oldestRetiredSid.val = 0;
    oldestRetiredSid.wrap = false;
    windowEndSid.val = 0;
    windowEndSid.wrap = false;

    current.Reset();
    next.Reset();
    tcmap.Reset(OperandType::OPD_TLINK);
    ucmap.Reset(OperandType::OPD_ULINK);
    predcmap.Reset(OperandType::OPD_PREDMASK);
}

void SPEROB::Work()
{
    if (!current.size) {
        return;
    }
    AssignLdStId();

    // ((OPEStats *)top->stats)->total_rob_size += current.size;
    commit();
    dealloc();
    PrintStatus();
    Stats();
}

bool SPEROB::IsInLdWindow(SimInst &inst)
{
    if (GetSim()->GetCore()->configs.load_ooo_enable) {
        return true;
    }

    if (inst->CheckOOOLoad() || inst->vcSid < ldCanIssMaxSid) {
        return true;
    } else {
        return false;
    }
}

bool SPEROB::IsInStWindow(SimInst &inst)
{
    if ((inst->vcSid <= windowEndSid) && (inst->vcSid >= oldestRetiredSid)) {
        return true;
    } else {
        return false;
    }
}

void SPEROB::Xfer()
{
    iex_pe_rslv_q->Work();
    PEResolve();
    next.stall = next.size + speTop->configs.decodeWidth > speTop->configs.speROBDepth;
    current = next;
    tileLdWindow.clear();
    tileStWindow.clear();
}

SimSys *SPEROB::GetSim()
{
    return speTop->GetSim();
}

void SPEROB::setNeedFlush(ROBID bid, ROBID rid, ROBID lsID)
{
    if (!next.entry[rid.val].vld ||
        next.entry[rid.val].bid != bid ||
        next.entry[rid.val].inst->lsID != lsID) {
            return;
    }
    if (next.entry[rid.val].status == INST_RETIRED) {
        for (ROBID temRid = rid; temRid.val != next.commitPtr.val; IncROBID(temRid, next.entry.size())) {
            if (next.entry[rid.val].status == INST_RETIRED) {
                next.osdSize++;
                next.entry[temRid.val].status = INST_NEEDFLUSH;
            }
        }
        next.commitPtr = rid;
    }
    next.entry[rid.val].status = INST_NEEDFLUSH;
}

bool SPEROB::getNeedFlush(ROBID bid, ROBID rid, ROBID lsID)
{
    if (bid != current.entry[rid.val].bid || lsID != current.entry[rid.val].inst->lsID) {
        return false;
    }
    if (current.entry[rid.val].status == INST_NEEDFLUSH) {
        return true;
    }
    return false;
}

ROBID SPEROB::getOldestRID()
{
    return current.entry[current.commitPtr.val].inst->rid;
}

bool SPEROB::IsInLdStWindow(SimInst inst)
{
    ASSERT(OpcodeIsLoad(inst->opcode) || OpcodeIsStore(inst->opcode));
    if (OpcodeIsLoad(inst->opcode)) {
        auto it = tileLdWindow.find(inst->gid);
        if (it != tileLdWindow.end()) {
            bool find = false;
            for (auto instEntry : it->second) {
                if (inst->gid == instEntry->gid && inst->rid == instEntry->rid) {
                    find = true;
                    break;
                }
            }
            return find;
        } else {
            return false;
        }
    }
    if (OpcodeIsStore(inst->opcode)) {
        auto it = tileStWindow.find(inst->gid);
        if (it != tileStWindow.end()) {
            bool find = false;
            for (auto instEntry : it->second) {
                if (inst->gid == instEntry->gid && inst->rid == instEntry->rid) {
                    find = true;
                    break;
                }
            }
            return find;
        } else {
            return false;
        }
    }
    return false;
}

IDBus SPEROB::getCommitID()
{
    IDBus bus;
    bus.vld = false;
    if (current.size != 0) {
        bus.vld = true;
        bus.bid = current.entry[current.deallocPtr.val].bid;
        bus.rid = current.deallocPtr;
        bus.iexTyp = ExecEngineTyp::SCALAR_IEX;
        if (current.entry[current.deallocPtr.val].inst)
            bus.lsID = current.entry[current.deallocPtr.val].inst->lsID;
    }
    return bus;
}

IDBus SPEROB::GetCommitBusID()
{
    return commitIdBus;
}

IDBus SPEROB::getRetireID()
{
    IDBus bus;
    bus.vld = true;
    bus.bid = current.entry[current.commitPtr.val].bid;
    bus.gid = current.entry[current.commitPtr.val].gid;
    bus.rid = current.commitPtr;
    bus.tpc = current.entry[current.commitPtr.val].tpc;
    bus.peID = speTop->peID;
    bus.coreId = speTop->peID;
    SimInst &inst = current.entry[current.commitPtr.val].inst;
    if (next.entry[current.commitPtr.val].status == INST_FREE) {
        bus.vld = false;
    } else if (inst) {
        bus.bid = inst->bid;
        bus.rid = inst->rid;
        bus.tid = inst->tid;
        bus.tpc = inst->pc;
        bus.lsID = inst->lsID;
        bus.lid = inst->load_id;
        bus.sid = inst->sid;
        bus.tSeq = inst->tSeq;
        bus.uSeq = inst->uSeq;
        bus.predSeq = inst->predSeq;
        bus.isTld = inst->opcode == Opcode::OP_TLD;
        // Deadlock check
        bus.isLoadStore = ((OpcodeIsLoad(inst->opcode) || OpcodeIsStore(inst->opcode))) &&
                        !(!inst->stack_check && inst->stack_type == StackInstType::STACK_GET);
        if (next.entry[current.commitPtr.val].status == INST_COMPLETED ||
            next.entry[current.commitPtr.val].status == INST_RETIRED) {
                bus.isLoadStore = false;
        }
        bus.isLoad = OpcodeIsLoad(inst->opcode) &&
                     !(!inst->stack_check && inst->stack_type == StackInstType::STACK_GET);
        bus.pipeID = inst->iqid;
        if (inst->bfuInfo) {
            bus.fbid = inst->bfuInfo ? inst->bfuInfo->fbid : 0;
            bus.fbid_local = inst->bfuInfo ? inst->bfuInfo->fbid_local : 0;
        }
        bus.first = inst->first;
    } else {
        bus.isLoadStore = false;
    }
    bus.iexTyp = ExecEngineTyp::SCALAR_IEX;
    return bus;
}

void SPEROB::SetOldestTileLd(SimInst &inst)
{
}
void SPEROB::SetOldestTileSt(SimInst &inst)
{
}

void SPEROB::DumpRob()
{
    if (peID < 4) return;
    std::cout << "=== dump rob===" << std::endl;
    for (auto &entry : current.entry) {
        if (!entry.vld) continue;
        std::cout << entry.inst;
        if (current.commitPtr.val == entry.rid.val) {
            cout<<" <- oldest";
        }
        cout << std::endl;
    }
}

bool SPEROB::checkDeadlock()
{
    return next.entry[current.commitPtr.val].status != INST_COMPLETED;
}

ROBIDBus SPEROB::GetOldestLSID()
{
    ROBIDBus bus = ROBIDBus();
    if (current.entry[current.commitPtr.val].status != INST_FREE) {
        bus.vld = true;
        bus.id = current.entry[current.commitPtr.val].inst->lsID;
    }
    return bus;
}

void SPEROB::allocROB(SimInst &inst)
{
    PROBEntry e;
    e.vld = true;
    e.status = INST_ALLOCATED;
    e.stack_complete = false;
    e.tpc = inst->pc;
    e.bid = inst->bid;
    e.gid = inst->gid;
    e.last = inst->isLastInBlock;
    e.rid = next.allocPtr;
    inst->rid = e.rid;
    e.inst = inst;
    next.entry[next.allocPtr.val] = e;

    uint64_t robDepth = speTop->configs.speROBDepth;
    IncROBID(next.allocPtr, robDepth);
    next.size++;
    next.osdSize++;
    LOG_INFO_M(Unit::BCC, Stage::D1) << "alloc pe rob, " << inst->Dump() << dec << ", occ:" << next.size;

    if (next.size > robDepth) {
        LOG_ERROR_M(speTop->machineType, Stage::NA) << "wrong at " << inst;
        ASSERT(0 && "PE rob entry overlow");
    }
}

void SPEROB::SetIssued(ROBID rid)
{
    ASSERT(next.entry[rid.val].inst != nullptr);
    next.entry[rid.val].inst->issued = true;
}

void SPEROB::AssignLdStId()
{
    return;
}

void SPEROB::PEResolve()
{
    while (!iex_pe_rslv_q->Empty()) {
        PEResolveBus peResolve = iex_pe_rslv_q->Read();
        if (peResolve.isComplete) {
            CompleteROB(peResolve);
            continue;
        }
        if (peResolve.isIssue) {
            issueROB(peResolve);
            continue;
        }
        if (peResolve.isSrcRead) {
            PreReleaseSrcROB(peResolve);
            continue;
        }
        if (peResolve.isLDCancel) {
            if (next.entry[peResolve.rid.val].status != INST_NEEDFLUSH &&
                next.entry[peResolve.rid.val].status != INST_FAULT &&
                next.entry[peResolve.rid.val].status != INST_FREE) {
                next.entry[peResolve.rid.val].status = INST_RENAMED;
            }
        }
    }
}

void SPEROB::issueROB(const PEResolveBus &peResolve)
{
    uint32_t rid = peResolve.rid.val;
    if (next.entry[rid].status == INST_NEEDFLUSH) {
        return;
    }
    next.entry[rid].status = INST_ISSUED;

    speTop->stats->curr_cycle_issued_cnt++;
}

void SPEROB::PreReleaseSrcROB(const PEResolveBus &peResolve)
{
    (void)peResolve;
}

bool SPEROB::CheckStackComplate(const PEResolveBus &peResolve, uint32_t rid)
{
    if (!current.entry[rid].stack_complete) {
        next.entry[rid].stack_complete = true;
        for (size_t i = 0; i < next.entry[rid].inst->pdsts_.size(); i++) {
            CheckDstDataOutVld(next.entry[rid].inst->pdsts_[i], peResolve.dsts[i]);
        }
        if (GetSim()->GetViewManager(next.entry[rid].inst->stid)->config.printPipeView ||
            GetSim()->core->pTracer->IsEnabled()) {
            next.entry[rid].inst->pipeCycle = peResolve.pipe_cycle;
            next.entry[rid].inst->iq_name = peResolve.iq_name;
            next.entry[rid].inst->pipeCycle->completeCycle = GetSim()->getCycles() + 1;
        }

        LOG_INFO_M(speTop->machineType, Stage::CT) << current.entry[rid].inst << " stack-get completed";
        return false;
    }
    return true;
}

bool SPEROB::CheckStack(const PEResolveBus &peResolve, uint32_t rid)
{
    // Stack load check only
    if (peResolve.stack) {
        if (!CheckStackComplate(peResolve, rid)) {
            return false;
        }

        for (size_t i = 0; current.entry[rid].inst->pdsts_.size(); i++) {
            if (!CheckDstDataOut(current.entry[rid].inst->pdsts_[i], peResolve.dsts[i]->data, peResolve, rid)) {
                return false;
            }
        }

        LOG_INFO_M(speTop->machineType, Stage::CT) << current.entry[rid].inst << " stack-get correctly completed";
        GetSim()->core->flushUnit->flush_stats->stackGetCorrect++;
    } else if (!current.entry[rid].inst->stack_check &&
               current.entry[rid].inst->stack_type == StackInstType::STACK_GET) {
        GetSim()->core->flushUnit->flush_stats->stackGetCorrect++;
    }
    return true;
}

bool SPEROB::CheckDstDataOut(const POperandPtr &dst, uint64_t peResolvDataOut,
    const PEResolveBus &peResolve, uint32_t rid)
{
    if (dst->dataVld && peResolvDataOut != dst->data) {
        FlushReq req;
        req.vld = true;
        req.bid = peResolve.bid;
        req.rid = peResolve.rid;
        req.tid = m_tid;
        req.stid = peResolve.stid;
        req.fetchTPCVld = true;
        req.fetchTPC = current.entry[rid].inst->pc;
        req.peID = peID;
        req.lsID = current.entry[rid].inst->lsID;
        req.type = FlushType::INNER_FLUSH;
        req.tSeq = current.entry[rid].inst->tSeq;
        req.uSeq = current.entry[rid].inst->uSeq;
        req.predSeq = current.entry[rid].inst->predSeq;
        req.fbid = current.entry[rid].inst->bfuInfo->fbid;
        req.fbid_local = current.entry[rid].inst->bfuInfo->fbid_local;
        // req.noSplitBlk = current.entry[rid].inst->noSplitBlk;
        req.firstInst = current.entry[rid].inst->first;
        req.iexTyp = current.entry[rid].inst->iexType;
        speTop->ReportBlockFlush(req);
        next.entry[rid].status = INST_NEEDFLUSH;
        GetSim()->core->flushUnit->flush_stats->IntraBlockMemoryAaccelssConflict++;
        GetSim()->core->flushUnit->flush_stats->smtIntraBlockMemoryAaccelssConflictArray[req.stid]++;
        GetSim()->core->flushUnit->flush_stats->stackGetIncorrect++;
        speTop->stack_error_pc_q->Write(current.entry[rid].inst->pc);
        return false;
    }
    return true;
}

void SPEROB::CheckDstDataOutVld(POperandPtr &dst, const POperandPtr &resolveDst)
{
    dst->dataVld = resolveDst->dataVld;
    if (dst->dataVld) {
        dst->data = resolveDst->data;
        if (dst->vecDataVld) {
            dst->vecData = resolveDst->vecData;
        }
    }
}

void SPEROB::PrintTlsPipeViewLog(SimInst &inst, uint64_t addr)
{
    InstPipeViewPtr instInfo = std::make_shared<InstPipeViewInfo>();
    instInfo->bid = inst->bid.val;
    instInfo->cycleInfo = std::make_shared<CycleInfo>();
    instInfo->cycleInfo->instStartCycle = inst->pipeCycle->instStartCycle;
    instInfo->cycleInfo->sendToScalperCycle = inst->pipeCycle->sendToScalperCycle;
    instInfo->cycleInfo->sendToTileReqCycle = inst->pipeCycle->sendToTileReqCycle;
    instInfo->cycleInfo->genPrefetchCycle = inst->pipeCycle->genPrefetchCycle;
    instInfo->cycleInfo->prefetchDataRetCycle = inst->pipeCycle->prefetchDataRetCycle;
    instInfo->cycleInfo->genLoadReadReqCycle = inst->pipeCycle->genLoadReadReqCycle;
    instInfo->cycleInfo->tileDataRetCycle = inst->pipeCycle->tileDataRetCycle;
    instInfo->cycleInfo->genStoreReqCycle = inst->pipeCycle->genStoreReqCycle;
    instInfo->cycleInfo->loadDataReturnCycle = inst->pipeCycle->loadDataReturnCycle;
    instInfo->cycleInfo->tlsCompleteCycle = GetSim()->getCycles();
    instInfo->cycleInfo->retireCycle = GetSim()->getCycles() + 1;
    std::stringstream oss;
    oss << "TStore ";
    oss << " Write GM 0x" << std::hex << addr;
    instInfo->label =  oss.str();
    GetSim()->GetViewManager(inst->stid)->RecordMinst(instInfo);
}

void SPEROB::CompleteROB(const PEResolveBus &peResolve)
{
    uint32_t rid = peResolve.rid.val;
    if (next.entry[rid].status == INST_NEEDFLUSH) {
        LOG_DEBUG_M(Unit::BCC, Stage::CT) << "CompleteROB need flush b:" <<  peResolve.bid.val
                                          <<":G0"  <<":R" << peResolve.rid.val;
        return;
    }
    if (current.entry[rid].inst->opcode == Opcode::OP_TSD) {
        next.entry[rid].inst->subInstCnt = current.entry[rid].inst->subInstCnt - 1;
        if (GetSim()->GetViewManager(next.entry[rid].inst->stid)->config.printPipeView) {
            PrintTlsPipeViewLog(current.entry[rid].inst, peResolve.dsts[DST0_IDX]->data);
        }

        LOG_INFO_M(Unit::MIEX, Stage::BROB) << " CompleteROB TStore bid: " << dec <<  peResolve.bid.val
                                         << " rid " << dec << rid
                                         << " dataOut0 :0x " << hex << peResolve.dsts[DST0_IDX]->data
                                         << " subInstCnt " << dec << next.entry[rid].inst->subInstCnt;

        if (next.entry[rid].inst->subInstCnt == 0) {
            next.entry[rid].status = INST_COMPLETED;
            return;
        }
    }

    if (!CheckStack(peResolve, rid)) {
        LOG_DEBUG_M(Unit::BCC, Stage::CT) << current.entry[rid].inst << " !CheckStack(peResolve, rid)";
        return;
    }

    if (!peResolve.isPipeStore) {
        next.entry[rid].status = INST_COMPLETED;
    }
    if (next.branchVld && next.branchPtr == peResolve.rid) {
        setBranch(false);
    }

    if (peResolve.srcMVld) {
        // next.entry[rid].inst->srcM.vld = true;
        // next.entry[rid].inst->srcM.simtMask = peResolve.simtMask;
    }

    // if (peResolve.dstMvld) {
        // next.entry[rid].inst->dstM.dataOut_vld = true;
        // next.entry[rid].inst->dstM.dataOut = peResolve.dstMval;
    // }

    for (size_t i = 0; i < next.entry[rid].inst->pdsts_.size(); i++) {
        CheckDstDataOutVld(next.entry[rid].inst->pdsts_[i], peResolve.dsts[i]);
    }
    if (peResolve.accMemInfo) {
        next.entry[rid].inst->accMemInfo = peResolve.accMemInfo;
    }
    if (peResolve.setcInfo) {
        next.entry[rid].inst->setcInfo = peResolve.setcInfo;
    }
    if (peResolve.ssrInfo) {
        next.entry[rid].inst->ssrInfo = peResolve.ssrInfo;
    }
    if (peResolve.atomicInfo) {
        next.entry[rid].inst->atomicInfo = peResolve.atomicInfo;
    }
    if (peResolve.reduceInfo) {
        next.entry[rid].inst->reduceInfo = peResolve.reduceInfo;
    }
    if (peResolve.brInfo) {
        next.entry[rid].inst->brInfo = peResolve.brInfo;
    }
    next.entry[rid].inst->pipeCycle = peResolve.pipe_cycle;
    next.entry[rid].inst->iq_name = peResolve.iq_name;
    next.entry[rid].inst->pipeCycle->completeCycle = GetSim()->getCycles() + 1;
    LOG_INFO_M(speTop->machineType, Stage::CT) << current.entry[rid].inst->Dump() << " completed";
}

void SPEROB::setROBInst(SimInst &inst)
{
    next.entry[inst->rid.val].inst = inst;
    return;
}

void SPEROB::setLastBlockEnd()
{
    ROBID rid = getAllocPtr();
    SubROBID(rid, 1, next.entry.size());
    PROBEntry &uop = next.entry[rid.val];
    if (uop.status == INST_FREE || !uop.inst) {
        return;
    }
    if (uop.inst->opcode == Opcode::OP_TLD || uop.inst->opcode == Opcode::OP_TSD) {
        return;
    }
    uop.inst->isLastInBlock = true;
    uop.last = true;
}

void SPEROB::CommitInsn()
{
    LOG_INFO_M(speTop->machineType, Stage::CT) << " full block retired dealloc entry " << dec << next.deallocPtr
        << current.entry[next.deallocPtr.val].inst->Dump() <<" rob size " << dec << current.size << " osd size " << dec
        << current.osdSize;

    SimInst inst = current.entry[next.deallocPtr.val].inst;
    PLpvInfo lpvInfo;
    CheckTagVld(inst, lpvInfo);

    next.entry[next.deallocPtr.val].status = INST_FREE;
    next.entry[next.deallocPtr.val].vld = false;
    IncROBID(next.deallocPtr, next.entry.size());
    ASSERT(next.size>0);
    next.size--;
}

void SPEROB::ReportLocalRegBlockCommit(ROBID bid, uint32_t stid)
{
    speTop->d2Stage.ReportSGPRBlockCommit(bid, stid);
}

void SPEROB::CommitBlock(PROBEntry &uop)
{
    BlockCommandPtr cmd = GetSim()->core->bctrl->blockROB.GetBlockCMDPtr(uop.inst->bid, uop.inst->stid);
    LOG_INFO_M(speTop->machineType, Stage::CT) << " commit block" << dec << uop.inst->bid <<" " << uop.inst; // todo fix
    cur_inst_cm_cnt = 0;
    // dealloc ROB entries
    GetSim()->core->bctrl->blockROB.reportRealBsize(uop.bid.val, &(speTop->peMInstStats), uop.inst->stid);
    speTop->peMInstStats.instCommitStats[uop.bid.val]->totalCommitInst = 0;
    for (uint32_t i = 0; i < current.entry.size(); i++) {
        if (current.entry[next.deallocPtr.val].vld
            && current.entry[next.deallocPtr.val].bid == uop.inst->bid) {
            CommitInsn();
        }
    }
    if (cmd == nullptr || IsBlockTypeParallel(cmd->blockType)) {
        return;
    }

    bool tmp = ((uop.inst->iexType == MEM_IEX) && ((uop.inst->opcode == Opcode::OP_TLD) || (uop.inst->opcode == Opcode::OP_TSD)));
    if ((peID == GetSim()->core->GetMtcPEIndex()) && tmp) {
        BlockCommandPtr cmd = GetSim()->core->bctrl->blockROB.GetBlockCMDPtr(uop.inst->bid, uop.inst->stid);
        cmd->cmdExecCompleted = true;
        ASSERT(GetSim()->core->mtcCores[0] != nullptr);
        GetSim()->core->mtcCores[0]->mtcBCCWakeupQ->Write(cmd);
    }
    speTop->PEBase::SetBlockComplete(uop.inst->bid, uop.inst->stid);
    CleanCMAP(uop.inst->bid);
    ReportLocalRegBlockCommit(uop.inst->bid, uop.inst->stid);
    ASSERT(speTop->d2Stage.sgprRenameUnit[0][0][0].CheckLegal());
    if (uop.inst->terminate) {
        speTop->PEBase::SetTerminate(uop.inst->bid, uop.inst->stid);
    }
}

void SPEROB::CleanCMAP(ROBID bid)
{
    CleanCMAP(tcmap, bid);
    CleanCMAP(ucmap, bid);
    CleanCMAP(predcmap, bid);
}

void SPEROB::CleanCMAP(RelateCmap& cmap, ROBID bid)
{
    RelateCmap newCmap;
    newCmap.type = cmap.type;
    RelateInfo info;
    while (!cmap.empty()) {
        info = cmap.read();
        if (info.bid == bid) {
            if (!info.kill) {
               SetRegReadyTable(cmap, info, SCALAR_IEX);
            }
            continue;
        }
        newCmap.write(info);
    }
    cmap = newCmap;
}

void SPEROB::CleanGroupCMAP(ROBID bid, ROBID gid)
{
    CleanGroupCMAP(tcmap, bid, gid);
    CleanGroupCMAP(ucmap, bid, gid);
    CleanGroupCMAP(predcmap, bid, gid);
}

void SPEROB::CleanGroupCMAP(RelateCmap& cmap, ROBID bid, ROBID gid)
{
    RelateCmap newCmap;
    RelateInfo info;
    newCmap.type = cmap.type;
    while (!cmap.empty()) {
        info = cmap.read();
        if (info.bid == bid && info.gid == gid) {
            if (info.kill) continue;
            if (GetSim()->core->IsVecPe(peID)) {
                SetRegReadyTable(cmap, info, SIMT_IEX);
            } else if (peID >= GetSim()->core->configs.stdPeCount + GetSim()->core->configs.simtPeCount) {
                SetRegReadyTable(cmap, info, MEM_IEX);
            }
            continue;
        }
        newCmap.write(info);
    }
    cmap = newCmap;
}

void SPEROB::CommitLast(PROBEntry &uop)
{
    CommitBlock(uop);
}

void SPEROB::CheckCAExecRes(PROBEntry &uop, SimInst &inst)
{
    // if reduced to local gpr, do not send request to rdunit
    // do it in UOP::Execute
    if (OpcodeInInstGroup(uop.inst->opcode, InstGroup::REDUCE) && inst->SrcTypeContain(OperandType::OPD_GREG)) {
        pe_iex_rd_q->Write(uop.inst);
    }
}

void SPEROB::IncrementStats(PROBEntry &uop)
{
    // increment stats
    speTop->stats->minsts++;
    std::shared_ptr<IEX> iex = GetSim()->core->iex[uop.inst->iexType];
    ++iex->stats->slots_retired;
    if (OpcodeIsLoad(uop.inst->opcode)) {
        speTop->stats->retired_load++;
    } else if (OpcodeIsStore(uop.inst->opcode)) {
        speTop->stats->retired_store++;
    } else if (OpcodeIsInnerJump(uop.inst->opcode)) {
        speTop->stats->retired_innerJump++;
    }

    if (OpcodeIsLoad(uop.inst->opcode)) {
        ++speTop->stats->retiredLoadInst;
    } else if (OpcodeIsStore(uop.inst->opcode)) {
        ++speTop->stats->retiredStoreInst;
    } else {
        // ALU
        ++speTop->stats->retiredAluInst;
        if (IsScalarInst(uop.inst->codeLen)) {
            ++speTop->stats->retiredSAluInst;
        } else {
            ++speTop->stats->retiredVAluInst;
        }
    }

    if (IsScalarInst(uop.inst->codeLen)) {
        speTop->stats->retiredScalar++;
    } else {
        speTop->stats->retiredVector++;
    }

    if (OpcodeIsLoad(uop.inst->opcode) || OpcodeIsStore(uop.inst->opcode)) {
        // TODO: Gather/Scatter
    }

    if (OpcodeIsDivSqrt(uop.inst->opcode)) {
        speTop->stats->retiredDivSqrt++;
    }

    auto checkMdbRelease = [uop](std::shared_ptr<IEX> iex) {
        iex->iexmdb.releaseConf(uop.inst->wait_store, uop.inst->addrWakeuped);
    };
    if (OpcodeIsLoad(uop.inst->opcode) && uop.inst->wait_store != -1) {
        checkMdbRelease(iex);
    }

    // When the super-block becomes the oldest, the commited registers should be
    // marked as inst_retired to avoid wrapping by rid.
    for (auto &pdst : uop.inst->pdsts_) {
        if (pdst->type == OperandType::OPD_GREG) {
            iex->rtable.setPtagRetire(pdst->ptag);
        }
    }
}

void SPEROB::HandleEnd(PROBEntry &uop, SimInst &inst)
{
    // exit_group is sim_end
    if (!GetSim()->core->IsVectorIex(inst->iexType) && (inst->iexType != MEM_IEX)) {
        if (uop.inst->terminate) {
            LOG_INFO_M(Unit::BCC, Stage::NA) << "inst: " << uop.inst->Dump() << " is terminate, report to BROB";
            GetSim()->core->bctrl->blockROB.reportTerminate(inst->bid, inst->stid);
        }
    }
}

void SPEROB::commit()
{
    // commit ROB entries, no need to keep T registers
    uint32_t bandwith = speTop->configs.retiredWidth;
    for (uint32_t i = 0; i < bandwith; i++) {
        PROBEntry &uop = current.entry[next.commitPtr.val];
        if (uop.vld && uop.inst) {
            if (OpcodeIsLoad(uop.inst->opcode)) {
                speTop->stats->sLoadInstsCycles += (IsScalarInst(uop.inst->codeLen)) ? 0 : 1;
                speTop->stats->vLoadInstsCycles += (IsScalarInst(uop.inst->codeLen)) ? 1 : 0;
            } else if (OpcodeIsStore(uop.inst->opcode)) {
                speTop->stats->sStoreInstsCycles += (IsScalarInst(uop.inst->codeLen)) ? 0 : 1;
                speTop->stats->vStoreInstsCycles += (IsScalarInst(uop.inst->codeLen)) ? 1 : 0;
            } else {
                speTop->stats->sAluInstsCycles += (IsScalarInst(uop.inst->codeLen)) ? 0 : 1;
                speTop->stats->vAluInstsCycles += (IsScalarInst(uop.inst->codeLen)) ? 1 : 0;
            }
        }
        if (!(uop.vld && uop.status == INST_COMPLETED)) {
            break;
            cout << " retire entry fail " << dec << next.commitPtr <<" " << uop.inst
                << " size " << dec << current.size << " osd size " << dec << current.osdSize << endl;
        }
        LOG_INFO_M(speTop->machineType, Stage::RE) << " retire entry " << dec << next.commitPtr <<" " << uop.inst
            << " size " << dec << current.size << " osd size " << dec << current.osdSize;
        next.entry[next.commitPtr.val].status = INST_RETIRED;
        if (GetSim()->GetViewManager(uop.inst->stid)->config.printPipeView && (uop.inst->opcode != Opcode::OP_TLD) &&
            (uop.inst->opcode != Opcode::OP_TSD)) {
            next.entry[next.commitPtr.val].inst->pipeCycle->retireCycle = GetSim()->getCycles() + 1;
        }
        if (uop.inst->first) {
            cur_inst_cm_cnt = 0;
        }
        cur_inst_cm_cnt++;
        retire_inst_cnt++;
        // Record the number of different types of minsts
        ReportInstCnt(uop.inst);
        RecordTrace(uop.inst);

        ReportLocalRegStats(uop.inst, uop.bid.val, &(speTop->peMInstStats));
        SimInst &inst = next.entry[next.commitPtr.val].inst;
        ASSERT(next.osdSize>0);
        next.osdSize--;
        IncrementStats(uop);

        CheckCAExecRes(uop, inst);
        next.commitBid = inst->bid;
        uint64_t robDepth = speTop->configs.speROBDepth;

        if (OpcodeIsStore(inst->opcode)) {
                youngest_noncommit_sid = uop.inst->sid;
                tileStoreCredit++;
                uint32_t dataSrc = GetStoreDataSrcIndex(inst->opcode);
                if (inst->accMemInfo && inst->psrcs_.size() > dataSrc) {
                    GetSim()->observeTestFinisher(inst->accMemInfo->accMemAddr, inst->psrcs_[dataSrc]->data,
                                                  GetLoadStoreBytes(inst->opcode));
                    GetSim()->observeUartWrite(inst->accMemInfo->accMemAddr, inst->psrcs_[dataSrc]->data,
                                               GetLoadStoreBytes(inst->opcode));
                }
        }

        if (OpcodeIsLoad(inst->opcode)) {
            tileLoadCredit++;
        }
        IncROBID(next.commitPtr, robDepth);

        const uint32_t maxInstCount = 512;
        if (!uop.last && cur_inst_cm_cnt >= maxInstCount &&
            !GetSim()->core->bctrl->blockROB.needFlush(uop.bid, uop.inst->stid)) {
            cur_inst_cm_cnt = 0;
            break;
        }
    }
}

void SPEROB::dealloc()
{
    // dealloc ROB entries, no need to keep T registers
    uint32_t bandwith = speTop->configs.retiredWidth;
    for (uint32_t i = 0; i < bandwith; i++) {
        PROBEntry uop = current.entry[next.deallocPtr.val];
        if (uop.vld && uop.status == INST_RETIRED) {
            LOG_INFO_M(speTop->machineType, Stage::RE) << " ROB: check retire dealloc entry "
                << next.deallocPtr << " " << current.entry[next.deallocPtr.val].inst <<" rob size "
                << dec << current.size << " osd size " << current.osdSize;
            if (uop.inst->stack) {
                GetSim()->core->sRenameUnit->stackRetire(uop.inst->bid, uop.inst->rid, uop.inst->lsID);
            }
            ReleaseRelative(uop);
            HandleEnd(uop, uop.inst);
            MinstResVerify(uop);
            if (uop.inst->opcode != Opcode::OP_BSTOP) {
                MinstPipeView(uop);
            }
            if (uop.last && !GetSim()->core->bctrl->blockROB.needFlush(uop.bid, uop.inst->stid)) {
                CommitLast(uop);
                break;
            }
            next.entry[next.deallocPtr.val].status = INST_FREE;
            next.entry[next.deallocPtr.val].vld = false;
            IncROBID(next.deallocPtr, next.entry.size());
            ASSERT(next.size>0);
            next.size--;
        } else {
            break;
        }
    }
}

bool SPEROB::CheckFlushReqBid(FlushBus &flushReq)
{
    if ((flushReq.baseOnBid && LessEqual(flushReq.req.bid, next.commitBid)) ||
        (!flushReq.baseOnBid && LessROBID(flushReq.req.bid, next.commitBid))) {
        return true;
    }
    return false;
}

void SPEROB::CheckFlushReq(FlushBus &flushReq, const ROBID fbid)
{
    if (flushReq.baseOnBid) {
        speTop->peMInstStats.ReleaseEntry(fbid.val);
    }

    ROBID bid = GetSim()->core->bctrl->blockROB.getOldestBlockID(flushReq.req.stid);
    if (CheckFlushReqBid(flushReq) || (bid == next.commitBid && bid == next.commitBid)) {
        cur_inst_cm_cnt = 0;
    }
    // 此处逻辑存在风险，从功能角度，被Flush 的块，相应指令统计信息清零
    if (CheckFlushReqBid(flushReq)) {
        speTop->peMInstStats.instCommitStats[bid.val]->totalCommitInst = 0;
    }
}

void SPEROB::HandleNextEntryVldAndLessEq(ROBID &old_alloc, ROBID &next_alloc, const ROBID &ptr, bool &found,
    SimInst &oldestFlushInst)
{
    if (!found) {
        /* (old_alloc, next_alloc] is the range of flushed non-retired Minst */
        old_alloc = next.allocPtr;
        next_alloc = ptr;
        next.allocPtr = ptr;
        found = true;
        if (!oldestFlushInst || LessEqual(next.entry[ptr.val].inst->bid, next.entry[ptr.val].inst->rid,
            oldestFlushInst->bid, oldestFlushInst->rid)) {
                oldestFlushInst = next.entry[ptr.val].inst;
        }
    }
}

// TODO:: Set simt flag for module
ExecEngineTyp SPEROB::GetIexType()
{
    return SCALAR_IEX;
}

void SPEROB::FlushRelativeReg(const FlushBus &flushReq, RelateCmap &cmap)
{
    ExecEngineTyp iexTpye = GetIexType();
    while (!cmap.empty()) {
        bool localNeedFlush = flushReq.baseOnBid ? LessEqual(flushReq.req.bid, cmap.back().bid) :
                        LessROBID(flushReq.req.bid, cmap.back().bid);
        if (!localNeedFlush) {
            break;
        }
        RelateInfo info = cmap.pop_back();
        SetRegReadyTable(cmap, info, iexTpye);
    }
}

void SPEROB::CheckNextEntryStatus(const ROBID &ptr)
{
    if (next.entry[ptr.val].status==INST_ALLOCATED || next.entry[ptr.val].status==INST_RENAMED
        || next.entry[ptr.val].status==INST_ISSUED
        || next.entry[ptr.val].status==INST_COMPLETED || next.entry[ptr.val].status==INST_NEEDFLUSH) {
        ASSERT(next.osdSize>0);
        next.osdSize--;
    }

    next.stall = false;
    next.entry[ptr.val].vld = false;
}

void SPEROB::CheckTagVld(SimInst &inst, PLpvInfo &lpvInfo)
{
    std::shared_ptr<IEX> iex;
    if (GetSim()->core->IsVectorIex(inst->iexType)) {
        iex = GetSim()->core->vectorTop->GetIex(speTop->coreId);
    } else {
        iex = GetSim()->core->iex[inst->iexType];
    }
    for (auto &pdst : inst->pdsts_) {
        // release the index register used as the destination register of the instruction
        if (OperandTypeIsLocalReg(pdst->type) && pdst->renamed) {
            iex->SetRegReadyTable(pdst->type, pdst->ptag, false, lpvInfo, inst->peID, this->m_tid);
        }
    }
}

void SPEROB::CheckDstTagVld(SimInst &inst)
{
    PLpvInfo lpvInfo;
    CheckTagVld(inst, lpvInfo);

    std::shared_ptr<IEX> iex = GetSim()->core->iex[inst->iexType];
    iex->rtable.setROBReadyTable(inst->peID, inst->rid.val, lpvInfo, false);
}

void SPEROB::SetNextBranchVld(bool hasInnerBR, const ROBID lastInnerPtr)
{
    if (hasInnerBR) {
        next.branchVld = true;
        next.branchPtr = lastInnerPtr;
    } else {
        next.branchVld = false;
    }
}

void SPEROB::HandleNextEntryVldAndFound(FlushBus &flushReq, const ROBID ptr, const ROBID fbid)
{
    CheckNextEntryStatus(ptr);
    SimInst inst = next.entry[ptr.val].inst;
    // Update reference pointer
    // if (inst->ref.gsInfo.ref_vld && !flushReq.baseOnBid && fbid == next.entry[ptr.val].bid) {
    //     GetSim()->refInfo.refTrace.entry[next.entry[ptr.val].bid.val].decGSRPtr();
    // }
    // if (inst->ref.lsInfo.ref_vld && !flushReq.baseOnBid && fbid == next.entry[ptr.val].bid) {
    //     GetSim()->refInfo.refTrace.entry[inst->bid.val].decLSRPtr();
    //     if (OpcodeIsStore(inst->opcode)) {
    //         GetSim()->recoverRefStq(inst->ref.lsInfo.id);
    //     }
    // }

    if (inst->pdsts_.size() > DST1_IDX && inst->pdsts_[DST1_IDX]->type == OperandType::OPD_GREG && inst->renamed &&
        inst->pdsts_[DST1_IDX]->value == 1) {
        next.stackVld = true;
    }

    CheckDstTagVld(inst);

    next.entry[ptr.val].status = INST_FREE;
}

void SPEROB::SetNextVal()
{
    if (!next.size) {
        next.commitPtr = next.allocPtr;
        next.deallocPtr = next.allocPtr;
        next.stackVld = true;
    }
    if (!next.osdSize) {
        next.commitPtr = next.allocPtr;
    }

    next.stall = next.size + speTop->configs.decodeWidth > speTop->configs.speROBDepth;
}

void SPEROB::HandleROBID(FlushBus &flushReq, ROBID fbid, ROBID frid, ROBID ptr)
{
    ROBID resetPtr = next.deallocPtr;
    for (uint32_t i = 0; i < next.entry.size(); ++i) {
        if (!next.entry[resetPtr.val].vld) break;
        bool lessEq = flushReq.baseOnBid ? LessEqual(fbid, next.entry[ptr.val].bid):
            LessEqual(fbid, frid, next.entry[ptr.val].bid, next.entry[ptr.val].rid);
        if (OpcodeIsInnerJump(next.entry[resetPtr.val].inst->opcode) && lessEq &&
            (next.entry[resetPtr.val].status == INST_RENAMED ||
             next.entry[resetPtr.val].status == INST_ISSUED)) {
            next.branchVld = true;
            next.branchPtr = resetPtr;
        }
        IncROBID(resetPtr, next.entry.size());
    }
}

void SPEROB::flush(FlushBus flushReq)
{
    // flush the ROB state
    ROBID fbid = flushReq.req.bid;
    ROBID frid = flushReq.req.rid;

    bool found = false;
    bool overCommitPtr = false;
    ROBID old_alloc;
    ROBID next_alloc;
    bool hasInnerBR = false;
    ROBID lastInnerPtr;

    CheckFlushReq(flushReq, fbid);

    ROBID ptr = next.deallocPtr;
    SimInst oldestFlushInst = std::shared_ptr<SimInstInfo>(nullptr);
    for (uint32_t i = 0; i < next.entry.size(); i++) {
        if (ptr == next.commitPtr) overCommitPtr = true;
        bool lessEq = flushReq.baseOnBid ? LessEqual(fbid, next.entry[ptr.val].bid):
            LessEqual(fbid, frid, next.entry[ptr.val].bid, next.entry[ptr.val].rid);
        if (LessEqual(fbid, next.entry[ptr.val].bid)) {
            speTop->peMInstStats.instCommitStats[next.entry[ptr.val].bid.val]->totalCommitInst = 0;
        }
        if (next.entry[ptr.val].vld && lessEq) {
            HandleNextEntryVldAndLessEq(old_alloc, next_alloc, ptr, found, oldestFlushInst);
            // Just for inner perfectBP debug
            if (OpcodeIsStore(next.entry[ptr.val].inst->opcode)) {
                youngest_noncommit_sid = next.entry[ptr.val].inst->sid;
                tileStoreCredit++;
            }

            if (OpcodeIsLoad(next.entry[ptr.val].inst->opcode)) {
                tileLoadCredit++;
            }
            if (flushReq.baseOnBid || fbid != next.entry[ptr.val].bid) {
                speTop->peMInstStats.ReleaseEntry(next.entry[ptr.val].bid.val);
            }
            if (!overCommitPtr) next.commitPtr = ptr;
        } else if (next.entry[ptr.val].vld && (next.entry[ptr.val].status == INST_ALLOCATED ||
                    next.entry[ptr.val].status == INST_RENAMED || next.entry[ptr.val].status == INST_ISSUED)
                    && OpcodeIsInnerJump(next.entry[ptr.val].inst->opcode)) {
            hasInnerBR = true;
            lastInnerPtr = ptr;
        }

        if (next.entry[ptr.val].vld && found) {
            ASSERT(next.size>0);
            next.size--;
            if (next.branchVld && ptr==next.branchPtr) {
                SetNextBranchVld(hasInnerBR, lastInnerPtr);
            }
            HandleNextEntryVldAndFound(flushReq, ptr, fbid);
        }
        IncROBID(ptr, next.entry.size());
    }

    SetNextVal();

    HandleROBID(flushReq, fbid, frid, ptr);

    // Relative register flush
    FlushRelativeReg(flushReq, tcmap);
    FlushRelativeReg(flushReq, ucmap);
    FlushRelativeReg(flushReq, predcmap);
    needFlush = false;
}

void SPEROB::PrintStatus()
{
    if (LoggerManager::GetManager().level > LoggerLevel::DETAIL) {
        return;
    }

    uint64_t robDepth = speTop->configs.speROBDepth;
    LOG_DETAIL_M(speTop->machineType, Stage::ROB) << "==================================================";
    LOG_DETAIL_M(speTop->machineType, Stage::ROB) << "PEID:" << std::dec << peID << " tid:" << m_tid << ", size "
        << current.size <<" osdSize " << current.osdSize;
    if (current.size != 0) {
        for (uint32_t i = 0; i < robDepth; i++) {
            uint32_t ptr = (current.deallocPtr.val+i) % robDepth;
            if (!current.entry[ptr].vld) {
                continue;
            }
            std::stringstream oss;
            oss << "\t|-- " << current.entry[ptr].Dump();
            if (current.commitPtr.val == ptr) {
                oss << "<- oldest";
            }
            if (current.entry[ptr].inst->isLastInBlock) {
                oss << "<- stop of B" << current.entry[ptr].bid;
            }
            LOG_DETAIL_M(speTop->machineType, Stage::ROB) << oss.str();
        }
    }
    LOG_DETAIL_M(speTop->machineType, Stage::ROB) << "==================================================";
}

void SPEROB::CheckReg(PROBEntry &uop, RelateCmap &cmap, RelateInfo &info)
{
    PLpvInfo lpvInfo;
    SimInst &inst = uop.inst;
    std::shared_ptr<IEX> iex;
    if (GetSim()->core->IsVectorIex(uop.inst->iexType)) {
        iex = GetSim()->core->vectorTop->GetIex(speTop->coreId);
    } else {
        iex = GetSim()->core->iex[uop.inst->iexType];
    }
    auto releaseReg = [this](PROBEntry &uop, RelateCmap &cmap, RelateInfo &info) {
        auto &sgpr = speTop->d2Stage.sgprRenameUnit[uop.inst->peID][uop.inst->stid][SGPRType2Idx(cmap.type)];
        sgpr.ReportRetired(info.seq.val, true);
    };
    if (uop.last || (!cmap.empty() && (cmap.back().bid != inst->bid || cmap.back().gid != inst->gid))) {
        while (!cmap.empty()) {
            info = cmap.read();
            if (info.kill) {
                continue;
            }
            switch (cmap.type) {
                case OperandType::OPD_TLINK:
                case OperandType::OPD_ULINK:
                case OperandType::OPD_PREDMASK:
                    iex->SetRegReadyTable(cmap.type, info.tag, false, lpvInfo, info.peid, this->m_tid);
                    releaseReg(uop, cmap, info);
                    break;
                case OperandType::OPD_VTLINK:
                case OperandType::OPD_VULINK:
                case OperandType::OPD_VMLINK:
                case OperandType::OPD_VNLINK:
                    iex->SetRegReadyTable(cmap.type, info.tag, false, lpvInfo);
                    break;
                default:
                    break;
            }
        }
    }
}

void SPEROB::CheckRelativeReg(PROBEntry &uop, RelateInfo &info)
{
    CheckReg(uop, tcmap, info);
    CheckReg(uop, ucmap, info);
    CheckReg(uop, predcmap, info);
}

void SPEROB::SetRegReadyTable(RelateCmap &cmap, RelateInfo &info, ExecEngineTyp iexTyp)
{
    PLpvInfo lpvInfo;
    std::shared_ptr<IEX> iex;
    if (GetSim()->core->IsVectorIex(iexTyp)) {
        iex = GetSim()->core->vectorTop->GetIex(speTop->coreId);
    } else {
        iex = GetSim()->core->iex[iexTyp];
    }

    switch (cmap.type) {
        case OperandType::OPD_TLINK:
        case OperandType::OPD_ULINK:
        case OperandType::OPD_VTLINK:
        case OperandType::OPD_VULINK:
        case OperandType::OPD_VMLINK:
        case OperandType::OPD_VNLINK:
        case OperandType::OPD_PREDMASK:
            iex->SetRegReadyTable(cmap.type, info.tag, false, lpvInfo, info.peid, this->m_tid);
            break;
        default:
            break;
    }
}

void SPEROB::ReleaseFunc(PROBEntry &uop, RelateCmap &cmap, RelateInfo &info)
{
    bool release = uop.last;
    cmap.write(info);

    speTop->d2Stage.RepLocalRetired(cmap.type, info.peid, info.seq, false, uop.inst->stid);
    release |= (cmap.size() - 1 >= LOGIC_UT_COUNT_4);
    if (release && !lock) {
        info = cmap.read();
        if (!(info.kill)) {
            SetRegReadyTable(cmap, info, uop.inst->iexType);
        }
        auto &sgprRenameUnit = speTop->d2Stage.sgprRenameUnit[uop.inst->peID][uop.inst->stid][SGPRType2Idx(cmap.type)];
        sgprRenameUnit.ReportRetired(info.seq.val, true);
    }
}

void SPEROB::ReportLocalRegKill(PROBEntry &uop, RelateCmap &cmap, RelateInfo &info)
{
    speTop->core->bctrl->blockROB.reportException(uop.bid, uop.inst->stid, "Register killed by scalarPE");
}

void SPEROB::SetRelateInfo(RelateInfo &info, uint32_t tag, ROBID &seq)
{
    info.vld = true;
    info.tag = tag;
    info.seq = seq;
}

void SPEROB::ReleaseRelative(PROBEntry &uop)
{
    PLpvInfo lpvInfo;
    SimInst &inst = uop.inst;
    RelateInfo info = RelateInfo();
    std::shared_ptr<IEX> iex;
    if (GetSim()->core->IsVectorIex(uop.inst->iexType)) {
        iex = GetSim()->core->vectorTop->GetIex(speTop->coreId);
    } else {
        iex = GetSim()->core->iex[uop.inst->iexType];
    }
    if (GetSim()->core->IsVectorIex(uop.inst->iexType) || (uop.inst->iexType == MEM_IEX)) {
        info.logic_long = true;
    }
    CheckRelativeReg(uop, info);

    info.bid = inst->bid;
    info.gid = inst->gid;
    info.peid = inst->peID;
    for (auto &pdst : inst->pdsts_) {
        switch (pdst->type) {
            case OperandType::OPD_TLINK:
                SetRelateInfo(info, pdst->ptag, inst->tSeq);
                ReleaseFunc(uop, tcmap, info);
                break;
            case OperandType::OPD_ULINK:
                SetRelateInfo(info, pdst->ptag, inst->uSeq);
                ReleaseFunc(uop, ucmap, info);
                break;
            case OperandType::OPD_VTLINK:
            case OperandType::OPD_VULINK:
            case OperandType::OPD_VMLINK:
            case OperandType::OPD_VNLINK:
            case OperandType::OPD_PREDMASK:
                speTop->core->bctrl->blockROB.reportException(uop.bid, uop.inst->stid,
                                                              "ScalarPE use vector dst operand");
                break;
            default:
                break;
        }
    }

    iex->rtable.setROBReadyTable(inst->peID, inst->rid.val, lpvInfo, false);
}

void SPEROB::HandleInstCntByType(const Opcode &opcode, ROBID instBid)
{
    speTop->peMInstStats.instCommitStats[instBid.val]->totalCommitInst++;
    InstGroup grp = OpcodeManager::Inst().GetOpcodeGroup(opcode);
    speTop->peMInstStats.instCommitStats[instBid.val]->instGroupCommitInstNum[grp]++;
}

void SPEROB::ReportInstCnt(const SimInst &inst)
{
    if (!inst) {
        return;
    }
    ROBID instBid = inst->bid;
    Opcode opcode = inst->opcode;

    if (inst->autogen) {  // automatically generated bend are not counted in the number of minsts
        return;
    }
    if (inst->isSysStateInst) {  // the system-state insts are not counted in the number of minsts
        return;
    }
    HandleInstCntByType(opcode, instBid);
}

void SPEROB::RecordTrace(const SimInst &inst) const
{
    if (!speTop->core->configs.dump_inst_trace || !inst) {
        return;
    }
    string peName = "scalar";
    auto info = make_shared<InstDumpInfo>(inst);
    info->peName = peName;
    speTop->core->pTracer->Push(inst->bid, info);
    speTop->core->pTracer->PushInstrEvent(inst->bid, inst, InstrEvent::COMMIT);
}

void SPEROB::ReportLocalRegStats(const SimInst &inst, const uint32_t bid, OPEState *opeState)
{
    if (!inst) {
        return;
    }

    auto calLocalSrcRegRobDist = [&bid, opeState](POperandPtr &src) {
        if (!src->IsLocalReg()) {
            return;
        }
        const uint64_t linkMaxSize = 8;
        ASSERT(src->value < linkMaxSize && "The index of local reg must be smaller than 8!");
        opeState->instRelativeRegStats[bid]->regRelativeDist[src->type][src->value]++;
        opeState->instRelativeRegStats[bid]->regRobDist[src->type][src->value] += src->localRegRobDist;
    };

    if (!inst->psrcs_.empty()) {
        for (auto &psrc : inst->psrcs_) {
            if (OperandTypeIsLocalReg(psrc->type)) {
                calLocalSrcRegRobDist(psrc);
            }
        }
    }

}

void SPEROB::MinstResVerify(PROBEntry &entry)
{
    InstVerifyInfo info;
    info.isReferenc = false;
    info.autoGen = entry.inst->autogen;
    info.terminate = entry.last;

    info.bid = entry.inst->bid.val;
    info.tid = entry.inst->stid;
    info.lgid = entry.inst->logicalGID;
    info.gid = entry.inst->gid.val;
    info.rid = entry.inst->rid.val;
    info.coreId = peID;
    info.tpc = entry.inst->pc;
    info.opcode = entry.inst->opcode;
    // 可能存在48bit 双输出指令，后续适配。
    info.data = entry.inst->GetResult();
    info.check = OpcodeManager::Inst().GetOpcodeGroup(entry.inst->opcode) != InstGroup::BLOCK_SPLIT;
    GetSim()->GetVerifyManager(entry.inst->stid)->RecordMinst(info);
}

void SPEROB::MinstPipeView(PROBEntry &entry)
{
    InstPipeViewPtr instInfo = std::make_shared<InstPipeViewInfo>();
    instInfo->bid = entry.bid.val;
    instInfo->tpc = entry.tpc;
    instInfo->logicGid = entry.inst->logicalGID;
    instInfo->cycleInfo = std::make_shared<CycleInfo>(*(entry.inst->pipeCycle));
    instInfo->label = MachineName(speTop->machineType) + " " + to_string(speTop->coreId) + entry.inst->DumpPipeViewInfo();
    GetSim()->GetViewManager(entry.inst->stid)->RecordMinst(instInfo);
}

void SPEROB::Stats()
{
    if (!current.size) {
        return;
    }
}

} // namespace JCore
