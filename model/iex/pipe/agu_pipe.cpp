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

void AGUPipe::Build(uint32_t id)
{
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

void AGUPipe::Reset()
{
    i1_inst = std::shared_ptr<SimInstInfo>(nullptr);
    i2_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e1_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e2_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e3_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e4_inst = std::shared_ptr<SimInstInfo>(nullptr);
    w1_inst = std::shared_ptr<SimInstInfo>(nullptr);
    w2_inst = std::shared_ptr<SimInstInfo>(nullptr);
    for (uint32_t stid = 0; stid < lda_req_q.size(); ++stid) {
        lda_req_q[stid]->unsetStall();
    }
}

void AGUPipe::Work()
{
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

void AGUPipe::moveLpv()
{
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

void AGUPipe::move()
{
    w2_inst = w1_inst;
    w1_inst = e4_inst;
    e4_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e3_inst = e2_inst;

    if ((e1_inst && checkLSUStall(e1_inst->stid)) || memStall) {
        return;
    }

    e2_inst = e1_inst;
    e1_inst = i2_inst;
    i2_inst = i1_inst;
    i1_inst = *p1_inst;
    *p1_inst = nullptr;
}

void AGUPipe::flush(FlushBus &flushReq)
{
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

void AGUPipe::runP1()
{
    if ((p1_inst == nullptr) || (*p1_inst == nullptr)) {
        return;
    }
    if (e1_inst && checkLSUStall(e1_inst->stid)) {
        return;
    }
    (*p1_inst)->pipeCycle->p1Cycle = sim->getCycles();
}

void AGUPipe::runI1()
{
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

void AGUPipe::runI2()
{
    if (!i2_inst) {
        return;
    }
    i2_inst->pipeCycle->i2Cycle = sim->getCycles();

    i2_inst->RFRetSetData(rf_data_ret);
    if (sys_data_ret.vld) {
        i2_inst->RecSYSReqRet(sys_data_ret);
    }
}

void AGUPipe::runE1()
{
    rf_ld_wr_req.Reset();
    if (!e1_inst) {
        for (auto &q : lda_req_q) {
            q->unsetStall();
        }
        return;
    }

    if (OpcodeIsLoad(e1_inst->opcode)) {
        runE1Load(e1_inst);
    } else if (OpcodeIsStore(e1_inst->opcode)) {
        runE1Store(e1_inst);
    } else {
        ASSERT(false);
    }
}

void AGUPipe::runE1Load(const SimInst &inst)
{
    ASSERT(inst != nullptr);
    rf_ld_wr_req.Reset();

    if (!checkLSUStall(inst->stid)) {
        inst->pipeCycle->e1Cycle = sim->getCycles();
    }

    // simt or scalor
    uint64_t addr = -1U;
    std::unordered_map<uint64_t, std::set<uint32_t>> addrLaneMap;
    auto predSrc = inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK);
    uint64_t curMask = predSrc == nullptr ? inst->predMask : predSrc->data;
    inst->realReqCnt = 0;
    bool toMem = inst->accMemInfo != nullptr ? !inst->accMemInfo->local : true;
    if (IsScalarInst(inst->codeLen)) {
        bool ret = inst->Execute();
        (!ret) ? top->core->bctrl->blockROB.reportException(inst->bid, inst->stid,
                                                            "LDA execute exception(SCALAR)") : void();
        addr = inst->pdsts_[DST0_IDX]->data;
        addrLaneMap[addr].insert(0);
        ++inst->realReqCnt;
        if (!toMem) {
            MemRequest memReq = CreateMemReq(inst, true, curMask, toMem);
            memReq.addrs.Init(static_cast<size_t>(OperandWidth::OPDW_D), 1);
            memReq.addrs.Set(inst->accMemInfo->accMemAddr, 0, static_cast<size_t>(OperandWidth::OPDW_D));
            memReq.data.width = static_cast<size_t>(inst->pdsts_[DST0_IDX]->width);
            memReq.data.size = 1;
            memReq.data.data.resize(memReq.data.size);

            top->iexVcoreReqQ->Write(memReq);
            top->stats->tile_load_inst_num++;
            top->stats->tile_gather_load_inst_num += inst->isGatherLd ? 1 : 0;
            top->stats->tile_continious_load_inst_num += inst->isGatherLd ? 1 : 0;
            return;
        }
    } else {
        VecData addrs = inst->accMemInfo->vecData;
        if (inst->dwDstType && (inst->pdsts_.size() > 0) && inst->pdsts_[0]->vecDataVld) {
            inst->pdsts_[0]->vecData.Init(static_cast<uint64_t>(OperandWidth::OPDW_D),
                inst->pdsts_[0]->vecData.m_lane);
        }
        for (uint32_t lane = 0; lane < inst->lanes; ++lane) {
            if ((curMask & (1ULL << lane)) == 0ULL) {
                continue;
            }
            ++inst->realReqCnt;
            bool ret = inst->Execute(lane);
            if (!ret) {
                std::string excpInfo = "LDA execute exception(SIMT)" + inst->Dump();
                top->core->bctrl->blockROB.reportException(inst->bid, inst->stid, excpInfo);
            }
            addr = inst->accMemInfo->accMemAddr;
            addrs.Set(addr, lane, static_cast<uint32_t>(JCore::OperandWidth::OPDW_D));
            addrLaneMap[addr].insert(lane);
        }
        if (toMem) {
            std::string excpInfo = "Vector load should not send to gm lsu" + inst->Dump();
            top->core->bctrl->blockROB.reportException(inst->bid, inst->stid, excpInfo);
            return;
        }

        if (top->configs.VAB_EN) {
            // gather/scatter
            HandleVabLd(inst, toMem, addrs);
        } else {
            ASSERT(inst->realReqCnt > 0);
            MemRequest memReq = CreateMemReq(inst, true, curMask, toMem);
            memReq.addrs = addrs;
            memReq.data.width = inst->pdsts_[DST0_IDX]->vecData.width;
            memReq.data.size = inst->pdsts_[DST0_IDX]->vecData.size;
            memReq.data.data.resize(memReq.data.size);

            if (!toMem) {
                top->iexVcoreReqQ->Write(memReq);
                top->stats->tile_load_inst_num++;
                top->stats->tile_gather_load_inst_num += inst->isGatherLd ? 1 : 0;
                top->stats->tile_continious_load_inst_num += inst->isGatherLd ? 1 : 0;
                return;
            } else {
                top->stats->memory_load_inst_num++;
                top->stats->memory_gather_load_inst_num += inst->isGatherLd ? 1 : 0;
                top->stats->memory_continious_load_inst_num += inst->isGatherLd ? 1 : 0;
            }
        }
    }
    // TODO: change to sim queue
    // TODO: Check for multi-pe.
    const uint32_t checkOneLane = 1;
    IDBus cmtBus = top->core->peArray[inst->peID]->GetRetireID();
    uint32_t stallLanes = top->IsLastLoadStore(inst, cmtBus) ? checkOneLane : inst->lanes;
    uint32_t width = top->iexAguIqCount * stallLanes;
    curMask = inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK)->data;
    for (uint32_t lane = 0; lane < inst->lanes; lane++) {
        if ((top->core->IsVectorIex(inst->iexType) || inst->iexType == MEM_IEX) &&
            (curMask & (1ULL << lane)) == 0) {
            continue;
        }

        uint64_t addr = (top->core->IsVectorIex(inst->iexType) || inst->iexType == MEM_IEX)
                        ? inst->pdsts_[DST0_IDX]->vecData.Get(lane, static_cast<uint32_t>(OperandWidth::OPDW_D))
                        : inst->pdsts_[DST0_IDX]->data;
        if (top->GetLsucheckLoadCltStall(addr, width, inst->stid)) {
            lda_req_q[inst->stid]->setStall();
            return;
        }
    }

    ASSERT(inst->realReqCnt != 0);
    lda_req_q[inst->stid]->unsetStall();
    uint32_t peCnt = GetSim()->core->configs.stdPeCount + GetSim()->core->configs.simtPeCount;
    for (auto it : addrLaneMap) {
        MemReqBus req = inst->GenMemReq(peCnt, *it.second.begin());
        req.realReqCnt = inst->realReqCnt;
        if (req.vld) {
            req.simtLane = *it.second.begin();
            req.laneSet = it.second;
            // TODO: simt iex is not support the speculation wakeup feature
            if (top->GetLsuload_to_use_enable() && !req.stack_vld &&
                ((!top->core->IsVectorIex(top->machineType)) && (top->id != MEM_IEX))) {
                req.pipeID = pipeid;
                req.specWakeup = true;
                top->setMemWakeup(req);
            }
            lda_req_q[inst->stid]->Write(req);
        }
    }
}

void AGUPipe::HandleVabLd(const SimInst &inst, bool toMem, const VecData &addrs)
{
    if (toMem) {
        top->stats->memory_load_inst_num++;
        ASSERT(inst->realReqCnt > 0);
        uint64_t mask = inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK)->data;
        MemRequest memReq = CreateMemReq(inst, true, mask, toMem);
        memReq.addrs = addrs;
        memReq.data.width = inst->pdsts_[DST0_IDX]->vecData.width;
        memReq.data.size = inst->pdsts_[DST0_IDX]->vecData.size;
        memReq.data.data.resize(memReq.data.size);
    } else {
        if (inst->gatherLd) {
            if (!top->iq.vab->Stall) {
                inst->gatherInfo.gather_stall_cycle_begin = !inst->gatherInfo.gather_stall ? GetSim()->getCycles() \
                    : inst->gatherInfo.gather_stall_cycle_begin;
                inst->gatherInfo.gather_stall_cycle_end = GetSim()->getCycles();
                top->stats->tile_gather_stall_cycle += \
                    inst->gatherInfo.gather_stall_cycle_end - inst->gatherInfo.gather_stall_cycle_begin;
                top->stats->tile_gather_load_inst_num++;
                top->iq.vab->pushVab(inst);
                HandleGatherSplit(inst, addrs);
                top->iq.vab->Stall = true;
            } else {
                inst->gatherInfo.gather_stall_cycle_begin = !inst->gatherInfo.gather_stall ? GetSim()->getCycles() \
                    : inst->gatherInfo.gather_stall_cycle_begin;
                inst->gatherInfo.gather_stall = !inst->gatherInfo.gather_stall ? true : false;
                CancelGather(inst);
            }
            return;
        } else {
            top->stats->memory_continious_load_inst_num++;
            ASSERT(inst->realReqCnt > 0);
            uint64_t mask = inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK)->data;
            MemRequest memReq = CreateMemReq(inst, true, mask, toMem);
            memReq.addrs = addrs;
            memReq.data.width = inst->pdsts_[DST0_IDX]->vecData.width;
            memReq.data.size = inst->pdsts_[DST0_IDX]->vecData.size;
            memReq.data.data.resize(memReq.data.size);

            top->iexVcoreReqQ->Write(memReq);
        }
    }
}

void AGUPipe::CancelGather(const SimInst &inst) const
{
    top->iq.setCancel(inst, pipeid);
}

void AGUPipe::HandleGatherSplit(const SimInst &inst, const VecData &addrs) const
{
    for (uint32_t lane = 0; lane < inst->lanes; ++lane) {
        uint64_t addr = addrs.Get(lane, static_cast<uint32_t>(OperandWidth::OPDW_D));
        top->iq.vab->split(addr);
    }
}

void AGUPipe::runE1Store(const SimInst &inst)
{
    ASSERT(inst != nullptr);
    auto simArrayStall = [this]() -> bool {
        bool stall = true;
        for (uint32_t i = 0; i < top->tTransTileRegStReqArray.size(); ++i) {
            if (!top->tTransTileRegStReqArray[i]->Full()) {
                stall = false;
                break;
            }
        }
        return stall;
    };
    if (inst->memInst && simArrayStall()) {
        memStall = true;
        return;
    }
    if (!inst->storeSplit) {
        inst->pipeCycle->e1Cycle = sim->getCycles();
    }

    // simt or scalor
    bool toMem = inst->accMemInfo != nullptr ? !inst->accMemInfo->local : true;
    if (IsScalarInst(inst->codeLen)) {
        inst->Execute();
        if (!toMem) {
            LocalStore(inst);
            return;
        }
        uint32_t peCnt = GetSim()->core->configs.stdPeCount + GetSim()->core->configs.simtPeCount;
        MemReqBus req = inst->GenMemReq(peCnt);
        if (req.vld) {
            req.realReqCnt = 1;
            req.laneSet.insert(0);
            sta_req_q[inst->stid]->Write(req);
        }
    } else {
        VecData addrs = inst->accMemInfo->vecData;
        uint64_t addr = -1U;
        POperandPtr maskSrcPtr = inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK);
        uint64_t curMask = (maskSrcPtr != nullptr) ? maskSrcPtr->data : inst->predMask;

        std::unordered_map<uint64_t, std::set<uint32_t>> addrLaneMap;
        if (inst->dwDstType && (inst->pdsts_.size() > 0) && inst->pdsts_[0]->vecDataVld) {
            inst->pdsts_[0]->vecData.Init(static_cast<uint64_t>(OperandWidth::OPDW_D),
                inst->pdsts_[0]->vecData.m_lane);
        }
        for (uint32_t lane = 0; lane < inst->lanes; ++lane) {
            if ((curMask & (1ULL << lane)) == 0ULL) {
                continue;
            }
            bool ret = inst->Execute(lane, inst->pc);
            if (!ret) {
                std::string excpInfo = "STA execute exception(SIMT)" + inst->Dump();
                top->core->bctrl->blockROB.reportException(inst->bid, inst->stid, excpInfo);
            }
            ++inst->realReqCnt;
            ASSERT(inst->accMemInfo != nullptr);
            addr = inst->accMemInfo->accMemAddr;
            addrs.Set(addr, lane, static_cast<uint32_t>(OperandWidth::OPDW_D));
            addrLaneMap[addr].insert(lane);
        }
        /*
        if (toMem) {
            std::string excpInfo = "Vector store should not send to gm lsu" + inst->Dump();
            top->core->bctrl->blockROB.reportException(inst->bid, excpInfo.c_str());
            return;
        }
        */
        ASSERT(inst->realReqCnt > 0);
        MemRequest memReq = CreateMemReq(inst, false, curMask, toMem);
        memReq.addrs = addrs;
        if (OperandTypeNeedVecData(inst->psrcs_[SRC0_IDX]->type)) {
            memReq.data = inst->psrcs_[SRC0_IDX]->vecData;
        } else {
            auto &psrc = inst->psrcs_[SRC0_IDX];
            memReq.data.Init(static_cast<uint32_t>(psrc->width), inst->lanes);
            for (uint64_t index = 0; index < inst->lanes; index++) {
                memReq.data.Set(psrc->data, index);
            }
        }

        if (!toMem) {
            top->iexVcoreReqQ->Write(memReq);
            top->stats->tile_store_inst_num++;
            if (inst->isScatterSt) {
                top->stats->tile_scatter_store_inst_num++;
            } else {
                top->stats->tile_continious_store_inst_num++;
            }
            return;
        } else {
            top->iexVcoreReqQ->Write(memReq);
            top->stats->memory_store_inst_num++;
            if (inst->isScatterSt) {
                top->stats->memory_scatter_store_inst_num++;
            } else {
                top->stats->memory_continious_store_inst_num++;
            }
            return;
        }

        uint32_t peCnt = GetSim()->core->configs.stdPeCount + GetSim()->core->configs.simtPeCount;
        for (auto it : addrLaneMap) {
            MemReqBus req = inst->GenMemReq(peCnt, *it.second.begin());
            req.realReqCnt = inst->realReqCnt;
            if (req.vld) {
                req.simtLane = *it.second.begin();
                req.laneSet = it.second;
                sta_req_q[inst->stid]->Write(req);
            }
        }
    }
}

