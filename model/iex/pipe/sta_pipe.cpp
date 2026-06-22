#include "core/Core.h"
#include "iex/iex.h"
#include "iex/pipe/iex_pipe.h"
#include "interface/TTransTileRegStReq.h"

namespace JCore {

using namespace std;
void STAPipe::Build(uint32_t id) {
    pipeid = id;

    i1_inst = std::make_shared<SimInstInfo>();
    i2_inst = std::make_shared<SimInstInfo>();
    e1_inst = std::make_shared<SimInstInfo>();
    w1_inst = std::make_shared<SimInstInfo>();
    w2_inst = std::make_shared<SimInstInfo>();
}

void STAPipe::Reset() {
    i1_inst = std::shared_ptr<SimInstInfo>(nullptr);
    i2_inst = std::shared_ptr<SimInstInfo>(nullptr);
    e1_inst = std::shared_ptr<SimInstInfo>(nullptr);
    w1_inst = std::shared_ptr<SimInstInfo>(nullptr);
    w2_inst = std::shared_ptr<SimInstInfo>(nullptr);
    sys_rd_req.Reset();
}

void STAPipe::Work() {
    LogPrint();
    runW2();
    runW1();
    runE1();
    runI2();
    runI1();
    runP1();
}

void STAPipe::moveLpv() {
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

void STAPipe::move() {
    w2_inst = w1_inst;
    if (memStall) {
        return;
    }
    w1_inst = e1_inst;
    e1_inst = i2_inst;
    i2_inst = i1_inst;
    i1_inst = *p1_inst;
    *p1_inst = nullptr;
}

void STAPipe::flush(FlushBus &flushReq) {
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
    flush_stage(w1_inst);
    flush_stage(w2_inst);
}

void STAPipe::runP1() {
    if ((p1_inst == nullptr) || (*p1_inst == nullptr)) {
        return;
    }
    if (!(*p1_inst)->storeSplit) {
        (*p1_inst)->pipeCycle->p1Cycle = sim->getCycles();
    }
}

void STAPipe::runI1() {
    rf_rd_req.Reset();
    if (!i1_inst) {
        return;
    }
    if (!i1_inst->storeSplit) {
        i1_inst->pipeCycle->i1Cycle = sim->getCycles();
    }

    rf_rd_req = i1_inst->GenRFReqBus(true);
    sys_rd_req = i1_inst->GenSYSReqBus();
}

void STAPipe::runI2() {
    if (!i2_inst) {
        return;
    }
    if (!i2_inst->storeSplit) {
        i2_inst->pipeCycle->i2Cycle = sim->getCycles();
    }

    if (!rf_data_ret.vld) {
        return;
    }
    i2_inst->RFRetSetData(rf_data_ret);
    if (!sys_data_ret.vld) {
        return;
    }
    i2_inst->RecSYSReqRet(sys_data_ret);
}

void STAPipe::TStoreSendNormReqToMscb(SimInst &inst, MemReqBus &scbWrReq) const
{
    uint32_t dataTypeSize = inst->psrcs_[SRC0_IDX]->mtcElementWidth;
    uint64_t srcTRBaseAddr = inst->psrcs_[SRC0_IDX]->baseAddr;
    uint64_t elemSrcAddr = inst->psrcs_[SRC0_IDX]->mtcRealAddr;
    uint64_t srcRowWidth = inst->psrcs_[SRC0_IDX]->mtcD1TR * dataTypeSize;
    uint64_t dstGmBaseAddr = inst->pdsts_[DST0_IDX]->mtcRealAddr;

    LOG_INFO_M(Unit::MTC, Stage::E1) << " [STA PIPE]TStoreSendNormReqToMscb Src Addr : " << hex << elemSrcAddr
                                    << " Memory Base Addr: 0x" << hex << dstGmBaseAddr
                                    << " Size " << dec << dataTypeSize << " srcRowWidth " << dec << srcRowWidth
                                    << " bid " << dec << scbWrReq.bid.val << " rid " << dec << scbWrReq.rid.val;
    // Send write inst
    for (uint32_t count = 0; count < scbWrReq.size / dataTypeSize; count++) {
        uint64_t srcAddrRow = srcTRBaseAddr + floor((elemSrcAddr - srcTRBaseAddr) / srcRowWidth) * srcRowWidth;
        uint32_t j = (elemSrcAddr - srcAddrRow) / dataTypeSize;
        uint32_t dstAddrRow = dstGmBaseAddr + (srcAddrRow - srcTRBaseAddr) * inst->pdsts_[DST0_IDX]->mtcStrideGM / srcRowWidth;
        uint32_t elemDstAddr = dstAddrRow + j * dataTypeSize;

        scbWrReq.addr = elemDstAddr;
        scbWrReq.tag = elemDstAddr & (~0xff);
        scbWrReq.size = dataTypeSize;
        scbWrReq.toMtcLsu = true;

        uint32_t byteOffset = count * dataTypeSize;
        memcpy(&scbWrReq.data, &scbWrReq.memData[byteOffset], dataTypeSize);
        // 不允许直接使用过指针传输数据。
        // top->core->peArray[top->core->GetMtcPEIndex()]->peScbWrQ->push_back(scbWrReq);
        elemSrcAddr = elemSrcAddr + count * dataTypeSize;
    }
}

void STAPipe::TStoreSendNDReqToMscb(SimInst &inst, MemReqBus &scbWrReq) const
{
    uint32_t dataTypeSize = inst->psrcs_[SRC0_IDX]->mtcElementWidth;
    uint64_t srcTRBaseAddr = inst->psrcs_[SRC0_IDX]->baseAddr;
    uint64_t elemSrcAddr = inst->psrcs_[SRC0_IDX]->mtcRealAddr;
    uint32_t fractalRows = top->core->mtcCores[0]->config.fractal_row_num;
    uint32_t fractalRowBytes = top->core->mtcCores[0]->config.fractal_row_bytes;
    uint32_t i;
    uint32_t j;
    uint32_t colInBlock;
    uint32_t remainder;
    uint32_t fractalIndex;

    LOG_INFO_M(Unit::MTC, Stage::E1) << " [STA PIPE]TStoreSendNDReqToMscb Src Addr : 0x" << hex << srcTRBaseAddr
                                    << " Memory Base Addr " << hex << inst->pdsts_[DST0_IDX]->mtcRealAddr
                                    << " size " << dec << dataTypeSize
                                    << " bid " << dec << scbWrReq.bid.val << " rid " << dec << scbWrReq.rid.val;

    uint64_t fractalSize = fractalRows * fractalRowBytes;
    uint32_t srcFracCol = fractalSize / (fractalRows * dataTypeSize);

    for (uint32_t count = 0; count < scbWrReq.size / dataTypeSize; ++count) {
        uint32_t srcIndex = (elemSrcAddr - srcTRBaseAddr) / dataTypeSize;
        fractalIndex = srcIndex / (fractalRows * srcFracCol);
        remainder = srcIndex % (fractalRows * srcFracCol);
        i = remainder / srcFracCol;
        colInBlock = remainder % srcFracCol;
        j = fractalIndex * srcFracCol + colInBlock;
        uint64_t dstGmAddr = inst->pdsts_[DST0_IDX]->mtcRealAddr + i * inst->pdsts_[DST0_IDX]->mtcStrideGM + j * dataTypeSize;
        scbWrReq.addr = dstGmAddr;
        scbWrReq.tag = dstGmAddr & (~0xff);
        scbWrReq.size = dataTypeSize;
        scbWrReq.toMtcLsu = true;
        uint32_t byteOffset = count * dataTypeSize;
        memcpy(&scbWrReq.data, &scbWrReq.memData[byteOffset], dataTypeSize);
        // top->core->peArray[top->core->GetMtcPEIndex()]->peScbWrQ->push_back(scbWrReq);
        elemSrcAddr = elemSrcAddr + count * dataTypeSize;
    }
}

void STAPipe::TstoreSendSeqToMscb(SimInst &inst) const
{
    MemReqBus scbWrReq = MemReqBus();
    scbWrReq.vld = true;
    scbWrReq.addr = inst->pdsts_[DST0_IDX]->mtcRealAddr;
    scbWrReq.tag = inst->pdsts_[DST0_IDX]->mtcRealAddr;
    scbWrReq.size = inst->lanes * 4;

    scbWrReq.rid = inst->rid;
    scbWrReq.bid = inst->bid;
    scbWrReq.gid = inst->gid;
    scbWrReq.instUid = inst->uid;
    scbWrReq.is_load = false;
    scbWrReq.opcode = inst->opcode;
    scbWrReq.peID = inst->peID;
    scbWrReq.stid = inst->stid;
    scbWrReq.iexTyp = inst->iexType;

    LOG_INFO_M(Unit::MTC, Stage::E1) << "STApipe Send req to Memory: " << scbWrReq;

    for (uint32_t lane = 0; lane < inst->lanes; ++lane) {
        uint64_t data = inst->pdsts_[DST0_IDX]->vecData.Get(lane);
        uint32_t baseIndex = lane * 4;
        for (int i = 0; i < 4; i++) {
            scbWrReq.memData[baseIndex + i] = (data >> (i * 8)) & 0xFF;
        }
    }

    if (inst->psrcs_[SRC0_IDX]->mtcLayout ==  LayOut::NORM) {
        TStoreSendNormReqToMscb(inst, scbWrReq);
    } else if (inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::ND2NZ) {
        TStoreSendNDReqToMscb(inst, scbWrReq);
    }
}

void STAPipe::runE1() {
    if (!e1_inst) {
        return;
    }
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
    if (e1_inst->memInst && simArrayStall()) {
        memStall = true;
        return;
    }
    if (!e1_inst->storeSplit) {
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
    if (IsScalarInst(e1_inst->codeLen)) {
        if (e1_inst->opcode == Opcode::OP_TSD) {
            e1_inst->pipeCycle->genStoreReqCycle = GetSim()->cycles;
            TstoreSendSeqToMscb(e1_inst);
            return;
        }
        e1_inst->Execute();
        uint32_t peCnt = GetSim()->core->configs.stdPeCount + GetSim()->core->configs.simtPeCount;
        MemReqBus req = e1_inst->GenMemReq(peCnt);
        auto sendScalarReq = [this](MemReqBus req) {
            req.realReqCnt = 1;
            req.laneSet.insert(0);
            sta_req_q[req.stid]->Write(req);
        };
        if (req.vld) {
            sendScalarReq(req);
            if (!req.is_load && IsLoadStorePair(req.opcode)) {
                ASSERT(e1_inst->psrcs_.size() > SRC1_IDX);
                MemReqBus second = req;
                second.addr += req.size;
                second.data = e1_inst->psrcs_[SRC1_IDX]->vecDataVld ?
                              e1_inst->psrcs_[SRC1_IDX]->vecData.Get(0,
                                  static_cast<uint32_t>(e1_inst->psrcs_[SRC1_IDX]->width)) :
                              e1_inst->psrcs_[SRC1_IDX]->data;
                second.tag = CalTag(second.addr, second.toMtcLsu);
                second.reqData.Reset();
                second.mtc_reqData.Reset();
                second.isCrossCacheLine = AddrCrossCacheline(second.addr, second.size, second.toMtcLsu);
                sendScalarReq(second);
            }
        }
    } else {
        bool toMem = e1_inst->accMemInfo != nullptr ? !e1_inst->accMemInfo->local : true;
        VecData addrs = e1_inst->pdsts_[DST0_IDX]->vecData;
        uint64_t addr = -1U;
        auto predSrc = e1_inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK);
        uint64_t curMask = predSrc == nullptr ? e1_inst->predMask : predSrc->data;
        std::unordered_map<uint64_t, std::set<uint32_t>> addrLaneMap;

        for (uint32_t lane = 0; lane < e1_inst->lanes; ++lane) {
            if (curMask & (1ULL << lane)) {
                bool ret = e1_inst->Execute(lane);
                (!ret) ? top->core->bctrl->blockROB.reportException(e1_inst->bid, e1_inst->stid,
                                                                    "STA execute exception(SIMT)") : void();
                ++e1_inst->realReqCnt;
                addr = e1_inst->pdsts_[DST0_IDX]->vecData.Get(lane, static_cast<uint32_t>(OperandWidth::OPDW_D));
                addrs.Set(addr, lane, static_cast<uint32_t>(OperandWidth::OPDW_D));
                addrLaneMap[addr].insert(lane);
            }
        }
        ASSERT(e1_inst->realReqCnt > 0);
        MemRequest memReq;
        memReq.tpc = e1_inst->pc;
        memReq.peID = e1_inst->peID;
        memReq.opcode = e1_inst->opcode;
        memReq.laneSize = OpcodeManager::Inst().GetOpcodeAccMemBaseInfo(memReq.opcode)->bytes;
        memReq.realReqCnt = e1_inst->realReqCnt;
        memReq.addrs = addrs;
        // if (OpcodeIsStoreReg(e1_inst->opcode)) {
        //     memReq.data = e1_inst->src2.vec_data;
        // } else {
        //     memReq.data = e1_inst->src0.vec_data;
        // }
        memReq.data = e1_inst->psrcs_[DST0_IDX]->vecData;
        memReq.mask = curMask;
        memReq.lanes = e1_inst->lanes;
        memReq.thread = e1_inst->peID;
        ASSERT(GetSim()->core->IsVecPe(e1_inst->peID) || memReq.thread == GetSim()->core->GetMtcPEIndex());
        memReq.bid = e1_inst->bid;
        memReq.gid = e1_inst->gid;
        memReq.rid = e1_inst->rid;
        memReq.lsID = e1_inst->lsID;
        memReq.tid = e1_inst->tid;
        memReq.stid = e1_inst->stid;
        memReq.width = OpcodeManager::Inst().GetOpcodeAccMemBaseInfo(memReq.opcode)->bytes;
        memReq.isLoad = false;
        memReq.toMemory = toMem;
        memReq.uinst = e1_inst;
        // 微指令不应该感知block 属于哪个BIQ
        memReq.isMTCMemReq = (e1_inst->biqType == BIQType::MTC_IQ) ? true : false;
        if (!toMem || (memReq.isMTCMemReq)) {
            top->iexVcoreReqQ->Write(memReq);
            top->stats->tile_store_inst_num++;
            if (e1_inst->isScatterSt) {
                top->stats->tile_scatter_store_inst_num++;
            } else {
                top->stats->tile_continious_store_inst_num++;
            }
            return;
        } else {
            top->stats->memory_store_inst_num++;
            if (e1_inst->isScatterSt) {
                top->stats->memory_scatter_store_inst_num++;
            } else {
                top->stats->memory_continious_store_inst_num++;
            }
            LOG_DEBUG_M(Unit::VECTOR, Stage::E1) << "send to memory " << e1_inst;
        }

        uint32_t peCnt = GetSim()->core->configs.stdPeCount + GetSim()->core->configs.simtPeCount;
        for (auto it : addrLaneMap) {
            MemReqBus req = e1_inst->GenMemReq(peCnt, *it.second.begin());
            req.realReqCnt = e1_inst->realReqCnt;
            if (req.vld) {
                req.simtLane = *it.second.begin();
                req.laneSet = it.second;
                sta_req_q[e1_inst->stid]->Write(req);
            }
        }
    }

    // ready table wtire
    // if (e1_inst->checkAlldstRanged() && top->core->configs.reno_dynamic_enable &&
    //     e1_inst->iexType == SCALAR_IEX) {
    //     RFReqBus req = e1_inst->genWRTableBus();
    //     if (req.vld) {
    //         iex_rt_wr_q->Write(req);
    //     }
    // }
}

void STAPipe::runW1() {
    if (!w1_inst) {
        return;
    }
    if (!w1_inst->storeSplit) {
        w1_inst->pipeCycle->w1Cycle = sim->getCycles();
    }
}


void STAPipe::runW2() {
    rf_wr_req.Reset();
    if (!w2_inst) {
        return;
    }
    if (!w2_inst->storeSplit) {
        w2_inst->pipeCycle->w2Cycle = sim->getCycles();
    }
    if (w2_inst->opcode != Opcode::OP_TSD) {
        // Generate readfile write bus
        rf_wr_req = w2_inst->GenRFReqBus(false);
    }

    // Generate PE resolved bus
    PEResolveBus rslv = w2_inst->GenRslvBus();
    rslv.isPipeStore = true;
    uint32_t tid = 0;
    if (top->GetSim()->core->IsVectorIex(top->machineType)) {
        tid = w2_inst->tid;
    } else {
        tid = w2_inst->stid;
    }
    rslv_array[w2_inst->peID][tid]->Write(rslv);
}

void STAPipe::LogPrint()
{
    bool logEnable = (p1_inst && (*p1_inst != nullptr)) || i1_inst || i2_inst || e1_inst || w1_inst || w2_inst;
    if (!logEnable) {
        return;
    }
    if (p1_inst && (*p1_inst != nullptr)) {
        LOG_DEBUG_M(top->machineType, Stage::P1) << "[STA Line" << dec << pipeid << "] " << (*p1_inst)->Dump();
    }
    if (i1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::I1) << "[STA Line" << dec << pipeid << "] " << i1_inst->Dump();
    }
    if (i2_inst) {
        LOG_DEBUG_M(top->machineType, Stage::I2) << "[STA Line" << dec << pipeid << "] " << i2_inst->Dump();
    }
    if (e1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::E1) << "[STA Line" << dec << pipeid << "] " << e1_inst->Dump();
    }
    if (w1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::WB) << "[STA Line" << dec << pipeid << "] " << w1_inst->Dump();
    }
    if (w2_inst) {
        LOG_DEBUG_M(top->machineType, Stage::W2) << "[STA Line" << dec << pipeid << "] " << w2_inst->Dump();
    }
}

} // namespace JCore
