#include "core/Core.h"
#include "iex/pipe/iex_pipe.h"
#include "interface/BCCMemLdReq.h"
#include "vectorcore/VectorCore.h"

namespace JCore {


using namespace std;

namespace {

bool IsScalarLocalLink(OperandType type)
{
    return type == OperandType::OPD_TLINK || type == OperandType::OPD_ULINK;
}

void WakeupScalarLocalLinks(IEX *top, const SimInst &inst)
{
    PLpvInfo lpvInfo = inst->GetLpv();
    for (auto pdst : inst->pdsts_) {
        if (!IsScalarLocalLink(pdst->type)) {
            continue;
        }
        top->iq.WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled),
            lpvInfo, inst->peID, inst->tid, inst->stid);
    }
}

} // namespace

void LDAPipe::Build(uint32_t id) {
    pipeid = id;

    i1_inst = std::make_shared<SimInstInfo>();
    i2_inst = std::make_shared<SimInstInfo>();
    e1_inst = std::make_shared<SimInstInfo>();
    e2_inst = std::make_shared<SimInstInfo>();
    e3_inst = std::make_shared<SimInstInfo>();
    e4_inst = std::make_shared<SimInstInfo>();
    w1_inst = std::make_shared<SimInstInfo>();
    w2_inst = std::make_shared<SimInstInfo>();
}

void LDAPipe::Reset() {
    i1_inst = std::shared_ptr<SimInstInfo>(nullptr);
    i2_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e1_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e2_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e3_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e4_inst = std::shared_ptr<SimInstInfo>(nullptr);
    w1_inst = std::shared_ptr<SimInstInfo>(nullptr);
    w2_inst = std::shared_ptr<SimInstInfo>(nullptr);
    if (pipeid < top->iexLdaIqCount * top->iexLdaPickCount) {
        for (uint32_t stid = 0; stid < lda_req_q.size(); ++stid) {
            lda_req_q[stid]->unsetStall();
        }
    }
}

void LDAPipe::Work() {
    runW2();
    runW1();
    runE4();
    runE3();
    runE2();
    runE1();
    runI2();
    if (e1_inst && checkLSUStall(e1_inst->stid)) {
        LogPrint();
        return;
    }
    runI1();
    runP1();
    LogPrint();
}

void LDAPipe::moveLpv() {
    auto pipeMoveLpv = [](SimInst &inst) {
        if (inst) {
            inst->MoveLpv();
        }
    };

    pipeMoveLpv(w2_inst);
    pipeMoveLpv(w1_inst);
    pipeMoveLpv(e1_inst);
    pipeMoveLpv(i2_inst);
    pipeMoveLpv(i1_inst);
}

void LDAPipe::move() {
    w2_inst = w1_inst;
    w1_inst = e4_inst;
    e4_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e3_inst = e2_inst;

    if ((e1_inst && checkLSUStall(e1_inst->stid)) || memStall) {
        return;
    }
    if (e1_inst && (!e1_inst->sentFromSc && GetSim()->core->configs.scalper_enable)) {
        e2_inst = nullptr;
    } else {
        e2_inst = e1_inst;
    }
    e1_inst = i2_inst;
    i2_inst = i1_inst;
    i1_inst = *p1_inst;
    *p1_inst = nullptr;
}

void LDAPipe::flush(FlushBus &flushReq) {
    auto flush_stage = [&flushReq] (SimInst &inst) {
        if (!inst) {
            return;
        }
        if (inst->stid != flushReq.req.stid) {
            return;
        }
        if (flushReq.baseOnPE && flushReq.req.peID != inst->peID) {
            return;
        }
        bool lessEq = flushReq.baseOnBid ? LessEqual(flushReq.req.bid, inst->bid):
            LessEqual(flushReq.req.bid, flushReq.req.rid, inst->bid, inst->rid);
        if (lessEq) {
            inst = std::shared_ptr<SimInstInfo>(nullptr);
        }
    };
    flush_stage(i1_inst);
    flush_stage(i2_inst);
    flush_stage(e1_inst);
    flush_stage(e2_inst);
    flush_stage(e3_inst);
    flush_stage(e4_inst);
    flush_stage(w1_inst);
    flush_stage(w2_inst);
}

void LDAPipe::runP1() {
    if ((p1_inst == nullptr) || (*p1_inst == nullptr)) {
        return;
    }
    if (e1_inst && checkLSUStall(e1_inst->stid)) {
        return;
    }
    (*p1_inst)->pipeCycle->p1Cycle = sim->getCycles();
}

