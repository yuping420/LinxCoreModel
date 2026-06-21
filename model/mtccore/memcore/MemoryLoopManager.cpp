#include "mtccore/memcore/MemoryLoopManager.h"

#include "core/Core.h"
#include "vectorcore/GROB.h"
#include "core/interface.h"
#include "iex/pipe/iex_pipe.h"
#if defined GENERIC_SOC || defined GENERIC_SOC_NEW
#include "generic_soc/soc_wrapper.h"
#else
#include "soc/soc_wrapper.h"
#endif


namespace JCore {

void MemoryLoopManager::Build()
{
    tloadInstQ.clear();
    tstoreInstQ.clear();
    m_lc0 = 0;
    m_lc1 = 0;
    m_lc2 = 0;
    m_lb0 = 0;
    m_lb1 = 0;
    m_lb2 = 0;
}

void MemoryLoopManager::Work()
{
    Dispatch();
    HandleInputReg();
    HandleOutputReg();
    HandleRegList();
    if (top->core->configs.mtc_tls_enable) {
        SendTLoadStoreInstReq();
    }
    ReceiveRequest();
    FakePickTcopy();
}

void MemoryLoopManager::ReqQueryInfo()
{
    uint32_t i = 0;
    while (!handleInputRegisterQ.empty() && i < top->core->configs.input_reg_num) {
        LocalFreeInfo info = handleInputRegisterQ.front();
        handleInputRegisterQ.pop();
        rreq_vec_srf_q->Write(info);
        ++i;
        LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Mtc Loop Manager]: Query info. bid " << info.bid
                    << " offset " << info.offset;
    }
}

void MemoryLoopManager::RespQueryInfo()
{
    while (!rrsp_srf_vec_q->Empty()) {
        LocalFreeInfo ptagInfo = rrsp_srf_vec_q->Read();

        LocalFreeInfo info = ptagInfo;
        data_vec_viex_q->Write(info);

        LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Mtc Loop Manager]: Response info and write to vector iex. bid "
            << ptagInfo.bid << " offset " << ptagInfo.offset << " ptag " << dec << ptagInfo.ptag
            << " data 0x" << ptagInfo.data;
    }
}

void MemoryLoopManager::RespViex()
{
    while (!data_viex_vec_q->Empty()) {
        LocalFreeInfo info = data_viex_vec_q->Read();
        lastInQ.push(info.bid);
        LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Mtc Loop Manager]: Response last from iex. bid " << info.bid;
    }
}

void MemoryLoopManager::HandleInputReg()
{
    RespViex();
    RespQueryInfo();
    ReqQueryInfo();
}

void MemoryLoopManager::ReqPtagFromOutFreeList()
{
    while (!handleOutputRegisterQ.empty()) {
        LocalFreeInfo info = handleOutputRegisterQ.front();
        handleOutputRegisterQ.pop();
        wreq_vec_srf_q->Write(info);
        LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Mtc Loop Manager]: Get ptag from freelist. bid " << info.bid
                    << " offset " << info.offset;
    }
}

void MemoryLoopManager::RespPtagFromOutFreeList()
{
    while (!wrsp_srf_vec_q->Empty()) {
        LocalFreeInfo info = wrsp_srf_vec_q->Read();
        lastOutQ.push(info.bid);

        LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Mtc Loop Manager]: Response fron freelist. bid " << info.bid;
    }
}

void MemoryLoopManager::HandleOutputReg()
{
    RespPtagFromOutFreeList();
    ReqPtagFromOutFreeList();
}

