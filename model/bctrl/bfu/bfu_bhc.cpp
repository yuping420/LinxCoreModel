#include "bctrl/bfu/bfu_bhc.h"

#include <algorithm>
#include <cstdint>

#include "../configs/bfu_config.h"
#include "bctrl/bfu/bfu.h"
#include "bctrl/bfu/bfu_utils.h"

namespace JCore {


using namespace std;
namespace NS_CORE {

BHC::BHC() {}
BHC::~BHC()
{
    DeletePtr(rep);
}

void BHC::Build() {
    rep = new NMRU(cfg->bhc_nset, cfg->bhc_nway);
    cache = vector<vector<CacheEntry>>{cfg->bhc_nset, vector<CacheEntry>{cfg->bhc_nway}};
    for (set_t s=0; s<cfg->bhc_nset; s++) {
        for (way_t w=0; w<cfg->bhc_nway; w++) {
            cache[s][w].set_idx = s;
            cache[s][w].way_idx = w;
        }
    }
}

bool BHC::fetch(PtrFB const& fb) {
    auto& info = fb->bhc_info;

    // this FB crosses n lines (when BFU_BANDWIDTH != BFU_ALIGNMENT)
    info.num_cl = utils->CalcCrossCLNum(fb->va);
    assert(info.num_cl && info.num_cl <= BFU_CROSS_CL_MAX);

    bool all_hit = true;
    // Lookup and fetch each of the n lines
    for (pos_t n=0; n<info.num_cl; n++) {
        addr_t va_cl = utils->AlignToNthCL(fb->va, n);
        addr_t pa_cl = utils->AlignToNthCL(fb->pa, n); // FIXME: suport for cross-page FBs
        info.va_cl[n] = va_cl;
        info.pa_cl[n] = pa_cl;

        auto hit_entry = Lookup(va_cl, pa_cl);
        bool hit = hit_entry != nullptr;

        if (cfg->bhc_perfect) {
            getHeader(fb, n);
        } else if (hit) {
            // update repl
            rep->touch(hit_entry->set_idx, hit_entry->way_idx);
            // update fb's bhc info
            info.hit[n] = true;
            info.set_idx[n] = hit_entry->set_idx;
            info.way_idx[n] = hit_entry->way_idx;
            // get header data
            getHeader(fb, n);
            if (cfg->debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F2) << dec << " hit bhc, fbid=" << fb->fbid
                    << " local fbid=" << fb->fbid_local << hex << " va=0x" << va_cl << dec << " nth_line" << n;
            }
            logger->debug("BHC", F2, "hit bhc, fbid=%d, local fid=%d, va=0x%llx, nth_line=%d\n",
                            fb->fbid, fb->fbid_local, va_cl, n);
        } else {
            // mark the current fb is causing cache stall
            if (fb->global) {
                stall_fb = fb;
            } else {
                stall_local_fb = fb;
            }
            auto missq_entry = lookupMissQ(pa_cl);
            if (missq_entry) {
                missq_entry->is_pf = false;
                if (cfg->debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F2) << dec << " hit missq, fbid=" << fb->fbid
                        << " local fbid=" << fb->fbid_local << hex << " va=0x" << va_cl << " pa=0x"
                        << pa_cl << dec << " nth_line" << n;
                }
                logger->debug("BHC", F2, "hit missq, fbid=%d, local fid=%d, va=0x%llx, pa=0x%llx, nth_line=%d\n",
                                fb->fbid, fb->fbid_local, va_cl, pa_cl, n);
            } else {
                // add to missq
                if (pa_cl != 0) {
                    missq.emplace_back(va_cl, pa_cl, fb->stid, false, bfu->GetSim()->getCycles());
                    if (cfg->debug_enable) {
                        LOG_INFO_M(Unit::BFU, Stage::F2) << dec << " miss missq, fbid=" << fb->fbid
                            << " local fbid=" << fb->fbid_local << hex << " va=0x" << dec << " size="
                            << missq.size() << dec << " nth_line" << n;
                    }
                    logger->debug("BHC", F2, "miss missq, fbid=%d, fbid_local=%d, va=0x%llx, size=%d, nth_line=%d\n",
                                    fb->fbid, fb->fbid_local, va_cl, missq.size(), n);
                }
            }
        }
        stats->bhc_aaccelss ++;
        stats->bhc_miss += !hit;
        if (!hit && !cfg->bhc_perfect) {
            all_hit = false;
        }
    }
    return all_hit;
}

