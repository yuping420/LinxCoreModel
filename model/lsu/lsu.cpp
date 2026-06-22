#include "lsu/lsu.h"

namespace JCore {

void LoadStoreUnit::Reset()
{
    for (auto& loadUnit : lu) {
        loadUnit.Reset();
    }
    for (auto& storeUnit: su) {
        storeUnit.Reset();
    }
    resetStats();
}

void LoadStoreUnit::Build()
{
    configs.overrideDefaultConfig(GetSim()->getCfgs());
    stats = std::make_shared<LSUStats>(GetSim()->getRpt());
    l1Cache = std::make_shared<L1Top>();
    l2Cache = std::make_shared<L2Cache>();

    uint32_t threadCnt = GetSim()->core->configs.scalar_smt_thread;
    su.resize(threadCnt);
    lu.resize(threadCnt);
    buildInterface();
    for (uint32_t stid = 0; stid < su.size(); ++stid) {
        auto& storeUnit = su[stid];
        storeUnit.stid = stid;
        storeUnit.pConfigs = &configs;
        storeUnit.core = core;
        storeUnit.top = this;
        storeUnit.sim = sim;
        storeUnit.Build();
    }

    for (uint32_t stid = 0; stid < lu.size(); ++stid) {
        auto& loadUnit = lu[stid];
        loadUnit.stid = stid;
        loadUnit.sim = sim;
        loadUnit.top = this;
        loadUnit.pConfigs = &configs;
        loadUnit.Build();
    }

    if (typeId == LSUType::VECTOR_LSU) {
        configs.pref_enable = false;
    }
    prefetcher.configs = &configs;
    prefetcher.sim = GetSim();
    prefetcher.top = this;
    prefetcher.Build();

    mdb.sim = sim;
    mdb.core = core;
    mdb.pConfigs = &configs;
    mdb.top = this;
    mdb.Build();

    l1Cache->sim = GetSim();
    l1Cache->top = this;
    l1Cache->lsuConfigs = &configs;
    l1Cache->debugLogger = debugLogger;
    l1Cache->memID_s = memID_s;
    l1Cache->Build();

    l2Cache->sim = GetSim();
    l2Cache->debugLogger = debugLogger;
    l2Cache->memID_s = memID_s;
    l2Cache->l2_mem_q = pkt_out_q;
    l2Cache->mem_l2_q = pkt_in_q;
    l2Cache->inst_l2_q = inst_l2_q;
    l2Cache->hpref_l2_q = hpref_l2_q;
    l2Cache->l2_inst_q = l2_inst_q;
    l2Cache->snp_l2_q = snp_l2_q;
    l2Cache->lsuTypeId = typeId;
    l2Cache->Build();

    fakeLSU.pe_ret_data_q.resize(core->GetPECount());
    fakeLSU.pe_resolve_q.resize(core->GetPECount());
    fakeLSU.pe_wakeup_q.resize(core->GetPECount());

    for (size_t i = 0; i < lu.size(); ++i) {
        lu[i].Reset();
        // TODO: 或许需要拆成多个信号？
        lu[i].pref_throw = &l2Cache->pref_throw;
    }
    for (auto& storeUnit: su) {
        storeUnit.Reset();
    }
    mdb.Reset();
    prefetcher.Reset();
    l1Cache->Reset();
    l2Cache->Reset();
}

void LoadStoreUnit::buildInterface()
{
    uint32_t cltCnt = configs.lsu_width;
    intf.pref_lu_l1_array.resize(cltCnt);
    for (auto &prefLuL1Q : intf.pref_lu_l1_array) {
        l1Cache->pref_lu_l1_array.emplace_back(&prefLuL1Q);
    }
    intf.tag_lu_l1_array.resize(cltCnt);
    for (auto &tagLuL1Q : intf.tag_lu_l1_array) {
        l1Cache->tag_lu_l1_array.emplace_back(&tagLuL1Q);
    }
    intf.lookup_lu_l1_array.resize(cltCnt);
    for (auto &lookup_lu_l1_q : intf.lookup_lu_l1_array) {
        l1Cache->lookup_lu_l1_array.emplace_back(&lookup_lu_l1_q);
    }
    intf.lookup_lu_scb_array.resize(cltCnt);
    for (auto &lookup_lu_scb_q : intf.lookup_lu_scb_array) {
        l1Cache->lookup_lu_scb_array.emplace_back(&lookup_lu_scb_q);
    }
    intf.tag_lu_scb_array.resize(cltCnt);
    for (auto &tag_lu_scb_q : intf.tag_lu_scb_array) {
        l1Cache->tag_lu_scb_array.emplace_back(&tag_lu_scb_q);
    }
    intf.commit_su_scb_array.resize(cltCnt);
    for (auto &commit_su_scb_q : intf.commit_su_scb_array) {
        l1Cache->commit_su_scb_array.emplace_back(&commit_su_scb_q);
    }

    intf.tag_l1_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.lookup_l1_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.tag_scb_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.lookup_scb_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.wakeup_l1_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.wakeup_scb_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.pref_l1_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.upgrade_l1_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.lookup_lu_su_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.wait_lu_su_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.lookup_su_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.wait_su_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.bypass_su_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.detect_su_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.wakeup_su_lu_q.resize(GetSim()->core->configs.scalar_smt_thread);
    intf.lsuTMABlockCmdQ.resize(GetSim()->core->configs.scalar_smt_thread);

    for (uint32_t stid = 0; stid < GetSim()->core->configs.scalar_smt_thread; ++stid) {
        lu[stid].stid = stid;
        lu[stid].iexLsuLdaArray = iex_lsu_lda_array[stid];
        lu[stid].lsuIexLretArray = lsu_iex_lret_array[stid];

        su[stid].stid = stid;
        su[stid].iexLsuStaArray = iex_lsu_sta_array[stid];
        su[stid].iexLsuStdArray = iex_lsu_std_array[stid];
    }
    for (auto &loadUnit : lu) {
        for (auto &prefLuL1Q : intf.pref_lu_l1_array) {
            loadUnit.pref_lu_l1_array.emplace_back(&prefLuL1Q);
        }
        for (auto &tagLuL1Q : intf.tag_lu_l1_array) {
            loadUnit.tag_lu_l1_array.emplace_back(&tagLuL1Q);
        }
        for (auto &lookup_lu_l1_q : intf.lookup_lu_l1_array) {
            loadUnit.lookup_lu_l1_array.emplace_back(&lookup_lu_l1_q);
        }
        for (auto &lookup_lu_scb_q : intf.lookup_lu_scb_array) {
            loadUnit.lookup_lu_scb_array.emplace_back(&lookup_lu_scb_q);
        }
        for (auto &tag_lu_scb_q : intf.tag_lu_scb_array) {
            loadUnit.tag_lu_scb_array.emplace_back(&tag_lu_scb_q);
        }
    }
    for (uint32_t stid = 0; stid < GetSim()->core->configs.scalar_smt_thread; ++stid) {
        auto& storeUnit = su[stid];
        for (auto &commit_su_scb_q : intf.commit_su_scb_array) {
            storeUnit.commit_su_scb_array.emplace_back(&commit_su_scb_q);
        }

        storeUnit.lookup_lu_su_q = &intf.lookup_lu_su_q[stid];
        storeUnit.wait_lu_su_q = &intf.wait_lu_su_q[stid];
        storeUnit.lookup_su_lu_q = &intf.lookup_su_lu_q[stid];
        storeUnit.wait_su_lu_q = &intf.wait_su_lu_q[stid];
        storeUnit.bypass_su_lu_q = &intf.bypass_su_lu_q[stid];
        storeUnit.detect_su_lu_q = &intf.detect_su_lu_q[stid];
        storeUnit.wakeup_su_lu_q = &intf.wakeup_su_lu_q[stid];
        storeUnit.lookup_mdb_su_q = &intf.lookup_mdb_su_q;
        storeUnit.atomic_lu_su_q = &intf.atomic_lu_su_q;
        storeUnit.bccLsuTstoreCommitQ = intf.bccLsuTstoreCommitQ;
        for (auto& lsuBridgeTstoreQ : intf.lsuBridgeTstoreArray) {
            storeUnit.lsuBridgeTstoreArray.emplace_back(lsuBridgeTstoreQ);
        }
        storeUnit.bccTMABlockCmdQ = &intf.lsuTMABlockCmdQ[stid];
        storeUnit.tmaBCCWakeupQ = intf.tmaBCCWakeupQ;
        ASSERT(storeUnit.atomic_lu_su_q);
    }

    for (uint32_t stid = 0; stid < GetSim()->core->configs.scalar_smt_thread; ++stid) {
        auto &loadUnit = lu[stid];
        loadUnit.lookup_lu_su_q = &intf.lookup_lu_su_q[stid];
        loadUnit.wait_lu_su_q = &intf.wait_lu_su_q[stid];
        loadUnit.lookup_su_lu_q = &intf.lookup_su_lu_q[stid];
        loadUnit.wait_su_lu_q = &intf.wait_su_lu_q[stid];
        loadUnit.bypass_su_lu_q = &intf.bypass_su_lu_q[stid];
        loadUnit.detect_su_lu_q = &intf.detect_su_lu_q[stid];
        loadUnit.wakeup_su_lu_q = &intf.wakeup_su_lu_q[stid];
        loadUnit.pref_lu_q = intf.pref_lu_q;
        loadUnit.lu_pref_q = intf.lu_pref_q;
        loadUnit.lookup_lu_mdb_q = &intf.lookup_lu_mdb_q;
        loadUnit.delete_lu_mdb_q = &intf.delete_lu_mdb_q;
        loadUnit.record_lu_mdb_q = &intf.record_lu_mdb_q;
        loadUnit.tag_l1_lu_q = &intf.tag_l1_lu_q[stid];
        loadUnit.lookup_l1_lu_q = &intf.lookup_l1_lu_q[stid];
        loadUnit.tag_scb_lu_q = &intf.tag_scb_lu_q[stid];
        loadUnit.lookup_scb_lu_q = &intf.lookup_scb_lu_q[stid];
        loadUnit.wakeup_scb_lu_q = &intf.wakeup_scb_lu_q[stid];
        loadUnit.lookup_mdb_lu_q = &intf.lookup_mdb_lu_q;
        loadUnit.upgrade_l1_lu_q = &intf.upgrade_l1_lu_q[stid];
        loadUnit.atomic_lu_su_q = &intf.atomic_lu_su_q;
        loadUnit.wakeup_l1_lu_q = &intf.wakeup_l1_lu_q[stid];
        loadUnit.bccLsuTloadCommitQ = intf.bccLsuTloadCommitQ;
        for (auto& lsuBridgeTloadQ : intf.lsuBridgeTloadArray) {
            loadUnit.lsuBridgeTloadArray.emplace_back(lsuBridgeTloadQ);
        }
        loadUnit.bccTMABlockCmdQ = &intf.lsuTMABlockCmdQ[stid];
        loadUnit.tmaBCCWakeupQ = intf.tmaBCCWakeupQ;
        ASSERT(loadUnit.atomic_lu_su_q);
        // lu.snoop_l1_lu_q = &intf.snoop_l1_lu_q;
        loadUnit.pref_l1_lu_q = &intf.pref_l1_lu_q[stid];
        loadUnit.snoop_lu_l2_q = snp_l2_q; // TODO: snoop interface is in core
        loadUnit.lookup_lu_l2_q = intf.lookup_lu_l2_q;
    }

    mdb.lookup_lu_mdb_q = &intf.lookup_lu_mdb_q;
    mdb.delete_lu_mdb_q = &intf.delete_lu_mdb_q;
    mdb.record_lu_mdb_q = &intf.record_lu_mdb_q;
    mdb.lookup_mdb_lu_q = &intf.lookup_mdb_lu_q;
    mdb.lookup_mdb_su_q = &intf.lookup_mdb_su_q;

    for (uint32_t stid = 0; stid < GetSim()->core->configs.scalar_smt_thread; ++stid) {
        l1Cache->tag_l1_lu_q.push_back(&intf.tag_l1_lu_q[stid]);
        l1Cache->pref_l1_lu_q.push_back(&intf.pref_l1_lu_q[stid]);
        l1Cache->lookup_l1_lu_q.push_back(&intf.lookup_l1_lu_q[stid]);
        l1Cache->tag_scb_lu_q.push_back(&intf.tag_scb_lu_q[stid]);
        l1Cache->lookup_scb_lu_q.push_back(&intf.lookup_scb_lu_q[stid]);
        l1Cache->wakeup_scb_lu_q.push_back(&intf.wakeup_scb_lu_q[stid]);
        l1Cache->wakeup_l1_lu_q.push_back(&intf.wakeup_l1_lu_q[stid]);
        l1Cache->upgrade_l1_lu_q.push_back(&intf.upgrade_l1_lu_q[stid]);
    }
    l1Cache->lookup_l2_l1_q = intf.lookup_l2_l1_q;

    prefetcher.pref_l1_q = intf.pref_lu_q;
    prefetcher.pref_l2_q = intf.pref_l2_q;
    prefetcher.lu_pref_q = intf.lu_pref_q;
    prefetcher.l1_pref_q = intf.feedback_lu_pref_q;
    prefetcher.l2_pref_q = intf.l2_pref_q;

    l2Cache->l1_l2_q = intf.lookup_lu_l2_q;
    l2Cache->l2_l1_q = intf.lookup_l2_l1_q;
    l2Cache->pref_l2_q = intf.pref_l2_q;
    l2Cache->l2_pref_q = intf.l2_pref_q;

    lsu_flush_rpt_q = new SimQueue<FlushReq>();
}

void LoadStoreUnit::Work()
{
    if (configs.seq_mode) {
        // increase frequentcy for fast simulation
        fakeLSU.Work();
        fakeLSU.Work();
        fakeLSU.Work();
        fakeLSU.Work();
        fakeLSU.returnPE();
        return;
	}
    prefetcher.l2_data_allow = l2Cache->pfl2_allow() && !intf.pref_l2_q->getStall();

    while (!intf.bccTMABlockCmdQ->Empty()) {
        auto cmd = intf.bccTMABlockCmdQ->Read();
        intf.lsuTMABlockCmdQ[cmd->stid].Write(cmd);
    }
    for (auto& loadUnit :  lu) {
        loadUnit.Work();
    }
    for (auto& storeUnit : su) {
        storeUnit.Work();
    }
    mdb.Work();
    l1Cache->Work();
    l2Cache->Work();
    prefetcher.Work();

    stats->cycles++;
}

void LoadStoreUnit::Xfer()
{
    for (auto& loadUnit : lu) {
        loadUnit.Xfer();
    }
    for (auto& storeUnit : su) {
        storeUnit.Xfer();
    }
    mdb.Xfer();
    intf.Work();
    l1Cache->Xfer();
    l2Cache->Xfer();
    prefetcher.Xfer();
}

void LoadStoreUnit::setFlush(FlushBus flushReq)
{
    if (!flushReq.req.vld) {
        return;
    }

    if (configs.seq_mode) {
        ROBID ptr = flushReq.req.bid;
        for (uint32_t i = 0; i <fakeLSU.fakeLSU.size(); i++) {
            fakeLSU.fakeLSU[ptr.val].memSet.clear();
            fakeLSU.fakeLSU[ptr.val].oldest.val = 0;
            fakeLSU.fakeLSU[ptr.val].oldest.wrap = false;
            IncROBID(ptr, fakeLSU.fakeLSU.size());
            if (ptr.val == flushReq.old_alloc.val) break;
        }
        return;
    }

    for (auto& loadUnit : lu) {
        loadUnit.flush(flushReq);
    }
    for (auto& storeUnit : su) {
        storeUnit.flush(flushReq);
    }
}

bool LoadStoreUnit::l1Allow()
{
    return l1Cache->dataAllow();
}

bool LoadStoreUnit::l2Allow()
{
    return l2Cache->data_allow();
}

bool LoadStoreUnit::getVerbose()
{
    return verbose;
}

SimSys *LoadStoreUnit::GetSim()
{
    return sim;
}

void LoadStoreUnit::sendL1(std::vector<std::deque<MemReqBus>*> &busArray, MemReqBus &bus)
{
    uint32_t l1cID = l1Cache->addr2L1cID(bus.addr);
    busArray[l1cID]->push_back(bus);
}

bool LoadStoreUnit::sendSimL1(std::vector<SimQueue<MemReqBus>*> &busArray, MemReqBus &bus)
{
    uint32_t l1cID = l1Cache->addr2L1cID(bus.addr);
    if (busArray[l1cID]->getStall()) {
        return false;
    }

    busArray[l1cID]->Write(bus);
    return true;
}

bool LoadStoreUnit::checkSimStall(std::vector<SimQueue<MemReqBus>*> &busArray, MemReqBus &bus)
{
    uint32_t l1cID = l1Cache->addr2L1cID(bus.addr);
    return busArray[l1cID]->getStall();
}

void LoadStoreUnit::StatsMemBound(bool& anyload, bool& l1miss, bool& l2miss, bool& stqfull, uint32_t stid)
{
    // memstall_anyload: anyload is inflight
    // memstall_l1miss: any inflight load has missed L1
    // memstall_l2miss: any inflight load has missed L2
    // memstall_store: no more stores can be issued (?)
    if (configs.seq_mode) {
        return; // no LSU stall in seq mode
    }

    lu[stid].statsMemBound(anyload, l1miss, l2miss);
    stqfull = su[stid].checkStall();
}

void LoadStoreUnit::ReportStat()
{
    if (configs.seq_mode) {
        fakeLSU.stats->report("Fake LSU");
        return;
    }

    std::string preName = "Scalar";
    if (typeId == LSUType::VECTOR_LSU) {
        preName = "Vector";
    }
    stats->report(preName + " LSU");
}

void LoadStoreUnit::resetStats(void)
{
    if (configs.seq_mode) {
        fakeLSU.stats->Reset();
        return;
    }

    stats->Reset();
    l2Cache->stats->Reset();
}

void LoadStoreUnit::aaccelssSoftMemory(MemReqBus &memReq)
{
    if (!memReq.vld) return;
    switch (memReq.opcode) {
        case Opcode::OP_LDI:
        case Opcode::OP_LR_D:
        case Opcode::OP_LD_PCR:
            memReq.data = GetSim()->loadData(memReq.addr, 8, false);
            break;
        case Opcode::OP_LWI:
        case Opcode::OP_LR_W:
        case Opcode::OP_LW_PCR:
            memReq.data = GetSim()->loadData(memReq.addr, 4, true);
            break;
        case Opcode::OP_LHI:
        case Opcode::OP_LH_PCR:
            memReq.data = GetSim()->loadData(memReq.addr, 2, true);
            break;
        case Opcode::OP_LBI:
        case Opcode::OP_LB_PCR:
            memReq.data = GetSim()->loadData(memReq.addr, 1, true);
            break;
        case Opcode::OP_LWUI:
        case Opcode::OP_LWU_PCR:
            memReq.data = GetSim()->loadData(memReq.addr, 4, false);
            break;
        case Opcode::OP_LHUI:
        case Opcode::OP_LHU_PCR:
            memReq.data = GetSim()->loadData(memReq.addr, 2, false);
            break;
        case Opcode::OP_LBUI:
        case Opcode::OP_LBU_PCR:
            memReq.data = GetSim()->loadData(memReq.addr, 1, false);
            break;
        case Opcode::OP_SDI:
        case Opcode::OP_SC_D:
        case Opcode::OP_SD_PCR:
            GetSim()->storeData(memReq.addr, memReq.data, 8);
            break;
        case Opcode::OP_SWI:
        case Opcode::OP_SC_W:
        case Opcode::OP_SW_PCR:
            GetSim()->storeData(memReq.addr, memReq.data, 4);
            break;
        case Opcode::OP_SHI:
        case Opcode::OP_SH_PCR:
            GetSim()->storeData(memReq.addr, memReq.data, 2);
            break;
        case Opcode::OP_SBI:
        case Opcode::OP_SB_PCR:
            GetSim()->storeData(memReq.addr, memReq.data, 1);
            break;
        default:
            ASSERT(0 && "Unrecognized opcode in LSU");
            break;
    }
}

bool LoadStoreUnit::CheckLoadStall(uint32_t stid)
{
    return lu[stid].checkStall();
}

bool LoadStoreUnit::CheckLoadCltStall(uint64_t addr, uint32_t width, uint32_t stid)
{
    return lu[stid].checkCltStall(addr, width);
}

bool LoadStoreUnit::checkStoreStall(uint32_t stid)
{
    return su[stid].checkStall();
}

bool LoadStoreUnit::CheckStoreStall(uint32_t size, uint32_t stid)
{
    return su[stid].checkStall(size);
}

void LoadStoreUnit::countAddr(uint64_t addr)
{
    if (!configs.addr_cnt_enable) {
        return;
    }

    auto it = addrCounter.find(addr);
    if (it != addrCounter.end()) {
        it->second++;
    } else {
        stats->addrCount++;
        addrCounter.insert(std::pair<uint64_t, uint64_t>(addr, 0));
    }
}

std::vector<SimQueue<MemReqBus>*> &LoadStoreUnit::GetIEXQueue(vector<std::vector<SimQueue<MemReqBus>*>> &lsuQueue)
{
    if (typeId == LSUType::SCALAR_LSU) {
        return lsuQueue[SCALAR_IEX];
    }

    ASSERT(typeId == LSUType::VECTOR_LSU);
    return lsuQueue[SIMT_IEX];
}

bool LoadStoreUnit::CheckLoadQEmpty(uint32_t stid)
{
    return lu[stid].CheckLoadQEmpty();
}

LoadStoreUnit::~LoadStoreUnit()
{
    DeletePtr(lsu_flush_rpt_q);
}

} // namespace JCore