void MemoryLoopManager::HandleRegList()
{
    while (!handleRegisterCMDQ.empty()) {
        BlockCommandPtr cmd = handleRegisterCMDQ.front();
        handleRegisterCMDQ.pop();

        // Input reg list
        for (uint32_t i = 0; i < cmd->srcPtag.size(); ++i) {
            LocalFreeInfo info;
            info.vld = true;
            info.bid = cmd->bid;
            info.stid = cmd->stid;
            info.ptag = cmd->srcPtag[i];
            info.data = cmd->srcData[i];
            info.offset = i;
            handleInputRegisterQ.push(info);
            LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Mtc Loop Manager]: Handle input list. bid " << info.bid
                    << " offset " << info.offset;
        }
        LocalFreeInfo in;
        in.vld = true;
        in.bid = cmd->bid;
        in.stid = cmd->stid;
        in.last = true;
        handleInputRegisterQ.push(in);

        // Output reg list
        for (uint32_t i = 0; i < cmd->dstPtag.size(); ++i) {
            LocalFreeInfo info;
            info.vld = true;
            info.bid = cmd->bid;
            info.stid = cmd->stid;
            info.ptag = cmd->dstPtag[i];
            info.offset = i;
            handleOutputRegisterQ.push(info);
            LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Mtc Loop Manager]: Handle output list. Bid " << info.bid
                    << " offset " << info.offset;
        }
        LocalFreeInfo info;
        info.vld = true;
        info.bid = cmd->bid;
        info.stid = cmd->stid;
        info.last = true;
        handleOutputRegisterQ.push(info);
        workingCMDQ.push(cmd);
    }

    HandleInputReg();
    HandleOutputReg();
}


void MemoryLoopManager::ProduceTStoreInst(MtcBCCTLoadStoreReq req, uint64_t addr, SimInst& inst) const
{
    constexpr uint32_t laneNum = 64;
    // constexpr uint32_t dataOffset = 4;
    // uint64_t cacheLineSize = MAX_TILE_DATA_BYTE;

    inst->bid = req.bid;
    inst->pc = req.tpc;
    inst->iexType = MEM_IEX;
    inst->biqType = BIQType::MTC_IQ;
    inst->peID = top->GetSim()->core->GetMtcPEIndex();
    inst->tid = 0;
    // 需要适配最新的inst/src/dst operand 结构
    // inst->isScalar = false;
    // inst->isStore256 = true;

    // inst->tileSrc0.vld = true;
    // inst->tileSrc0.rdy = true;
    // inst->tileSrc0.shapeSize = cacheLineSize;
    // inst->tileSrc0.realAddr = addr;
    // inst->tileSrc0.baseAddrTR = req.tile.baseAddrTR;
    // inst->tileSrc0.dType = req.tile.dType;
    // inst->tileSrc0.layout = req.tile.layout;
    // inst->tileSrc0.d1TR = req.tile.d1TR;

    // inst->tileDst0.vld = true;
    // inst->tileDst0.rdy = true;
    // inst->tileDst0.realAddr = req.addr;
    // inst->tileDst0.strideGM = req.tile.strideGM;

    // inst->dst0.vld = true;
    // inst->dst0.dataOut = req.addr;
    // inst->opcode = Opcode::OP_TSD;
    // inst->dst0.vecDataVld = false;
    // inst->dst0->vecData.Init(static_cast<uint64_t>(OperandWidth::OPDW_D), laneNum);
    inst->lanes = laneNum;
    // inst->srcM.simtMask = -1;
    // for (uint32_t k = 0; k < laneNum; k++) {
    //     inst->dst0->vecData.Set((addr + k * dataOffset), k);
    // }
}

static uint32_t GetTStoreInstCount(MtcBCCTLoadStoreReq req)
{
    uint32_t dataSize = BytesOf(req.tile.dType);
    uint64_t cacheLineSize = MAX_TILE_DATA_BYTE;
    uint64_t tileSize = req.tile.d1TR * req.tile.d2TR * dataSize;
    uint64_t readStartAddr = req.tile.baseAddrTR;

    ASSERT((readStartAddr & ~(cacheLineSize - 1)) == readStartAddr);

    // 计算需要多少条cache line
    uint64_t totalBytes = tileSize;
    uint32_t instCount = 0;

    if (totalBytes > 0) {
        instCount = (totalBytes + cacheLineSize - 1) / cacheLineSize;
    }

    return instCount;
}

