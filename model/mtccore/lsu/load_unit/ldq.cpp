#include <cmath>
#include "mtccore/lsu/load_unit/MtcLDQInfo.h"
#include "mtccore/lsu/mtc_lsu_stats.h"
#include "mtccore/lsu/MtcLoadStoreUnit.h"

namespace JCore {


/* MtcResolveQ init */
void MtcResolveQ::Build(uint32_t depth, uint32_t rslvNum)
{
    capcity = depth;
    this->rslvNum = rslvNum;
}

void MtcResolveQ::Reset(void)
{
    entryArr.clear();
}

void MtcResolveQ::insert(MemReqBus &bus)
{
    entryArr.emplace_back(bus);
}

bool MtcResolveQ::empty(void)
{
    return entryArr.empty();
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

bool MtcResolveQ::Full(MemReqBus &bus)
{
    uint32_t startIdx = top->core->GetMtcPEIndex();
    uint32_t size = 1;
    ROBID oldestId;
    IDBus commitBus = GetSimtOldestInfo(top->core->peArray, startIdx, size, oldestId);
    if (LessEqualROBID(bus.bid, bus.rid, commitBus.bid, commitBus.rid) &&
        commitBus.isTld && (bus.opcode == Opcode::OP_TLD)) {
        return entryArr.size() >= capcity;
    } else {
        return (entryArr.size() + 64) >= capcity;
    }
}

bool MtcResolveQ::Full(void)
{
    return entryArr.size() >= capcity;
}

uint32_t MtcResolveQ::retired(IDBus &commitBus, LSUType typeId)
{
    uint32_t retireCnt = 0;
    for (auto it = entryArr.begin(); it != entryArr.end();) {
        if ((typeId == LSUType::SCALAR_LSU && LessROBID(it->bid, it->lsID, commitBus.bid, commitBus.lsID)) ||
            (typeId == LSUType::MEMORY_LSU && (LessROBID(it->bid, it->gid, it->lsID,
                commitBus.bid, commitBus.gid, commitBus.lsID) || (LessEqualROBID(it->bid, it->lsID,
                commitBus.bid, commitBus.lsID) && commitBus.isTld)))) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << "retired from resolveQ B"<<it->bid.val << ":G" << it->gid.val <<":R" <<
                it->rid.val << ":SUBRID" << it->subrid.val;

            it = entryArr.erase(it);
            retireCnt++;
            continue;
        }
        ++it;
    }
    return retireCnt;
}

MemReqBus MtcResolveQ::detect(MemReqBus &bus)
{
    std::vector<MemReqBus>::iterator pOldest = entryArr.end();
    for (auto it = entryArr.begin(); it != entryArr.end(); ++it) {
        if (!(AddrOverlap(bus.addr, bus.size, it->addr, it->size) && LessEqual(bus.bid, bus.lsID, it->bid, it->lsID)))
            continue;

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

void MtcResolveQ::flush(FlushBus &flushReq)
{
    for (auto it = entryArr.begin(); it != entryArr.end();) {
        if (flushReq.match(*it)) {
            it = entryArr.erase(it);
            continue;
        }
        ++it;
    }
}

/* MtcPickEntry init */
void MtcPickEntry::Reset(void)
{
    for (auto &e : entryArr) {
        e = NULL;
    }
}

void MtcPickEntry::Build(uint32_t depth)
{
    entryArr = std::vector<PMTCLUEntryInfo>(depth);
}

void MtcCrossBuffer::Reset(void)
{
    crossQ.clear();
    completed.clear();
}

void MtcCrossBuffer::flush(FlushBus &flushReq)
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
void MtcLDQInfo::Reset(void)
{
    for (MtcClusterInfo &c : clusters) {
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

void MtcLDQInfo::Build(void)
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
    rslvQ.top = top;
    ldqPkts = std::vector<PtrPacket>(pConfigs->lu_clusters_count * count);
}

SimSys *MtcLDQInfo::GetSim(void)
{
    return sim;
}

void MtcLDQInfo::Work(void)
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

void MtcLDQInfo::Xfer(void)
{
    if (!top->L1Allow()) {
        ++top->stats->l2_stall_l1d_cycles;
    }
}

/*
E1:
    check state
    1. check if wait for store
    2. check if hit in MDB
    3. check if hit in L1
*/
void MtcLDQInfo::queryByState(void)
{
    // Capcity is equal to iex
    for (auto &pipeQ : top->iex_lsu_lda_array) {
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
void MtcLDQInfo::insert(void)
{
    // Capcity is equal to iex
    for (auto &pipeQ : top->iex_lsu_lda_array) {
        while (!pipeQ->Empty()) {
            MemReqBus bus = pipeQ->Read();
            if (!bus.isCrossCacheLine) {
                handleInsert(bus);
                continue;
            }

            // Cross Cacheline
            MemReqBus bus1;
            MemReqBus bus2;

            MTCGetCrossReq(bus, bus1, bus2);
            handleInsert(bus1);
            handleInsert(bus2);
        }
    }

    for (auto &tTransMemLdReqQ : top->tTransMemLdReqArray) {
        while (!tTransMemLdReqQ->Empty()) {
            TTransMemLdReq req = tTransMemLdReqQ->Read();
            MemReqBus bus;
            bus.toMtcLsu = true;
            bus.vld = true;
            bus.addr = req.GetAddr();
            bus.size = req.GetSize();
            bus.tTransId = req.GetReqId();
            bus.tTransReq = true;
            handleInsert(bus);
        }
    }
}

/*
    E2: PIPELINE
    update state:
    1. if wait for store
    2. if hit in MDB
    3. if hit in L1
*/
void MtcLDQInfo::mergeStateInfo(void)
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
void MtcLDQInfo::pick(void)
{
    // Pick L1-lookup entries
    for (auto &pickE : pL1LookupEntries.entryArr) {
        if (pickE != nullptr && pickE->fsm == MTC_LDQ_REPICK) {
            pickE->fsm = MTC_LDQ_WAIT;
        }
        pickE = NULL;
    }
    if (top->L1Allow()) {
        for (uint32_t cID = 0; cID < clusters.size(); ++cID) {
            pickL1(cID);
        }
    }

    // Pick L2-lookup entries
    for (auto &pickE : pL2LookupEntries.entryArr) {
        pickE = NULL;
    }
    if (top->L2Allow()) {
        for (uint32_t cID = 0; cID < clusters.size(); ++cID) {
            pickL2(cID);
        }
    }
}

/*
    E2:
    lookup (L1/L2)
*/
void MtcLDQInfo::lookup(void)
{
    // L1-lookup
    for (auto &pickE : this->pL1LookupEntries.entryArr) {
        if (!pickE)
            continue;
        handleL1Lookup(pickE->memReq);
    }

    // L2-lookup
    for (auto &pickE : this->pL2LookupEntries.entryArr) {
        if (!pickE)
            continue;
        handleL2Lookup(pickE->memReq);
        pickE->fsm = MTC_LDQ_L2_WAIT;
    }
}

/*
    E3:
    merge data
*/
void MtcLDQInfo::receiveData(void)
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
    } else if (top->typeId == LSUType::MEMORY_LSU) {
        iexMaxPipe = top->core->iex[MEM_IEX]->configs.iexLdaIqCount;
    }

    // Older return first
    if (pL1LookupEntries.entryArr.size() > iexMaxPipe) {
        sort(pL1LookupEntries.entryArr.begin(), pL1LookupEntries.entryArr.end(),
            [](PMTCLUEntryInfo a, PMTCLUEntryInfo b) {
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
        if (!(pickE && pickE->fsm == MTC_LDQ_REPICK)) { continue; }
        if (pickE->memReq.opcode == Opcode::OP_TLD && GetSim()->core->configs.mtc_tls_enable &&
            GetSim()->core->configs.mtc_tload_retire_in_order) { continue; }

        if (!simtModeCheck) {
            simtModeCheck = true;
            if (pickE->memReq.iexTyp == MEM_IEX) {
                iexMaxPipe = GetSim()->core->iex[MEM_IEX]->iexLdaIqCount;
            } else {
                ASSERT(0 && "not support");
            }
        }

        // Multi-load-request may merge 1 for simt, so these load-request send 1 iex pipe
        uint32_t lastPipeID = iexIdx;
        if (!first && !(pickE->memReq.bid == lastBid && pickE->memReq.rid == lastRid)) {
            ++lastPipeID;
        }

        // Data return, but not enough
        if (!checkDataPosionValid(pickE->memReq.addr, pickE->memReq.size, pickE->memReq.mtc_reqData)) {
            pickE->fsm = MTC_LDQ_L1_DC_MISS;
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
            pickE->fsm = MTC_LDQ_WAIT;
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
            if (GetSim()->core->configs.mtc_tls_enable && pickE->memReq.opcode == Opcode::OP_TLD) { break; }
        }
    }
    if (!(GetSim()->core->configs.mtc_tls_enable && GetSim()->core->configs.mtc_tload_retire_in_order)) { return; }

    uint32_t pipeIndex = 0;
    iexMaxPipe = 1;
    while (scLdqOrder->size() > 0) {
        bool found = false;
        for (auto &pickE : this->pL1LookupEntries.entryArr) {
            if (!(pickE && pickE->fsm == MTC_LDQ_REPICK)) { continue; }
            if (!(pickE->memReq.opcode == Opcode::OP_TLD && GetSim()->core->configs.mtc_tls_enable)) { continue; }
            if ((pickE->memReq.bid.val != scLdqOrder->front().bid)) { continue; }
            if ((pickE->memReq.subrid.val != scLdqOrder->front().rid)) { continue; }
            if ((pickE->memReq.gid.val != scLdqOrder->front().gid)) { continue; }
            ASSERT(pickE->memReq.iexTyp == MEM_IEX);
            // Data return, but not enough
            if (!checkDataPosionValid(pickE->memReq.addr, pickE->memReq.size, pickE->memReq.mtc_reqData)) {
                pickE->fsm = MTC_LDQ_L1_DC_MISS;
                if (!pickE->memReq.l1_miss) {
                    pickE->memReq.l1_miss = true;
                    pickE->memReq.l1MissCycle = GetSim()->getCycles();
                }
                ++top->stats->load_miss_count;
                pickE->rewait();
                continue;
            }

            if (!((pickE->ldqRnt || pickE->l1Rnt) && pickE->scbRnt && pickE->stqRnt)) {
                // Wait for re-pick
                pickE->fsm = MTC_LDQ_WAIT;
                pickE->storeBypass = true;
                pickE->rewait();
                continue;
            }

            // Return
            if (returnData(pickE->memReq, pipeIndex)) {
                lsuExecEngine(pickE->memReq);
                first = false;
                found = true;
                break;
            }
        }
        if (found) {
            pipeIndex++;
            scLdqOrder->pop_front();
        } else {
            break;
        }
        if (pipeIndex >= iexMaxPipe) { break; }
    }
}

void MtcLDQInfo::lsuExecEngine(MemReqBus &bus)
{
    MemReqBus stBus = bus;
    const uint32_t uinT32BitsMask = 0xffffffff;
    uint64_t dataL = stBus.data;
    uint64_t dataR = stBus.src1->data;
    uint64_t dataOut = 0;
    const uint32_t sizeVal = 4;
    if (!bus.src1->dataVld) {ASSERT(0);}
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
void MtcLDQInfo::checkCancel(void)
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
                if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                    LOG_INFO_M(Unit::MLSU, Stage::NA) << ":[ Cluster " << e.memReq.cID << ", Entry " << dec <<
                        e.memReq.eID << "] Delayed launch in LDQ. Cancel " << e.memReq;
                }
                if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                    LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                     e.memReq, "MLSU: [ Cluster %u, Entry %u] Delayed launch in LDQ. Cancel ",
                                     e.memReq.cID, e.memReq.eID);
                }

                e.delayCnt = 0;
                handleCancel(e.memReq);
                // Statistic for PMU
                ++top->stats->ld_cancel;
                if (e.fsm == MTC_LDQ_L1_DC_MISS || e.fsm == MTC_LDQ_L2_WAIT) {
                    ++top->stats->ld_not_hit_cancel;
                    continue;
                }

                if (e.fsm == MTC_LDQ_WAIT) {
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

void MtcLDQInfo::prefetch(void)
{
    // Prefecher
    for (uint32_t i = 0; i < pConfigs->pref_width && !pref_l1_lu_q->empty() && top->L2Allow(); ++i) {
        MemReqBus bus = pref_l1_lu_q->front();
        pref_l1_lu_q->pop_front();
        if (bus.l1_miss) {
            handleL2Lookup(bus);
        }
    }
}

void MtcLDQInfo::conflictDetect(void)
{
    while (!detect_su_lu_q->empty()) {
        MemReqBus bus = detect_su_lu_q->front();
        detect_su_lu_q->pop_front();
        handleDetect(bus);
    }
}

static IDBus GetScalarOldestInfo(std::vector<std::shared_ptr<PEBase>> &peArray, uint32_t start,
    uint32_t size, ROBID &oldestId)
{
    IDBus oldCmt;
    uint32_t end = start + size;
    for (uint32_t pe = start; pe < end; ++pe) {
        IDBus cmtBus = peArray[pe]->GetRetireID(0);
        if (!cmtBus.vld || cmtBus.bid != oldestId) {
            continue;
        }
        oldCmt = (!oldCmt.vld || LessEqual(cmtBus.bid, cmtBus.lsID, oldCmt.bid, oldCmt.lsID)) ? cmtBus : oldCmt;
    }
    return oldCmt;
}

void MtcLDQInfo::CheckMovRslvQ()
{
    // Check insert to resolve queue
    uint32_t cnt = 0;
    for (auto &clt : clusters) {
        for (auto &e : clt.entryArr) {
            if (e.fsm != MTC_LDQ_RESOLVED) { continue; }
            if (!rslvQ.Full(e.memReq)) {
                if (!e.memReq.isCrossCacheLine) {
                    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << e.memReq.cID << ", Entry " << dec <<
                            e.memReq.eID << "] resolve from in LDQ.  " << e.memReq;
                    }
                    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX,
                                         LogLevel::CRITICAL, e.memReq,
                                         "MLSU: [ Cluster %u, Entry %u] resolve from in LDQ.  ",
                                         e.memReq.cID, e.memReq.eID);
                    }
                    rslvQ.insert(e.memReq);
                    ++cnt;
                }
                if (e.memReq.opcode == Opcode::OP_TLD) {
                    CommitInfo info(e.memReq.bid.GetVal(), e.memReq.gid.GetVal(), e.memReq.subrid.GetVal());
                    top->sc.pe_scalper_commit_q->push_back(info);
                    LOG_INFO_M(Unit::MLSU, Stage::NA) <<" send commit info to scalper, mem: " << e.memReq;
                }
                e.Reset();
                ASSERT(clusters[e.cID].size > 0);
                --clusters[e.cID].size;
                if (cnt == rslvQ.rslvNum || rslvQ.Full(e.memReq)) { break; }
            }
        }
    }
}

bool MtcLDQInfo::checkFlushStall(ROBID &oldestBID)
{
    if (top->core->bctrl->blockROB.needFlush(oldestBID, 0)) {
        if (ldqCommitStallCyc != OLD_COMMIT_WAIT_CYC || oldestBID != lastId.bid) {
            lastId.bid = oldestBID;
            ldqCommitStallCyc++;
            return true;
        }
    }
    ldqCommitStallCyc = 0;
    return false;
}

bool MtcLDQInfo::LDQRetired(IDBus &oldCmt, ROBID &oldestBID, ROBID &oldestGID)
{
    if (!oldCmt.vld || top->typeId == LSUType::SCALAR_LSU) {
        oldCmt.bid = oldestBID;
    }
    if (!oldCmt.vld && ((top->typeId == LSUType::SCALAR_LSU && lastId.bid != oldestBID)
        || (top->typeId == LSUType::MEMORY_LSU && LessEqual(lastId.bid, oldestBID)))) {
        oldCmt.vld = true;
        oldCmt.lsID.val = 0;
        oldCmt.lsID.wrap = false;
    }

    if (oldCmt.vld) {
        uint32_t retirecnt = rslvQ.retired(oldCmt, top->typeId);
        retirecnt += retire(oldCmt);
        if (retirecnt > 0) {
            if (top->typeId == LSUType::SCALAR_LSU) {
                top->core->iex[SCALAR_IEX]->iq.windowSlides(retirecnt, true);
            } else if (top->typeId == LSUType::MEMORY_LSU) {
                BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(oldestBID, 0);
                if (IsBlockTypeParallel(cmd->blockType)) {
                    LOG_INFO_M(Unit::SCALPER, Stage::PF) << "Because load request retired. Reset the idle cycle.";
                    GetSim()->ResetWaitCycle();
                }
                // BIQType::STORE_IQ 没有对应的块类型，仅用于标量ld/st? 或者可删除
                // top->core->bctrl->blockIssueQueueUnit.WindowSlides(retirecnt, BIQType::LOAD_IQ, true);
                top->core->iex[MEM_IEX]->iq.windowSlides(retirecnt, true);
            } else {
                ASSERT(0 && "not support");
            }
        }
        return true;
    }
    return false;
}

void MtcLDQInfo::checkCommit(void)
{
    CheckMovRslvQ();
    // Check if remove from resolve queue
    // Return when need to flush or not block commit
    ROBID oldestBID = top->core->bctrl->blockROB.getOldestBlockID(0);
    ROBID oldestGid = top->core->mtcCores[0]->GetOldestGid();
    if (checkFlushStall(oldestBID)) {
        return;
    }

    IDBus oldCmt;
    if (top->typeId == LSUType::SCALAR_LSU) {
        uint32_t startIdx = 0;
        uint32_t size = top->core->configs.stdPeCount;
        oldCmt = GetScalarOldestInfo(top->core->peArray, startIdx, size, oldestBID);
    } else if (top->typeId == LSUType::VECTOR_LSU) {
        uint32_t startIdx = top->core->configs.stdPeCount;
        uint32_t size = top->core->configs.simtPeCount;
        oldCmt = GetSimtOldestInfo(top->core->peArray, startIdx, size, oldestBID);
    } else if (top->typeId == LSUType::MEMORY_LSU) {
        uint32_t startIdx = top->core->configs.stdPeCount + top->core->configs.simtPeCount;
        uint32_t size = top->core->configs.memPeCount;
        oldCmt = GetSimtOldestInfo(top->core->peArray, startIdx, size, oldestBID);
    } else {
        ASSERT(0 && "not support");
    }
    if (LDQRetired(oldCmt, oldestBID, oldestBID)) {
        lastId.bid = oldestBID;
        lastId.gid = oldestGid;
    }
}

uint64_t MtcLDQInfo::addr2LDQcID(uint64_t addr)
{
    ASSERT(pConfigs->lu_clusters_count <= pow(2, pConfigs->lu_cluster_disp_bit_arr.size()));
    return Addr2cID(addr, pConfigs->lu_cluster_disp_bit_arr);
}

void MtcLDQInfo::handleInsert(MemReqBus &bus)
{
    uint64_t cID = addr2LDQcID(bus.tag);
    // insert to LDQ
    for (MTCLUEntryInfo &e : clusters[cID].entryArr) {
        if (e.fsm == MTC_LDQ_IDLE) {
            if (checkLdqHit(e.cID, bus)) {
                bus.ldq_miss = false;
                e.ldqHit = true;
            }
            e.insert(bus);
            top->CountAddr(bus.addr);
            ++clusters[cID].size;

            // prefetch for hardware
            if (!bus.prefetch) {
                lu_pref_q->Write(bus);
            }

            if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || this->pConfigs->verbose) {
                LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << cID << ", Entry " << dec << e.memReq.eID <<
                    "] Insert into LDQ. " << bus;
            }
            if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                 bus, "MLSU: [ Cluster %u, Entry %u] Insert into LDQ. ", cID, e.memReq.eID);
            }
            return;
        }
    }

    cout << "Not insert but" << bus << endl;
    ASSERT(0 && "not insert");
}

