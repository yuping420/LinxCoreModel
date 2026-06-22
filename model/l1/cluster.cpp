#include "l1/cluster.h"

#include "core/Bus.h"
#include "l1/clusters_top.h"

namespace JCore {
using namespace std;
void L1Clusters::Reset(void)
{
    dcache.Reset();
    scb.Reset();

    tag_lu_l1_q->clear();
    lookup_lu_l1_q->clear();
    tag_lu_scb_q->clear();
    lookup_lu_scb_q->clear();
    resp_l2_l1_q->clear();
    for (auto &q : wakeup_scb_lu_q) {
        q->Reset();
    }
    for (auto &q : top->tag_l1_lu_q) {
        q->clear();
    }
    for (auto &q : top->lookup_l1_lu_q) {
        q->clear();
    }
    for (auto &q : top->tag_scb_lu_q) {
        q->clear();
    }
    for (auto &q : top->lookup_scb_lu_q) {
        q->clear();
    }
}

void L1Clusters::Work(void)
{
    scb.Work();
    queryByTag();
    lookupCache();
    refillCache();
    lookupSCB();
    commitStore();
}

void L1Clusters::Xfer(void)
{
    scb.Xfer();
    allocCachePort();
}

void L1Clusters::Build(void)
{
    stats = std::make_shared<LSUStats>();

    dcache.cluster = this;
    dcache.pConfigs = pConfigs;
    dcache.sim = GetSim();
    dcache.lsuConfigs = top->lsuConfigs;
    dcache.debugLogger = top->debugLogger;
    dcache.Build();

    scb.cluster = this;
    scb.scbID = cID;
    scb.memID_s = memID_s;
    scb.dcache = &dcache;
    scb.configs = &top->configs;
    scb.stats = stats;
    scb.sim = GetSim();
    scb.lsuConfigs = top->lsuConfigs;
    scb.debugLogger = top->debugLogger;
    scb.pkt_out_q = pkt_out_q;
    scb.wakeup_scb_lu_q = wakeup_scb_lu_q;
    scb.top = this;
    scb.Build();
}

SimSys *L1Clusters::GetSim(void)
{
    return sim;
}

bool L1Clusters::hasDCacheReq(void)
{
    return !lookup_lu_l1_q->empty();
}

void L1Clusters::allocCachePort(void) {
    // Priorty: refill > lookup
    if (!resp_l2_l1_q->empty()) {
        dcacheAllow = false;
        return;
    }
    dcacheAllow = true;
}

void L1Clusters::queryByTag(void)
{
    if (!dcacheAllow) {
        return;
    }

    for (uint32_t i = 0; i < pConfigs->cluster_width && !tag_lu_l1_q->empty(); ++i) {
        MemReqBus bus = tag_lu_l1_q->front();
        tag_lu_l1_q->pop_front();
        handleTagQuery(bus);
    }

    for (uint32_t i = 0; i < pConfigs->cluster_width && !pref_lu_l1_q->empty(); ++i) {
        MemReqBus bus = pref_lu_l1_q->front();
        pref_lu_l1_q->pop_front();
        handleTagQuery(bus);
    }
}

void L1Clusters::lookupCache(void)
{
    if (!dcacheAllow) {
        return;
    }

    for (uint32_t i = 0; i < pConfigs->cluster_width && !lookup_lu_l1_q->empty(); ++i) {
        MemReqBus bus = lookup_lu_l1_q->front();
        lookup_lu_l1_q->pop_front();
        handleDCacheLookup(bus);
    }
}

void L1Clusters::lookupSCB(void)
{
    for (uint32_t i = 0; i < pConfigs->cluster_width && !tag_lu_scb_q->empty(); ++i) {
        MemReqBus bus = tag_lu_scb_q->front();
        tag_lu_scb_q->pop_front();
        handleSCBLookup(bus, true);
    }

    for (uint32_t i = 0; i < pConfigs->cluster_width && !lookup_lu_scb_q->empty(); ++i) {
        MemReqBus bus = lookup_lu_scb_q->front();
        lookup_lu_scb_q->pop_front();
        handleSCBLookup(bus, false);
    }
}

void L1Clusters::refillCache(void)
{
    for (uint32_t i = 0; i < pConfigs->cluster_width && !resp_l2_l1_q->empty(); ++i) {
        PtrPacket pkt = resp_l2_l1_q->front();
        LOG_INFO_M(Unit::LSU, Stage::NA) << "L1 recv resp pkt: " << *pkt;
        bool anyBlock = false;
        for (auto it = top->wakeup_l1_lu_q.begin(); it != top->wakeup_l1_lu_q.end(); ++it) {
            auto* ptr = *it;
            constexpr uint32_t entryTaken = 1;
            if (ptr->toBeOverflow(entryTaken)) {
                anyBlock = true;
            }
        }
        if (pkt->stid == -1U && anyBlock) {
            break;
        }
        if (pkt->stid != -1U && top->wakeup_l1_lu_q[pkt->stid]->toBeOverflow(1)) {
            break;
        }

        resp_l2_l1_q->pop_front();
        handleRefill(pkt);
    }
}

void L1Clusters::commitStore(void)
{
    for (uint32_t i = 0; i < pConfigs->cluster_width && !commit_su_scb_q->Empty() && !scb.full(); ++i) {
        MemReqBus bus = commit_su_scb_q->Read();
        scb.insert(bus.addr, bus.size, bus.data);
        LOG_INFO_M(Unit::LSU, Stage::NA) << "[CLT" << scb.scbID << "]: store " << bus
                                         << " is written to SCB" << dec << scb.scbID << ". tag 0x" << hex
                                         << (bus.addr&~0x3f) << ", addr 0x" << bus.addr << ", size 0x" << bus.size
                                         << ", data 0x" << bus.data << dec << ". Req: " << bus;
    }
    if (scb.full() || !commit_su_scb_q->Empty()) {
        commit_su_scb_q->setStall();
    } else {
        commit_su_scb_q->unsetStall();
    }
}

void L1Clusters::handleTagQuery(MemReqBus &bus)
{
    bus.l1_miss = !dcache.queryByTag(bus.tag);
    LOG_INFO_M(Unit::L1D, Stage::NA) << "L1 [CLT " << cID << "]: query tag" << bus << ", miss:" << bus.l1_miss;

    if (bus.prefetch) {
        if (bus.stid == -1U) {
            for (auto &q : top->pref_l1_lu_q) {
                q->push_back(bus);
            }
        } else {
            top->pref_l1_lu_q[bus.stid]->push_back(bus);
        }
        return;
    }

    if (bus.stid == -1U) {
        for (auto &q : top->tag_l1_lu_q) {
            q->push_back(bus);
        }
    } else {
        top->tag_l1_lu_q[bus.stid]->push_back(bus);
    }
}

void L1Clusters::handleDCacheLookup(MemReqBus &bus)
{
    if (!dcache.lookup(bus.tag)) {
        bus.data_vld = false;
    } else {
        uint512_t data = dcache.getData(bus.tag);
        bus.reqData.insertCacheData(data);
        bus.data_vld = true;
    }
    if (bus.stid == -1U) {
        for (auto &q : top->lookup_l1_lu_q) {
            q->push_back(bus);
        }
    } else {
        top->lookup_l1_lu_q[bus.stid]->push_back(bus);
    }

    std::string info = bus.data_vld ? bus.reqData.data.toStr() : " miss";
    LOG_INFO_M(Unit::LSU, Stage::NA) << "L1 [CLT" << cID << "]: lookup L1 D-Cache, " << info << ", " << bus;
}

void L1Clusters::handleSCBLookup(MemReqBus &bus, bool checkOnly)
{
    // SCB
    bool hit = false;
    uint8_t *scbData;
    bool *scbValid;
    bus.reqData.Reset();
    // TODO: TLOAD check SCB here
    // if (bus.tile.vld) {
    //     uint8_t *data;
    //     bool *valid;
    //     uint32_t ldSize = bus.size;
    //     uint64_t addr = bus.addr;
    //     while (ldSize != 0) {
    //         if (scb.lookup(CalTag(addr), &data, &valid)) {
    //             cerr << bus << endl;
    //             ASSERT(0 && "TLOAD conflict with SCB");
    //         }
    //         ldSize = ldSize > (0x40 - (addr & 0x3f)) ? ldSize - (0x40 - (addr & 0x3f)) : 0;
    //         addr = CalTag(addr) + 0x40;
    //     }
    // }
    if (scb.lookup(bus.tag, &scbData, &scbValid)) {
        UpdateData(bus.reqData.data, scbData, scbValid);
        for (uint32_t i = 0; i < pConfigs->dcache_way_size; i++) {
            bus.reqData.positionVld[i] = scbValid[i];
        }
        hit = true;
    }

    auto commit_read_q = commit_su_scb_q->GetRawReadData();
    for (auto &st : commit_read_q) {
        // Iterate from old to young
        if (AddrOverlap(st.addr, st.size, bus.addr, bus.size)) {
            // TODO: TLOAD check SCB commitQ here
            // if (bus.tile.vld) {
            //     cerr << bus << endl;
            //     ASSERT(0 && "TLOAD conflict with SCB");
            // }
            if (OpcodeManager::Inst().GetOpcodeGroup(st.opcode) == InstGroup::CACHE_MAINTAIN) {
                bus.reqData.zero();
                hit = true;
                continue;
            }
            UpdateData(bus.reqData.data, st.addr, st.size, st.data, bus.tag);
            UpdateSTValid(bus.reqData.positionVld, st.addr, st.size, true, bus.tag);
            hit = true;
        }
    }
    auto commit_write_q = commit_su_scb_q->GetRawWriteData();
    for (auto &st : commit_write_q) {
        // Iterate from old to young
        if (AddrOverlap(st.addr, st.size, bus.addr, bus.size)) {
            // TODO: TLOAD check SCB commitQ here
            // if (bus.tile.vld) {
            //     cerr << bus << endl;
            //     ASSERT(0 && "Tload conflict with SCB");
            // }
            if (OpcodeManager::Inst().GetOpcodeGroup(st.opcode) == InstGroup::CACHE_MAINTAIN) {
                bus.reqData.zero();
                hit = true;
                continue;
            }

            UpdateData(bus.reqData.data, st.addr, st.size, st.data, bus.tag);
            UpdateSTValid(bus.reqData.positionVld, st.addr, st.size, true, bus.tag);
            hit = true;
        }
    }
    bus.data_vld = hit;

    if (checkOnly) {
        if (bus.stid == -1U) {
            for (auto &q : top->tag_scb_lu_q) {
                q->push_back(bus);
            }
        } else {
            top->tag_scb_lu_q[bus.stid]->push_back(bus);
        }
    } else {
        if (bus.stid == -1U) {
            for (auto &q : top->lookup_scb_lu_q) {
                q->push_back(bus);
            }
        } else {
            top->lookup_scb_lu_q[bus.stid]->push_back(bus);
        }
    }
    std::string info = bus.data_vld ? bus.reqData.data.toStr() : " miss";
    LOG_INFO_M(Unit::LSU, Stage::NA) << "L1 [CLT" << cID << "]: lookup SCB, " << info << ", " << bus;
}

void L1Clusters::handleRefill(PtrPacket &pkt)
{
    pkt->tid = pkt->tid >> 2;
    if ((pkt->isWrite() || pkt->isUpgrade()) && pkt->isResp()) {
        LOG_INFO_M(Unit::LSU, Stage::NA) << "L1 [CLT" << cID << "]: Response to SCB addr 0x" << hex << pkt->addr << dec;
        scb.setMemResp(pkt);
    }

    bool duplicate = dcache.lookup(pkt->addr);
    pkt->data = duplicate ? dcache.getData(pkt->addr) : pkt->data;
    if (pkt->stid != -1U) {
        top->wakeup_l1_lu_q[pkt->stid]->Write(pkt);
    } else {
        for (auto &q : top->wakeup_l1_lu_q) {
            q->Write(pkt);
        }
    }

    if (pkt->isDup()) {
        LOG_INFO_M(Unit::LSU, Stage::NA) << "L1 [CLT" << cID << "]: pkt 0x" << std::hex << pkt->addr
            << " is duplicated in L1";
		return;
	}

    if (pkt->isUpgrade()) {
        bool r;
        r = dcache.upgrade(pkt->addr);
        LOG_INFO_M(Unit::LSU, Stage::NA) << "L1 [CLT" << cID << "]: addr 0x" << std::hex << pkt->addr << dec
            << " upgrade fails";
        stats->dcache_upgrade_count++;

        if (r) {
            return;
        }
    }

    LOG_INFO_M(Unit::LSU, Stage::NA) << "L1 [CLT" << cID << "]: packet " << *pkt << " is refilled";

    // Handle read resp , write resp or upgrade resp
    CacheState cs;
    if (pkt->isExcl()) {
        cs = CS_EXCL;
    } else {
        cs = CS_SHARE;
    }

    if (pConfigs->l1d_l2_inclusion_policy == "WI") {
        if (duplicate) {
            LOG_INFO_M(Unit::LSU, Stage::NA) << "L1 [CLT" << cID << "]: pkt (0x" << pkt->addr
                << ") is already in DCache";
            return;
        }
    }
    L1Entry repl = dcache.refill(pkt->addr, pkt->data.bits, cs, pkt);
    stats->dcache_refill_count++;

    if (repl.state != CS_INV) {
        bool dirty = repl.state == CS_MOD;
        LOG_INFO_M(Unit::LSU, Stage::NA) << "L1 [CLT" << cID << "]:" << (dirty ? "dirty" : "clean") << " block (0x"
            << std::hex << repl.addr << ") is evict from DCache";
        if (pConfigs->l1d_l2_inclusion_policy == "WI" && !dirty) {
            return;
        }
        // Send writeback entry
        pkt = Packet::createWBPkt(dirty);
        if (repl.state == CS_EXCL) {
            pkt->setExcl();
        }
        pkt->id = top->memID_s;
        pkt->addr = repl.addr;
        pkt->size = pConfigs->dcache_way_size;
        pkt->data = repl.data;
        pkt->user_type = repl.pref_type;
        if (repl.prefetch) {
            pkt->setPref();
        }
        pkt->demand = repl.demand;
        if (pkt->stid == -1U) {
            for (auto &q : top->wakeup_l1_lu_q) {
                q->Write(pkt);
            }
        } else {
            top->wakeup_l1_lu_q[pkt->stid]->Write(pkt);
        }
    }
}

void L1Clusters::broadcastUpgrade(uint64_t addr, uint512_t &data, bool *p)
{
     // when SCB upgrade
    MemReqBus bus;
    bus.vld = true;
    bus.addr = addr & ~0x3f;
    bus.tag = addr & ~0x3f;
    bus.size = pConfigs->dcache_way_size;
    bus.data_vld = true;
    bus.reqData.insertValidCacheData(data, p);
    for (auto &q : top->upgrade_l1_lu_q) {
        q->Write(bus);
    }
}

bool L1Clusters::dataAllow()
{
    return dcacheAllow;
}

} // namespace JCore