void MemoryLoopManager::ProduceTStoreReq(MtcBCCTLoadStoreReq req)
{
    ROBID currentRid;
    bool firstInst = true;
    uint32_t subridCounter = 0;
    uint32_t dataSize = BytesOf(req.tile.dType);
    uint64_t cacheLineSize = MAX_TILE_DATA_BYTE;
    uint64_t tileSize = req.tile.d1TR *  req.tile.d2TR * dataSize;
    uint64_t readStartAddr = req.tile.baseAddrTR;
    uint64_t readEndAddr = readStartAddr + tileSize;
    ASSERT((readStartAddr & ~(cacheLineSize - 1)) == readStartAddr);
    uint64_t readMidAddr = readStartAddr;

    LOG_INFO_M(Unit::MTC, Stage::D1) << " ProduceTStoreReq bid:" << dec << req.bid.val
                                      << " Read TileRegister Addr : 0x" << hex << readStartAddr
                                      << " tile size " << dec << tileSize << "Cacheline size" << dec << cacheLineSize;

    while (readMidAddr < readEndAddr) {
        SimInst inst = std::make_shared<SimInstInfo>();
        ProduceTStoreInst(req, readMidAddr, inst);
        if (firstInst) {
            inst->isLastInBlock = true;
            // dynamic_pointer_cast<OPE>(top->core->peArray[top->GetSim()->core->GetMtcPEIndex()])->
            //     prob[0]->allocROB(inst);
            currentRid = inst->rid;
            firstInst = false;
        } else {
            inst->rid = currentRid;
        }
        inst->pipeCycle->instStartCycle = top->GetSim()->cycles;
        inst->subrid.val = subridCounter;
        inst->subInstCnt = GetTStoreInstCount(req);
        subridCounter++;
        tstoreInstQ.push_back(inst);
        readMidAddr += cacheLineSize;
    }
}

static uint32_t GetNormTStoreInstCount(MtcBCCTLoadStoreReq req)
{
    std::unordered_set<uint64_t> uniqueCacheLines;
    uint32_t dataSize = BytesOf(req.tile.dType);
    uint64_t cacheLineSize = MAX_TILE_DATA_BYTE;
    uint64_t effectiveSize = req.tile.d1GM * dataSize;
    uint32_t instCount = 0;

    for (uint32_t i = 0; i < req.tile.d2GM; ++i) {
        uint64_t startAddr = req.tile.baseAddrTR + i * req.tile.d1TR * dataSize;
        uint64_t endAddr = startAddr + effectiveSize;
        uint64_t readAddr = startAddr & (~(cacheLineSize - 1));
        for (; readAddr < endAddr; readAddr += cacheLineSize) {
            if (uniqueCacheLines.count(readAddr) == 0) {
                uniqueCacheLines.insert(readAddr);
                instCount++;  // 只计数不实际产生指令
            }
        }
    }
    return instCount;
}

void MemoryLoopManager::ProduceNormTStoreReq(MtcBCCTLoadStoreReq req)
{
    ROBID currentRid;
    bool firstInst = true;
    uint32_t subridCounter = 0;
    std::unordered_set<uint64_t> uniqueCacheLines;
    uint32_t dataSize = BytesOf(req.tile.dType);
    uint64_t cacheLineSize = MAX_TILE_DATA_BYTE;
    uint64_t effectiveSize = req.tile.d1GM * dataSize;

    LOG_INFO_M(Unit::MTC, Stage::D1) << " [MemoryLoopManager] ProduceNormTStoreReq bid:" << dec << req.bid.val
                                     << " Read TileRegister BaseAddr : 0x" << hex << req.tile.baseAddrTR
                                     << " tile size: " << dec << effectiveSize
                                     << " dataSize: " << dec << dataSize;

    for (uint32_t i = 0; i < req.tile.d2GM; ++i) {
        uint64_t startAddr = req.tile.baseAddrTR + i * req.tile.d1TR * dataSize;
        uint64_t endAddr = startAddr + effectiveSize;
        uint64_t readAddr = startAddr & (~(cacheLineSize - 1));
        for (; readAddr < endAddr; readAddr += cacheLineSize) {
            if (uniqueCacheLines.count(readAddr) != 0) { continue; }
            SimInst inst = std::make_shared<SimInstInfo>();
            ProduceTStoreInst(req, readAddr, inst);
            if (firstInst) {
                inst->isLastInBlock = true;
                // dynamic_pointer_cast<OPE>(top->core->peArray[top->core->GetMtcPEIndex()])->prob[0]->allocROB(inst);
                currentRid = inst->rid;
                firstInst = false;
            } else {
                inst->rid = currentRid;
            }
            inst->pipeCycle->instStartCycle = top->GetSim()->cycles;
            inst->subrid.val = subridCounter;
            inst->subInstCnt = GetNormTStoreInstCount(req);
            subridCounter++;
            tstoreInstQ.push_back(inst);
            uniqueCacheLines.insert(readAddr);
        }
    }
}