static MDBBus getMDBBus(MemReqBus &bus)
{
    MDBBus mBus;
    mBus.vld = true;
    mBus.ldInfo = bus;
    mBus.conf = 1;
    mBus.stid = bus.stid;

    return mBus;
}

void MtcLDQInfo::handleStateQuery(MemReqBus &bus)
{
    bus.lsuRecvCycle = GetSim()->getCycles();
    ++top->stats->ld_input_reqs;

    MDBBus mBus = getMDBBus(bus);
    lookup_lu_mdb_q->push_back(mBus);
    if (!bus.tTransReq) {
        wait_lu_su_q->push_back(bus);
    }

    if (!bus.isCrossCacheLine) {
        if (bus.prefetch) {
            top->SendL1(pref_lu_l1_array, bus);
            return;
        }

        top->SendL1(tag_lu_l1_array, bus);
        top->SendL1(tag_lu_scb_array, bus);
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << ": send request of querying " << bus <<
                 " cycle: " << dec << GetSim()->getCycles();
        }
        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             bus, "MLSU: send request of querying ");
        }
        return;
    }

    MemReqBus bus1;
    MemReqBus bus2;

    MTCGetCrossReq(bus, bus1, bus2);

    top->SendL1(tag_lu_l1_array, bus1);
    top->SendL1(tag_lu_l1_array, bus2);
    top->SendL1(tag_lu_scb_array, bus1);
    top->SendL1(tag_lu_scb_array, bus2);
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": send request of querying [cross1] " << bus1 << " [cross2] " << bus2;
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus1, "MLSU: send request of querying [cross1] ");
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus2, "MLSU: send request of querying [cross2] ");
    }
}

