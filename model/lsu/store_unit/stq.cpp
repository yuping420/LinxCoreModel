#include "lsu/store_unit/stq.h"

#include "lsu/lsu.h"
#include "ModelCommon/SimInstInfo.h"

namespace JCore {

void STQueueEntryInfo::Reset()
{
    ROBID clr;
    clr.val = 0;
    clr.wrap = false;

    memStReq.vld = false;
    bid = clr;
    rid = clr;
    peid = 0;
    size = 0;
    addr = 0;
    data = 0;
    miss = false;
    fsm = STQ_IDLE;
    vld = false;
    StqIdx = 0;
    stackVld = 0;
    addrRdy = false;
    dataRdy = false;
}

void STQueueEntryInfo::init(MemReqBus &bus)
{
    if (!bus.vld) {
        return;
    }

    if (vld) {
        // Merge sta & std
        if (bus.type == ST_ADDR) {
            if (memStReq.type != ST_DATA) {
                LOG_ERROR_M(Unit::LSU, Stage::NA) << "Wait request: " << memStReq << " coming request: " << bus;
                ASSERT(0 && "memStReq.type != ST_DATA");
            }
            addrRdy = true;
            memStReq.addr = bus.addr;
            memStReq.tag = bus.tag;
        } else if (bus.type == ST_DATA) {
            if (memStReq.type != ST_ADDR) {
                LOG_ERROR_M(Unit::LSU, Stage::NA) << "Wait request: " << memStReq << " coming request: " << bus;
                ASSERT(0 && "memStReq.type != ST_ADDR");
            }
            addrRdy = true;
            dataRdy = true;
            memStReq.addr = bus.addr;
            memStReq.data = bus.data;
            memStReq.tag = bus.tag;
        } else {
            ASSERT("Exceptional store type");
        }
        memStReq.type = ST_ALL;
        stackVld = bus.stack_vld ? true : stackVld;
    } else {
        // Insert only
        memStReq = bus;
        vld = true;
        fsm = STQ_WAIT;
        bid = bus.bid;
        rid = bus.rid;
        peid = bus.peID;
        size = bus.size;
        stackVld = bus.stack_vld;
        if (bus.type == ST_ALL) {
            dataRdy = true;
            addrRdy = true;
        } else if (bus.type == ST_DATA) {
            dataRdy = true;
            addrRdy = true;
        } else if (bus.type == ST_ADDR) {
            dataRdy = false;
            addrRdy = true;
        }
        if (OpcodeManager::Inst().GetOpcodeGroup(bus.opcode) == InstGroup::CACHE_MAINTAIN) {
            dataRdy = true;
            addrRdy = true;
        }
    }
}

void STQ::Reset()
{
    for (auto &e : stEntry)
        e.Reset();
    size = 0;
    osdSize = 0;
    storeCommitQ.clear();
}

void STQ::Build(uint32_t depth)
{
    stEntry = std::vector<STQueueEntryInfo>(depth);
}

void STQ::insert(MemReqBus &bus)
{
    for (auto &e : stEntry) {
        if (!e.vld) {
            e.init(bus);
            ASSERT(size < stEntry.size());
            ++size;
            ASSERT(osdSize < size);
            ++osdSize;
            return;
        }
    }
    ASSERT("No free entry in STQ!");
}

void STQ::free(uint32_t index)
{
    ASSERT(size > 0);
    --size;
    if (stEntry[index].fsm == STQ_WAIT) {
        ASSERT(osdSize > 0);
        --osdSize;
    }
    stEntry[index].Reset();
}

bool STQ::full()
{
    if (top->top->typeId == LSUType::SCALAR_LSU) {
        return (size == pConfigs->scalar_stq_depth);
    }
    ASSERT(top->top->typeId == LSUType::VECTOR_LSU);
    return (size == pConfigs->stq_depth);
}

bool STQ::full(uint32_t sz)
{
    if (top->top->typeId == LSUType::SCALAR_LSU) {
        return (size + sz > pConfigs->scalar_stq_depth);
    }
    ASSERT(top->top->typeId == LSUType::VECTOR_LSU);
    return (size + sz > pConfigs->stq_depth);
}

bool STQ::Empty()
{
    return (size == 0);
}

bool STQ::stall()
{
    return (full() && osdSize == size);
}

bool STQ::mergeStore(MemReqBus &bus)
{
    if (!bus.vld) {
        return false;
    }

    if (bus.type == ST_ALL) {
        return true;
    }

    for (uint32_t i = 0; i < stEntry.size(); i++) {
        STQueueEntryInfo &e = stEntry[i];
        if (e.fsm == STQ_WAIT && bus.bid == e.memStReq.bid && bus.lsID == e.memStReq.lsID) {
            if (bus.iexTyp == SCALAR_IEX || bus.simtLane == e.memStReq.simtLane) {
                stEntry[i].init(bus);
                return true;
            }
        }
    }

    return false;
}

bool STQ::isStqCmtable(ROBID oldestBID, IDBus commitBus, STQueueEntryInfo &e)
{
    if ((e.fsm != STQ_WAIT) || !e.addrRdy || !e.dataRdy) {
        return false;
    }

    if (top->top->typeId == LSUType::VECTOR_LSU) {
        return LessEqual(e.memStReq.bid, e.memStReq.gid, e.memStReq.lsID, commitBus.bid, commitBus.gid, commitBus.lsID);
    }

    ASSERT(top->top->typeId == LSUType::SCALAR_LSU);
    if (e.memStReq.IsTileLS()) {
        if (LessEqual(e.memStReq.bid, commitBus.nonFlushBid)) {
            return true;
        }
        if (!LessEqual(oldestBID, e.memStReq.bid)) {
            LOG_ERROR_M(Unit::TMA, Stage::NA) << "oldestBID: B" << std::dec << oldestBID << ", stq B" << e.memStReq.bid;
            abort();
        }
    }
    // entry's block has retired
    if (LessROBID(e.bid, oldestBID)) {
        return true;
    }
    // entry's block is the oldest block and COMPLETED.
    bool completed = top->core->bctrl->blockROB.IsOldestBlkComplete(top->stid);
    if (oldestBID == e.bid && completed) {
        return true;
    }
    // entry's block is the oldest block and RUNNING
    if (commitBus.vld && oldestBID == commitBus.bid && oldestBID == e.bid &&
        LessROBID(e.memStReq.lsID, commitBus.lsID)) {
        return true;
    }
    if (commitBus.vld && oldestBID == commitBus.bid && oldestBID == e.bid) {
        ROBIDBus oldestLSID = top->core->peArray[e.memStReq.peID]->GetOldestLSID(e.memStReq.tid);
        if (!oldestLSID.vld || (oldestLSID.vld && LessROBID(e.memStReq.lsID, oldestLSID.id))) {
            return true;
        }
    }
    return false;
}

void STQ::retire(IDBus &commitBus)
{
    uint32_t size_idx = 0;
    std::list<uint32_t> store_list;
    ROBID oldestBID = top->core->bctrl->blockROB.getOldestBlockID(top->stid);
    for (uint32_t i = 0; i < stEntry.size(); i++) {
        if (size_idx >= size)
            break;
        if (!(stEntry[i].vld && stEntry[i].addrRdy && stEntry[i].dataRdy && stEntry[i].memStReq.type == ST_ALL)) {
            continue;
        }
        ++size_idx;
        STQueueEntryInfo &e = stEntry[i];
        if (e.memStReq.tTransReq) {
            e.fsm = STQ_COMMIT;
            store_list.insert(store_list.end(), i);
            ASSERT(osdSize > 0);
            --osdSize;
            continue;
        }
        if ((e.fsm != STQ_WAIT || !isStqCmtable(oldestBID, commitBus, e)
            || top->core->flushUnit->needFlush(e.bid, e.memStReq.lsID)) && !e.memStReq.tTransReq) {
            continue;
        }
        e.fsm = STQ_COMMIT;
        ASSERT(osdSize > 0);
        --osdSize;
        if (e.memStReq.IsTileLS()) {
            continue;
        }
        auto it = store_list.begin();
        MemReqBus &sel = e.memStReq;
        for (; it != store_list.end(); it++) {
            if (!stEntry[*it].vld) continue;
            MemReqBus &cur = stEntry[*it].memStReq;
            if (LessEqual(sel.bid, sel.lsID, cur.bid, cur.lsID)) {
                break;
            }
        }
        store_list.insert(it, i);
        LOG_INFO_M(Unit::LSU, Stage::NA) << "Store " << e.memStReq << " is about to commit. oldestBID = "
            << oldestBID << " oldestGID = " << commitBus.gid << ". commitBus B" << commitBus.bid << ":T"
            << commitBus.rid << ", commitBus.V = " << commitBus.vld;
    }

    for (auto &idx : store_list) {
        storeCommitQ.push_back(idx);
    }
}

void STQ::commit()
{
    if (osdSize == size) {
        return;
    }
    // Commit store from old to young
    uint32_t commit_num = 0;
    ROBID oldestBID = top->core->bctrl->blockROB.getOldestBlockID(top->stid);
    BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(oldestBID, top->stid);
    // In order to insert by the same SCB
    for (auto it = storeCommitQ.begin(); it != storeCommitQ.end() && commit_num < pConfigs->store_commit_count;) {
        uint32_t i = *it;
        ASSERT(stEntry[i].vld);

        MemReqBus &wb_st = stEntry[i].memStReq;
        LOG_INFO_M(Unit::LSU, Stage::NA) << "Store " << i << " " << wb_st << " is commited";
        if (AddrCrossCacheline(wb_st.addr, wb_st.size)) {
            MemReqBus bus1 = MemReqBus();
            MemReqBus bus2 = MemReqBus();
            GetCrossReq(wb_st, bus1, bus2);

            if (top->top->checkSimStall(top->commit_su_scb_array, bus1) ||
                top->top->checkSimStall(top->commit_su_scb_array, bus2)) {
                    ++it;
                    continue;
                }

            top->top->sendSimL1(top->commit_su_scb_array, bus1);
            top->top->sendSimL1(top->commit_su_scb_array, bus2);
        } else {
            if (top->top->checkSimStall(top->commit_su_scb_array, wb_st)) {
                ++it;
                continue;
            }
            top->top->sendSimL1(top->commit_su_scb_array, wb_st);
        }
        if (top->core->configs.BSB_enable) {
            lsu_iex_sid_q->push_back(wb_st.sid);
        }
        if (wb_st.tTransReq) {
            MemTTransStRes res;
            res.SetReqId(wb_st.tTransId);
            res.SetRsp(0);
            // FIXME: support multiple memory core
            top->top->tTransMemStResArray[res.GetCoreId()]->Write(res);
        }
        if (top->top->typeId == LSUType::VECTOR_LSU && IsBlockTypeParallel(cmd->blockType)) {
            LOG_INFO_M(Unit::LSU, Stage::NA) << "[LSU]: Because STQ entries retire. Reset the idle cycle.";
            GetSim()->GetVerifyManager(this->top->stid)->ResetWaitCycle();
        }
        free(i);
        it = storeCommitQ.erase(it);
        ++commit_num;
    }

    if (top->top->typeId == JCore::LSUType::SCALAR_LSU) {
        // BIQType::STORE_IQ 没有对应的块类型，仅用于标量ld/st? 或者可删除
        // top->core->bctrl->blockIssueQueueUnit.WindowSlides(commit_num, BIQType::STORE_IQ, false);
        top->core->iex[SCALAR_IEX]->iq.windowSlides(commit_num, false);
    }
}

void STQ::getData(MemReqBus &ldReq)
{
    lookupForLoad(ldReq, true);
}

void STQ::flush(FlushBus &flushReq)
{
    for (uint32_t i = 0; i < stEntry.size(); i++) {
        if (!stEntry[i].vld) continue;
        auto &st_req = stEntry[i].memStReq;
        if (flushReq.match(st_req) && stEntry[i].fsm == STQ_WAIT) {
            free(i);
        }
    }
}

void STQ::Work(void)
{
}

void STQ::Xfer(void)
{
    // debugging: check stq
    // cout << "check stq size "<<dec<< osdSize << " " << size << endl;
    // for (STQueueEntryInfo &e : stEntry) {
    //     if (!e.vld)
    //         continue;
    //     cout<<e.fsm<<" req:"<<e.memStReq << "sid:"<< e.memStReq.sid <<endl;
    // }
}

SimSys *STQ::GetSim(void)
{
    return sim;
}

MemReqBus STQ::mdbCheck(MemReqBus &bus)
{
    MemReqBus notHitReq;
    for (STQueueEntryInfo &e : stEntry) {
        if (!e.vld)
            continue;

        if (e.memStReq.bid == bus.bid && e.memStReq.tpc == bus.tpc && !e.memStReq.IsTileLS()) {
            if (!e.dataRdy || !e.addrRdy)
                return notHitReq;
            return e.memStReq;
        }
    }

    return notHitReq;
}

void STQ::checkWait(MemReqBus &ldReq)
{
    lookupForLoad(ldReq, false);
}

void STQ::SUStats_tick(void)
{
    if (stall()) {
        top->top->stats->stq_full_cycles++;
        LOG_DETAIL_M(Unit::LSU, Stage::NA) << "[LSU]: store queue is full! Stall cycle: " << top->stq_stall_cyc;
    }

    uint64_t stq_occupied_entries = 0;
    for (uint32_t i = 0; i < stEntry.size(); i++) {
        if (!stEntry[i].vld) continue;
        stq_occupied_entries++;
    }

    if (stq_occupied_entries != 0) {
        top->top->stats->stq_total_occupied += stq_occupied_entries;
        top->top->stats->stq_occupied_count++;
        uint32_t count = pConfigs->stq_depth;
        if (top->top->typeId == LSUType::SCALAR_LSU) {
            count = pConfigs->scalar_stq_depth;
        }
        uint32_t stq_depth = count;
        if (stq_occupied_entries < (stq_depth * 0.1)) {
            top->top->stats->stq_occupied_10++;
        } else if (stq_occupied_entries < (stq_depth * 0.25)) {
            top->top->stats->stq_occupied_25++;
        } else if (stq_occupied_entries < (stq_depth * 0.5)) {
            top->top->stats->stq_occupied_50++;
        } else if (stq_occupied_entries < (stq_depth * 0.75)) {
            top->top->stats->stq_occupied_75++;
        } else if (stq_occupied_entries < (stq_depth * 0.9)) {
            top->top->stats->stq_occupied_90++;
        } else {
            top->top->stats->stq_occupied_100++;
        }
    }
}

MemReqBus STQ::checkDeadLock(IDBus &retireID)
{
    MemReqBus ret = MemReqBus();
    ret.vld = false;
    for (uint32_t i = 0; i < stEntry.size(); ++i) {
        STQueueEntryInfo &e = stEntry[i];
        if (!e.vld || e.fsm == STQ_COMMIT) {
            ret.vld = false;
            break;
        }
        if (LessEqual(e.memStReq.bid, e.memStReq.lsID, retireID.bid, retireID.lsID)
            && !(e.memStReq.bid == retireID.bid && e.memStReq.lsID == retireID.lsID && e.memStReq.type != ST_ALL)) {
                ret.vld = false;
                break;
            }
        if (!ret.vld || LessEqual(ret.bid, ret.lsID, e.memStReq.bid, e.memStReq.lsID)) {
            ret = e.memStReq;
        }
    }
    return ret;
}

void STQ::lookupForLoad(MemReqBus &ldReq, bool needData)
{
    // Check STQ for STQ Bypass
    // First, find all conflict stores and sort them
    std::list<int> conf_stores;
    for (uint32_t i = 0; i < stEntry.size(); i++) {
        if (!stEntry[i].vld) continue;
        STQueueEntryInfo &st_e = stEntry[i];
        MemReqBus &stReq = st_e.memStReq;
        if (stReq.vld && st_e.addrRdy && st_e.IsWorking()
            && AddrOverlap(stReq.addr, stReq.size, ldReq.addr, ldReq.size)
            && LessEqual(stReq.bid, stReq.lsID, ldReq.bid, ldReq.lsID)) {
            // TODO: TLOAD/TSTORE bypass
            // if (ldReq.IsTileLS() || stReq.IsTileLS()) {
            //     cerr << ldReq << " <----------> " << stReq << endl;
            //     ASSERT(0 && "TLOAD/TSTORE address conflict!");
            // }

            auto& sReqCmd = stReq.blockCmd;
            if (sReqCmd && sReqCmd->tileOp == JCore::TileOp::TSTORE) {
                ldReq.wait_store = true;
                ldReq.wait_tpc = stReq.tpc;
                ldReq.wait_bid = stReq.bid;
                ldReq.wait_rid = stReq.rid;
                return;
            }

            // old first
            auto it = conf_stores.begin();
            for (; it != conf_stores.end(); it++) {
                if (!stEntry[*it].vld) continue;
                MemReqBus &st = stEntry[*it].memStReq;
                if (LessEqual(stReq.bid, stReq.lsID, st.bid, st.lsID)) {
                    break;
                }
            }
            conf_stores.insert(it, i);
        }
    }

    bool waitPosionVld[64] = {false};
    MemReqBus *confStReq = NULL;
    ldReq.data_vld = false;
    ldReq.reqData.Reset();
    // Bypass: store is abort to commit
    for (auto &idx : storeCommitQ) {
        auto &e = stEntry[idx];
        auto &stReq = e.memStReq;
        if (e.vld && AddrOverlap(stReq.addr, stReq.size, ldReq.addr, ldReq.size)) {
            // TODO: TLOAD/TSTORE bypass
            // if (ldReq.IsTileLS() || stReq.IsTileLS()) {
            //     cerr << ldReq << " <----------> " << stReq << endl;
            //     ASSERT(0 && "TLOAD/TSTORE address conflict!");
            // }
            ldReq.data_vld = true;
            if (OpcodeManager::Inst().GetOpcodeGroup(stReq.opcode) == InstGroup::CACHE_MAINTAIN && needData) {
                ldReq.reqData.zero();
                ldReq.data_vld = true;
                continue;
            }
            if (needData)
                UpdateData(ldReq.reqData.data, stReq.addr, stReq.size, stReq.data, ldReq.tag);
            UpdateSTValid(ldReq.reqData.positionVld, stReq.addr, stReq.size, true, ldReq.tag);
        }
    }
    // Combine all stores. From old to young
    for (auto idx : conf_stores) {
        if (!stEntry[idx].vld) continue;

        STQueueEntryInfo &e = stEntry[idx];
        MemReqBus &stReq = e.memStReq;
        if (OpcodeManager::Inst().GetOpcodeGroup(stReq.opcode) == InstGroup::CACHE_MAINTAIN && needData) {
            ldReq.reqData.zero();
            ldReq.data_vld = true;
            continue;
        }

        if (e.dataRdy) {
            ldReq.data_vld = true;
            if (needData)
                UpdateData(ldReq.reqData.data, stReq.addr, stReq.size, stReq.data, ldReq.tag);
            UpdateSTValid(ldReq.reqData.positionVld, stReq.addr, stReq.size, true, ldReq.tag);
            // Not waiting for the bit
            UpdateSTValid(waitPosionVld, stReq.addr, stReq.size, false, ldReq.tag);
            continue;
        }

        // Waiting for the bit
        UpdateSTValid(waitPosionVld, stReq.addr, stReq.size, true, ldReq.tag);
        confStReq = &stReq;
    }

    // Check if wait for the nearest store
    ldReq.wait_store = false;
    for (uint64_t i = 0; i < 64; i++) {
        if (waitPosionVld[i]) {
            ldReq.wait_store = true;
            ldReq.wait_tpc = confStReq->tpc;
            ldReq.wait_bid = confStReq->bid;
            ldReq.wait_rid = confStReq->rid;
            break;
        }
    }
}

void STQ::ResolveTstore(IDBus idBus)
{
    for (uint32_t i = 0; i < stEntry.size(); i++) {
        STQueueEntryInfo &e = stEntry[i];
        if (e.vld && e.fsm == STQ_WAIT && e.memStReq.IsTileLS() && e.bid == idBus.bid) {
            e.fsm = STQ_COMMIT;
            --osdSize;
            break;
        }
    }
}

MemReqBus STQ::PickTStore(uint32_t id)
{
    bool found = false;
    uint32_t oldEid = 0;
    for (uint32_t eid = 0; eid < stEntry.size(); eid++) {
        auto &e = stEntry[eid];
        if (!e.vld || !e.memStReq.IsTileLS()) {
            continue;
        }

        if (top->core->configs.ruminateEnable && id >= top->core->configs.tauStartId) {
            break;
        }

        if (e.fsm == STQ_COMMIT) {
            if (!found || (found && LessEqual(e.bid, stEntry[oldEid].bid))) {
                found = true;
                oldEid = eid;
            }
        }
    }
    MemReqBus req = MemReqBus();
    if (found) {
        req = stEntry[oldEid].memStReq;
        free(oldEid);
        // BIQType::STORE_IQ 没有对应的块类型，仅用于标量ld/st? 或者可删除
        // top->core->bctrl->blockIssueQueueUnit.WindowSlides(1, BIQType::STORE_IQ, false);
        top->core->iex[SCALAR_IEX]->iq.windowSlides(1, false);
    }
    return req;
}

} // namespace JCore