static void TlsInitInsts(SimInst& inst, MtcBCCTLoadStoreReq& req)
{
    inst->opcode = Opcode::OP_TLD;
    inst->bid = req.bid;
    inst->pc = req.tpc;
    inst->iexType = MEM_IEX;
    inst->biqType = BIQType::MTC_IQ;
    inst->biqType = BIQType::TMA_IQ;
    // 需要适配最新的
    // inst->tileSrc0.baseAddrGM = req.addr;
    // inst->isScalar = true;
    // inst->isStore256 = false;
    // inst->tileDst0.rdy = true;
    // inst->tileSrc0.vld = true;
    // inst->tileSrc0.rdy = true;
    // inst->tileSrc0.shapeSize = MAX_TILE_DATA_BYTE;
    // inst->tileSrc0.strideGM = req.tile.strideGM;
    // inst->tileSrc0.layout = req.tile.layout;
    // inst->tileSrc0.elementType = req.tile.dType;
    // inst->tileSrc0.d1GM =  req.tile.d1GM;
    // inst->tileSrc0.d2GM =  req.tile.d2GM;
    // inst->tileDst0.realAddr = req.tile.baseAddrTR;
    // inst->tileDst0.d1TR = req.tile.d1TR;
    // inst->tileDst0.d2TR = req.tile.d2TR;
    // inst->tileDst0.tag = req.tile.ptag;
    // inst->dst0.vld = true;
    inst->isLastInBlock = false;
    // inst->cmd = req.cmd;
}


static uint32_t GetTotalSendInstCnt(MtcBCCTLoadStoreReq req)
{
    std::set<uint64_t> uniqueCacheLines;
    uint32_t totalInstructions = 0;
    uint64_t cacheLineSize = 256;
    uint64_t effectiveSize = req.tile.d1GM * BytesOf(req.tile.dType);

    // 第一遍：计算总指令数
    for (uint32_t i = 0; i < req.tile.d2GM; ++i) {
        uint64_t startAddr = req.addr + i * req.tile.strideGM;
        uint64_t endAddr = startAddr + effectiveSize;
        uint64_t readAddr = startAddr & (~(cacheLineSize - 1));

        for (; readAddr < endAddr; readAddr += cacheLineSize) {
            if (uniqueCacheLines.count(readAddr) == 0) {
                uniqueCacheLines.insert(readAddr);
                totalInstructions++;
            }
        }
    }
    return totalInstructions;
}
void MemoryLoopManager::ProduceTloadInsts(MtcBCCTLoadStoreReq req)
{
    if (tloadInstQ.size() > 0) { return; }
    LOG_INFO_M(Unit::MTC, Stage::D1) << "receive Tload: bid:" << dec <<req.bid;
    uint64_t cacheLineSize = 256;
    uint64_t dataSize = BytesOf(req.tile.dType);
    uint32_t subridCounter = 0;
    uint32_t totalInstCnt = GetTotalSendInstCnt(req);
    std::unordered_set<uint64_t> uniqueCacheLines;
    uint64_t effectiveSize = req.tile.d1GM * dataSize;
    ASSERT(req.tile.strideGM >= (effectiveSize));
    if (m_tloadInfo.subCnt > 0) {
        ASSERT(!m_tloadInfo.lastInstFin);
    }
    for (uint32_t i = 0; i < req.tile.d2GM; ++i) {
        uint64_t startAddr = req.addr + i * req.tile.strideGM;
        uint64_t endAddr = startAddr + effectiveSize;

        uint64_t readAddr = startAddr & (~(cacheLineSize - 1));
        for (; readAddr < endAddr; readAddr += cacheLineSize) {
            if (uniqueCacheLines.count(readAddr) != 0) { continue; }
            uniqueCacheLines.insert(readAddr);
            if ((subridCounter < m_tloadInfo.subCnt) && !m_tloadInfo.lastInstFin) {
                subridCounter++;
                continue;
            }
            SimInst inst = std::make_shared<SimInstInfo>();
            TlsInitInsts(inst, req);
            inst->peID = top->GetSim()->core->GetMtcPEIndex();
            // inst->tileSrc0.realAddr = readAddr;
            // inst->dst0.dataOut = readAddr;
            if (subridCounter == 0) {
                inst->isLastInBlock = true;
                // dynamic_pointer_cast<OPE>(top->core->peArray[top->GetSim()->core->GetMtcPEIndex()])->
                //     prob[0]->allocROB(inst);
                m_tloadInfo.currentRid = inst->rid;
            } else {
                inst->rid = m_tloadInfo.currentRid;
            }
            inst->subrid.val = subridCounter;
            inst->subInstCnt = totalInstCnt;
            // inst->dst0.tileDstAddr = readAddr;
            // inst->dst0.dataOut_vld = true;
            inst->pipeCycle->instStartCycle = top->GetSim()->cycles;

            m_tloadInfo.lastInstFin = false;
            subridCounter++;
            m_tloadInfo.subCnt = subridCounter;
            tloadInstQ.push_back(inst);
            return;
        }
    }
    ASSERT(subridCounter == totalInstCnt);
    m_tloadInfo.lastInstFin = true;
    m_tloadInfo.subCnt = 0;
}
void MemoryLoopManager::FakePickTcopy()
{
    // fake tcopy
    auto it = tcopyCmdEntries.begin();
    while (it != tcopyCmdEntries.end()) {
        it->timer--;

        if (it->timer == 0) {
            LOG_INFO_M(Unit::MTC, Stage::NA) << " commit block" << dec << it->cmd->bid;
            it->cmd->cmdExecCompleted = true;
            ASSERT(top->GetSim()->core->mtcCores[0] != nullptr);
            top->GetSim()->core->mtcCores[0]->mtcBCCWakeupQ->Write(it->cmd);
            uint32_t peIndex = top->GetSim()->core->GetMtcPEIndex();
            dynamic_pointer_cast<VecPE>(top->core->peArray[peIndex])->SetBlockComplete(it->cmd->bid, it->cmd->stid);
            it = tcopyCmdEntries.erase(it);
        } else {
            ++it;
        }
    }
}