void MtcLDQInfo::LDQWakup(void)
{
    auto localPrint = [](bool cond, string outs) {
        if (cond) {
            cout << outs << endl;
        }
    };

    while (!wakeup_l1_lu_q->Empty()) {
        PtrPacket pkt = wakeup_l1_lu_q->Front();
        ASSERT(pkt->isWriteBack() || pkt->isRead());
        // Snoop to L2
        if (pkt->isWriteBack()) {
            if (lookup_lu_l2_q->toBeOverflow(1)) {
                localPrint(((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose),
                    "[MTC LSU]: L2 is stall when snooping.");
                break;
            }
            top->stats->dcache_l2_pkt_count++;
            sendMemReq(pkt);
            if (pkt->isResp())
                ++top->stats->dcache_l2_resppkt_count;
            else
                ++top->stats->dcache_l2_writepkt_count;
        } else if ((pkt->isWrite() || pkt->isUpgrade())) {
                localPrint(((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose),
                    "[MLSU]: L2 is stall when upgrading.");
            if (!pkt->isResp()) {
                sendMemReq(pkt);
            }

            if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                LOG_INFO_M(Unit::MLSU, Stage::NA) << ": send packet " << *pkt;
            }
            if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                 *pkt, "MLSU: send packet ");
            }
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

bool MtcLDQInfo::checkLdqHit(uint32_t cID, MemReqBus &bus)
{
    return clusters[cID].checkHit(bus.tag);
}

void MtcLDQInfo::updateHitInfo(MemReqBus &bus)
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
                e.fsm = MTC_LDQ_L1_DC_MISS;
            }

            if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << cID << ", Entry " << dec << e.memReq.eID <<
                    "] L1 Hit: " << e.l1Hit << ". Request " << bus;
            }
            if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                    bus, "MLSU:[ Cluster %u, Entry %u] L1 Hit: %d. Request", cID, e.memReq.eID, e.l1Hit);
            }
            return;
        }
    }
    cout << "Not found bus " << bus << endl;
    ASSERT(0 && "not found");
}