void AGUPipe::LocalStore(const SimInst &inst)
{
    MemRequest memReq = CreateMemReq(inst, false, 1U, true);
    memReq.realReqCnt = 1U;
    memReq.addrs.Init(static_cast<size_t>(OperandWidth::OPDW_D), 1);
    memReq.addrs.Set(inst->accMemInfo->accMemAddr, 0, static_cast<size_t>(OperandWidth::OPDW_D));
    if (OperandTypeNeedVecData(inst->psrcs_[SRC0_IDX]->type)) {
        memReq.data = inst->psrcs_[SRC0_IDX]->vecData;
    } else {
        auto &psrc = inst->psrcs_[SRC0_IDX];
        memReq.data.Init(static_cast<uint32_t>(psrc->width), inst->lanes);
        for (uint64_t index = 0; index < inst->lanes; index++) {
            memReq.data.Set(psrc->data, index);
        }
    }

    top->iexVcoreReqQ->Write(memReq);
    top->stats->tile_store_inst_num++;
    if (inst->isScatterSt) {
        top->stats->tile_scatter_store_inst_num++;
    } else {
        top->stats->tile_continious_store_inst_num++;
    }
}

void AGUPipe::runE2()
{
    if (!e2_inst) {
        return;
    }
    if (OpcodeIsStore(e2_inst->opcode)) {
        if (!e2_inst->storeSplit) {
            e2_inst->pipeCycle->w1Cycle = sim->getCycles();
        }
    }
}