void MemoryLoopManager::TstoreSendReadTileReq(SimInst &inst) const
{
    VecData addrs = inst->pdsts_[DST0_IDX]->vecData;
    bool toMem = true;
    uint64_t addr = -1U;
    for (uint32_t lane = 0; lane < inst->lanes; ++lane) {
        uint64_t curMask = inst->srcs[SRC0_IDX]->value; // 需要指定 一个src 单独存储mask？inst->srcM.simtMask
        if ((curMask & (1ULL << lane)) != 0) {
            addr = inst->pdsts_[DST0_IDX]->vecData.Get(lane, static_cast<uint32_t>(OperandWidth::OPDW_D));
            if ((addr & TILE_MASK) == TILE_MASK) {
                toMem = false;
            }
            addrs.Set(addr, lane, static_cast<uint32_t>(OperandWidth::OPDW_D));
        }
    }

    MemRequest memReq;
    memReq.tpc = inst->pc;
    memReq.tpc = inst->pc;
    memReq.peID = inst->peID;
    memReq.opcode = inst->opcode;
    memReq.laneSize = GetLoadStoreBytes(memReq.opcode);
    memReq.realReqCnt = inst->realReqCnt;
    memReq.addrs = addrs;
    memReq.data.width = inst->pdsts_[DST0_IDX]->vecData.width;
    memReq.data.size = inst->pdsts_[DST0_IDX]->vecData.size;
    memReq.data.data.resize(memReq.data.size);
    // memReq.mask = inst->srcM.simtMask;
    memReq.lanes = inst->lanes;
    memReq.thread = inst->peID;
    memReq.bid = inst->bid;
    memReq.rid = inst->rid;
    memReq.subrid = inst->subrid;
    memReq.gid = inst->gid;
    memReq.tid = inst->tid;
    memReq.lsID = inst->lsID;
    memReq.width = GetLoadStoreBytes(inst->opcode);
    memReq.isLoad = true;
    memReq.start = 0;
    memReq.toMemory = false;
    memReq.uinst = inst;
    // 微指令不应该感知block 属于哪个BIQ
    memReq.isMTCMemReq = (inst->biqType == BIQType::MTC_IQ) ? true : false;
    if (!toMem) {
        top->iexVcoreReqQ->Write(memReq);
        LOG_INFO_M(Unit::MTC, Stage::P1) << " [MemoryLoopManager] Send to Tile Register Read Req: " << memReq
                                         << " Subrid: " << dec << inst->subrid.val
                                         << " TileRegister Addr: 0x"  << hex << inst->pdsts_[DST0_IDX]->vecData.Get(0, static_cast<uint32_t>(OperandWidth::OPDW_D))
                                         << " ToalInst cnt : " << dec << inst->subInstCnt;
    }
}