bool MtcLDQInfo::checkDataPosionValid(uint64_t addr, uint64_t size, ReqData256 cData)
{
    uint32_t off = addr & 0xff;
    for (uint32_t i = off; i < off + size; ++i) {
        if (!cData.positionVld[i]) {
            return false;
        }
    }
    return true;
}

void MtcLDQInfo::updateSCBHitInfo(MemReqBus &bus)
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
            e.memReq.mtc_reqData.mergePosition(bus.mtc_reqData);
            e.storeBypass = bus.data_vld && checkDataPosionValid(e.memReq.addr, e.memReq.size, e.memReq.mtc_reqData);
            if (e.storeBypass)
                e.fsm = MTC_LDQ_WAIT;
            return;
        }
    }

    cout << "Not found bus " << bus << endl;
    ASSERT(0 && "not found");
}

void MtcLDQInfo::updateMDBInfo(MDBBus &bus)
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

            if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << cID << ", Entry " << dec << e.memReq.eID <<"] ";
                cout << "Wait for store by MDB. " << e.memReq;
                cout << ",wait-store PC " << hex << bus.stInfo.tpc << dec << "-ID" << bus.stInfo.bid << endl;
            }
            if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                 e.memReq, "MLSU:[Cluster %u,Entry %u] Wait for store by MDB. wait-store PC %x-ID%u",
                                 cID, e.memReq.eID, bus.stInfo.tpc, bus.stInfo.bid);
            }
        }
    }
}

void MtcLDQInfo::updateWaitInfo(MemReqBus &bus)
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
                if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                    LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << cID << ", Entry " << dec <<
                        e.memReq.eID <<"] "<< "load request: " << bus << ",wait-store PC 0x" << hex <<
                        bus.wait_tpc << dec << ",B" << bus.wait_bid << ":T" << bus.wait_rid;
                }
                if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                    LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                     bus, "MLSU: [ Cluster %u, Entry %u] wait-store PC 0x%x,B%u:T%u, load request: ",
                                     cID, e.memReq.eID, bus.wait_tpc, bus.wait_bid, bus.wait_rid);
                }
            }
            if (bus.data_vld) {
                e.memReq.mtc_reqData.mergePosition(bus.mtc_reqData);
                e.storeBypass = checkDataPosionValid(e.memReq.addr, e.memReq.size, e.memReq.mtc_reqData);
            }
            if (e.storeBypass) {
                e.fsm = MTC_LDQ_WAIT;
            } else if (e.fsm == MTC_LDQ_L1_DC_MISS) {
                e.memReq.l1_miss = true;
                e.memReq.l1MissCycle = GetSim()->getCycles();
                ++top->stats->load_miss_count;
            }
        }
    }
}

bool checkPicked(MtcPickEntry &pEntries, MTCLUEntryInfo &e)
{
    for (auto &pk : pEntries.entryArr) {
        if (!pk)
            return false;
        if (pk->memReq.bid == e.memReq.bid && pk->memReq.lsID == e.memReq.lsID && pk->memReq.addr == e.memReq.addr)
            return true;
    }

    return false;
}

void MtcLDQInfo::loadRepick(PMTCLUEntryInfo &e, uint32_t &idx)
{
    // Pick
    e->fsm = MTC_LDQ_REPICK;
    pL1LookupEntries.entryArr[idx] = e;
    ++top->stats->ldq_pick_reqs;
    e->memReq.ldqPickCycle = GetSim()->getCycles();
    uint64_t stime = e->memReq.l1_miss ?
                        e->memReq.l2RntCycle : e->memReq.lsuRecvCycle;
    top->stats->ldq_pick_latency += (e->memReq.ldqPickCycle - stime);
}

bool MtcLDQInfo::PickPriority(PMTCLUEntryInfo &pickE, MTCLUEntryInfo &e, LSUType type)
{
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
    if (type == LSUType::MEMORY_LSU) {
        if ((e.memReq.opcode == Opcode::OP_TLD) && (pickE->memReq.opcode == Opcode::OP_TLD)
            && GetSim()->core->configs.mtc_tload_retire_in_order) {
            if (LessEqual(e.memReq.bid, e.memReq.rid, e.memReq.subrid,
                pickE->memReq.bid, pickE->memReq.rid, pickE->memReq.subrid)
                && (pickE->dataHit() == e.dataHit())) {
                return true;
            }
        } else if (LessEqual(e.memReq.bid, e.memReq.gid, e.memReq.lsID,
            pickE->memReq.bid, pickE->memReq.gid, pickE->memReq.lsID)
            && (pickE->dataHit() == e.dataHit())) {
            return true;
        }
    }
    return false;
}

void MtcLDQInfo::pickL1(uint32_t cID)
{
    if (clusters[cID].size == 0)
        return;

    uint32_t pickNum = 0;
    for (uint32_t cnt = 0; cnt < pConfigs->l1_pick_count && pickNum < clusters[cID].size; ++cnt) {
        // Pick the Oldest request
        PMTCLUEntryInfo oldestE = NULL;
        uint32_t itCnt = 0;
        for (auto &e : clusters[cID].entryArr) {
            if (!e.IsIdle() && clusters[cID].IsIteratorEnding(itCnt++)) {
                break;
            }
            if (e.fsm != MTC_LDQ_WAIT || e.waitStore || !e.dataHit())
                continue;

            if (PickPriority(oldestE, e, top->typeId))
                oldestE = &e;
        }

        if (!oldestE)
            break;
        ++pickNum;
        uint32_t pickIdx = cID * pConfigs->l1_pick_count + cnt;
        loadRepick(oldestE, pickIdx);

        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << dec << "[MLSU]: [ Cluster " << cID << ", Entry "
                << oldestE->memReq.eID << "] Pick L1 request. "
                << oldestE->memReq << " ldq/l1/scb hit: "<< oldestE->dataHit()
                << " cycle: " << dec << GetSim()->getCycles();
        }
        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             oldestE->memReq, "MLSU: [ Cluster %u, Entry %u] Pick L1 request. ldq/l1/scb hit: %d. ",
                             cID, oldestE->memReq.eID, oldestE->dataHit());
        }
    }
}

void MtcLDQInfo::pickL2(uint32_t cID)
{
    if (clusters[cID].size == 0)
        return;

    for (uint32_t cnt = 0; cnt < pConfigs->l2_pick_count; ++cnt) {
        // Pick the Oldest request
        PMTCLUEntryInfo oldestE = NULL;
        uint32_t itCnt = 0;
        for (auto &e : clusters[cID].entryArr) {
            if (!e.IsIdle() && clusters[cID].IsIteratorEnding(itCnt++)) {
                break;
            }

            if (e.fsm != MTC_LDQ_L1_DC_MISS || missQ.count(e.memReq.tag) != 0)
                continue;

            if (!oldestE || (((top->typeId == LSUType::SCALAR_LSU) &&
                LessEqual(oldestE->memReq.bid, oldestE->memReq.lsID, e.memReq.bid, e.memReq.lsID)) ||
                ((top->typeId == LSUType::MEMORY_LSU) &&
                LessEqual(oldestE->memReq.bid, oldestE->memReq.gid, oldestE->memReq.lsID,
                    e.memReq.bid, e.memReq.gid, e.memReq.lsID)))) {
                oldestE = &e;
            }
        }

        if (!oldestE)
            break;

        // Pick
        uint32_t pickIdx = cID * pConfigs->l2_pick_count + cnt;
        oldestE->fsm = MTC_LDQ_L2_WAIT;
        pL2LookupEntries.entryArr[pickIdx] = oldestE;
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << dec << "[MLSU]: [ Cluster " << cID
                << ", Entry " << oldestE->memReq.eID
                << "] Pick L2 request. " << oldestE->memReq;
        }
        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             oldestE->memReq, "MLSU: [ Cluster %u, Entry %u] Pick L2 request.  ",
                             cID, oldestE->memReq.eID);
        }
    }
}