void AGUPipe::runE3()
{
    if (!e3_inst) {
        return;
    }
    if (OpcodeIsStore(e3_inst->opcode)) {
        if (!e3_inst->storeSplit) {
            e3_inst->pipeCycle->w2Cycle = sim->getCycles();
        }

        // Generate readfile write bus
        // Generate PE resolved bus
        PEResolveBus rslv = e3_inst->GenRslvBus();
        rslv.isPipeStore = true;
        uint32_t tid = 0;
        if (top->GetSim()->core->IsVectorIex(top->machineType)) {
            tid = e3_inst->GetTid();
        } else {
            tid = e3_inst->stid;
        }
        rslv_array[e3_inst->peID][tid]->Write(rslv);
    }
}

void AGUPipe::runE4()
{
    if (!e4_inst) {
        return;
    }
}

void AGUPipe::runW1()
{
    if (!w1_inst) {
        return;
    }
    w1_inst->pipeCycle->w1Cycle = sim->getCycles();
}

void AGUPipe::runW2()
{
    rf_wr_req.Reset();
    if (!w2_inst) {
        return;
    }
    w2_inst->pipeCycle->w2Cycle = sim->getCycles();

    // Generate readfile write bus
    if (w2_inst->stack_type != StackInstType::STACK_LOAD) {
        rf_wr_req = w2_inst->GenRFReqBus(false);
    }

    // Generate PE resolved bus
    PEResolveBus rslv = w2_inst->GenRslvBus();
    uint32_t tid = 0;
    if (top->GetSim()->core->IsVectorIex(top->machineType)) {
        tid = w2_inst->GetTid();
    } else {
        tid = w2_inst->stid;
    }
    rslv_array[w2_inst->peID][tid]->Write(rslv);
    WakeupScalarLocalLinks(top, w2_inst);
}