#if defined GENERIC_SOC || defined GENERIC_SOC_NEW
void MemoryLoopManager::SendPrefetchReqToSoc(SimInst inst)
{
    if (!GetSim()->core->configs.soc_enable || !GetSim()->core->configs.mtc_prefetch_enable) {
        return;
    }
    static uint32_t g_cnt = 0;
    uint64_t addr = inst->pdsts_[DST0_IDX]->baseAddr;
    uint32_t size = PREFETCH_SIZE_BYTE;
    uint32_t reqtype = PREFETCH_REQ_TYPE;
    uint32_t tid = (PREFETCH_TID_TYPE << TID_TYPE_OFFSET) + (g_cnt++);
    bool ret = GetSim()->core->soc->MtcSendPrefetchReqToSoc(tid, addr, size, reqtype);
    if (ret == false) {
        g_cnt--;
    }
}
#endif

void MemoryLoopManager::SendTLoadStoreInstReq()
{
    while (!tloadInstQ.empty()) {
        if ((m_scalperInstQ->size() + top->GetSim()->core->GetMLSUScalpersize()) >=
            top->GetSim()->core->configs.scalper_size) {
            break;
        }
        SimInst inst = tloadInstQ.front();
        tloadInstQ.pop_front();
#if defined GENERIC_SOC || defined GENERIC_SOC_NEW
        SendPrefetchReqToSoc(inst);
#endif
        inst->pipeCycle->sendToScalperCycle  = top->GetSim()->cycles;
        m_scalperInstQ->push_back(inst);
        LOG_INFO_M(Unit::MTC, Stage::P1) << " Send tload to scalper: " << inst;
        if (GetSim()->core->configs.soc_enable && GetSim()->core->configs.mtc_prefetch_enable) {
            break;
        }
    }

    for (auto it = tstoreInstQ.begin(); it != tstoreInstQ.end(); ++it) {
        if (!(*it)->haveSend) {
            (*it)->pipeCycle->sendToTileReqCycle  = top->GetSim()->cycles;
            TstoreSendReadTileReq(*it);
            (*it)->haveSend = true;
        }
    }

    while (!tstoreInstQ.empty()) {
        SimInst inst = tstoreInstQ.front();
        if (inst->subissued) {
            GetSim()->core->iex[MEM_IEX]->dispatchUnit.pe_iex_sta_array[GetSim()->core->GetMtcPEIndex()]->Write(inst);
            LOG_INFO_M(Unit::MTC, Stage::E1) << " [Memory Core]Tile Data Ret Send to Iex Store pipe: " << inst;
            tstoreInstQ.pop_front();
        }

        break;
    }
}