bool BHC::prefetch(PtrHWPFInfo const& pfi) {
    assert(!cfg->bhc_perfect && "prefetch should be disabled in perfect BHC mode");
    auto hit_entry = Lookup(pfi->va, pfi->pa);
    bool hit = hit_entry != nullptr;

    if (hit) {
        // prefetch hit cache
        logger->debug("BHC", NIL, "prefetch hit bhc, va=%d, pa=%d\n", pfi->va, pfi->pa);
        // todo: add pf hit counter
    } else {
        auto missq_entry = lookupMissQ(pfi->va);
        if (missq_entry) {
            logger->debug("BHC", NIL, "prefetch hit missq, va=0x%llx, pa=0x%llx\n", pfi->va, pfi->pa);
            // todo: add pf hit counter
        } else {
            // add to missq
            if (missq.size() >= cfg->bhc_pf_nostd) {
                logger->debug("BHC", F2, "drop prefetch due to missq capacity, va=0x%llx, pa=0x%llx\n", pfi->va, pfi->pa);
            } else if (utils->AlignToCL(pfi->pa) != 0) {
                missq.emplace_back(utils->AlignToCL(pfi->va), utils->AlignToCL(pfi->pa), pfi->stid, true,
                    bfu->GetSim()->getCycles());
                logger->debug("BHC", F2, "add prefetch to missq, va=0x%llx, pa=0x%llx, size=%d\n", pfi->va, pfi->pa, missq.size());
            }
        }
    }

    return hit;
}

void BHC::SendMissReq() {
    for (auto& e : missq) {
        if (!e.sent) {
            auto req_type = e.is_pf? BFUReqType::BHC_PF : BFUReqType::BHC_DMD;
            bfu->RequestL2C(e.pa_cl, req_type);
            e.sent = true;
            if (cfg->debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F2) << dec << " send miss req, pa=0x" << hex << e.pa_cl
                    << " is_pf=" << dec << e.is_pf;
            }
            logger->debug("BHC", NIL, "send miss req, pa=0x%llx, is_pf=%d\n", e.pa_cl, e.is_pf);
            break;
        }
    }
}

bool BHC::invalidate(addr_t const pa) {
    // invalidate all copies using PA to avoid the aliasing probelm in VIPT cache
    bool suaccelss = false;
    for (auto& set : cache) {
        for (auto& e : set) {
            if (utils->AlignToCL(pa) == e.pa_cl && e.state != CacheState::I) {
                e.state = CacheState::I;
                rep->invalidate(e.set_idx, e.way_idx);
                suaccelss = true;
            }
        }
    }
    return suaccelss;
}

std::pair<bool, addr_t> BHC::refill(addr_t const pa_cl, uint512_t const& data) {
    // release all missq entries with matching PA in case downstream merges requests with the same PA
    bool suaccelss = false;
    bool victim_vld = false;
    addr_t victim_pa_cl = 0;
    list<MissQEntry>::iterator it = missq.begin();
    while (it != missq.end()) {
        if (it->pa_cl == pa_cl) {
            // get victim
            set_t set_idx = CalcSetIdx(it->va_cl);
            way_t way_idx = rep->getVictim(set_idx);
            // update cache and replacement policy
            auto& cache_entry = cache[set_idx][way_idx];
            if (cache_entry.state != CacheState::I) {
                victim_vld = true;
                victim_pa_cl = cache_entry.pa_cl;
            }
            cache_entry.pa_cl = pa_cl;
            cache_entry.state = CacheState::S;
            cache_entry.data = data;
            rep->insert(set_idx, way_idx);
            // release stall fb if pa match
            releaseStallFB(true, set_idx, way_idx, pa_cl);
            releaseStallFB(false, set_idx, way_idx, pa_cl);
            suaccelss = true;
            it = missq.erase(it);
        } else {
            it ++;
        }
    }

    assert(suaccelss && "refill should be in missq (unless in VIPT aliasing situation)");
    return std::make_pair(victim_vld, victim_pa_cl);
}

void BHC::releaseStallFB(bool global, set_t const set, way_t const way, addr_t const pa_cl) {
    auto& fb = global ? stall_fb : stall_local_fb;
    if (fb == nullptr) return;
    auto& info = fb->bhc_info;
    for (pos_t n=0; n<info.num_cl; n++) {
        if (pa_cl != info.pa_cl[n])
            continue;
        info.set_idx[n] = set;
        info.way_idx[n] = way;
        info.hit[n] = true;
        getHeader(fb, n);
        if (info.allLinesHit()) {
            if (global) {
                stall_fb = nullptr;
            } else {
                stall_local_fb = nullptr;
            }
            break;
        }
    }
}

PtrFB BHC::getStallLocalFB() {
    return stall_local_fb;
}

bool BHC::paMatch(PtrFB const& fb, addr_t const pa_cl) {
    if (fb == nullptr) return false;
    auto& info = fb->bhc_info;
    for (pos_t n=0; n<info.num_cl; n++) {
        if (pa_cl == info.pa_cl[n]) {
            return true;
        }
    }
    return false;
}

bool BHC::needStall() {
    assert(missq.size() <= cfg->bhc_nostd);
    if (cfg->bhc_timeout_check) {
        auto curr_cycle = bfu->GetSim()->getCycles();
        for (auto& e : missq) {
            if (curr_cycle - e.enqueue_time > 10000) {
                printf("@%lu BHC missq timeout, va=0x%lx, pa=0x%lx, sent=%d, pf=%d\n",
                    curr_cycle, e.va_cl, e.pa_cl, e.sent, e.is_pf);
                assert(0);
            }
        }
    }

    LOG_INFO_M(Unit::BFU, Stage::NA) << "BHC Check Stall:, stall_fb=" << boolalpha << (stall_fb != nullptr)
                                     << ", missq overflow=" << (missq.size() + BFU_CROSS_CL_MAX > cfg->bhc_nostd);
    if (stall_fb) {
        LOG_INFO_M(Unit::BFU, Stage::NA) << "stall_fb: {fbid: " << dec << stall_fb->fbid << ", stid: " << dec
                                         << stall_fb->stid << ", addr: 0x" << hex << stall_fb->va;
    }

    return stall_fb || (missq.size() + BFU_CROSS_CL_MAX > cfg->bhc_nostd);
}