void AGUPipe::LogPrint()
{
    bool logEnable = (p1_inst && (*p1_inst != nullptr)) || i1_inst || i2_inst || e1_inst || e2_inst || e3_inst ||
        e4_inst || w1_inst || w2_inst;
    if (!logEnable) {
        return;
    }
    if (p1_inst && (*p1_inst != nullptr)) {
        LOG_DEBUG_M(top->machineType, Stage::P1) << "[AGU Line" << dec << pipeid << "] " << (*p1_inst)->Dump();
    }
    if (i1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::I1) << "[AGU Line" << dec << pipeid << "] " << i1_inst->Dump();
    }
    if (i2_inst) {
        LOG_DEBUG_M(top->machineType, Stage::I2) << "[AGU Line" << dec << pipeid << "] " << i2_inst->Dump();
    }
    if (e1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::E1) << "[AGU Line" << dec << pipeid << "] " << e1_inst->Dump();
    }
    if (e2_inst) {
        LOG_DEBUG_M(top->machineType, Stage::E2) << "[AGU Line" << dec << pipeid << "] " << e2_inst->Dump();
    }
    if (e3_inst) {
        LOG_DEBUG_M(top->machineType, Stage::E3) << "[AGU Line" << dec << pipeid << "] " << e3_inst->Dump();
    }
    if (e4_inst) {
        LOG_DEBUG_M(top->machineType, Stage::E4) << "[AGU Line" << dec << pipeid << "] " << e4_inst->Dump();
    }
    if (w1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::WB) << "[AGU Line" << dec << pipeid << "] " << w1_inst->Dump();
    }
    if (w2_inst) {
        LOG_DEBUG_M(top->machineType, Stage::W2) << "[AGU Line" << dec << pipeid << "] " << w2_inst->Dump();
    }
    if (e1_inst && checkLSUStall(e1_inst->stid)) {
        LOG_DEBUG_M(top->machineType, Stage::NA) << "AGU pipe stall ";
    }
}