void MemoryLoopManager::InitTloadTstoreReq(BlockCommandPtr cmd, MtcBCCTLoadStoreReq& req) const
{
    req.tile.dType = cmd->dataType;
    if (cmd->blockAttr != nullptr) {
        req.tile.layout = cmd->blockAttr->layout;
    }
    if (cmd->tileOp == TileOp::TLOAD || cmd->tileOp == TileOp::TSTORE) {
        ASSERT(cmd->srcData.size() >= 1);
        if (cmd->srcData.size() == 1) {
            req.tile.strideGM = cmd->lb0 * BytesOf(req.tile.dType);
        } else {
            req.tile.strideGM = cmd->srcData[1];
        }
        req.addr = cmd->srcData[0];
    }
    req.cmd = cmd;
    req.bid = cmd->bid;
    req.tpc = cmd->bTextPC;
    switch (cmd->tileOp) {
        case TileOp::TLOAD:
            if (!cmd->dsts.empty()) {
                req.tile.baseAddrTR = cmd->dsts[0]->baseAddr;
            }
            break;
        case TileOp::TSTORE:
            if (!cmd->srcs.empty()) {
                req.tile.baseAddrTR = cmd->srcs[0]->baseAddr;
            }
            break;
        default:
            return;
    }
    TileOperandPtr opdPtr = (cmd->tileOp == TileOp::TLOAD) ? cmd->dsts[0] : cmd->srcs[0];
    switch (req.tile.layout) {
        case LayOut::NORM:
        case LayOut::ND2NZ:
        case LayOut::ND2ZN:
            req.tile.d1GM = cmd->lb0;
            req.tile.d2GM = cmd->lb1;
            req.tile.d1TR = cmd->lb2;
            req.tile.d2TR = opdPtr->size / (req.tile.d1TR * BytesOf(cmd->dataType));
            break;
        default:
            req.tile.d2GM = cmd->lb0;
            req.tile.d1GM = cmd->lb1;
            req.tile.d2TR = cmd->lb2;
            req.tile.d1TR = opdPtr->size / (req.tile.d2TR * BytesOf(cmd->dataType));
    }
}

void MemoryLoopManager::ReceiveRequest()
{
    if (bccMtcBlockCmdQ->Empty()) {
        return;
    }
    BlockCommandPtr cmd = bccMtcBlockCmdQ->Front();
    LOG_INFO_M(Unit::MTC, Stage::NA) << "[Memory Loop Manager]: recv block cmd:" << cmd->Dump();

    MtcBCCTLoadStoreReq req;
    InitTloadTstoreReq(cmd, req);

    uint64_t fractalSize = top->GetConfig().fractal_row_bytes * top->GetConfig().fractal_row_num;
    uint32_t dataSize = BytesOf(req.tile.dType);

    if (cmd->tileOp == TileOp::TLOAD) {
        ProduceTloadInsts(req);
        if (!m_tloadInfo.lastInstFin) { return; }
        bccMtcBlockCmdQ->Read();
        return;
    } else if (cmd->tileOp == TileOp::TSTORE) {
        if (req.tile.layout == LayOut::ND2NZ) {
            ASSERT(req.tile.layout == LayOut::ND2NZ);
            ASSERT((req.tile.d1TR * req.tile.d2TR * dataSize) % fractalSize == 0);
            ASSERT((req.tile.d1TR * dataSize) % top->GetConfig().fractal_row_bytes == 0);
            ASSERT(req.tile.d2TR % top->GetConfig().fractal_row_num == 0);
            ProduceTStoreReq(req);
        } else if (req.tile.layout == LayOut::NORM) {
            ProduceNormTStoreReq(req);
        } else {
            ASSERT(0 && "TSTORE DONOT SUPPORT THIS LAYOUT");
        }
        bccMtcBlockCmdQ->Read();
        return;
    } else if (cmd->tileOp == TileOp::TMOV) {
        tcopyCmdEntries.emplace_back(FakeTcopyEntry(cmd, top->core->configs.fake_tcopy_execute_lat));
        bccMtcBlockCmdQ->Read();
        return;
    }
    if (top->core->configs.mtc_tls_enable) {
        ASSERT(false && "should return!");
    }
    bccMtcBlockCmdQ->Read();
    const uint32_t maxReadLGrp = 12;
    const uint32_t maxWrtiteLGrp = 4;
    if (GetSim()->core->lgprRF->CheckStallInput(maxReadLGrp)) {
        return;
    }
    if (GetSim()->core->lgprRF->CheckStallOutput(maxWrtiteLGrp)) {
        return;
    }
    handleRegisterCMDQ.push(cmd);
}

bool MemoryLoopManager::CheckDispEnable()
{
    return !lastInQ.empty() && !lastOutQ.empty() && lastInQ.front() == lastOutQ.front();
}