void MtcLDQInfo::handleL1Lookup(MemReqBus &bus)
{
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec <<
            bus.eID << "] L1 lookup. " << bus;
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "MLSU: [ Cluster %u, Entry %u] L1 lookup.  ", bus.cID, bus.eID);
    }

    bus.mtc_reqData.Reset();
    // To SCB/STQ
    top->SendL1(lookup_lu_scb_array, bus);
    lookup_lu_su_q->push_back(bus);

    MTCLUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    // Data from LDQ or L1
    if (entry.ldqHit) {
        uint2048_t data = clusters[bus.cID].cData.getData(bus.tag);
        entry.memReq.ldq_miss = false;
        entry.memReq.mtc_reqData.insertCacheData(data);
        entry.ldqRnt = true;
    } else {
        ++top->stats->ldq_miss_count;
        top->SendL1(lookup_lu_l1_array, bus);
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec <<
                bus.eID << "] L1 lookup miss."<< bus;
        }
    }
}


void MtcLDQInfo::handleL2Lookup(MemReqBus &bus)
{
    if (!top->L2Allow()) {
        *pref_throw = true;
        return;
    }

    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << "[mtcLSU]: [ Cluster " << bus.cID << ", Entry "
            << dec << bus.eID << "] L2 lookup. " << bus;
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "mtc LSU: [ Cluster %u, Entry %u] L2 lookup.  ", bus.cID, bus.eID);
    }

    // Need to send L2
    if (bus.prefetch && !wakeup_l1_lu_q->Empty()) {
        auto &writeQ = wakeup_l1_lu_q->GetRawReadData();
        for (auto it = writeQ.cbegin(); it != writeQ.cend(); ++it) {
            if ((*it)->addr == (bus.tag & (~0xff)) && (*it)->isRead()) {
                return;
            }
        }
    }

    top->stats->dcache_l2_pkt_count++;
    uint64_t tag = bus.tag & (~0xff);
    assert((bus.tag & 0xff) == 0);
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << "[256byte LSU]: [ Cluster " << bus.cID << ", Entry "
            << dec << bus.eID << "] L2 lookup. "<< bus;
    }
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
        pkt->size = 256;
        pkt->tpc = bus.tpc;
        pkt->tid = (static_cast<int>(LSUType::MEMORY_LSU) << TID_TYPE_OFFSET) +
            (bus.bid.val << 17) + (bus.rid.val << 9);

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
        sendMemReq(pkt);

        top->stats->dcache_l2_readpkt_count++;
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": send packet " << *pkt << " to Soc";
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": send load L1 miss packet to Soc, " << bus;

        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             *pkt, "MLSU: send packet to Soc. packet: ");
        }
        return;
    }

    ldqPkts[ldq2PktIdx] = missQ[tag];
    top->stats->dcache_l2_missq_filt_count++;
    if (bus.prefetch) {
        ++top->stats->dcache_l2_pref_missq_filt_count;
    } else {
        ++top->stats->dcache_l2_demand_missq_filt_count;
    }

    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": PFL1 requeset(0x << 0x" << bus.addr << ") hit miss queue";
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                  "MLSU: PFL1 requeset(0x << 0x%x) hit miss queue", bus.addr);
    }
    return;
}

void MtcLDQInfo::handleL1Receive(MemReqBus &bus)
{
    MTCLUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    if (entry.fsm != MTC_LDQ_REPICK) {
        return;
    }

    entry.l1Hit = bus.data_vld;
    entry.l1Rnt = true;
    entry.memReq.mtc_reqData.Reset();
    if (!entry.l1Hit) {
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec << bus.eID <<
                "] l1 miss : " << bus;
        }
        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             bus, "MLSU: [ Cluster %u, Entry %u] l1 miss : ",
                             bus.cID, bus.eID);
        }
        return;
    }

    // Update cluster data
    clusters[bus.cID].cData.merge(bus.tag, bus.mtc_reqData);
    // Update by Cacheline
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec << bus.eID <<
            "] receive L1 response " << bus << " data "<< bus.mtc_reqData.data.toStr();
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "MLSU: [ Cluster %u, Entry %u] receive L1 response, data %s, ",
                         bus.cID, bus.eID, bus.mtc_reqData.data.toStr());
    }
    handleMerge(bus);
}

void MtcLDQInfo::handleL1Upgrade(MemReqBus &bus)
{
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec << bus.eID <<
            "] receive L1 upgrade " << bus << " data "<< bus.mtc_reqData.data.toStr();
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "MLSU: [ Cluster %u, Entry %u] receive L1 upgrade, data %s, ",
                         bus.cID, bus.eID, bus.mtc_reqData.data.toStr());
    }

    bus.cID = addr2LDQcID(bus.addr);
    clusters[bus.cID].cData.merge(bus.addr, bus.mtc_reqData);
    for (MTCLUEntryInfo &e : clusters[bus.cID].entryArr) {
        if (e.fsm == MTC_LDQ_REPICK && bus.addr == e.memReq.tag) {
            bus.eID = e.memReq.eID;
            handleMerge(bus);
        }
    }
}

void MtcLDQInfo::handleSCBReceive(MemReqBus &bus)
{
    MTCLUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    // Wait for L2 or store, discard other response
    if (entry.fsm != MTC_LDQ_REPICK) {
        return;
    }

    entry.scbRnt = true;
    if (!bus.data_vld) {
        entry.storeBypass = false;
        return;
    }
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec <<
            bus.eID << "] receive SCB response " << bus << " data "<< bus.mtc_reqData.data.toStr();
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "MLSU: [ Cluster %u, Entry %u] receive SCB response, data %s, ",
                         bus.cID, bus.eID, bus.mtc_reqData.data.toStr());
    }
    handleMerge(bus);
}

void MtcLDQInfo::handleBypass(MemReqBus &bus)
{
    // SCB should return earlier or in the same time
    MTCLUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    // Wait for L2 or store, discard other response
    if (entry.fsm != MTC_LDQ_REPICK) {
        return;
    }
    ASSERT(entry.scbRnt);

    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec <<
            bus.eID << "] receive SU bypass " << bus << " data "<< bus.mtc_reqData.data.toStr();
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "MLSU: [ Cluster %u, Entry %u] receive SU bypass, data %s, ",
                         bus.cID, bus.eID, bus.mtc_reqData.data.toStr());
    }

    uint64_t cID = addr2LDQcID(bus.tag);
    for (auto &e : clusters[cID].entryArr) {
        if (e.fsm == MTC_LDQ_REPICK && AddrOverlap(bus.addr, bus.size, e.memReq.addr, e.memReq.size) &&
            LessEqual(bus.bid, bus.lsID, e.memReq.bid, e.memReq.lsID)) {
            bus.cID = cID;
            bus.eID = e.eID;
            handleMerge(bus);
        }
    }
}

void MtcLDQInfo::handleSTQReceive(MemReqBus &bus)
{
    MTCLUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    // Wait for L2 or store, discard other response
    if (entry.fsm != MTC_LDQ_REPICK) {
        return;
    }

    // SCB should return earlier or in the same time
    ASSERT(entry.scbRnt);
    entry.stqRnt = true;

    // Check if need to wait store
    if (bus.wait_store) {
        waitStore(bus);
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec <<
                bus.eID << "] wait store pending " << bus;
        }
        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             bus, "MLSU: [ Cluster %u, Entry %u] wait store pending, ", bus.cID, bus.eID);
        }
        return;
    }

    // Wait for L2 or store, discard other response
    if (entry.fsm != MTC_LDQ_REPICK) {
        return;
    }

    entry.stqRnt = true;
    if (!bus.data_vld) {
        return;
    }
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec << bus.eID
            << "] receive SU response " << bus << " cdata:"<< bus.mtc_reqData.data.toStr();
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "MLSU: [ Cluster %u, Entry %u] receive SU response, cdata %s, ",
                         bus.cID, bus.eID, bus.mtc_reqData.data.toStr());
    }
    handleMerge(bus);
}

void MtcLDQInfo::waitStore(MemReqBus &bus)
{
    MTCLUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    entry.fsm = MTC_LDQ_WAIT;
    entry.waitStore = true;
    entry.waitStoreTpc = bus.wait_tpc;
    entry.waitBid = bus.wait_bid;
    entry.rewait();
}