void LDAPipe::runI1() {
    // TODO: Requests will be sent repeatedly when stalling
    rf_rd_req.Reset();
    if (!i1_inst) {
        return;
    }
    i1_inst->pipeCycle->i1Cycle = sim->getCycles();

    rf_rd_req = i1_inst->GenRFReqBus(true);
    rf_rd_req.pipeid = pipeid;

    sys_rd_req = i1_inst->GenSYSReqBus();
}

void LDAPipe::runI2() {
    if (!i2_inst) {
        return;
    }
    i2_inst->pipeCycle->i2Cycle = sim->getCycles();

    i2_inst->RFRetSetData(rf_data_ret);

    if (!sys_data_ret.vld) {
        return;
    }
    i2_inst->RecSYSReqRet(sys_data_ret);
}

void LDAPipe::runE1() {
    if (GetSim()->core->configs.scalper_enable) {
        if ((top->id == MEM_IEX) && (pipeid >= top->iexLdaIqCount * top->iexLdaPickCount)) {
            if (e1_inst && !(IsScalarInst(e1_inst->codeLen))) {
                SimInst inst = std::make_shared<SimInstInfo>(*e1_inst);
                ASSERT(!e1_inst->fromSC);
                top->iexScalperInstQ->push_back(inst);
            }
            return;
        }
    }
    rf_ld_wr_req.Reset();
    if (!e1_inst) {
        for (auto &q : lda_req_q) {
            q->unsetStall();
        }
        return;
    }

    if (!checkLSUStall(e1_inst->stid)) {
        e1_inst->pipeCycle->e1Cycle = sim->getCycles();
    }

    // if (sim->perfectGetSet) {
    //     if (e1_inst->ref.gsInfo.src0_vld) {
    //         e1_inst->src0.data = e1_inst->ref.gsInfo.src0_data;
    //     }
    //     if (e1_inst->ref.gsInfo.src1_vld) {
    //         e1_inst->src1.data = e1_inst->ref.gsInfo.src1_data;
    //     }
    //     if (e1_inst->ref.gsInfo.src2_vld) {
    //         e1_inst->src2.data = e1_inst->ref.gsInfo.src2_data;
    //     }
    //     if (e1_inst->ref.gsInfo.src3_vld) {
    //         e1_inst->src3.data = e1_inst->ref.gsInfo.src3_data;
    //     }
    // }
    // simt or scalor
    uint64_t addr = -1U;
    std::unordered_map<uint64_t, std::set<uint32_t>> addrLaneMap;
    e1_inst->realReqCnt = 0;
    uint64_t curMask = e1_inst->predMask;
    if (IsScalarInst(e1_inst->codeLen)) {
        bool ret = e1_inst->Execute();
        (!ret) ? top->core->bctrl->blockROB.reportException(e1_inst->bid, e1_inst->stid,
                                                            "LDA execute exception(SCALAR)") : void();
        addr = e1_inst->accMemInfo->accMemAddr;
        addrLaneMap[addr].insert(0);
        e1_inst->realReqCnt = 1;
        if (IsLoadStorePair(e1_inst->opcode)) {
            addrLaneMap[addr + GetLoadStoreBytes(e1_inst->opcode)].insert(1);
            e1_inst->realReqCnt = 2;
        }
    } else {
        bool toMem = e1_inst->accMemInfo != nullptr ? !e1_inst->accMemInfo->local : true;
        ASSERT(!toMem);
        for (uint32_t lane = 0; lane < e1_inst->lanes; ++lane) {
            POperandPtr maskSrcPtr = e1_inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK);
            if (maskSrcPtr != nullptr) curMask = maskSrcPtr->data;
            if (curMask & (1ULL << lane)) {
                ++e1_inst->realReqCnt;
                bool ret = e1_inst->Execute(lane);
                (!ret) ? top->core->bctrl->blockROB.reportException(e1_inst->bid, e1_inst->stid,
                                                                    "LDA execute exception(SIMT)") : void();
                addr = e1_inst->accMemInfo->vecData.Get(lane, static_cast<uint32_t>(OperandWidth::OPDW_D));
                addrLaneMap[addr].insert(lane);
            }
        }
        ASSERT(e1_inst->realReqCnt > 0);
        MemRequest memReq;
        memReq.tpc = e1_inst->pc;
        memReq.peID = e1_inst->peID;
        memReq.opcode = e1_inst->opcode;
        memReq.laneSize = GetLoadStoreBytes(memReq.opcode);
        memReq.realReqCnt = e1_inst->realReqCnt;
        memReq.addrs = e1_inst->accMemInfo->vecData;
        memReq.data.width = e1_inst->pdsts_[DST0_IDX]->vecData.width;
        memReq.data.size = e1_inst->pdsts_[DST0_IDX]->vecData.size;
        memReq.data.data.resize(memReq.data.size);
        memReq.mask = e1_inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK)->data;
        memReq.lanes = e1_inst->lanes;
        memReq.thread = e1_inst->peID;
        ASSERT(GetSim()->core->IsVecPe(e1_inst->peID) || memReq.thread == GetSim()->core->GetMtcPEIndex());
        memReq.bid = e1_inst->bid;
        memReq.rid = e1_inst->rid;
        memReq.subrid = e1_inst->subrid;
        memReq.gid = e1_inst->gid;
        memReq.tid = e1_inst->tid;
        memReq.stid = e1_inst->stid;
        memReq.lsID = e1_inst->lsID;
        memReq.width = GetLoadStoreBytes(e1_inst->opcode);
        memReq.isLoad = true;
        memReq.start = 0;
        memReq.toMemory = toMem;
        memReq.uinst = e1_inst;
        // 微指令不应该感知block 在哪个BIQ。
        memReq.isMTCMemReq = (e1_inst->biqType == BIQType::MTC_IQ) ? true : false;
        if (!toMem || (memReq.isMTCMemReq)) {
            top->iexVcoreReqQ->Write(memReq);
            top->stats->tile_load_inst_num++;
            if (e1_inst->isGatherLd) {
                top->stats->tile_gather_load_inst_num++;
            } else {
                top->stats->tile_continious_load_inst_num++;
            }
            LOG_DEBUG_M(Unit::VECTOR, Stage::E1) << "send to tile, check_ld=" << e1_inst->vecTileLoad << " " << e1_inst;
            return;
        } else {
            top->stats->memory_load_inst_num++;
            if (e1_inst->isGatherLd) {
                top->stats->memory_gather_load_inst_num++;
            } else {
                top->stats->memory_continious_load_inst_num++;
            }
            LOG_DEBUG_M(Unit::VECTOR, Stage::E1) << "send to memory, " << e1_inst;
        }

        curMask = e1_inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK)->data;
    }
    // TODO: change to sim queue
    // TODO: Check for multi-pe.
    const uint32_t checkOneLane = 1;
    IDBus cmtBus = top->core->peArray[e1_inst->peID]->GetRetireID();
    uint32_t stallLanes = top->IsLastLoadStore(e1_inst, cmtBus) ? checkOneLane : e1_inst->lanes;
    uint32_t width = top->iexLdaIqCount * stallLanes;
    // uint32_t width = top->iexLdaIqCount * e1_inst->lanes;
    for (uint32_t lane = 0; lane < e1_inst->lanes; lane++) {
        if ((top->core->IsVectorIex(e1_inst->iexType) || e1_inst->iexType == MEM_IEX) &&
            (curMask & (1ULL << lane)) == 0) {
            continue;
        }
        uint64_t addr = (top->core->IsVectorIex(e1_inst->iexType) || e1_inst->iexType == MEM_IEX)
                        ? e1_inst->accMemInfo->vecData.Get(lane, static_cast<uint32_t>(OperandWidth::OPDW_D))
                        : e1_inst->accMemInfo->accMemAddr;
        if (top->GetLsucheckLoadCltStall(addr, width, e1_inst->stid)) {
            lda_req_q[e1_inst->stid]->setStall();
            LOG_INFO_M(Unit::VECTOR, Stage::E1) << "LDA Pipe Line%u:" << pipeid << ", LSU Stall";
            return;
        }
    }
    if (e1_inst->opcode == Opcode::OP_TLD) {
        width = top->iexLdaIqCount;
        if (top->GetLsucheckLoadCltStall(e1_inst->pdsts_[DST0_IDX]->data, 1, e1_inst->stid)) {
            lda_req_q[e1_inst->stid]->setStall();
            return;
        }
    }

    ASSERT(e1_inst->realReqCnt != 0);
    lda_req_q[e1_inst->stid]->unsetStall();
    uint32_t peCnt = GetSim()->core->configs.stdPeCount + GetSim()->core->configs.simtPeCount;
    for (auto it : addrLaneMap) {
        MemReqBus req = e1_inst->GenMemReq(peCnt, *it.second.begin());
        e1_inst->pipeCycle->genLoadReadReqCycle = top->GetSim()->cycles;
        req.genLoadReadReqCycle = top->GetSim()->cycles;
        req.prefetchDataRetCycle = e1_inst->pipeCycle->prefetchDataRetCycle;
        req.sendToScalperCycle = e1_inst->pipeCycle->sendToScalperCycle;
        req.genPrefetchCycle = e1_inst->pipeCycle->genPrefetchCycle;
        req.sendFromScalperCycle = e1_inst->pipeCycle->sendFromScalperCycle;
        req.sendMemoryReqCycle = e1_inst->pipeCycle->sendMemoryReqCycle;
        req.realReqCnt = e1_inst->realReqCnt;
        if (req.vld) {
            req.simtLane = *it.second.begin();
            req.laneSet = it.second;
            if (IsScalarInst(e1_inst->codeLen) && IsLoadStorePair(e1_inst->opcode)) {
                req.addr = it.first;
                req.tag = CalTag(req.addr, req.toMtcLsu);
                req.isCrossCacheLine = AddrCrossCacheline(req.addr, req.size, req.toMtcLsu);
            }
            // TODO: simt iex is not support the speculation wakeup feature
            if (top->GetLsuload_to_use_enable() && !req.stack_vld &&
                ((!top->core->IsVectorIex(top->machineType)) && (top->id != MEM_IEX))) {
                req.pipeID = pipeid;
                req.specWakeup = true;
                top->setMemWakeup(req);
            }
            LOG_INFO_M(Unit::MIEX, Stage::E1) << " [IEX E1]: Send req to lsu: " << req
                                              << " cycels " << dec << top->GetSim()->cycles;
            lda_req_q[req.stid]->Write(req);
        }
    }
}