// loop Ctrl 分发
void MemoryLoopManager::Dispatch()
{
    if (!workingCMDQ.empty()) {
        if (!lastInQ.empty()) {
        }
        if (!lastOutQ.empty()) {
        }
    }
    if (workingCMDQ.empty() || !CheckDispEnable()) {
        return;
    }

    lastInQ.pop();
    lastOutQ.pop();
    BlockCommandPtr cmd = workingCMDQ.front();
    if (top->m_grob->CheckStall(1, cmd->stid)) {
        return;
    }
    workingCMDQ.pop();
    uint32_t lb0 = cmd->lb0 == 0 ? 1 : cmd->lb0;
    uint32_t lb1 = cmd->lb1 == 0 ? 1 : cmd->lb1;
    uint32_t lb2 = cmd->lb2 == 0 ? 1 : cmd->lb2;
    uint64_t shapeSize = lb0 * lb1 * lb2;
    uint64_t groupNum = (shapeSize + m_lanes - 1) / m_lanes;
    uint64_t avgGroupNum = groupNum / m_thdN;
    if (avgGroupNum == 0) {
        avgGroupNum = 1;
    }
    uint64_t lastGroupNum = shapeSize % m_lanes;
    if (lastGroupNum == 0) {
        lastGroupNum = m_lanes;
    }

    GetSim()->GetVerifyManager(0)->InitPARGroup(cmd->bid.val, groupNum);

    LOG_INFO_M(Unit::MTC, Stage::NA) << "[Memory Core Loop Manager]: LM handle:" << cmd->Dump();
    LOG_INFO_M(Unit::MTC, Stage::NA) << "[Memory Core Loop Manager]: generate group num: " << std::dec << groupNum;
    GroupAllocInfo grobAllocInfo;
    grobAllocInfo.bid = cmd->bid;
    grobAllocInfo.tpc = cmd->bTextPC;
    if (cmd->dsts.size() > 0) {
        grobAllocInfo.tag = cmd->dsts[0]->tileTag;
        grobAllocInfo.dest = cmd->dsts[0]->baseAddr;
        grobAllocInfo.destVld = true;
    } else {
        grobAllocInfo.tag = 0;
        grobAllocInfo.dest = 0;
        grobAllocInfo.destVld = false;
    }
    grobAllocInfo.blockCmd = cmd;
    grobAllocInfo.tileId = cmd->bid.val;
    grobAllocInfo.createCycle = GetSim()->getCycles();
    m_lm2GROB->Write(grobAllocInfo);

    VCore::GBufferAllocReq gBufferReq;
    ShapeLoopInfo shapeInfo = ShapeLoopInfo(lb0, lb1, lb2, groupNum, avgGroupNum, lastGroupNum, m_lanes);
    gBufferReq.vld = true;
    gBufferReq.blockCmd = cmd;
    gBufferReq.shapelpinfo = shapeInfo;
    m_lm2GBufferQ->Write(gBufferReq);
    LOG_INFO_M(Unit::MTC, Stage::NA) << "[Memory Core Loop Manager]: Dispatch" << cmd->Dump() << " to group buffer";
}

void MemoryLoopManager::ReportCommitGroup(const ROBID &bid, const ROBID &gid, uint32_t stid)
{
    ASSERT(top->m_grob->Find(bid, gid, stid));
    top->m_grob->ReportCommitGroup(bid, gid, stid);
}


SimSys* MemoryLoopManager::GetSim()
{
    return m_sim;
}

void MemoryLoopManager::SetSim(SimSys *sim)
{
    m_sim = sim;
}

void MemoryLoopManager::SetFlush(FlushBus &bus)
{
    top->m_grob->SetFlush(bus);

    auto match = [&bus](LocalFreeInfo &info) {
        return LessEqual(bus.req.bid, info.bid);
    };

    rreq_vec_srf_q->FlushIf(match);
    rrsp_srf_vec_q->FlushIf(match);
    wreq_vec_srf_q->FlushIf(match);
    wrsp_srf_vec_q->FlushIf(match);
    req_vec_siex_q->FlushIf(match);
    rsp_siex_vec_q->FlushIf(match);
    data_vec_viex_q->FlushIf(match);
    data_viex_vec_q->FlushIf(match);
}

} // namespace JCore