bool BHC::LocalFetchStall() {
    return stall_local_fb || (missq.size() + BFU_CROSS_CL_MAX * 2 > cfg->bhc_nostd);
}

void BHC::flush(uint32_t stid) {
    // clear stall_fb and all unsent requests
    if (stall_fb && stall_fb->stid == stid) {
        stall_fb = nullptr;
    }
    if (stall_local_fb && stall_local_fb->stid == stid) {
        stall_local_fb = nullptr;
    }
    missq.erase(remove_if(missq.begin(), missq.end(), [stid](MissQEntry& e){return !e.sent && e.stid == stid;}),
        missq.end());
}

void BHC::flushLocal(PtrFB fb) {
    if (stall_local_fb && fb && stall_local_fb->fbid == fb->fbid && stall_local_fb->fbid_local == fb->fbid_local) {
        uint32_t stid = fb->stid;
        missq.erase(remove_if(missq.begin(), missq.end(),
            [this, stid](MissQEntry& e) {
                return !e.sent && e.stid == stid && paMatch(stall_local_fb, e.pa_cl) && !paMatch(stall_fb, e.pa_cl);
            }),
            missq.end());
        if (stall_local_fb && stall_local_fb->stid == fb->stid) {
            stall_local_fb = nullptr;
        }
    }
}

void BHC::flushGlobal(uint32_t stid) {
    if (stall_fb) {
        missq.erase(remove_if(missq.begin(), missq.end(),
            [this, stid](MissQEntry& e) {
                return !e.sent && e.stid == stid && !paMatch(stall_local_fb, e.pa_cl) && paMatch(stall_fb, e.pa_cl);
            }),
            missq.end());
        if (stall_fb && stall_fb->stid == stid) {
            stall_fb = nullptr;
        }
    }
}

set_t BHC::CalcSetIdx(addr_t const a) {
    return utils->CalcCLIdx(a) & (cfg->bhc_nset - 1);
}

BHC::CacheEntry* BHC::Lookup(addr_t va, addr_t pa) {
    set_t set_idx = CalcSetIdx(va);
    CacheEntry* hit_entry = nullptr;
    for (auto& e : cache[set_idx]) {
        if (e.state != CacheState::I && utils->AlignToCL(pa) == e.pa_cl && rep->isValid(e.set_idx, e.way_idx)) {
            hit_entry = &e;
            break;
        }
    }
    return hit_entry;
}

BHC::MissQEntry* BHC::lookupMissQ(addr_t pa) {
    MissQEntry* hit_entry = nullptr;
    for (auto& e : missq) {
        if (e.pa_cl == utils->AlignToCL(pa)) {
            hit_entry = &e;
            break;
        }
    }
    return hit_entry;
}

void BHC::getHeader(PtrFB const& fb, pos_t nth_line) {
    // static_assert(HEADER_SIZE==8, "the following codes works only when header size is 8B");
    // get the headers from the nth cacheline of this FB to the proper slots
    pos_t pos_start = utils->CalcPosInFB(std::max(utils->AlignToNthCL(fb->va, nth_line), fb->va), fb->va);
    pos_t pos_end = std::min(utils->CalcPosInFB(utils->AlignToNthCL(fb->va, nth_line+1), fb->va), pos_t(BFU_BANDWIDTH));
    assert(pos_start >= 0 && pos_start < BFU_BANDWIDTH);
    assert(pos_end > 0 && pos_end <= BFU_BANDWIDTH);
    for (pos_t pos=pos_start; pos<pos_end; pos++) { // pos in FB
        addr_t va = utils->CalcPC(fb->va, pos);
        if (va < fb->va) continue;
        if (cfg->bhc_perfect) {
            uint64_t data = bfu->GetSim()->fetchData(va, MIN_BUNDLE_SIZE);
            fb->bin[pos].vld = true;
            fb->bin[pos].bin = data;
        } else {
            CacheEntry* entry = &cache[fb->bhc_info.set_idx[nth_line]][fb->bhc_info.way_idx[nth_line]];
            assert(entry->state != CacheState::I);
            uint32_t ndwords = (va & (BFU_CL_NBYTE - 1)) >> 3; // header position in cache data bits[8]
            uint64_t data = entry->data.bits[ndwords];
            fb->bin[pos].vld = true;
            // header size 16 bits
            fb->bin[pos].bin = (uint16_t)((data >> (8 * MIN_BUNDLE_SIZE * ((va - (va >> 3 << 3)) >> 1))) & 0xffff);
        }
    }
    stats->bhc_total_lat += (bfu->GetSim()->getCycles() - fb->create_time);
}

}   // namespace NS_CORE

} // namespace JCore