void LDAPipe::runE2() {
    if (!e2_inst) {
        return;
    }
}

void LDAPipe::runE3() {
    if (!e3_inst) {
        return;
    }
}

void LDAPipe::runE4() {
    if (!e4_inst) {
        return;
    }
}

void LDAPipe::runW1() {
    if (!w1_inst) {
        return;
    }
    w1_inst->pipeCycle->w1Cycle = sim->getCycles();
}

void LDAPipe::runW2() {
    rf_wr_req.Reset();
    if (!w2_inst) {
        return;
    }
    w2_inst->pipeCycle->w2Cycle = sim->getCycles();

    // Generate readfile write bus
    if ((w2_inst->stack_type != StackInstType::STACK_LOAD) && (w2_inst->biqType != BIQType::TMA_IQ)) {
        rf_wr_req = w2_inst->GenRFReqBus(false);
    }
    // ready table wtire
    // if (w2_inst->checkAlldstRanged() && top->core->configs.reno_dynamic_enable &&
    //     w2_inst->iexType == SCALAR_IEX) {
    //     RFReqBus req = w2_inst->genWRTableBus();
    //     if (req.vld) {
    //         iex_rt_wr_q->Write(req);
    //     }
    // }
    // Generate PE resolved bus
    PEResolveBus rslv = w2_inst->GenRslvBus();
    uint32_t tid = 0;
    if (top->GetSim()->core->IsVectorIex(top->machineType)) {
        tid = w2_inst->tid;
    } else {
        tid = w2_inst->stid;
    }
    rslv_array[w2_inst->peID][tid]->Write(rslv);
    WakeupScalarLocalLinks(top, w2_inst);
}