void MtcLDQInfo::handleMerge(MemReqBus &bus)
{
    MTCLUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    entry.memReq.mtc_reqData.merge(bus.mtc_reqData);
}

bool MtcLDQInfo::returnData(MemReqBus &bus, uint32_t iexIdx)
{
    uint2048_t data = bus.mtc_reqData.data;
    MTCLUEntryInfo &entry = clusters[bus.cID].entryArr[bus.eID];
    bus.data_vld = true;
    bus.peID = top->GetSim()->core->GetMtcPEIndex();
    entry.fsm = MTC_LDQ_RESOLVED;

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
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec << bus.eID << "] " <<
                "return data to TTrans " << bus << " iex pipe: " << iexIdx << " l1 hit:" << entry.l1Hit <<
                " scb/stq hit:" << entry.storeBypass;
        }
        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             bus, "MLSU: [ Cluster %u, Entry %u] return data to TTrans."
                             "iex pipe:%u l1 hit:%d scb/stq hit:%d  ",
                             bus.cID, bus.eID, iexIdx, entry.l1Hit, entry.storeBypass);
        }
        return false;
    } else {
        bus.data = ExtractData(data, bus.addr, bus.size);
    }
    // if not cross cacheline, then return
    if (!entry.crossLine) {
        bus.data = SignExtend(bus.data, bus.opcode);

        top->lsu_iex_lret_array[iexIdx]->Write(bus);

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
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << bus.cID << ", Entry " << dec << bus.eID << "] " <<
                "Load request handle over and return data. " << bus << " iex pipe: " << iexIdx <<" l1 hit:" <<
                entry.l1Hit << " scb/stq hit:" << entry.storeBypass << dec << ", total_load_request=" <<
                top->stats->total_load_request << " cycle: " << dec << GetSim()->getCycles();
        }
        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             bus, "MLSU: [ Cluster %u, Entry %u] Load request handle over and return data. "
                             "iex pipe:%u l1 hit:%d scb/stq hit:%d  ",
                             bus.cID, bus.eID, iexIdx, entry.l1Hit, entry.storeBypass);
        }
        return true;
    }

    // if cross cacheline, wait for second req
    processCrossRtn(bus);
    return sendCrossRtn(bus.peID, iexIdx);
}

void MtcLDQInfo::processCrossRtn(MemReqBus &rtnBus)
{
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || this->pConfigs->verbose) {
        cout<< "LSTop get cross lsu return "<<rtnBus<<endl;
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         rtnBus, "MLSU: LSTop get cross lsu return ");
    }

    bool hit = false;
    for (auto it = crossBuffer[rtnBus.peID].crossQ.begin();
            it != crossBuffer[rtnBus.peID].crossQ.end(); it++) {
        if (rtnBus.bid == (*it).bid && rtnBus.rid == (*it).rid) {
            hit = true;
            MemReqBus first;
            MemReqBus second;
            if ((rtnBus.addr & 0xff) == 0) {
                first = (*it);
                second = rtnBus;
            } else {
                first = rtnBus;
                second = (*it);
            }
            clusters[first.cID].entryArr[first.eID].memReq.isCrossCacheLine = false;
            if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || this->pConfigs->verbose) {
                LOG_INFO_M(Unit::MLSU, Stage::NA) <<"LSTop matched first  half " << first;
                LOG_INFO_M(Unit::MLSU, Stage::NA) <<"LSTop matched second half " << second;
            }
            if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                 first, "MLSU: LSTop matched first  half ");
                LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                 second, "MLSU: LSTop matched second half ");
            }
            first.data = first.data | (second.data << (first.size * 8));
            first.size = first.size + second.size;
            first.data = SignExtend(first.data, first.opcode);
            if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || this->pConfigs->verbose) {
                LOG_INFO_M(Unit::MLSU, Stage::NA) << "LSTop merged crossLsu get " << first;
            }
            if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                 first, "MLSU: LSTop merged crossLsu get ");
            }
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

bool MtcLDQInfo::sendCrossRtn(uint32_t pe, uint32_t iexIdx)
{
    if (crossBuffer[pe].completed.size() > 0) {
        MemReqBus crossBus = crossBuffer[pe].completed.front();
        crossBuffer[pe].completed.pop_front();
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << crossBus.cID << ", Entry " << crossBus.eID <<
                "] Load request handle over and return cross data. " << crossBus;
        }
        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             crossBus, "MLSU:[Cluster %u,Entry %u] Load request handle over and return cross data. ",
                             crossBus.cID, crossBus.eID);
        }

        if (!crossBus.specWakeup && !crossBus.stack_vld) {
            std::shared_ptr<IEX> iex;
            if (top->core->IsVectorIex(crossBus.iexTyp)) {
                iex = GetSim()->core->vectorTop->GetIex(crossBus.coreId);
            } else {
                iex = GetSim()->core->iex[crossBus.iexTyp];
            }
            iex->setMemWakeup(crossBus);
        }

        top->lsu_iex_lret_array[iexIdx]->Write(crossBus);

        crossBus.lsu_ret_cycle = GetSim()->getCycles();
        ++top->stats->total_load_request;
        top->stats->total_load_latency += (crossBus.lsu_ret_cycle - crossBus.lsuRecvCycle);
        return true;
    }

    return false;
}

void MtcLDQInfo::handleCancel(MemReqBus &mem)
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

void MtcLDQInfo::flush(FlushBus &flushReq)
{
    for (uint32_t i = 0; i < top->core->GetPECount(); i++) {
        crossBuffer[i].flush(flushReq);
    }

    auto match = [&flushReq] (MemReqBus &bus) -> bool {
        return flushReq.match(bus);
    };
    for (auto simQ: top->iex_lsu_lda_array) {
        simQ->FlushIf(match);
    }
    for (auto simQ: top->lsu_iex_lret_array) {
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
            if (e.fsm != MTC_LDQ_IDLE && match(e.memReq)) {
                e.fsm = MTC_LDQ_IDLE;
                e.Reset();
                ASSERT(clt.size > 0);
                --clt.size;
            }
        }
    }

    rslvQ.flush(flushReq);
}

void MtcLDQInfo::sendMemReq(PtrPacket &pkt)
{
    GFUMemReq req;
    req.vld = true;
    req.tid = pkt->tid;
    req.lsuTypeId = LSUType::MEMORY_LSU;
    ASSERT(pkt->size == 256);
    req.size = pkt->size;
    req.prefetch = (pkt->isPref() || pkt->hasPref());
    req.addr = pkt->addr & (uint64_t)~0xff;
    req.is_store = pkt->isWriteBack();
    if (req.is_store) {
        pkt->mtc_data.copyTo(req.data);
    }
    req.is_inst = pkt->isInst();
    req.cmd = pkt->cmd;
    req.bypassL2 = true;
    top->pkt_out_q->Write(req);
}


void MtcLDQInfo::handleDetect(MemReqBus &bus)
{
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": store comming detect. " << bus;
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "MLSU: store comming detect. ");
    }

    MemReqBus *pOldConfBus = NULL;
    // Detect in Clusters
    for (auto &clt : clusters) {
        for (auto &e : clt.entryArr) {
            if (e.fsm == MTC_LDQ_IDLE)
                continue;

            if (!(AddrOverlap(bus.addr, bus.size, e.memReq.addr, e.memReq.size)
                  && LessEqual(bus.bid, bus.lsID, e.memReq.bid, e.memReq.lsID))) {
                continue;
            }

            // Check when load had resolved
            if (e.fsm == MTC_LDQ_RESOLVED &&
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

                if (e.fsm == MTC_LDQ_REPICK) {
                    e.fsm = MTC_LDQ_WAIT;
                    e.rewait();

                    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": Store detect and cancel the request. " << e.memReq;
                    }
                    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX,
                                         LogLevel::CRITICAL, e.memReq, "MLSU: Store detect and cancel the request. ");
                    }
                }
                if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                    LOG_INFO_M(Unit::MLSU, Stage::NA) << ": load: " << e.memReq << " wait for store PC:0x" <<
                        hex << bus.tpc << dec << " id:" << e.waitBid;
                }
                if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                    LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                     e.memReq, "MLSU: wait for store PC:0x%x id:%u load: ", bus.tpc, e.waitBid);
                }
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

    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": load(" << *pOldConfBus << ") conflict with store(" <<bus << ")";
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         *pOldConfBus, "MLSU: load conflict with store, load : ");
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "MLSU: load conflict with store, store: ");
    }

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