bool AGUPipe::checkLSUStall(uint32_t stid)
{
    return lda_req_q[stid]->getStall();
}

MemRequest AGUPipe::CreateMemReq(const SimInst &inst, bool isLoad, uint64_t mask, bool toMem)
{
    MemRequest memReq;
    memReq.tpc = inst->pc;
    memReq.peID = inst->peID;
    memReq.opcode = inst->opcode;
    memReq.laneSize = OpcodeManager::Inst().GetOpcodeAccMemBaseInfo(memReq.opcode)->bytes;
    memReq.realReqCnt = inst->realReqCnt;
    memReq.mask = mask;
    memReq.lanes = inst->lanes;
    memReq.thread = inst->peID;
    ASSERT(GetSim()->core->IsVecPe(inst->peID) || memReq.thread == GetSim()->core->GetMtcPEIndex());
    memReq.bid = inst->bid;
    memReq.rid = inst->rid;
    memReq.gid = inst->gid;
    memReq.tid = inst->tid;
    memReq.stid = inst->stid;
    memReq.lsID = inst->lsID;
    memReq.width = OpcodeManager::Inst().GetOpcodeAccMemBaseInfo(memReq.opcode)->bytes;
    memReq.isLoad = isLoad;
    memReq.start = 0;
    memReq.toMemory = toMem;
    memReq.isCTInst = inst->autogen;
    if (inst->isVgather) {
        memReq.cmdType = ReqCmdType::VGATHER;
    }
    memReq.transactionId = inst->transactionId;
    memReq.groupSlotId = inst->groupSlotId;
    memReq.uinst = inst;
    return memReq;
}

} // namespace JCore