void LDAPipe::LogPrint()
{
    bool logEnable = (p1_inst && (*p1_inst != nullptr)) || i1_inst || i2_inst || e1_inst || e2_inst || e3_inst ||
        e4_inst || w1_inst || w2_inst;
    if (!logEnable) {
        return;
    }
    if (p1_inst && (*p1_inst != nullptr)) {
        LOG_DEBUG_M(top->machineType, Stage::P1) << "[LDA Line" << dec << pipeid << "] " << (*p1_inst)->Dump();
    }
    if (i1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::I1) << "[LDA Line" << dec << pipeid << "] " << i1_inst->Dump();
    }
    if (i2_inst) {
        LOG_DEBUG_M(top->machineType, Stage::I2) << "[LDA Line" << dec << pipeid << "] " << i2_inst->Dump();
    }
    if (e1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::E1) << "[LDA Line" << dec << pipeid << "] " << e1_inst->Dump();
    }
    if (e2_inst) {
        LOG_DEBUG_M(top->machineType, Stage::E2) << "[LDA Line" << dec << pipeid << "] " << e2_inst->Dump();
    }
    if (e3_inst) {
        LOG_DEBUG_M(top->machineType, Stage::E3) << "[LDA Line" << dec << pipeid << "] " << e3_inst->Dump();
    }
    if (e4_inst) {
        LOG_DEBUG_M(top->machineType, Stage::E4) << "[LDA Line" << dec << pipeid << "] " << e4_inst->Dump();
    }
    if (w1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::WB) << "[LDA Line" << dec << pipeid << "] " << w1_inst->Dump();
    }
    if (w2_inst) {
        LOG_DEBUG_M(top->machineType, Stage::W2) << "[LDA Line" << dec << pipeid << "] " << w2_inst->Dump();
    }
}

bool LDAPipe::checkLSUStall(uint32_t stid)
{
    return lda_req_q[stid]->getStall();
}

} // namespace JCore