void MtcLDQInfo::handleFlush(MemReqBus &confbus, MemReqBus &stBus)
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
    if (stBus.bid == confbus.bid) {
        BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(flushReq.bid, flushReq.stid);
        if (cmd && cmd->opcode != Opcode::OP_INVALID) {
            return;
        }
        top->core->flushUnit->flush_stats->IntraBlockMemoryAaccelssConflict++;
        flushReq.type = top->typeId == LSUType::SCALAR_LSU ? FlushType::INNER_FLUSH : FlushType::SIMT_INNER_FLUSH;
    } else {
        top->core->flushUnit->flush_stats->InterBlockMemoryAaccelssConflict++;
        flushReq.type = FlushType::NUKE_FLUSH;
        // TODO: simt only support for simt mode.
        if (flushReq.iexTyp == MEM_IEX) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << "simt mode should not be inner load/store conflict. B"
                << flushReq.bid << ":T" << flushReq.rid << " tpc: 0x" << hex << confbus.tpc
                << " addr: 0x" << confbus.addr;
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

    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        printf("load-store flush(%s).", (flushReq.type == FlushType::NUKE_FLUSH) ? "inter": "inner");
        cout << "load " << confbus << ". store " << stBus << "\n";
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        string flushReqType = (flushReq.type == FlushType::NUKE_FLUSH) ? "inter" : "inner";
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         confbus, "MLSU: load-store flush(%s). load : ", flushReqType.c_str());
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         stBus, "MLSU: load-store flush(%s). store: ", flushReqType.c_str());
    }
}

bool MtcLDQInfo::checkStall()
{
    uint32_t clusterDepth = pConfigs->lu_clusters_depth;
    ASSERT(top->typeId == LSUType::MEMORY_LSU);
    for (auto &clt : clusters) {
        if (top->typeId == LSUType::MEMORY_LSU &&
            ((clt.size + top->iex_lsu_lda_array.size() * top->core->configs.simtLane) > clusterDepth)) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << " MTC MEMORY_LSU clusterDepth: " << clusterDepth;
            return true;
        }
    }

    return false;
}

bool MtcLDQInfo::checkCltStall(uint64_t addr, uint32_t width)
{
    uint32_t cID = addr2LDQcID(addr);
    uint32_t curCnt = 0;
    for (auto &pipe : top->iex_lsu_lda_array) {
        if (!pipe->Empty()) {
            auto &readQ =  pipe->GetRawReadData();
            for (auto &bus: readQ) {
            curCnt += addr2LDQcID(bus.addr) == cID ? 1 : 0;
            }
        }
    }

    uint32_t clusterDepth = pConfigs->lu_clusters_depth;
    if (clusterDepth < width + curCnt + clusters[cID].size) {
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << "ldq stall, clusterDepth=" << clusterDepth
                << ", addr=0x" << hex << addr << dec << ", width=" << width << ", curCnt="
                << curCnt << ", clusters[cID].size=" << clusters[cID].size;
        }
        return true;
    }

    return false;
}

void MtcLDQInfo::handleSUWakeup(MemReqBus &bus)
{
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << ": SU wakeup. " << bus;
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         bus, "MLSU: SU wakeup. ");
    }

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

                ++top->stats->mdb_suaccelss_count;
                if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                    LOG_INFO_M(Unit::MLSU, Stage::NA) << "Waiting load request is waken up. Requst: "<< e.memReq;
                }
                if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                    LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                     e.memReq, "MLSU: Waiting load request is waken up. Requst: ");
                }
            }

            if ((e.fsm == MTC_LDQ_L1_DC_MISS || e.fsm == MTC_LDQ_L2_WAIT) && bus.tag == e.memReq.tag &&
                LessEqual(bus.bid, bus.lsID, e.memReq.bid, e.memReq.lsID)) {
                e.memReq.mtc_reqData.mergePosition(bus.mtc_reqData);
                if (checkDataPosionValid(e.memReq.addr, e.memReq.size, e.memReq.mtc_reqData)) {
                    e.storeBypass = true;
                    e.fsm = MTC_LDQ_WAIT;
                    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                        LOG_INFO_M(Unit::MLSU, Stage::NA) << "Miss load request: " << e.memReq
                            << " is waken up by SU. Requst: "<< bus;
                    }
                    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX,
                                         LogLevel::CRITICAL, e.memReq,
                                         "MLSU: Miss load request is waken up by SU. Miss load request: ");
                        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX,
                                         LogLevel::CRITICAL, bus,
                                         "MLSU: Miss load request is waken up by SU. Requst: ");
                    }
                }
            }
        }
    }
}

void MtcLDQInfo::handleSCBWakeup(MemReqBus &bus)
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

            if (e.memReq.tag == bus.tag && e.fsm != MTC_LDQ_REPICK && e.IsWorking()) {
                e.memReq.mtc_reqData.mergePosition(bus.mtc_reqData);
                if (checkDataPosionValid(e.memReq.addr, e.memReq.size, bus.mtc_reqData)) {
                    e.storeBypass = true;
                    e.fsm = MTC_LDQ_WAIT;
                    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                        LOG_INFO_M(Unit::MLSU, Stage::NA) << "Waiting load request is waken up by SCB."
                            << "Requst: "<< e.memReq;
                    }
                    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX,
                                         LogLevel::CRITICAL, e.memReq,
                                         "MLSU: Waiting load request is waken up by SCB. Requst: ");
                    }
                }
            }
        }
    }
}

bool MtcLDQInfo::sendMissReqToL2(uint64_t tag)
{
    return (missQ.count(tag) != 0);
}

void MtcLDQInfo::handleL1Wakeup(PtrPacket pkt)
{
    if (!pkt->isWrite()) {
        feedbackPref(pkt);
    }

    if (!pkt->isRead()) {
        return;
    }

    LOG_INFO_M(Unit::MLSU, Stage::NA) << " L1 data coming and wakeup. " << *pkt;

    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                         *pkt, "MLSU: L1 data coming and wakeup. ");
    }

    ReqData256 reqD;
    uint64_t cID = addr2LDQcID(pkt->addr);
    missQ.erase(pkt->addr);
    prefSet.erase(pkt->addr);
    reqD.insertCacheData(pkt->mtc_data);
    clusters[cID].cData.merge(pkt->addr, reqD);
    for (auto &e : clusters[cID].entryArr) {
        if (!e.ldqHit && !e.l1Hit && e.fsm != MTC_LDQ_IDLE && e.memReq.tag == pkt->addr
            && e.fsm == MTC_LDQ_L2_WAIT) {
            e.fsm = MTC_LDQ_WAIT;
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

            if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                LOG_INFO_M(Unit::MLSU, Stage::NA) << "Missed load request is waken up. Requst: "<< e.memReq
                    << dec << ", l2_miss_lat="<< (GetSim()->getCycles() - e.memReq.l1MissCycle)
                    << ", total_l2_lat=" << top->stats->ld_l2_latency
                    << ", ld_l2_reqs=" << top->stats->ld_l2_reqs;
            }
            if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                 e.memReq, "MLSU: Missed load request is waken up. Requst: ");
            }
        }
    }
}

void MtcLDQInfo::feedbackPref(PtrPacket pkt)
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

