#include "lsu/load_unit/ldq.h"

#include <cmath>

#include "lsu/lsu.h"

namespace JCore {

/* ResolveQ init */
void ResolveQ::Build(uint32_t depth, uint32_t rslvNum)
{
    capcity = depth;
    this->rslvNum = rslvNum;
}

void ResolveQ::Reset(void)
{
    entryArr.clear();
}

void ResolveQ::insert(MemReqBus &bus)
{
    entryArr.emplace_back(bus);
}

bool ResolveQ::empty(void)
{
    return entryArr.empty();
}

bool ResolveQ::full(void)
{
    return entryArr.size() >= capcity;
}

uint32_t ResolveQ::retired(IDBus &commitBus, LSUType typeId)
{
    uint32_t retireCnt = 0;
    LOG_DEBUG_M(Unit::LSU, Stage::NA) << "LDQ ResolveQ commitBus: " << commitBus;
    for (auto it = entryArr.begin(); it != entryArr.end();) {
        if ((typeId == LSUType::SCALAR_LSU && LessROBID(it->bid, it->lsID, commitBus.bid, commitBus.lsID)) ||
            (typeId == LSUType::VECTOR_LSU && LessROBID(it->bid, it->gid, it->lsID,
                commitBus.bid, commitBus.gid, commitBus.lsID))) {
            LOG_DEBUG_M(Unit::LSU, Stage::NA) << "REQ: " << *it << " retired";
            it = entryArr.erase(it);
            retireCnt++;
            continue;
        }
        ++it;
    }
    return retireCnt;
}

MemReqBus ResolveQ::detect(MemReqBus &bus)
{
    std::vector<MemReqBus>::iterator pOldest = entryArr.end();
    LOG_DEBUG_M(Unit::LSU, Stage::NA) << "ldq resolveQ detect bus: " << bus;
    for (auto it = entryArr.begin(); it != entryArr.end(); ++it) {
        LOG_DEBUG_M(Unit::LSU, Stage::NA) << dec << "BID: " << it->bid << ", LSID: " << it->lsID;
        LOG_DEBUG_M(Unit::LSU, Stage::NA) << hex << "it->addr: 0x" << hex << it->addr << ", size: " << dec << it->size;
        if (!(AddrOverlap(bus.addr, bus.size, it->addr, it->size)
            && LessEqual(bus.bid, bus.lsID, it->bid, it->lsID))) {
            continue;
        }
        // TODO: Enable TLOAD/TSTORE nuke flush
        if (bus.IsTileLS() || it->IsTileLS()) {
            continue;
            // cerr << bus << " <------->" << *it << endl;
            // ASSERT(0 && "TLOAD/TSTORE address overlap conflict!");
        }
        if (pOldest == entryArr.end() || LessEqual(it->bid, it->lsID, pOldest->bid, pOldest->lsID)) {
            pOldest = it;
        }
    }

    if (pOldest == entryArr.end()) {
        MemReqBus bus;
        return bus;
    }

    return (*pOldest);
}

void ResolveQ::flush(FlushBus &flushReq)
{
    for (auto it = entryArr.begin(); it != entryArr.end();) {
        if (flushReq.match(*it)) {
            it = entryArr.erase(it);
            continue;
        }
        ++it;
    }
}

/* PickEntry init */
void PickEntry::Reset(void)
{
    for (auto &e : entryArr) {
        e = NULL;
    }
}

void PickEntry::Build(uint32_t depth)
{
    entryArr = std::vector<PLUEntryInfo>(depth);
}

void CrossBuffer::Reset(void)
{
    crossQ.clear();
    completed.clear();
}

void CrossBuffer::flush(FlushBus &flushReq)
{
    auto flushMemReq = [&flushReq] (deque<MemReqBus> &q) {
        for (auto it = q.begin(); it != q.end();) {
            if (flushReq.match(*it)) {
                it = q.erase(it);
            } else {
                it++;
            }
        }
    };

    flushMemReq(crossQ);
    flushMemReq(completed);
}

/* LDQ init */
void LDQInfo::Reset(void)
{
    for (ClusterInfo &c : clusters) {
        c.Reset();
    }

    for (auto &pkt : ldqPkts) {
        pkt = nullptr;
    }

    for (auto &buf: crossBuffer) {
        buf.Reset();
    }

    pL1LookupEntries.Reset();
    pL2LookupEntries.Reset();
    rslvQ.Reset();

    missQ.clear();
    prefSet.clear();
    loadPending = false;
    loadPendingCyc = 0;
    lastId = IDBus();
}

void LDQInfo::Build(void)
{
    loadPending = false;
    loadPendingCyc = 300;

    uint32_t count = pConfigs->lu_clusters_depth;
    if (top->typeId == LSUType::SCALAR_LSU) {
        count = pConfigs->scalar_lu_clusters_depth;
    }
    clusters.resize(pConfigs->lu_clusters_count);
    for (uint32_t cID = 0; cID < pConfigs->lu_clusters_count; ++cID)
        clusters[cID].Build(count, cID, pConfigs->lu_clusters_data_size);

    crossBuffer.resize(top->core->peArray.size());
    pL1LookupEntries.Build(pConfigs->l1_pick_count * pConfigs->lu_clusters_count);
    pL2LookupEntries.Build(pConfigs->l2_pick_count * pConfigs->lu_clusters_count);
    rslvQ.Build(pConfigs->rslv_capcity, pConfigs->l1_pick_count * pConfigs->lu_clusters_count);
    ldqPkts = std::vector<PtrPacket>(pConfigs->lu_clusters_count * count);
}

SimSys *LDQInfo::GetSim(void)
{
    return sim;
}

void LDQInfo::Work(void)
{
    // Oldest load is stalled
    checkDeadLock();
    // MDB check
    checkLoadPending();
    // Send L2-prefetch
    prefetch();
    // E3: receive DCache/ check for cancel
    receiveData();

    // E2: insert into LDQ/handle state info/lookup L1 and L2
    insert();
    mergeStateInfo();
    pick();
    lookup();
    checkCancel();

    // E1: query MDB/Hit
    queryByState();

    // Store is comming, check for conflict
    conflictDetect();
    // Check for retiring
    checkCommit();
    // Wakeup by STQ/L1
    LDQWakup();
    // Doing for statistics per cycle
    LUStatsTick();
}

void LDQInfo::Xfer(void)
{
    if (!top->l1Allow()) {
        ++top->stats->l2_stall_l1d_cycles;
    }

    // TODO: for DEBUG, do not delete.
    // uint32_t i = 0;
    // for (auto &clt : clusters) {
    //     ++i;
    //     if (clt.size == 0) {
    //         continue;
    //     }

    //     cout << "clt" << dec << i << endl;
    //     for (auto &e : clt.entryArr) {
    //         if (!e.fsm == LDQ_IDLE)
    //             cout << "LSU bus " << e.memReq << " fsm: " << e.fsm << " wait store: " << e.waitStore
    //             << " hit: " << e.dataHit() << endl;
    //     }
    //     cout << endl;
    // }

    // for (auto it = rslvQ.entryArr.begin(); it != rslvQ.entryArr.end(); ++it) {
    //     cout << "rslv bus " << *it << endl;
    // }

    // for (auto &simQ : iex_lsu_lda_array) {
    //     auto &readQ = simQ->GetRawReadData();
    //     for (MemReqBus &req : readQ) {
    //         cout<<"sim readQ request "<<req<<endl;
    //     }
    //     auto &wirteQ = simQ->GetRawWriteData();
    //     for (MemReqBus &req : wirteQ) {
    //         cout<<"sim writeQ request "<<req<<endl;
    //     }
    // }
}

/*
E1:
    check state
    1. check if wait for store
    2. check if hit in MDB
    3. check if hit in L1
*/
void LDQInfo::queryByState(void)
{
    std::deque<BlockCommandPtr> &bccTMABlockCmdWriteQ = bccTMABlockCmdQ->GetRawWriteData();
    for (auto &cmd : bccTMABlockCmdWriteQ) {
        if (cmd->tileOp != TileOp::TLOAD || top->typeId != LSUType::SCALAR_LSU) {
            break;
        }
        MemReqBus info;
        info.iexTyp = SCALAR_IEX;
        info.vld = true;
        info.bid = cmd->bid;
        info.stid = cmd->stid;
        info.tid = 0;
        info.is_load = true;
        info.blockCmd = cmd;
        info.addr = cmd->srcData[0];
        info.CalcGMSize();
        info.tag = CalTag(info.addr);
        handleStateQuery(info);
    }
    // Capcity is equal to iex
    for (auto &pipeQ : iexLsuLdaArray) {
        std::deque<MemReqBus> &writeQ = pipeQ->GetRawWriteData();
        if (writeQ.empty()) {
            continue;
        }

        for (std::deque<MemReqBus>::iterator it = writeQ.begin(); it != writeQ.end(); ++it) {
            handleStateQuery(*it);
        }
    }

    for (auto &tTransMemLdReqQ : top->tTransMemLdReqArray) {
        std::deque<TTransMemLdReq> &writeQ = tTransMemLdReqQ->GetRawWriteData();
        for (std::deque<TTransMemLdReq>::iterator it = writeQ.begin(); it != writeQ.end(); ++it) {
            MemReqBus req;
            req.vld = true;
            // TODO: set stid
            req.addr = it->GetAddr();
            req.size = it->GetSize();
            req.tTransId = it->GetReqId();
            req.tTransReq = true;
            handleStateQuery(req);
        }
    }

    // Prefecher
    for (uint32_t i = 0; i < pConfigs->pref_width && !pref_lu_q->Empty(); ++i) {
        MemReqBus bus = pref_lu_q->Read();
        handleStateQuery(bus);
    }
}

/*
    E2:
    insert into LDQ
*/
void LDQInfo::insert(void)
{
    while (!bccTMABlockCmdQ->Empty() && top->typeId == LSUType::SCALAR_LSU) {
        BlockCommandPtr cmd = bccTMABlockCmdQ->Front();
        if (cmd->tileOp != TileOp::TLOAD) {
            break;
        }
        bccTMABlockCmdQ->Read();
        LOG_INFO_M(top->machineType, Stage::NA) << "STID: "
                                                << dec << this->stid << " ldq recv blockCMD " << cmd->Dump();
        MemReqBus info;
        info.iexTyp = SCALAR_IEX;
        info.vld = true;
        info.bid = cmd->bid;
        info.stid = cmd->stid;
        info.tid = 0;
        info.is_load = true;
        info.blockCmd = cmd;
        info.addr = cmd->srcData[0];
        info.size = cmd->lb0 * cmd->lb1 * BytesOf(cmd->dataType);
        info.tag = CalTag(info.addr);
        handleInsert(info);
    }

    // Capcity is equal to iex
    for (auto &pipeQ : iexLsuLdaArray) {
        while (!pipeQ->Empty()) {
            MemReqBus bus = pipeQ->Read();
            if (!bus.isCrossCacheLine) {
                handleInsert(bus);
                continue;
            }

            // Cross Cacheline
            MemReqBus bus1, bus2;
            GetCrossReq(bus, bus1, bus2);
            handleInsert(bus1);
            handleInsert(bus2);
        }
    }

    for (auto &tTransMemLdReqQ : top->tTransMemLdReqArray) {
        while (!tTransMemLdReqQ->Empty()) {
            TTransMemLdReq req = tTransMemLdReqQ->Read();
            MemReqBus bus;
            bus.vld = true;
            // TODO: set stid
            bus.addr = req.GetAddr();
            bus.size = req.GetSize();
            bus.tTransId = req.GetReqId();
            bus.tTransReq = true;
            handleInsert(bus);
        }
    }
}

/*
    E2:
    update state:
    1. if wait for store
    2. if hit in MDB
    3. if hit in L1
*/
void LDQInfo::mergeStateInfo(void)
{
    while (!tag_l1_lu_q->empty()) {
        MemReqBus bus = tag_l1_lu_q->front();
        tag_l1_lu_q->pop_front();
        updateHitInfo(bus);
    }

    while (!tag_scb_lu_q->empty()) {
        MemReqBus bus = tag_scb_lu_q->front();
        tag_scb_lu_q->pop_front();
        updateSCBHitInfo(bus);
    }

    while (!lookup_mdb_lu_q->empty()) {
        MDBBus bus = lookup_mdb_lu_q->front();
        lookup_mdb_lu_q->pop_front();
        updateMDBInfo(bus);
    }

    // TODO: load is coming, but the overlap store had stayed in STQ.
    while (!wait_su_lu_q->empty()) {
        MemReqBus bus = wait_su_lu_q->front();
        wait_su_lu_q->pop_front();
        updateWaitInfo(bus);
    }
}

/*
    E2:
    pick entries for L1/L2
*/
void LDQInfo::pick(void)
{
    // Pick L1-lookup entries
    for (auto &pickE : pL1LookupEntries.entryArr) {
        pickE = NULL;
    }
    if (top->l1Allow()) {
        for (uint32_t cID = 0; cID < clusters.size(); ++cID) {
            pickL1(cID);
        }
    }

    // Pick L2-lookup entries
    for (auto &pickE : pL2LookupEntries.entryArr) {
        pickE = NULL;
    }
    if (top->l2Allow()) {
        for (uint32_t cID = 0; cID < clusters.size(); ++cID) {
            pickL2(cID);
        }
    }
    // pick tile load
    auto pickTload = [this](uint32_t id) {
        uint32_t cid = 0;
        uint32_t eid = 0;
        bool found = false;
        for (uint32_t i = 0; i < clusters.size() && !rslvQ.full() && !lsuBridgeTloadArray[id]->getStall(); ++i) {
            if (clusters[i].size == 0) {
                continue;
            }
            for (uint32_t j = 0; j < clusters[i].entryArr.size(); ++j) {
                auto &curEntry = clusters[i].entryArr[j];
                if (!curEntry.memReq.vld || !curEntry.memReq.IsTileLS() || curEntry.fsm == LDQ_IDLE) {
                    continue;
                }
                if (top->core->configs.ruminateEnable && id >= top->core->configs.tauStartId) {
                    break;
                }
                auto &oldEntry = clusters[cid].entryArr[eid];
                if (!found || (found && LessEqual(curEntry.memReq.bid, oldEntry.memReq.bid))) {
                    cid = i;
                    eid = j;
                    found = true;
                }
            }
        }
        if (found) {
            LOG_INFO_M(Unit::LSU, Stage::NA) << "LSU: [ Cluster " << cid << ", Entry " << eid
                << "] Send Tload issue to bridge " << clusters[cid].entryArr[eid].memReq;
            lsuBridgeTloadArray[id]->Write(clusters[cid].entryArr[eid].memReq);
            rslvQ.insert(clusters[cid].entryArr[eid].memReq);
            clusters[cid].entryArr[eid].Reset();
            ASSERT(clusters[cid].size > 0);
            clusters[cid].size--;
        }
    };
    std::map<uint64_t, std::vector<uint32_t>> bpqSizeWithTMAId;
    for (uint32_t i = 0; i < top->core->configs.tileBridgeNum; ++i) {
        bpqSizeWithTMAId[top->core->tileBridges[i]->GetBPQSize()].push_back(i);
    }
    for (auto& kv : bpqSizeWithTMAId) {
        std::vector<uint32_t>& tmaIds = kv.second;
        for (uint32_t id : tmaIds) {
            pickTload(id);
        }
    }
}

/*
    E2:
    lookup (L1/L2)
*/
void LDQInfo::lookup(void)
{
    // L1-lookup
    for (auto &pickE : this->pL1LookupEntries.entryArr) {
        if (pickE == nullptr) {
            continue;
        }
        handleL1Lookup(pickE->memReq);
    }

    // L2-lookup
    for (auto &pickE : this->pL2LookupEntries.entryArr) {
        if (pickE == nullptr) {
            continue;
        }
        handleL2Lookup(pickE->memReq);
        pickE->fsm = LDQ_L2_WAIT;
    }
}

/*
    E3:
    merge data
*/
void LDQInfo::receiveData(void)
{
    while (!lookup_l1_lu_q->empty()) {
        MemReqBus bus = lookup_l1_lu_q->front();
        lookup_l1_lu_q->pop_front();
        handleL1Receive(bus);
    }

    while (!upgrade_l1_lu_q->Empty()) {
        MemReqBus bus = upgrade_l1_lu_q->Read();
        handleL1Upgrade(bus);
    }

    while (!lookup_scb_lu_q->empty()) {
        MemReqBus bus = lookup_scb_lu_q->front();
        lookup_scb_lu_q->pop_front();
        handleSCBReceive(bus);
    }

    while (!lookup_su_lu_q->empty()) {
        MemReqBus bus = lookup_su_lu_q->front();
        lookup_su_lu_q->pop_front();
        handleSTQReceive(bus);
    }

    uint32_t iexIdx = 0;
    uint32_t iexMaxPipe = 0;
    if (top->typeId == LSUType::SCALAR_LSU) {
        iexMaxPipe = top->core->iex[SCALAR_IEX]->configs.iexLdaIqCount;
    } else if (top->typeId == LSUType::VECTOR_LSU) {
        iexMaxPipe = top->core->vectorTop->IexLdPipeCount();
    } else {
        ASSERT(0 && "not support");
    }

    // Older return first
    if (pL1LookupEntries.entryArr.size() > iexMaxPipe) {
        sort(pL1LookupEntries.entryArr.begin(), pL1LookupEntries.entryArr.end(),
            [](PLUEntryInfo a, PLUEntryInfo b) {
            if (a && b)
                return LessROBID(a->memReq.bid, a->memReq.gid, a->memReq.lsID,
                    b->memReq.bid, b->memReq.gid, b->memReq.lsID);
            if (a)
                return true;
            return false;
        });
    }

    bool simtModeCheck = false;
    bool first = true;
    ROBID lastBid;
    ROBID lastRid;
    for (auto &pickE : this->pL1LookupEntries.entryArr) {
        if (!(pickE && pickE->fsm == LDQ_REPICK))
            continue;

        if (!simtModeCheck) {
            simtModeCheck = true;
            if (top->core->IsVectorIex(pickE->memReq.iexTyp)) {
                iexMaxPipe = top->core->vectorTop->IexLdPipeCount();
            }
        }

        // Multi-load-request may merge 1 for simt, so these load-request send 1 iex pipe
        uint32_t lastPipeID = iexIdx;
        if (!first && !(pickE->memReq.bid == lastBid && pickE->memReq.rid == lastRid)) {
            ++lastPipeID;
        }

        // Data return, but not enough
        if (!checkDataPosionValid(pickE->memReq.addr, pickE->memReq.size, pickE->memReq.reqData)) {
            pickE->fsm = LDQ_L1_DC_MISS;
            if (!pickE->memReq.l1_miss) {
                pickE->memReq.l1_miss = true;
                pickE->memReq.l1MissCycle = GetSim()->getCycles();
            }
            ++top->stats->load_miss_count;
            pickE->rewait();
            continue;
        }

        if (!((pickE->ldqRnt || pickE->l1Rnt) && pickE->scbRnt && pickE->stqRnt && lastPipeID < iexMaxPipe)) {
            // Wait for re-pick
            pickE->fsm = LDQ_WAIT;
            pickE->storeBypass = true;
            pickE->rewait();
            continue;
        }

        // Return
        if (returnData(pickE->memReq, lastPipeID)) {
            lsuExecEngine(pickE->memReq);
            lastBid = pickE->memReq.bid;
            lastRid = pickE->memReq.rid;
            iexIdx = lastPipeID;
            first = false;
        }
    }
}

void LDQInfo::lsuExecEngine(MemReqBus &bus)
{
    MemReqBus stBus = bus;
    const uint32_t uinT32BitsMask = 0xffffffff;
    uint64_t dataL = stBus.data;
    uint64_t dataR = stBus.src1->data;
    uint64_t dataOut = 0;
    const uint32_t sizeVal = 4;
    if (bus.opcode != Opcode::OP_INVALID && OpcodeInInstGroup(bus.opcode, InstGroup::ATOMIC) && !bus.src1->dataVld) {
        ASSERT(0);
    }
    if (stBus.size == sizeVal) {
        dataL &= uinT32BitsMask;
        dataR &= uinT32BitsMask;
    }
    stBus.is_load = false;
    switch (bus.opcode) {
        case Opcode::OP_LW_ADD:
        case Opcode::OP_LD_ADD:
        case Opcode::OP_SW_ADD:
        case Opcode::OP_SD_ADD:
            dataOut = dataL + dataR;
            break;
        case Opcode::OP_LW_AND:
        case Opcode::OP_LD_AND:
        case Opcode::OP_SW_AND:
        case Opcode::OP_SD_AND:
            dataOut = dataL & dataR;
            break;
        case Opcode::OP_LW_OR:
        case Opcode::OP_LD_OR:
        case Opcode::OP_SW_OR:
        case Opcode::OP_SD_OR:
            dataOut = dataL | dataR;
            break;
        case Opcode::OP_LW_XOR:
        case Opcode::OP_LD_XOR:
        case Opcode::OP_SW_XOR:
        case Opcode::OP_SD_XOR:
            dataOut = dataL ^ dataR;
            break;
        case Opcode::OP_LW_MIN:
        case Opcode::OP_SW_MIN:
        case Opcode::OP_LD_MIN:
        case Opcode::OP_SD_MIN:
            dataOut = dataL < dataR ? dataL : dataR;
            break;
        case Opcode::OP_SW_MAX:
        case Opcode::OP_LW_MAX:
        case Opcode::OP_SD_MAX:
        case Opcode::OP_LD_MAX:
            dataOut = dataL > dataR ? dataL : dataR;
            break;
        default:
            return;
    }
    stBus.data = dataOut;
    ASSERT(atomic_lu_su_q);
    atomic_lu_su_q->Write(stBus);
    return;
}

/*
    E3:
    report cancel
*/
void LDQInfo::checkCancel(void)
{
    for (auto &clt : clusters) {
        if (clt.size == 0) {
            continue;
        }

        uint32_t itCnt = 0;
        for (auto &e : clt.entryArr) {
            if (!e.IsIdle() && clt.IsIteratorEnding(itCnt++)) {
                break;
            }

            if (!e.memReq.specWakeup || !e.IsWorking())
                continue;

            ++e.delayCnt;
            if (e.delayCnt == UNEXPECTED_DELAY) {
                LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]:[ Cluster " << e.memReq.cID << ", Entry " << std::dec
                    << e.memReq.eID << "] Delayed launch in LDQ. Cancel " << e.memReq;

                e.delayCnt = 0;
                handleCancel(e.memReq);

                // Statistic for PMU
                ++top->stats->ld_cancel;
                if (e.fsm == LDQ_L1_DC_MISS || e.fsm == LDQ_L2_WAIT) {
                    ++top->stats->ld_not_hit_cancel;
                    continue;
                }

                if (e.fsm == LDQ_WAIT) {
                    if (e.waitStore) {
                        ++top->stats->ld_wait_store_cancel;
                        continue;
                    }
                    // Delay in LDQ, because not the oldest
                }

                // Delay to repick
                ++top->stats->ldq_wait_cancel;
            }
        }
    }
}

void LDQInfo::prefetch(void)
{
    // Prefecher
    for (uint32_t i = 0; i < pConfigs->pref_width && !pref_l1_lu_q->empty() && top->l2Allow(); ++i) {
        MemReqBus bus = pref_l1_lu_q->front();
        pref_l1_lu_q->pop_front();
        if (bus.l1_miss) {
            handleL2Lookup(bus);
        }
    }
}

void LDQInfo::conflictDetect(void)
{
    while (!detect_su_lu_q->empty()) {
        MemReqBus bus = detect_su_lu_q->front();
        detect_su_lu_q->pop_front();
        handleDetect(bus);
    }
}

static IDBus GetScalarOldestInfo(std::vector<std::shared_ptr<PEBase>> &peArray, uint32_t start,
    uint32_t size, ROBID &oldestId, uint32_t stid)
{
    IDBus oldCmt;
    uint32_t end = start + size;
    for (uint32_t pe = start; pe < end; ++pe) {
        IDBus cmtBus = peArray[pe]->GetRetireID(stid);
        if (!cmtBus.vld || cmtBus.bid != oldestId) {
            continue;
        }

        oldCmt = (!oldCmt.vld || LessEqual(cmtBus.bid, cmtBus.lsID, oldCmt.bid, oldCmt.lsID)) ? cmtBus : oldCmt;
    }

    oldCmt.vld = true;
    oldCmt.bid = oldestId;
    return oldCmt;
}

static IDBus GetSimtOldestInfo(std::vector<std::shared_ptr<PEBase>> &peArray, uint32_t start,
    uint32_t size, ROBID &oldestId)
{
    IDBus oldCmt;
    uint32_t end = start + size;
    for (uint32_t pe = start; pe < end; ++pe) {
        IDBus cmtBus = peArray[pe]->GetRetireID();
        if (!cmtBus.vld) {
            continue;
        }
        oldCmt = (!oldCmt.vld || LessEqual(cmtBus.bid, cmtBus.gid, cmtBus.lsID,
            oldCmt.bid, oldCmt.gid, oldCmt.lsID)) ? cmtBus : oldCmt;
    }

    return oldCmt;
}

void LDQInfo::CheckMovRslvQ()
{
    // Check insert to resolve queue
    if (!rslvQ.full()) {
        uint32_t cnt = 0;
        for (auto &clt : clusters) {
            for (auto &e : clt.entryArr) {
                if (e.fsm != LDQ_RESOLVED)
                    continue;

                if (!e.memReq.isCrossCacheLine) {
                    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << e.memReq.cID << ", Entry " << std::dec
                        << e.memReq.eID << "] resolve from in LDQ.  " << e.memReq;
                    rslvQ.insert(e.memReq);
                    ++cnt;
                }

                e.Reset();
                ASSERT(clusters[e.cID].size > 0);
                --clusters[e.cID].size;

                if (cnt == rslvQ.rslvNum || rslvQ.full())
                    break;
            }
        }
    }
}

bool LDQInfo::checkFlushStall(ROBID &oldestBID)
{
    if (top->core->bctrl->blockROB.needFlush(oldestBID, stid)) {
        if (ldqCommitStallCyc != OLD_COMMIT_WAIT_CYC || oldestBID != lastId.bid) {
            lastId.bid = oldestBID;
            ldqCommitStallCyc++;
            return true;
        }
    }
    ldqCommitStallCyc = 0;
    return false;
}

bool LDQInfo::LDQRetired(IDBus &oldCmt, ROBID &oldestBID, ROBID &oldestGID)
{
    if (!oldCmt.vld || top->typeId == LSUType::SCALAR_LSU) {
        oldCmt.bid = oldestBID;
    }
    if (!oldCmt.vld && ((top->typeId == LSUType::SCALAR_LSU && lastId.bid != oldestBID)
        || (top->typeId == LSUType::VECTOR_LSU && LessEqual(lastId.bid, oldestBID)))) {
        oldCmt.vld = true;
        oldCmt.lsID.val = 0;
        oldCmt.lsID.wrap = false;
    }

    if (oldCmt.vld) {
        uint32_t retirecnt = rslvQ.retired(oldCmt, top->typeId);
        retirecnt += retire(oldCmt);
        if (retirecnt > 0) {
            if (top->typeId == LSUType::SCALAR_LSU) {
                // BIQType::STORE_IQ 没有对应的块类型，仅用于标量ld/st? 或者可删除
                // top->core->bctrl->blockIssueQueueUnit.WindowSlides(retirecnt, BIQType::LOAD_IQ, true);
                top->core->iex[SCALAR_IEX]->iq.windowSlides(retirecnt, true);
            } else if (top->typeId == LSUType::VECTOR_LSU) {
                BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(oldestBID, stid);
                if (IsBlockTypeParallel(cmd->blockType)) {
                    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: Because load request retired. Reset the idle cycle.";
                    GetSim()->ResetWaitCycle();
                }
            } else {
                ASSERT(0 && "not support");
            }
        }

        return true;
    }

    return false;
}

void LDQInfo::checkCommit(void)
{
    CheckMovRslvQ();
    // Check if remove from resolve queue
    // Return when need to flush or not block commit
    ROBID oldestBID = top->core->bctrl->blockROB.GetNonFlushOldestBid(stid);
    ROBID oldestGid = top->core->vectorTop->GetOldestGid(stid);
    if (checkFlushStall(oldestBID)) {
        return;
    }

    IDBus oldCmt;
    if (top->typeId == LSUType::SCALAR_LSU) {
        uint32_t startIdx = 0;
        uint32_t size = top->core->configs.stdPeCount;
        oldCmt = GetScalarOldestInfo(top->core->peArray, startIdx, size, oldestBID, stid);
    } else if (top->typeId == LSUType::VECTOR_LSU) {
        uint32_t startIdx = top->core->configs.stdPeCount;
        uint32_t size = top->core->configs.simtPeCount;
        oldCmt = GetSimtOldestInfo(top->core->peArray, startIdx, size, oldestBID);
    } else {
        ASSERT(0 && "not support");
    }

    if (LDQRetired(oldCmt, oldestBID, oldestBID)) {
        lastId.bid = oldestBID;
        lastId.gid = oldestGid;
    }
}

uint64_t LDQInfo::addr2LDQcID(uint64_t addr) {
    ASSERT(pConfigs->lu_clusters_count <= pow(2, pConfigs->lu_cluster_disp_bit_arr.size()));
    return Addr2cID(addr, pConfigs->lu_cluster_disp_bit_arr);
}

void LDQInfo::handleInsert(MemReqBus &bus)
{
    uint64_t cID = addr2LDQcID(bus.tag);
    // insert to LDQ
    for (LUEntryInfo &e : clusters[cID].entryArr) {
        if (e.fsm == LDQ_IDLE) {
            if (checkLdqHit(e.cID, bus) && !bus.IsTileLS()) {
                bus.ldq_miss = false;
                e.ldqHit = true;
            }
            e.insert(bus);
            top->countAddr(bus.addr);
            ++clusters[cID].size;

            // prefetch for hardware
            if (!bus.prefetch && !bus.IsTileLS()) {
                lu_pref_q->Write(bus);
            }

            LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << cID << ", Entry " << std::dec << e.memReq.eID <<
                "] Insert into LDQ. " << bus;
            return;
        }
    }

    cout << "Not insert but" << bus << endl;
    ASSERT(0 && "not insert");
}

MDBBus getMDBBus(MemReqBus &bus)
{
    MDBBus mBus;
    mBus.vld = true;
    mBus.ldInfo = bus;
    mBus.conf = 1;
    mBus.stid = bus.stid;

    return mBus;
}

void LDQInfo::handleStateQuery(MemReqBus &bus)
{
    bus.lsuRecvCycle = GetSim()->getCycles();
    ++top->stats->ld_input_reqs;

    if (bus.IsTileLS()) {
        // TODO: check overlap only
        // wait_lu_su_q->push_back(bus);
        // top->sendL1(tag_lu_scb_array, bus);
        return;
    }

    MDBBus mBus = getMDBBus(bus);
    lookup_lu_mdb_q->push_back(mBus);
    if (!bus.tTransReq) {
        wait_lu_su_q->push_back(bus);
    }

    if (!bus.isCrossCacheLine) {
        if (bus.prefetch) {
            top->sendL1(pref_lu_l1_array, bus);
            return;
        }

        top->sendL1(tag_lu_l1_array, bus);
        top->sendL1(tag_lu_scb_array, bus);
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: send request of querying " << bus;
        return;
    }

    MemReqBus bus1;
    MemReqBus bus2;
    GetCrossReq(bus, bus1, bus2);

    top->sendL1(tag_lu_l1_array, bus1);
    top->sendL1(tag_lu_l1_array, bus2);
    top->sendL1(tag_lu_scb_array, bus1);
    top->sendL1(tag_lu_scb_array, bus2);
    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: send request of querying [cross1] " << bus1 << " [cross2] " << bus2;
}

void LDQInfo::LDQWakup(void)
{
    while (!wakeup_l1_lu_q->Empty()) {
        PtrPacket pkt = wakeup_l1_lu_q->Front();
        // Snoop to L2
        if (pkt->isWriteBack()) {
            if (lookup_lu_l2_q->toBeOverflow(1)) {
                LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: L2 is stall when snooping.";
                break;
            }
            top->stats->dcache_l2_pkt_count++;
            sendMemPkt(pkt, pkt->isResp());
            if (pkt->isResp())
                ++top->stats->dcache_l2_resppkt_count;
            else
                ++top->stats->dcache_l2_writepkt_count;
        } else if (pkt->isWrite() || pkt->isUpgrade()) {
            if (lookup_lu_l2_q->toBeOverflow(1)) {
                LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: L2 is stall when upgrading.";
                break;
            }
            lookup_lu_l2_q->Write(pkt);

            LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: send packet " << *pkt;
        }
        wakeup_l1_lu_q->Read();
        handleL1Wakeup(pkt);
    }

    while (!wakeup_scb_lu_q->Empty()) {
        MemReqBus bus = wakeup_scb_lu_q->Read();
        handleSCBWakeup(bus);
    }

     while (!wakeup_su_lu_q->empty()) {
        MemReqBus bus = wakeup_su_lu_q->front();
        wakeup_su_lu_q->pop_front();
        handleSUWakeup(bus);
    }
}

bool LDQInfo::checkLdqHit(uint32_t cID, MemReqBus &bus)
{
    if (bus.IsTileLS()) {
        return false;
    }
    return clusters[cID].checkHit(bus.tag);
}

void LDQInfo::updateHitInfo(MemReqBus &bus)
{
    uint64_t cID = addr2LDQcID(bus.tag);
    if (clusters[cID].size == 0) {
        return;
    }

    uint32_t itCnt = 0;
    for (auto &e : clusters[cID].entryArr) {
        if (!e.IsIdle() && clusters[cID].IsIteratorEnding(itCnt++)) {
            break;
        }

        if (e.memReq.addr == bus.addr && e.IsWorking() && e.memReq.bid == bus.bid &&
            e.memReq.gid == bus.gid && e.memReq.lsID == bus.lsID) {
            e.l1Hit = !bus.l1_miss;
            if (!e.ldqHit && !e.l1Hit) {
                e.fsm = LDQ_L1_DC_MISS;
            }

            LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << cID << ", Entry " << std::dec << e.memReq.eID <<
                "] L1 Hit: " << e.l1Hit << ". Request " << bus;
            return;
        }
    }
    cout << "Not found bus " << bus << endl;
    ASSERT(0 && "not found");
}

bool LDQInfo::checkDataPosionValid(uint64_t addr, uint64_t size, ReqData cData)
{
    uint32_t off = addr & 0x3f;
    for (uint32_t i = off; i < off + size; ++i) {
        if (!cData.positionVld[i]) {
            return false;
        }
    }
    return true;
}

void LDQInfo::updateSCBHitInfo(MemReqBus &bus)
{
    uint64_t cID = addr2LDQcID(bus.tag);
    if (clusters[cID].size == 0) {
        return;
    }

    uint32_t itCnt = 0;
    for (auto &e : clusters[cID].entryArr) {
        if (!e.IsIdle() && clusters[cID].IsIteratorEnding(itCnt++)) {
            break;
        }

        if (e.memReq.addr == bus.addr && e.IsWorking() && e.memReq.bid == bus.bid && e.memReq.gid == bus.gid &&
            e.memReq.lsID == bus.lsID) {
            e.memReq.reqData.mergePosition(bus.reqData);
            e.storeBypass = bus.data_vld && checkDataPosionValid(e.memReq.addr, e.memReq.size, e.memReq.reqData);
            if (e.storeBypass)
                e.fsm = LDQ_WAIT;
            return;
        }
    }

    cout << "Not found bus " << bus << endl;
    ASSERT(0 && "not found");
}

void LDQInfo::updateMDBInfo(MDBBus &bus)
{
    if (!bus.hit) {
        return;
    }

    uint64_t cID = addr2LDQcID(bus.ldInfo.tag);
    if (clusters[cID].size == 0) {
        return;
    }

    uint32_t itCnt = 0;
    for (auto &e : clusters[cID].entryArr) {
        if (!e.IsIdle() && clusters[cID].IsIteratorEnding(itCnt++)) {
            break;
        }

        if (e.IsWorking() && e.memReq.bid == bus.ldInfo.bid && e.memReq.lsID == bus.ldInfo.lsID) {
            e.waitStore = true;
            e.waitBid = bus.stInfo.bid;
            e.waitStoreTpc = bus.stInfo.tpc;
            e.stallCycle = GetSim()->getCycles();

            e.memReq.wait_store = true;
            e.memReq.wait_bid = bus.stInfo.bid;
            e.memReq.wait_tpc = bus.stInfo.tpc;

            loadPending = true;

            LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << cID << ", Entry " << dec << e.memReq.eID <<"] "
                << "Wait for store by MDB. " << e.memReq
                << ",wait-store PC " << hex << bus.stInfo.tpc << dec << "-ID" << bus.stInfo.bid;
        }
    }
}

void LDQInfo::updateWaitInfo(MemReqBus &bus)
{
    uint64_t cID = addr2LDQcID(bus.tag);
    if (clusters[cID].size == 0) {
        return;
    }

    uint32_t itCnt = 0;
    for (auto &e : clusters[cID].entryArr) {
        if (!e.IsIdle() && clusters[cID].IsIteratorEnding(itCnt++)) {
            break;
        }

        if (e.IsWorking() && e.memReq.bid == bus.bid && e.memReq.gid == bus.gid && e.memReq.lsID == bus.lsID) {
            e.waitStore = bus.wait_store ? bus.wait_store : e.waitStore;
            e.waitBid = bus.wait_store ? bus.wait_bid : e.waitBid;
            e.waitStoreTpc = bus.wait_store ? bus.wait_tpc : e.waitStoreTpc;
            if (bus.wait_store) {
                LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << cID << ", Entry " << dec << e.memReq.eID
                    << "] " << "load request: " << bus << ",wait-store PC 0x" << hex << bus.wait_tpc << dec << ",B"
                    << bus.wait_bid << ":T" << bus.wait_rid;
            }
            if (bus.data_vld) {
                e.memReq.reqData.mergePosition(bus.reqData);
                e.storeBypass = checkDataPosionValid(e.memReq.addr, e.memReq.size, e.memReq.reqData);
            }
            if (e.storeBypass) {
                e.fsm = LDQ_WAIT;
            } else if (e.fsm == LDQ_L1_DC_MISS) {
                e.memReq.l1_miss = true;
                e.memReq.l1MissCycle = GetSim()->getCycles();
                ++top->stats->load_miss_count;
            }
        }
    }
}

bool checkPicked(PickEntry &pEntries, LUEntryInfo &e)
{
    for (auto &pk : pEntries.entryArr) {
        if (!pk)
            return false;
        if (pk->memReq.bid == e.memReq.bid && pk->memReq.lsID == e.memReq.lsID && pk->memReq.addr == e.memReq.addr)
            return true;
    }

    return false;
}

void LDQInfo::loadRepick(PLUEntryInfo &e, uint32_t &idx)
{
    // Pick
    e->fsm = LDQ_REPICK;
    pL1LookupEntries.entryArr[idx] = e;
    ++top->stats->ldq_pick_reqs;
    e->memReq.ldqPickCycle = GetSim()->getCycles();
    uint64_t stime = e->memReq.l1_miss ?
                        e->memReq.l2RntCycle : e->memReq.lsuRecvCycle;
    top->stats->ldq_pick_latency += (e->memReq.ldqPickCycle - stime);
}

static bool pickPriority(PLUEntryInfo &pickE, LUEntryInfo &e, LSUType type)
{
    if (e.memReq.IsTileLS()) {
        return false;
    }
    if (pickE == nullptr) {
        return true;
    }
    if (!pickE->dataHit() && e.dataHit()) {
        return true;
    }
    if ((type == LSUType::SCALAR_LSU) && LessEqual(e.memReq.bid, e.memReq.lsID, pickE->memReq.bid, pickE->memReq.lsID)
        && (pickE->dataHit() == e.dataHit())) {
        return true;
    }
    if ((type == LSUType::VECTOR_LSU) &&
        LessEqual(e.memReq.bid, e.memReq.gid, e.memReq.lsID, pickE->memReq.bid, pickE->memReq.gid, pickE->memReq.lsID)
        && (pickE->dataHit() == e.dataHit())) {
        return true;
    }
    return false;
}

void LDQInfo::pickL1(uint32_t cID)
{
    if (clusters[cID].size == 0) {
        return;
    }

    uint32_t pickNum = 0;
    for (uint32_t cnt = 0; cnt < pConfigs->l1_pick_count && pickNum < clusters[cID].size; ++cnt) {
        // Pick the Oldest request
        PLUEntryInfo oldestE = NULL;
        uint32_t itCnt = 0;
        for (auto &e : clusters[cID].entryArr) {
            if (!e.IsIdle() && clusters[cID].IsIteratorEnding(itCnt++)) {
                break;
            }
            if (e.fsm != LDQ_WAIT || e.waitStore || !e.dataHit() || e.memReq.IsTileLS()) {
                continue;
            }

            if (pickPriority(oldestE, e, top->typeId)) {
                oldestE = &e;
            }
        }

        if (oldestE == nullptr) {
            break;
        }

        ++pickNum;
        uint32_t pickIdx = cID * pConfigs->l1_pick_count + cnt;
        loadRepick(oldestE, pickIdx);

        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << cID << ", Entry " << oldestE->memReq.eID
            << "] Pick L1 request. " << oldestE->memReq << " ldq/l1/scb hit: "<< oldestE->dataHit();
    }
}

void LDQInfo::pickL2(uint32_t cID)
{
    if (clusters[cID].size == 0) {
        return;
    }

    for (uint32_t cnt = 0; cnt < pConfigs->l2_pick_count; ++cnt) {
        // Pick the Oldest request
        PLUEntryInfo oldestE = NULL;
        uint32_t itCnt = 0;
        for (auto &e : clusters[cID].entryArr) {
            if (!e.IsIdle() && clusters[cID].IsIteratorEnding(itCnt++)) {
                break;
            }

            if (e.fsm != LDQ_L1_DC_MISS || missQ.count(e.memReq.tag) != 0 || e.memReq.IsTileLS()) {
                continue;
            }

            if (!oldestE || (((top->typeId == LSUType::SCALAR_LSU) &&
                LessEqual(oldestE->memReq.bid, oldestE->memReq.lsID, e.memReq.bid, e.memReq.lsID)) ||
                ((top->typeId == LSUType::VECTOR_LSU) &&
                LessEqual(oldestE->memReq.bid, oldestE->memReq.gid, oldestE->memReq.lsID,
                    e.memReq.bid, e.memReq.gid, e.memReq.lsID)))) {
                oldestE = &e;
            }
        }

        if (oldestE == nullptr) {
            break;
        }

        // Pick
        uint32_t pickIdx = cID * pConfigs->l2_pick_count + cnt;
        oldestE->fsm = LDQ_L2_WAIT;
        pL2LookupEntries.entryArr[pickIdx] = oldestE;
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << cID << ", Entry " << oldestE->memReq.eID <<
            "] Pick L2 request. " << oldestE->memReq;
    }
}

void LDQInfo::handleL1Lookup(MemReqBus &bus)
{
    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << std::dec << bus.eID
        << "] L1 lookup. " << bus;

    bus.reqData.Reset();
    // To SCB/STQ
    top->sendL1(lookup_lu_scb_array, bus);
    lookup_lu_su_q->push_back(bus);

    LUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    // Data from LDQ or L1
    if (entry.ldqHit) {
        uint512_t data = clusters[bus.cID].cData.getData(bus.tag);
        entry.memReq.ldq_miss = false;
        entry.memReq.reqData.insertCacheData(data);
        entry.ldqRnt = true;
    } else {
        ++top->stats->ldq_miss_count;
        top->sendL1(lookup_lu_l1_array, bus);
    }
}

void LDQInfo::handleL2Lookup(MemReqBus &bus)
{
    if (!top->l2Allow()) {
        *pref_throw = true;
        return;
    }

    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << std::dec << bus.eID
        << "] L2 lookup. " << bus;

    // Need to send L2
    if (bus.prefetch && !wakeup_l1_lu_q->Empty()) {
        auto &writeQ = wakeup_l1_lu_q->GetRawReadData();
        for (auto it = writeQ.cbegin(); it != writeQ.cend(); ++it) {
            if ((*it)->addr == bus.tag && (*it)->isRead()) {
                return;
            }
        }
    }

    top->stats->dcache_l2_pkt_count++;

    uint64_t tag = bus.tag;
    uint32_t count = pConfigs->lu_clusters_depth;
    if (top->typeId == LSUType::SCALAR_LSU) {
        count = pConfigs->scalar_lu_clusters_depth;
    }
    uint64_t ldq2PktIdx = bus.cID * count + bus.eID;
    // Prefetch request had not responsed
    if (!bus.prefetch && prefSet.count(tag) != 0) {
        ++top->stats->pref_late_count;
        prefSet.erase(tag);
    }

    if (missQ.count(tag) == 0) {
        PtrPacket pkt = Packet::CreateRWPkt(!bus.is_load, 0);
        pkt->addr = tag;
        pkt->size = 64;
        pkt->tpc = bus.tpc;
        pkt->stid = bus.stid;

        if (bus.prefetch) {
            pkt->setPref();
            pkt->user_type = bus.type;
            prefSet.insert(bus.addr);

            switch (bus.type) {
                case PFTYPE_STRIDE:
                    top->stats->pref_stride_count++;
                    break;
                case PFTYPE_STREAM:
                    top->stats->pref_stream_count++;
                    break;
                case PFTYPE_SW_L1D:
                case PFTYPE_SW_L1I:
                case PFTYPE_SW_L2:
                    top->stats->pref_sw_inst++;
                    break;
                default:
                    cout<<"Error cyc "<<dec<<GetSim()->getCycles()<<endl;
                    cout<<"Error correctBCount "<<dec<<GetSim()->correctBCount<<endl;
                    ASSERT(0);
                    break;
            }

            top->stats->dcache_l2_pref_readpkt_count++;
        } else {
            top->stats->dcache_l2_demand_readpkt_count++;
        }

        missQ.emplace(tag, pkt);
        sendMemPkt(pkt, false);

        top->stats->dcache_l2_readpkt_count++;
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: send packet " << *pkt << " to L2";
        return;
    }

    ldqPkts[ldq2PktIdx] = missQ[tag];
    top->stats->dcache_l2_missq_filt_count++;
    if (bus.prefetch) {
        ++top->stats->dcache_l2_pref_missq_filt_count;
    } else {
        ++top->stats->dcache_l2_demand_missq_filt_count;
    }

    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: PFL1 requeset(0x << 0x" << bus.addr << ") hit miss queue";
    return;
}

void LDQInfo::handleL1Receive(MemReqBus &bus)
{
    LUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    if (entry.fsm != LDQ_REPICK) {
        return;
    }

    entry.l1Hit = bus.data_vld;
    entry.l1Rnt = true;
    entry.memReq.reqData.Reset();
    if (!entry.l1Hit) {
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << std::dec << bus.eID
            << "] l1 miss : " << bus;
        return;
    }

    // Update cluster data
    clusters[bus.cID].cData.merge(bus.tag, bus.reqData);
    // Update by Cacheline
    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << std::dec << bus.eID
        << "] receive L1 response " << bus << " data "<< bus.reqData.data.toStr();
    handleMerge(bus);
}

void LDQInfo::handleL1Upgrade(MemReqBus &bus)
{
    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << std::dec << bus.eID\
        << "] receive L1 upgrade " << bus << " data "<< bus.reqData.data.toStr();

    bus.cID = addr2LDQcID(bus.addr);
    clusters[bus.cID].cData.merge(bus.addr, bus.reqData);
    for (LUEntryInfo &e : clusters[bus.cID].entryArr) {
        if (e.fsm == LDQ_REPICK && bus.addr == e.memReq.tag) {
            bus.eID = e.memReq.eID;
            handleMerge(bus);
        }
    }
}

void LDQInfo::handleSCBReceive(MemReqBus &bus)
{
    LUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    // Wait for L2 or store, discard other response
    if (entry.fsm != LDQ_REPICK) {
        return;
    }

    entry.scbRnt = true;
    if (!bus.data_vld) {
        entry.storeBypass = false;
        return;
    }
    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << std::dec << bus.eID
        << "] receive SCB response " << bus << " data "<< bus.reqData.data.toStr();
    handleMerge(bus);
}

void LDQInfo::handleBypass(MemReqBus &bus)
{
    // SCB should return earlier or in the same time
    LUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    // Wait for L2 or store, discard other response
    if (entry.fsm != LDQ_REPICK) {
        return;
    }
    ASSERT(entry.scbRnt);

    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << std::dec << bus.eID
        << "] receive SU bypass " << bus << " data "<< bus.reqData.data.toStr();

    uint64_t cID = addr2LDQcID(bus.tag);
    for (auto &e : clusters[cID].entryArr) {
        if (e.fsm == LDQ_REPICK && AddrOverlap(bus.addr, bus.size, e.memReq.addr, e.memReq.size) &&
            LessEqual(bus.bid, bus.lsID, e.memReq.bid, e.memReq.lsID)) {
            bus.cID = cID;
            bus.eID = e.eID;
            handleMerge(bus);
        }
    }
}

void LDQInfo::handleSTQReceive(MemReqBus &bus)
{
    LUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    // Wait for L2 or store, discard other response
    if (entry.fsm != LDQ_REPICK) {
        return;
    }

    // SCB should return earlier or in the same time
    ASSERT(entry.scbRnt);
    entry.stqRnt = true;

    // Check if need to wait store
    if (bus.wait_store) {
        waitStore(bus);
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << std::dec << bus.eID
            << "] wait store pending " << bus;
        return;
    }

    // Wait for L2 or store, discard other response
    if (entry.fsm != LDQ_REPICK) {
        return;
    }

    entry.stqRnt = true;
    if (!bus.data_vld) {
        return;
    }
    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << std::dec << bus.eID
        << "] receive SU response " << bus << " cdata:"<< bus.reqData.data.toStr();
    handleMerge(bus);
}

void LDQInfo::waitStore(MemReqBus &bus)
{
    LUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    entry.fsm = LDQ_WAIT;
    entry.waitStore = true;
    entry.waitStoreTpc = bus.wait_tpc;
    entry.waitBid = bus.wait_bid;
    entry.rewait();
}

void LDQInfo::handleMerge(MemReqBus &bus)
{
    LUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    entry.memReq.reqData.merge(bus.reqData);
}

bool LDQInfo::returnData(MemReqBus &bus, uint32_t iexIdx)
{
    uint512_t data = bus.reqData.data;
    LUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    bus.data_vld = true;
    entry.fsm = LDQ_RESOLVED;

    if (bus.tTransReq) {
        // send to tile transfer
        MemTTransLdRes res;
        uint8_t dst[256];
        uint32_t bytes = 8;
        uint32_t bits = 8;
        union {
            uint64_t data;
            uint8_t src[8];
        } unionData;
        for (uint32_t i = 0; i < bytes; i++) {
            unionData.data = data.bits[i];
            for (uint32_t j = 0; j < bits; j++) {
                dst[j + i * bytes] = unionData.src[j];
            }
        }
        res.SetData(dst);
        res.SetReqId(bus.tTransId);
        // FIXME: support multiple transfer core
        top->tTransMemLdResArray[res.GetCoreId()]->Write(res);
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << dec << bus.eID << "] "
            << "return data to TTrans " << bus << " iex pipe: " << iexIdx << " l1 hit:" << entry.l1Hit
            << " scb/stq hit:" << entry.storeBypass;
        return false;
    } else {
        bus.data = ExtractData(data, bus.addr, bus.size);
    }
    // if not cross cacheline, then return
    if (!entry.crossLine) {
        bus.data = SignExtend(bus.data, bus.opcode);

        lsuIexLretArray[iexIdx]->Write(bus);

        bus.lsu_ret_cycle = GetSim()->getCycles();
        ++top->stats->total_load_request;
        top->stats->total_load_latency += (bus.lsu_ret_cycle - bus.lsuRecvCycle);

        if (!bus.specWakeup && !bus.stack_vld) {
            std::shared_ptr<IEX> iex;
            if (top->core->IsVectorIex(bus.iexTyp)) {
                iex = GetSim()->core->vectorTop->GetIex(bus.coreId);
            } else {
                iex = GetSim()->core->iex[bus.iexTyp];
            }
            iex->setMemWakeup(bus);
        }
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << bus.cID << ", Entry " << dec << bus.eID << "] "
            << "Load request handle over and return data. " << bus << " iex pipe: " << iexIdx
            << " l1 hit:" << entry.l1Hit << " scb/stq hit:" << entry.storeBypass;
        return true;
    }

    // if cross cacheline, wait for second req
    processCrossRtn(bus);
    return sendCrossRtn(bus.peID, iexIdx);
}

void LDQInfo::processCrossRtn(MemReqBus &rtnBus) {
    LOG_INFO_M(Unit::LSU, Stage::NA) << "LSTop get cross lsu return "<<rtnBus;

    bool hit = false;
    for (auto it = crossBuffer[rtnBus.peID].crossQ.begin();
            it != crossBuffer[rtnBus.peID].crossQ.end(); it++) {
        if (rtnBus.bid == (*it).bid && rtnBus.rid == (*it).rid) {
            hit = true;
            MemReqBus first;
            MemReqBus second;
            if ((rtnBus.addr & 0x3f) == 0) {
                first = (*it);
                second = rtnBus;
            } else {
                first = rtnBus;
                second = (*it);
            }
            clusters[first.cID].entryArr[first.eID].memReq.isCrossCacheLine = false;
            LOG_INFO_M(Unit::LSU, Stage::NA) << "LSTop matched first  half " << first << "LSTop matched second half "
                << second;
            first.data = first.data | (second.data << (first.size * 8));
            first.size = first.size + second.size;
            first.data = SignExtend(first.data, first.opcode);
            LOG_INFO_M(Unit::LSU, Stage::NA) << "LSTop merged crossLsu get " << first;
            crossBuffer[rtnBus.peID].completed.push_back(first);
            crossBuffer[rtnBus.peID].crossQ.erase(it);
            break;
        }
    }
    if (!hit) {
        if (rtnBus.specWakeup) {
            handleCancel(rtnBus);
            ++top->stats->ld_cancel;
            // Delay because miss in the other request.
            ++top->stats->ldq_crossline_cancel;
        }
        crossBuffer[rtnBus.peID].crossQ.push_back(rtnBus);
    }
}

bool LDQInfo::sendCrossRtn(uint32_t pe, uint32_t iexIdx)
{
    if (crossBuffer[pe].completed.size() > 0) {
        MemReqBus crossBus = crossBuffer[pe].completed.front();
        crossBuffer[pe].completed.pop_front();
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << crossBus.cID << ", Entry " << crossBus.eID <<
            "] Load request handle over and return cross data. " << crossBus;

        if (!crossBus.specWakeup && !crossBus.stack_vld) {
            std::shared_ptr<IEX> iex;
            if (top->core->IsVectorIex(crossBus.iexTyp)) {
                iex = GetSim()->core->vectorTop->GetIex(crossBus.coreId);
            } else {
                iex = GetSim()->core->iex[crossBus.iexTyp];
            }
            iex->setMemWakeup(crossBus);
        }

        lsuIexLretArray[iexIdx]->Write(crossBus);

        crossBus.lsu_ret_cycle = GetSim()->getCycles();
        ++top->stats->total_load_request;
        top->stats->total_load_latency += (crossBus.lsu_ret_cycle - crossBus.lsuRecvCycle);
        return true;
    }

    return false;
}

void LDQInfo::handleCancel(MemReqBus &mem)
{
    mem.specWakeup = false;
    std::shared_ptr<IEX> iex;
    if (top->core->IsVectorIex(mem.iexTyp)) {
        iex = GetSim()->core->vectorTop->GetIex(mem.coreId);
    } else {
        iex = GetSim()->core->iex[mem.iexTyp];
    }
    iex->reportCancel(mem);
}

void LDQInfo::flush(FlushBus &flushReq)
{
    for (uint32_t i = 0; i < top->core->GetPECount(); i++) {
        crossBuffer[i].flush(flushReq);
    }

    auto match = [&flushReq] (MemReqBus &bus) -> bool {
        return flushReq.match(bus);
    };
    for (auto simQ: iexLsuLdaArray) {
        simQ->FlushIf(match);
    }
    for (auto simQ: lsuIexLretArray) {
        simQ->FlushIf(match);
    }
    for (auto& simQ : lsuBridgeTloadArray) {
        simQ->FlushIf(match);
    }

    auto flushMemReq = [&match] (deque<MemReqBus>* q) {
        for (auto it = q->begin(); it != q->end();) {
            if (match(*it)) {
                it = q->erase(it);
            } else {
                it++;
            }
        }
    };

    flushMemReq(lookup_su_lu_q);
    flushMemReq(wait_su_lu_q);
    flushMemReq(detect_su_lu_q);
    flushMemReq(bypass_su_lu_q);
    flushMemReq(lookup_lu_su_q);
    flushMemReq(wait_lu_su_q);
    flushMemReq(tag_l1_lu_q);
    flushMemReq(lookup_l1_lu_q);
    flushMemReq(tag_scb_lu_q);
    flushMemReq(lookup_scb_lu_q);

    for (auto &q: pref_lu_l1_array)
        flushMemReq(q);
    for (auto &q: tag_lu_l1_array)
        flushMemReq(q);
    for (auto &q: lookup_lu_l1_array)
        flushMemReq(q);
    for (auto &q: tag_lu_scb_array)
        flushMemReq(q);
    for (auto &q: lookup_lu_scb_array)
        flushMemReq(q);

    for (auto &clt : clusters) {
        for (auto &e : clt.entryArr) {
            if (e.fsm != LDQ_IDLE && match(e.memReq)) {
                e.fsm = LDQ_IDLE;
                e.Reset();
                ASSERT(clt.size > 0);
                --clt.size;
            }
        }
    }

    rslvQ.flush(flushReq);
}

void LDQInfo::sendMemPkt(PtrPacket &pkt, bool resp)
{
    // top->stats->dcache_l2_pkt_count++;
    pkt->tid = (pkt->tid << 2) | 2;
    pkt->id = top->memID_s;
    pkt->l1_out_cycle = GetSim()->getCycles();
    if (resp) {
        // L1 snoop to L2
        snoop_lu_l2_q->Write(pkt);
    } else {
        lookup_lu_l2_q->Write(pkt);
    }
    LOG_INFO_M(Unit::LSU, Stage::NA) << "send packet"<< *pkt;
}

void LDQInfo::handleDetect(MemReqBus &bus)
{
    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: store comming detect. " << bus;

    MemReqBus *pOldConfBus = NULL;
    // Detect in Clusters
    for (auto &clt : clusters) {
        for (auto &e : clt.entryArr) {
            if (e.fsm == LDQ_IDLE) {
                continue;
            }
            LOG_DEBUG_M(Unit::LSU, Stage::NA) << "Load:";
            LOG_DEBUG_M(Unit::LSU, Stage::NA) << "\taddr: 0x" << hex << e.memReq.addr << ", size: "
                                              << dec << e.memReq.size;
            LOG_DEBUG_M(Unit::LSU, Stage::NA) << "\tBID: " << dec << e.memReq.bid << ", lsID: " << e.memReq.lsID;
            LOG_DEBUG_M(Unit::LSU, Stage::NA) << "Store:";
            LOG_DEBUG_M(Unit::LSU, Stage::NA) << "\taddr: 0x" << hex << bus.addr << ", size: " << dec << bus.size;
            LOG_DEBUG_M(Unit::LSU, Stage::NA) << "\tBID: " << dec << bus.bid << ", lsID: " << bus.lsID;
            LOG_DEBUG_M(Unit::LSU, Stage::NA) << "OVERLAP=" << boolalpha
                                              << AddrOverlap(bus.addr, bus.size, e.memReq.addr, e.memReq.size)
                                              << ", younger="
                                              << LessEqual(bus.bid, bus.lsID, e.memReq.bid, e.memReq.lsID);
            if (!(AddrOverlap(bus.addr, bus.size, e.memReq.addr, e.memReq.size)
                  && LessEqual(bus.bid, bus.lsID, e.memReq.bid, e.memReq.lsID))) {
                continue;
            }

            // TODO: Enable TLOAD/TSTORE nuke flush
            if (bus.IsTileLS() || e.memReq.IsTileLS()) {
                LOG_DEBUG_M(Unit::LSU, Stage::NA) << "TILE LOAD/TILE STORE conflict";
                continue;
                // cerr << bus << " <----------> " <<  e.memReq << endl;
                // ASSERT(0 && "TLOAD/TSTORE momery confilct!");
            }

            // Check when load had resolved
            if (e.fsm == LDQ_RESOLVED &&
                (!pOldConfBus || LessEqual(e.memReq.bid, e.memReq.lsID, pOldConfBus->bid, pOldConfBus->lsID))) {
                    pOldConfBus = &e.memReq;
                    continue;
            }

            if (bus.type == ST_ADDR) {
                e.waitStore = true;
                e.waitBid = bus.bid;
                e.waitStoreTpc = bus.tpc;
                e.stallCycle = GetSim()->getCycles();

                e.memReq.wait_store = true;
                e.memReq.wait_bid = bus.bid;
                e.memReq.wait_tpc = bus.tpc;
                loadPending = true;

                if (e.fsm == LDQ_REPICK) {
                    e.fsm = LDQ_WAIT;
                    e.rewait();

                    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: Store detect and cancel the request. " << e.memReq;
                }
                LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: load: " << e.memReq << " wait for store PC:0x" << std::hex
                    << bus.tpc << std::dec << " id:" << e.waitBid;
            }
        }
    }

    // Detect in resolve Queue
    MemReqBus conflictBus = rslvQ.detect(bus);
    if (conflictBus.vld &&
        (!pOldConfBus || LessEqual(conflictBus.bid, conflictBus.lsID, pOldConfBus->bid, pOldConfBus->lsID))) {
        pOldConfBus = &conflictBus;
    }

    // No need to flush
    if (!pOldConfBus)
        return;

    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: load(" << *pOldConfBus << ") conflict with store(" <<bus << ")";

    // Record
    MDBBus mBus = getMDBBus(*pOldConfBus);
    bool intraConflict = (bus.bid == pOldConfBus->bid);
    mBus.stInfo = bus;
    record_lu_mdb_q->push_back(mBus);
    top->core->bctrl->bmdb.reportConfilict(pOldConfBus->bid, bus.bid, bus.stid);
    std::shared_ptr<IEX> iex;
    if (top->core->IsVectorIex(pOldConfBus->iexTyp)) {
        iex = GetSim()->core->vectorTop->GetIex(pOldConfBus->coreId);
    } else {
        iex = GetSim()->core->iex[pOldConfBus->iexTyp];
    }
    iex->iexmdb.insert(pOldConfBus->tpc, pOldConfBus->bid, pOldConfBus->lsID,
        bus.tpc, bus.bid, bus.lsID, intraConflict);
    // Flush
    handleFlush(*pOldConfBus, bus);
}

void LDQInfo::handleFlush(MemReqBus &confbus, MemReqBus &stBus) const
{
    FlushReq flushReq;
    flushReq.vld = true;
    flushReq.bid = confbus.bid;
    flushReq.gid = confbus.gid;
    flushReq.rid = confbus.rid;
    flushReq.tid = confbus.tid;
    flushReq.fetchTPCVld = true;
    flushReq.fetchTPC = confbus.tpc;
    flushReq.peID = confbus.peID;
    flushReq.lsID = confbus.lsID;
    flushReq.peID = confbus.peID;
    flushReq.fbid = confbus.fbid;
    flushReq.iexTyp = confbus.iexTyp;
    flushReq.fbid_local = confbus.fbid_local;
    flushReq.noSplitBlk = confbus.noSplitBlk;
    flushReq.tSeq = confbus.tSeq;
    flushReq.uSeq = confbus.uSeq;
    flushReq.predSeq = confbus.predSeq;
    flushReq.stid = confbus.stid;
    if (stBus.bid == confbus.bid) {
        BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(flushReq.bid, flushReq.stid);
        if (cmd && cmd->opcode != Opcode::OP_INVALID) {
            return;
        }
        top->core->flushUnit->flush_stats->IntraBlockMemoryAaccelssConflict++;
        top->core->flushUnit->flush_stats->smtIntraBlockMemoryAaccelssConflictArray[flushReq.stid]++;
        flushReq.type = top->typeId == LSUType::SCALAR_LSU ? FlushType::INNER_FLUSH : FlushType::SIMT_INNER_FLUSH;
    } else {
        top->core->flushUnit->flush_stats->InterBlockMemoryAaccelssConflict++;
        top->core->flushUnit->flush_stats->smtInterBlockMemoryAaccelssConflictArray[flushReq.stid]++;
        flushReq.type = FlushType::NUKE_FLUSH;
        // TODO: simt only support for simt mode.
        if (top->core->IsVectorIex(flushReq.iexTyp)) {
            cout << "simt mode should not be inner load/store conflict. B" << flushReq.bid << ":T" << flushReq.rid
                << " tpc: 0x" << hex << confbus.tpc << " addr: 0x" << confbus.addr << endl;
            ASSERT(0);
        }
    }
    if (top->typeId == LSUType::SCALAR_LSU) {
        top->lsu_flush_rpt_q->Write(flushReq);
    } else {
        ASSERT(flushReq.type != FlushType::INNER_FLUSH);
        top->GetSim()->core->peArray[flushReq.peID]->ReportBlockFlush(flushReq);
    }
    top->stats->store_load_conflicts++;

    LOG_INFO_M(Unit::LSU, Stage::NA) << "load-store flush("
        << ((flushReq.type == FlushType::NUKE_FLUSH) ? "inter" : "inner") << "). load " << confbus << ". store "
        << stBus;
}

bool LDQInfo::checkStall()
{
    uint32_t clusterDepth = top->typeId == LSUType::SCALAR_LSU ?
        pConfigs->scalar_lu_clusters_depth :pConfigs->lu_clusters_depth;
    for (auto &clt : clusters) {
        if (top->typeId == LSUType::SCALAR_LSU && clt.size + iexLsuLdaArray.size() > clusterDepth) {
            return true;
        }

        if (top->typeId == LSUType::VECTOR_LSU &&
            clt.size + iexLsuLdaArray.size() * top->core->configs.simtLane > clusterDepth) {
            return true;
        }
    }

    return false;
}

bool LDQInfo::checkCltStall(uint64_t addr, uint32_t width)
{
    uint32_t cID = addr2LDQcID(addr);
    uint32_t curCnt = 0;
    for (auto &pipe : iexLsuLdaArray) {
        if (!pipe->Empty()) {
            auto &readQ =  pipe->GetRawReadData();
            for (auto &bus: readQ) {
            curCnt += addr2LDQcID(bus.addr) == cID ? 1 : 0;
            }
        }
    }

    uint32_t clusterDepth = top->typeId == LSUType::SCALAR_LSU ?
        pConfigs->scalar_lu_clusters_depth : pConfigs->lu_clusters_depth;
    return clusterDepth < width + curCnt + clusters[cID].size;
}

void LDQInfo::handleSUWakeup(MemReqBus &bus)
{
    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: SU wakeup. " << bus;

    for (auto &clt : clusters) {
        if (clt.size == 0) {
            continue;
        }

        uint32_t itCnt = 0;
        for (auto &e : clt.entryArr) {
            if (!e.IsIdle() && clt.IsIteratorEnding(itCnt++)) {
                break;
            }

            if (!e.IsWorking())
                continue;

            if (e.waitStore && e.waitBid == bus.bid && e.waitStoreTpc == bus.tpc) {
                e.waitStore = false;
                // TODO: Enable store wakeup TLOAD
                // if (e.memReq.IsTileLS()) {
                //     ASSERT(0 && "Not support store wakeup TLOAD");
                // }

                ++top->stats->mdb_suaccelss_count;
                LOG_INFO_M(Unit::LSU, Stage::NA) << "Waiting load request is waken up. Requst: "<< e.memReq;
            }

            if ((e.fsm == LDQ_L1_DC_MISS || e.fsm == LDQ_L2_WAIT) && bus.tag == e.memReq.tag &&
                LessEqual(bus.bid, bus.lsID, e.memReq.bid, e.memReq.lsID)) {
                e.memReq.reqData.mergePosition(bus.reqData);
                // TODO: Enable store wakeup TLOAD
                // if (e.memReq.IsTileLS()) {
                //     ASSERT(0 && "Not support store wakeup TLOAD");
                // }
                if (checkDataPosionValid(e.memReq.addr, e.memReq.size, e.memReq.reqData)) {
                    e.storeBypass = true;
                    e.fsm = LDQ_WAIT;
                    LOG_INFO_M(Unit::LSU, Stage::NA) << "Miss load request: " << e.memReq
                        << " is waken up by SU. Requst: "<< bus;
                }
            }
        }
    }
}

void LDQInfo::handleSCBWakeup(MemReqBus &bus)
{
    for (auto &clt : clusters) {
        if (clt.size == 0) {
            continue;
        }

        uint32_t itCnt = 0;
        for (auto &e : clt.entryArr) {
            if (!e.IsIdle() && clt.IsIteratorEnding(itCnt++)) {
                break;
            }

            if (e.memReq.tag == bus.tag && e.fsm != LDQ_REPICK && e.IsWorking()) {
                // TODO: Enable store wakeup TLOAD
                // if (e.memReq.IsTileLS()) {
                //     ASSERT(0 && "SCB TLOAD wakeup is not support yet!");
                // }
                e.memReq.reqData.mergePosition(bus.reqData);
                if (checkDataPosionValid(e.memReq.addr, e.memReq.size, bus.reqData)) {
                    e.storeBypass = true;
                    e.fsm = LDQ_WAIT;
                    LOG_INFO_M(Unit::LSU, Stage::NA) << "Waiting load request is waken up by SCB. Requst: "<< e.memReq;
                }
            }
        }
    }
}

void LDQInfo::handleL1Wakeup(PtrPacket pkt)
{
    if (!pkt->isWrite()) {
        feedbackPref(pkt);
    }

    if (!pkt->isRead()) {
        return;
    }

    LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: L1 data coming and wakeup. " << *pkt;

    ReqData reqD;
    uint64_t cID = addr2LDQcID(pkt->addr);
    missQ.erase(pkt->addr);
    prefSet.erase(pkt->addr);
    reqD.insertCacheData(pkt->data);
    clusters[cID].cData.merge(pkt->addr, reqD);
    for (auto &e : clusters[cID].entryArr) {
        if (!e.ldqHit && !e.l1Hit && e.fsm != LDQ_IDLE && e.memReq.tag == pkt->addr
            && e.fsm != LDQ_RESOLVED && !e.memReq.IsTileLS()) {
            e.fsm = LDQ_WAIT;
            e.memReq.l2MissCycle = pkt->l2MissCycle;
            e.memReq.l2RntCycle = pkt->l2RntCycle;
            e.memReq.memRntCycle = pkt->memRntCycle;
            e.memReq.l2_miss = pkt->l2_miss;
            e.l1Hit = true;
            top->stats->ld_l2_latency += (GetSim()->getCycles() - e.memReq.l1MissCycle);
            top->stats->ld_l2_reqs++;
            if (pkt->l2_miss) {
                top->stats->ld_l2miss_latency += (GetSim()->getCycles() - e.memReq.l1MissCycle);
                top->stats->ld_l2miss_reqs++;
            } else {
                top->stats->ld_l2hit_latency += (GetSim()->getCycles() - e.memReq.l1MissCycle);
                top->stats->ld_l2hit_reqs++;
            }

            LOG_INFO_M(Unit::LSU, Stage::NA) << "Missed load request is waken up. Requst: "<< e.memReq;
        }
    }
}

void LDQInfo::feedbackPref(PtrPacket pkt)
{
    // Feedback to prefetcher
    if (pkt->isPref() && !pkt->demand) {
        // Bad prefetch
        switch (pkt->user_type) {
            case PFTYPE_STRIDE:
                top->stats->pref_bad_stride_count++;
                break;
            case PFTYPE_STREAM:
                top->stats->pref_bad_stream_count++;
                break;
            case PFTYPE_SW_L1D:
            case PFTYPE_SW_L1I:
            case PFTYPE_SW_L2:
                top->stats->pref_bad_sw_inst++;
                break;
            default:
                ASSERT(0);
                break;
        }

        PtrPrefFB fb = std::make_shared<PrefFB>();
        fb->bad  = true;
        fb->type = pkt->user_type;
        fb->addr = pkt->addr;
        feedback_lu_pref_q->Write(fb);
    }
}

void LDQInfo::checkLoadPending()
{
    if (!loadPending) {
        return;
    }

    // Get oldest bid
    ROBID oldestBID = top->core->bctrl->blockROB.getOldestBlockID(stid);
    // Check if waiting is too long or it is oldest
    bool oldestPending = false;
    loadPending = false;
    for (auto &clt : clusters) {
        if (clt.size == 0) {
            continue;
        }

        uint32_t itCnt = 0;
        for (auto &e : clt.entryArr) {
            if (!e.IsIdle() && clt.IsIteratorEnding(itCnt++)) {
                break;
            }

            if (!e.IsWorking() || !e.waitStore)
                continue;
            // Get current block oldest lsid
            ROBIDBus oldestLSID = top->core->peArray[e.memReq.peID]->GetOldestLSID(e.memReq.tid);
            // Block is oldest and only wait for retired store, set the timer for it.
            // And unlock it when the timer is zero.
            if (loadPendingCyc == 0 || LessROBID(e.waitBid, oldestBID)
                || (oldestBID == e.memReq.bid && (!oldestLSID.vld || (oldestLSID.vld && LessEqual(e.memReq.lsID, oldestLSID.id))))) {
                e.waitStore = false;
                MDBBus mBus = getMDBBus(e.memReq);
                delete_lu_mdb_q->push_back(mBus);
                ++top->stats->mdb_fail_count;
                top->stats->mdb_fail_wait_cycle += (GetSim()->getCycles() - e.stallCycle);
                LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: [ Cluster " << e.memReq.cID << ", Entry " << std::dec
                    << e.memReq.eID << "] Load in oldest block is wakeuped. " << e.memReq;

                continue;
            }
            loadPending = true;
        }
    }

    loadPendingCyc = (oldestPending && loadPendingCyc > 0) ? loadPendingCyc - 1 : 300;
}

void LDQInfo::statsMemBound(bool& anyload, bool& l1miss, bool& l2miss)
{
    for (auto &clt : clusters) {
        if (clt.size == 0) {
            continue;
        }

        uint32_t itCnt = 0;
        for (auto &e : clt.entryArr) {
            if (!e.IsIdle() && clt.IsIteratorEnding(itCnt++)) {
                break;
            }

            if (!e.IsWorking()) {
                continue;
            }

            anyload = true;
            if (e.fsm == LDQ_L2_WAIT) {
                uint32_t count = pConfigs->lu_clusters_depth;
                if (top->typeId == LSUType::SCALAR_LSU) {
                    count = pConfigs->scalar_lu_clusters_depth;
                }
                uint64_t ldq2PktIdx = e.memReq.cID * count + e.memReq.eID;
                l1miss = true;
                if (ldqPkts[ldq2PktIdx] && ldqPkts[ldq2PktIdx]->l2_miss) {
                    l2miss = true;
                }
            }
        }
    }
}


void LDQInfo::LDQCheckDeadLockSendFlushReq(IDBus& oldRetire, bool isImmFlush)
{
    ROBID oldestBID = top->core->bctrl->blockROB.getOldestBlockID(stid);
    BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(oldestBID, stid);
    FlushReq flushReq;
    flushReq.vld = true;
    flushReq.iexTyp = oldRetire.iexTyp;
    flushReq.noSplitBlk = false;
    if (cmd && cmd->opcode != Opcode::OP_INVALID) {
        return;
    }
    flushReq.bid = oldRetire.bid;
    flushReq.gid = oldRetire.gid;
    flushReq.rid = oldRetire.rid;
    flushReq.tid = oldRetire.tid;
    flushReq.fetchTPCVld = true;
    flushReq.fetchTPC = oldRetire.tpc;
    flushReq.peID = oldRetire.peID;
    flushReq.lsID = oldRetire.lsID;
    flushReq.fbid = oldRetire.fbid;
    flushReq.fbid_local = oldRetire.fbid_local;
    flushReq.stid = oldRetire.stid;
    SimInst inst;
    if (GetSim()->core->IsVecPe(oldRetire.peID)) {
        std::shared_ptr<VecPE> pe = GetSim()->core->vectorTop->GetPE(oldRetire.coreId);
        inst = pe->prob[oldRetire.tid]->getROBEntry(oldRetire.rid.val).inst;
    } else {
        std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(GetSim()->core->peArray[oldRetire.peID]);
        inst = pe->prob[oldRetire.tid]->getROBEntry(oldRetire.rid.val).inst;
    }
    // if current instruction do not need the local reg, then we need to flush from the one before
    flushReq.tSeq = oldRetire.tSeq;
    if (inst->DstTypeContain(OperandType::OPD_TLINK)) {
        flushReq.tSeq = GetPrevRegSeq(GetSim(), flushReq.tSeq, OperandType::OPD_TLINK, flushReq.peID, flushReq.tid);
    }
    flushReq.uSeq = oldRetire.uSeq;
    if (inst->DstTypeContain(OperandType::OPD_ULINK)) {
        flushReq.uSeq = GetPrevRegSeq(GetSim(), flushReq.uSeq, OperandType::OPD_ULINK, flushReq.peID, flushReq.tid);
    }
    flushReq.predSeq = oldRetire.predSeq;
    if (inst->DstTypeContain(OperandType::OPD_PREDMASK)) {
        flushReq.predSeq = GetPrevRegSeq(GetSim(), flushReq.predSeq, OperandType::OPD_PREDMASK, flushReq.peID, flushReq.tid);
    }
    flushReq.fbid_local = oldRetire.fbid_local;
    top->core->flushUnit->flush_stats->InterBlockMemoryAaccelssConflict++;
    top->core->flushUnit->flush_stats->smtInterBlockMemoryAaccelssConflictArray[flushReq.stid]++;
    flushReq.type = top->typeId == LSUType::SCALAR_LSU ? FlushType::INNER_FLUSH : FlushType::SIMT_INNER_FLUSH;
    flushReq.immediateFlush = isImmFlush;
    flushReq.firstInst = oldRetire.first;
    if (top->typeId == LSUType::SCALAR_LSU) {
        top->lsu_flush_rpt_q->Write(flushReq);
    } else {
        ASSERT(flushReq.type != FlushType::INNER_FLUSH);
        top->GetSim()->core->peArray[flushReq.peID]->ReportBlockFlush(flushReq);
    }
    top->stats->store_load_conflicts++;
    LOG_INFO_M(Unit::LSU, Stage::NA) << "Oldest load is stalled, report inner flush from B" << oldRetire.bid
        << ":T" << oldRetire.rid;
}

void LDQInfo::checkDeadLock()
{
    if (!checkStall()) {
        return;
    }

    if (!rslvQ.full()) {
        return;
    }

    ROBID oldestBID = top->core->bctrl->blockROB.getOldestBlockID(stid);
    BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(oldestBID, stid);

    IDBus oldRetire;
    for (uint32_t pe = 0; pe < top->core->peArray.size(); ++pe) {
        oldRetire = top->core->peArray[pe]->GetRetireID();
        if (oldRetire.vld && oldRetire.bid == oldestBID) {
            break;
        }
        oldRetire.vld = false;
    }

    // The oldest load is disptached(pipeID is initialized) and is the oldest instruction
    if (!oldRetire.vld || oldRetire.bid != oldestBID || !oldRetire.isLoadStore || !oldRetire.isLoad
        || (oldRetire.pipeID >= iexLsuLdaArray.size())) {
        return;
    }
    MemReqBus youngestLoad;
    youngestLoad.vld = false;
    // Lookup ldq
    for (ClusterInfo &clt : clusters) {
        for (LUEntryInfo &e : clt.entryArr) {
            if (e.fsm == LDQ_IDLE || e.memReq.IsTileLS()) {
                continue;
            }
            if (LessEqual(e.memReq.bid, e.memReq.lsID, oldRetire.bid, oldRetire.lsID)) {
                return;
            }
            if (!youngestLoad.vld || LessEqual(youngestLoad.bid, youngestLoad.lsID, e.memReq.bid, e.memReq.lsID)) {
                youngestLoad = e.memReq;
            }
        }
    }
    for (MemReqBus &req : rslvQ.entryArr) {
        if (!req.vld || req.IsTileLS()) {
            continue;
        }
        if (LessEqual(req.bid, req.lsID, oldRetire.bid, oldRetire.lsID)) {
            return;
        }
        if (!youngestLoad.vld || LessEqual(youngestLoad.bid, youngestLoad.lsID, req.bid, req.lsID)) {
            youngestLoad = req;
        }
    }
    for (auto &simQ : iexLsuLdaArray) {
        auto &readQ = simQ->GetRawReadData();
        for (MemReqBus &req : readQ) {
            if (LessEqual(req.bid, req.lsID, oldRetire.bid, oldRetire.lsID)) {
                return;
            }
            if (!youngestLoad.vld || LessEqual(youngestLoad.bid, youngestLoad.lsID, req.bid, req.lsID)) {
                youngestLoad = req;
            }
        }
        auto &wirteQ = simQ->GetRawWriteData();
        for (MemReqBus &req : wirteQ) {
            if (LessEqual(req.bid, req.lsID, oldRetire.bid, oldRetire.lsID)) {
                return;
            }
            if (!youngestLoad.vld || LessEqual(youngestLoad.bid, youngestLoad.lsID, req.bid, req.lsID)) {
                youngestLoad = req;
            }
        }
    }
    if (!youngestLoad.vld) {
        return;
    }

    if (top->typeId == LSUType::VECTOR_LSU && checkStall()) {
        for (uint32_t pe = 0; pe < top->core->peArray.size(); ++pe) {
            IDBus oldPeRetire = top->core->peArray[pe]->GetRetireID();
            if (!oldPeRetire.vld || oldPeRetire.bid == oldestBID || !oldPeRetire.isLoadStore || !oldPeRetire.isLoad ||
             oldRetire.pipeID >= iexLsuLdaArray.size()) {
                continue;
            }

            LDQCheckDeadLockSendFlushReq(oldPeRetire, true);
        }
        return;
    }
    LDQCheckDeadLockSendFlushReq(oldRetire, false);
}

uint32_t LDQInfo::retire(IDBus &commitBus)
{
    uint32_t reitedCnt = 0;
    for (auto &clt : clusters) {
        for (auto &e : clt.entryArr) {
            if (e.fsm != LDQ_RESOLVED ||
                (top->typeId == LSUType::SCALAR_LSU &&
                !LessROBID(e.memReq.bid, e.memReq.lsID, commitBus.bid, commitBus.lsID)) ||
                (top->typeId == LSUType::VECTOR_LSU &&
                !LessROBID(e.memReq.bid, e.memReq.gid, e.memReq.lsID, commitBus.bid, commitBus.gid, commitBus.lsID))) {
                continue;
            }

            LOG_INFO_M(Unit::LSU, Stage::NA) << "Load request is retired in LDQ. " << e.memReq;

            e.Reset();
            ASSERT(clusters[e.cID].size > 0);
            --clusters[e.cID].size;
            ++reitedCnt;
        }
    }
    return reitedCnt;
}

PLUEntryInfo LDQInfo::findOldestLoad(void)
{
    PLUEntryInfo oldestE = NULL;
    for (auto &clt: clusters) {
        if (clt.size == 0)
            continue;

        for (auto &e : clt.entryArr) {
            if (e.fsm == LDQ_IDLE)
                continue;

            if (!oldestE || LessEqual(e.memReq.bid, e.memReq.gid, e.memReq.lsID,
                oldestE->memReq.gid, oldestE->memReq.bid, oldestE->memReq.lsID)) {
                oldestE = &e;
            }
        }
    }

    return oldestE;
}

void LDQInfo::LUStatsTick(void)
{
    if (checkStall()) {
        top->stats->ldq_full_cycles++;
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: load queue is full! Stall cycle: " << std::dec
            << top->stats->ldq_full_cycles;
    }

    uint32_t occupied_ldq_entry = 0;
    for (auto &clt: clusters) {
        for (auto &e : clt.entryArr) {
            if (e.fsm == LDQ_WAIT) {
                if (e.waitStore) {
                    top->stats->load_wait_store_cycles++;
                } else {
                    top->stats->load_wait_pick_cycles++;
                }
            }
        }

        occupied_ldq_entry += clt.size;
    }

    if (LoggerManager::GetManager().level <= LoggerLevel::INFO) {
        PLUEntryInfo oldestE = findOldestLoad();
        if (oldestE) {
            MemReqBus &oldest = oldestE->memReq;
            LOG_INFO_M(Unit::LSU, Stage::NA) << std::dec << "[LSU]: " << occupied_ldq_entry
                << " Entries Occupied in Load Queue." << " Oldest is [ Cluster " << oldest.cID << ", Entry "
                << dec << oldest.eID << "] " << oldest << " dataHit " << oldestE->dataHit() << " wait_store "
                << oldestE->waitStore << " fsm " << oldestE->fsm;
        }
    }

    if (occupied_ldq_entry != 0) {
        top->stats->ldq_total_occupied += occupied_ldq_entry;
        top->stats->ldq_occupied_count++;
        uint32_t count = pConfigs->lu_clusters_depth;
        if (top->typeId == LSUType::SCALAR_LSU) {
            count = pConfigs->scalar_lu_clusters_depth;
        }
        uint32_t ldq_depth = count * pConfigs->lu_clusters_count;
        if (occupied_ldq_entry < (ldq_depth * 0.1)) {
            top->stats->ldq_occupied_10++;
        } else if (occupied_ldq_entry < (ldq_depth * 0.25)) {
            top->stats->ldq_occupied_25++;
        } else if (occupied_ldq_entry < (ldq_depth * 0.5)) {
            top->stats->ldq_occupied_50++;
        } else if (occupied_ldq_entry < (ldq_depth * 0.75)) {
            top->stats->ldq_occupied_75++;
        } else if (occupied_ldq_entry < (ldq_depth * 0.9)) {
            top->stats->ldq_occupied_90++;
        } else {
            top->stats->ldq_occupied_100++;
        }
    }
}

bool LDQInfo::CheckLoadQEmpty()
{
    for (auto &clt : clusters) {
        for (auto &e : clt.entryArr) {
            if (e.fsm != LDQ_IDLE && e.fsm != LDQ_RESOLVED) {
                return false;
            }
        }
    }

    return true;
}

} // namespace JCore