void MtcLDQInfo::checkLoadPending()
{
    if (!loadPending) {
        return;
    }

    // Get oldest bid
    ROBID oldestBID = top->core->bctrl->blockROB.getOldestBlockID(0);
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
            if (loadPendingCyc == 0 || (oldestBID == e.memReq.bid && (!oldestLSID.vld || (oldestLSID.vld && LessEqual(e.memReq.lsID, oldestLSID.id))))
                || LessROBID(e.waitBid, oldestBID)) {
                e.waitStore = false;
                MDBBus mBus = getMDBBus(e.memReq);
                delete_lu_mdb_q->push_back(mBus);
                ++top->stats->mdb_fail_count;
                top->stats->mdb_fail_wait_cycle += (GetSim()->getCycles() - e.stallCycle);
                if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                    LOG_INFO_M(Unit::MLSU, Stage::NA) << ": [ Cluster " << e.memReq.cID << ", Entry " <<
                        dec << e.memReq.eID <<  "] Load in oldest block is wakeuped. " << e.memReq;
                }
                if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                    LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                     e.memReq, "MLSU: [ Cluster %u, Entry %u] Load in oldest block is wakeuped. ",
                                     e.memReq.cID, e.memReq.eID);
                }

                continue;
            }
            loadPending = true;
        }
    }

    loadPendingCyc = (oldestPending && loadPendingCyc > 0) ? loadPendingCyc - 1 : 300;
}

void MtcLDQInfo::statsMemBound(bool& anyload, bool& l1miss, bool& l2miss)
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
            if (e.fsm == MTC_LDQ_L2_WAIT) {
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


void MtcLDQInfo::LDQCheckDeadLockSendFlushReq(IDBus& oldRetire, bool isImmFlush)
{
    ROBID oldestBID = top->core->bctrl->blockROB.getOldestBlockID(0);
    BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(oldestBID, 0);
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
    auto inst = dynamic_pointer_cast<SPE>(GetSim()->core->peArray[oldRetire.peID])
                ->prob[oldRetire.tid]->getROBEntry(oldRetire.rid.val).inst;
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
    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        LOG_INFO_M(Unit::MLSU, Stage::NA) << "Oldest load is stalled, report inner flush from B" << oldRetire.bid <<
            ":T" << oldRetire.rid;
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        LOG_DEBUG(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                  "MLSU:Oldest load is stalled,report inner flush from B%u:T%u", oldRetire.bid.val, oldRetire.rid.val);
    }
}

void MtcLDQInfo::checkDeadLock()
{
    if (!checkStall()) {
        return;
    }

    if (!rslvQ.Full()) {
        return;
    }

    ROBID oldestBID = top->core->bctrl->blockROB.getOldestBlockID(0);
    BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(oldestBID, 0);

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
        || (oldRetire.pipeID >= top->iex_lsu_lda_array.size())) {
        return;
    }
    MemReqBus youngestLoad;
    youngestLoad.toMtcLsu = true;
    youngestLoad.vld = false;
    // Lookup ldq
    for (MtcClusterInfo &clt : clusters) {
        for (MTCLUEntryInfo &e : clt.entryArr) {
            if (e.fsm == MTC_LDQ_IDLE) {
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
        if (!req.vld) {
            continue;
        }
        if (LessEqual(req.bid, req.lsID, oldRetire.bid, oldRetire.lsID)) {
            return;
        }
        if (!youngestLoad.vld || LessEqual(youngestLoad.bid, youngestLoad.lsID, req.bid, req.lsID)) {
            youngestLoad = req;
        }
    }
    for (auto &simQ : top->iex_lsu_lda_array) {
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

    if (top->typeId == LSUType::MEMORY_LSU && checkStall()) {
        for (uint32_t pe = 0; pe < top->core->peArray.size(); ++pe) {
            IDBus oldPeRetire = top->core->peArray[pe]->GetRetireID();
            if (!oldPeRetire.vld || oldPeRetire.bid == oldestBID || !oldPeRetire.isLoadStore || !oldPeRetire.isLoad ||
             oldRetire.pipeID >= top->iex_lsu_lda_array.size()) {
                continue;
            }

            LDQCheckDeadLockSendFlushReq(oldPeRetire, true);
        }
        return;
    }
    LDQCheckDeadLockSendFlushReq(oldRetire, false);
}

uint32_t MtcLDQInfo::retire(IDBus &commitBus)
{
    uint32_t reitedCnt = 0;
    for (auto &clt : clusters) {
        for (auto &e : clt.entryArr) {
            if (e.fsm != MTC_LDQ_RESOLVED ||
                (top->typeId == LSUType::SCALAR_LSU &&
                !LessROBID(e.memReq.bid, e.memReq.lsID, commitBus.bid, commitBus.lsID)) ||
                (top->typeId == LSUType::MEMORY_LSU &&
                !LessROBID(e.memReq.bid, e.memReq.gid, e.memReq.lsID, commitBus.bid, commitBus.gid, commitBus.lsID))) {
                continue;
            }

            if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
                LOG_INFO_M(Unit::MLSU, Stage::NA) << "Load request is retired in LDQ. " << e.memReq;
            }
            if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
                LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                                 e.memReq, "MLSU: Load request is retired in LDQ. ");
            }

            e.Reset();
            ASSERT(clusters[e.cID].size > 0);
            --clusters[e.cID].size;
            ++reitedCnt;
        }
    }
    return reitedCnt;
}

PMTCLUEntryInfo MtcLDQInfo::findOldestLoad(void)
{
    PMTCLUEntryInfo oldestE = NULL;
    for (auto &clt: clusters) {
        if (clt.size == 0)
            continue;

        for (auto &e : clt.entryArr) {
            if (e.fsm == MTC_LDQ_IDLE)
                continue;

            if (!oldestE || LessEqual(e.memReq.bid, e.memReq.gid, e.memReq.lsID,
                oldestE->memReq.gid, oldestE->memReq.bid, oldestE->memReq.lsID)) {
                oldestE = &e;
            }
        }
    }

    return oldestE;
}

void MtcLDQInfo::LUStatsTick(void)
{
    if (checkStall()) {
        top->stats->ldq_full_cycles++;
        if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || top->configs.verbose) {
            LOG_INFO_M(Unit::MLSU, Stage::NA) << ": MTC load queue is full! Stall cycle: " <<
                dec << top->stats->ldq_full_cycles;
        }
        if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
            LOG_DEBUG(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                      "MLSU: load queue is full! Stall cycle: %u", top->stats->ldq_full_cycles);
        }
    }

    uint32_t occupied_ldq_entry = 0;
    for (auto &clt: clusters) {
        for (auto &e : clt.entryArr) {
            if (e.fsm == MTC_LDQ_WAIT) {
                if (e.waitStore) {
                    top->stats->load_wait_store_cycles++;
                } else {
                    top->stats->load_wait_pick_cycles++;
                }
            }
        }

        occupied_ldq_entry += clt.size;
    }

    if ((VERBOSE_ON && STAGE_ENABLED(StageID::LSU_ALL)) || pConfigs->verbose) {
        PMTCLUEntryInfo oldestE = findOldestLoad();
        if (oldestE) {
            MemReqBus &oldest = oldestE->memReq;
            cout << std::dec << "[MLSU]: " << occupied_ldq_entry << " Entries Occupied in Load Queue.";
            cout << " Oldest is [ Cluster " << oldest.cID << ", Entry " << dec << oldest.eID << "] "
                << oldest << " dataHit " << oldestE->dataHit();
            cout << " wait_store " << oldestE->waitStore << " fsm " << oldestE->fsm << endl;
        }
    }
    if (DEBUG_VERBOSE_ON || pConfigs->verbose) {
        PMTCLUEntryInfo oldestE = findOldestLoad();
        if (oldestE) {
            MemReqBus &oldest = oldestE->memReq;
            LOG_DEBUG_STRUCT(top->debugLogger, Unit::LSU, Stage::NA, UINT32_MAX, UINT64_MAX, LogLevel::CRITICAL,
                             oldest, "MLSU:%u Entries Occupied in Load Queue. Oldest is [Cluster %u, Entry %u] "
                             "dataHit:%d, wait_store:%d, fsm:%u. ",
                             occupied_ldq_entry, oldest.cID, oldest.eID, oldestE->dataHit(),
                             oldestE->waitStore, oldestE->fsm);
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

} // namespace JCore
