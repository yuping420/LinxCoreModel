#include "bctrl/bfu/bfu.h"

#include <cstdint>
#include <list>
#include <memory>
#include <vector>

#include "bctrl/bfu/bfu_common.h"
#include "lsu/lsu.h" // fixme: use interface

namespace JCore {


using namespace std;

namespace NS_CORE {

bool BFU::MisAtF4(const PtrFB &fb) {
    if (fb == nullptr) {
        return false;
    }
    // Use loop buffer
    if (fb->lb_info.hit) {
        return true;
    }
    // F1/F4 stage direction or target is different
    if (fb->main_info.taken != fb->ubtb_info.taken) {
        return true;
    }
    if (fb->main_info.taken && fb->ubtb_info.taken && fb->main_info.tgt != fb->ubtb_info.tgt) {
        return true;
    }
    // F1/F4 taken position is different
    if (fb->main_info.taken && fb->ubtb_info.taken && fb->main_info.taken_pc != fb->ubtb_info.taken_pc) {
        return true;
    }
    // F1/static-preditor taken type is differnt
    if (fb->main_info.taken && fb->ubtb_info.taken && fb->ubtb_info.taken_type != fb->main_info.taken_type) {
        // CALL blocks
        return !(utils.IsCallOnly(fb->ubtb_info.taken_type) && utils.IsDirectOnly(fb->main_info.taken_type));
    }
    return false;
}

void BFUComponent::RegisterComponent(BFU* bfu) {
    this->bfu = bfu;
    cfg = &bfu->cfg;
    stats = bfu->stats; // use ptr of BCtrl.stats
    logger = &bfu->logger;
    utils = &bfu->utils;
    Build();
}

BFU::~BFU() {}

void BFU::Build() {
    cfg.overrideDefaultConfig(GetSim()->getCfgs());

    for (auto &p : pipe) {
        p.Build(cfg.bfu_ntaken);
    }

    local_fu.resize(cfg.local_pipe_num);
    for (auto &local : local_fu) {
        for (auto &p : local.pipe) {
            p.Build(cfg.bfu_ntaken);
        }
    }

    logger.RegisterComponent(this);
    ubtb.RegisterComponent(this);
    pbtb.RegisterComponent(this);
    bhc.RegisterComponent(this);
    btlb.RegisterComponent(this);
    ras.resize(GetSim()->core->configs.scalar_smt_thread);
    for (auto &r : ras) {
        r.RegisterComponent(this);
    }
    ibtb.RegisterComponent(this);
    bim.RegisterComponent(this);
    brq.RegisterComponent(this);
    sp.RegisterComponent(this);
    hwpf.RegisterComponent(this);
    lp.RegisterComponent(this);
    lb.RegisterComponent(this);
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        tage[i].SetID(i);
        tage[i].RegisterComponent(this);
        ghrq[i].SetID(i);
        ghrq[i].RegisterComponent(this);
    }

    sp.Build(cfg.local_pipe_num, GetSim()->core->configs.scalar_smt_thread);

    brq.top = this;
    fetchThQ = ThdRingQ<PtrFB>(GetSim()->core->configs.scalar_smt_thread, cfg.fetch_depth);
}

void BFU::Reset() {
}

void BFU::Xfer()
{
}

void BFU::Dump()
{
    uint32_t tid = 0;
    cout << "================ fetchThQ Dump ====================" << endl;
    for (auto &q : fetchThQ.GetData()) {
        cout << "stid=" << dec << tid << endl;
        uint64_t ptr = 0;
        bool hasContent = false;
        for (auto& content : q.ringQ) {
            if (content) {
                cout << "ptr=" << dec << ptr << ", content: {fbid: " << content->fbid << ", stid: " << content->stid << "}\n";
                hasContent = true;
            }
            ++ptr;
        }
        if (!hasContent) {
            cout << "empty\n";
        }
        ++tid;
    }
    cout << "================ fetchThQ Dump End ================" << endl;
    DumpPipeStatus();
}

void BFU::Work() {
    if (cfg.bcache_verbose) {
        cout<<endl<<"Cycle : "<<dec<<GetSim()->getCycles()<<", correctBCount"<<GetSim()->correctBCount<<endl;
    }
    PrintPipeState();
    lb.printLB();
    RunAtPre();
    if (pipe[F4].state == NS_CORE::PipeState::INVALID && lb.useLoopBuffer()) {
        if (intf.be_bfu_nuke || brq.HasRedirect()) {
            if (cfg.bcache_verbose) {
                cout << "[BFU LB]: redirect LoopBuffer due to nuke or missBP"<<endl;
            }
            lb.stopLBFetch();
        } else {
            RunLoopBuffer();
            return;
        }
    }
    RunF4();
    RunF3();
    RunF2();
    RunF1();
    RunF0();

    PipeCtrl();

    PrintPipeState();
    PipeMove();
}

void BFU::RunAtPre() {
    if (L2CAvailable()) {
        bhc.SendMissReq();
    }

    HandleL2Pkt();
    HandleBEMsg();
}

bool BFU::L2CAvailable() {
    // FIXME: use interface!
    // TODO (sufang): use credit mechanism?
    bool available = GetSim()->core->memIntf[static_cast<int>(LSUType::SCALAR_LSU)]->l2Cache->inst_allow();
    logger.debug("TOP", NIL, "L2C available = %d\n", available);
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::NA) << "TOP L2C available = " << boolalpha << available;
    }
    return available;
}

void BFU::RequestL2C(addr_t pa_cl, BFUReqType req_type) {
    PtrPacket pkt = Packet::CreateRWPkt(false, 0);
    pkt->addr = pa_cl;
    pkt->size = BFU_CL_NBYTE;
    pkt->src = Packet::Requestor::BCtrl;
    pkt->id = 1; // TODO (sufang): remove this after all pkt->id related codes are refactored
    pkt->user_type = req_type;
    pkt->l1_out_cycle = GetSim()->getCycles();
    intf.bfu_l2_q->Write(pkt);
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::NA) << "TOP request L2C, pa=0x" << hex << pa_cl;
    }
    logger.debug("TOP", NIL, "request L2C, pa=0x%x, type=%d\n", pa_cl, req_type);
    if (cfg.report_header_footprint) {
        if (req_type == BFUReqType::BHC_DMD || req_type == BFUReqType::BHC_PF) {
            hdr_fp_spec.insert(pa_cl);
        }
        if (req_type == BFUReqType::BHC_PF) {
            hdr_fp_pref.insert(pa_cl);
        }
    }
}

void BFU::HandleL2Pkt() {
    // TODO: BTLB recv
    while (!intf.l2_bfu_q->Empty()) {
        // packet uses physical address
        PtrPacket pkt = intf.l2_bfu_q->Read();
        assert(pkt->id == 1);
        if (pkt->addr != 0) {
            HandleL2SinglePkt(pkt);
        }
    }
}

void BFU::HandleL2SinglePkt(PtrPacket const& pkt)
{
    if (pkt->id == 1) {
        if (pkt->isInv()) {
            if (bhc.invalidate(pkt->addr)) {
                // create a response packet
                // FIXME (sufang): createResp doesn't hide any information!
                PtrPacket resp = Packet::createInvResp(0);
                resp->tid = pkt->tid;
                resp->addr = pkt->addr;
                resp->size = pkt->size;
                resp->src = pkt->src;
                resp->id = pkt->id;
                resp->user_type = pkt->user_type;
                intf.snp_l2_q->Write(resp);
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::NA) << "TOP L2C header invalidate, addr=0x" << hex << pkt->addr;
                }
                logger.debug("TOP", NIL, "L2C header invalidate, addr=0x%x\n", pkt->addr);
            }
        } else if (pkt->isRead()) {
            bool victim_vld;
            addr_t victim_pa_cl;
            bool gStall = bhc.needStall();
            std::tie(victim_vld, victim_pa_cl) = bhc.refill(pkt->addr, pkt->data);
            PtrFB fb = bhc.getStallLocalFB();
            if (fb == nullptr && select_info.vld) {
                select_info.stall = false;
            }
            if (gStall && !bhc.needStall()) {
                globalL2Reurn = true;
            }
            if (victim_vld && cfg.bhc_l2_inclusion_policy != "WI") {
                PtrPacket resp = Packet::createWBPkt(false);
                resp->tid = pkt->tid;
                resp->addr = victim_pa_cl;
                resp->size = pkt->size;
                resp->src = pkt->src;
                resp->id = pkt->id;
                resp->user_type = pkt->user_type;
                intf.bfu_l2_q->Write(resp);
            }
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::NA) << "TOP L2C header refill, addr=0x" << hex << pkt->addr;
            }
            logger.debug("TOP", NIL, "L2C header refill, addr=0x%x\n", pkt->addr);
        }
    }
}

void BFU::DeliverInst(PtrMachineInst const& h) {
    // FIXME (sufang): this is a pass through interface ...
    ASSERT(h->stid < intf.bfu_be_q.size());
    intf.bfu_be_q[h->stid]->Write(h);
    if (cfg.bcache_verbose) {
        printf("send inst, fbid=%d, fbid_local=%d, hid=%d, pc=0x%lx, size=%d, type=%s, h\n",
                h->bfuInfo->fbid, h->bfuInfo->fbid_local, h->bfuInfo->hid, h->pc, h->bfuInfo->spInfo->hsize, GetBlockBranchTypeName(h->GetBranchType()).c_str());
    }
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::NA) << " TOP send inst, fbid=" << dec << h->bfuInfo->fbid
            << ", fbid_local=" << h->bfuInfo->fbid_local << ", hid=" << h->bfuInfo->hid
            << hex << ", pc=0x" << h->pc << ", size=" << h->bfuInfo->spInfo->hsize
            << ", type=" << GetBlockBranchTypeName(h->GetBranchType()).c_str() << ", inst="
            << h->bfuInfo->spInfo->isInst << ", last=" << h->bfuInfo->spInfo->isLast;
    }
    logger.debug("TOP", NIL, "send header, fbid=%d, fbid_local=%d, hid=%d, pc=0x%x, size=%d, type=%s, inst=%d\n",
                h->bfuInfo->fbid, h->bfuInfo->fbid_local, h->bfuInfo->hid, h->GetBundlePosPC(), h->bfuInfo->spInfo->hsize, GetBlockBranchTypeName(h->GetBranchType()).c_str(), h->bfuInfo->spInfo->isInst);
}

void BFU::HandleBEMsg() {
    PtrMachineInst h;
    if (!intf.be_bfu_nuke_info_q->Empty()) {
        NukeInfo info = intf.be_bfu_nuke_info_q->Read();
        brq.ReportFlush(info.fbid, info.fbid_local, info.pc, info.stid);
    }

    for (auto &rslvQ : intf.be_bfu_rslv_q) {
        auto &q = rslvQ->GetRawReadData();
        for (auto it = q.begin(); it != q.end();) {
            h = (*it);
            if (brq.ResolveHeader(h)) {
                it = q.erase(it);
            } else {
                ++it;
            }
        }
    }
    brq.UpdateInorder();

    for (auto &q : intf.be_bfu_cmt_q) {
        while (!q->Empty()) {
            h = q->Read();
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::NA) << "TOP commit header, va=0x" << hex << h->GetBundlePosPC()
                    << dec << ", " << h->bfuInfo->hid << ", fbid=" << h->bfuInfo->fbid
                    << ", fbid_local=" << h->bfuInfo->fbid_local
                    << ", type=" << GetBlockBranchTypeName(h->GetBranchType()).c_str()
                    << ", misp=" << h->bfuInfo->resolve_mispredict << ", cmt_cnt=" << commit_cnt;
            }
            logger.debug("TOP", NIL, "commit header, va=0x%x, hid=%d, fbid=%d, fbid_local=%d, type=%s, misp=%d, cmt_cnt=%d\n",
                h->GetBundlePosPC(), h->bfuInfo->hid, h->bfuInfo->fbid, h->bfuInfo->fbid_local, GetBlockBranchTypeName(h->GetBranchType()).c_str(), h->bfuInfo->resolve_mispredict, commit_cnt);
            auto fb = brq.CommitHeader(h);
            ASSERT(fb->stid < ras.size());
            ras[fb->stid].RunAtCommit(h, fb);
            lp.RunAtCommit(h, fb);
            commit_cnt ++;
            lb.train(h);
            if (cfg.bcache_verbose) {
                cout << "[BFU]: commit header of fb fbid="<<dec<< fb->fbid <<"pc=0x"<<hex<<h->GetBundlePosPC()<<" fb_spec_wptr:"<<dec<<fb->ras_info.spec_wptr<<", is_from_lb:"<<boolalpha<<fb->lb_info.hit<<endl;
            }
            if (fb->lb_info.hit && !fb->lb_info.front) {
                stats->lb_commitHdrCnt++;
            }

            if (cfg.report_header_footprint) {
                hdr_fp_cmt.insert(utils.AlignToCL(h->GetBundlePosPC()));
            }
        }
    }

    brq.CommitFetchBundle();
}

void BFU::ArbitratePrediction(PtrFB &fb, pos_t pos) {
    auto& main_info = fb->main_info;
    auto& sfb_info = fb->sfb_info[pos];
    auto& cPHTInfo = fb->cPHTInfo;
    auto& tage_info = sfb_info.vld ? sfb_info.tage_info[cPHTInfo.selectID] : fb->tage_info[cPHTInfo.selectID];
    auto& bim_info = sfb_info.vld ? sfb_info.bim_info : fb->bim_info;
    auto& ubtb_info = sfb_info.vld ? sfb_info.ubtb_info : fb->ubtb_info;
    auto& ibtb_info = fb->ibtb_info;

    main_info.taken = ubtb_info.taken;
    main_info.taken_pc = ubtb_info.taken_pc;
    main_info.tgt = ubtb_info.tgt;
    main_info.end_pc = ubtb_info.end_pc;
    main_info.taken_type = ubtb_info.taken_type;
    bool inCondTaken = (ubtb_info.hit && !utils.IsCond(ubtb_info.taken_type));

    // If TAGE gives a prediction, it should be updated
    if (tage_info.prim_idx != -1U) {
        main_info.is_from_tage = true;
    }
    // If TAGE misses or predicts NT for an earlier position, then BIM should also be involved
    if ((tage_info.prim_idx == -1U || !tage_info.taken) && bim_info.taken) main_info.is_from_bim = true;

    if (tage_info.prim_idx != -1U && tage_info.taken && (!inCondTaken || ubtb_info.taken_pc >= tage_info.taken_pc)) {
        // use TAGE if TAGE has a pred taken on this pc
        main_info.taken = tage_info.taken;
        main_info.taken_pc = tage_info.taken_pc;
        main_info.tgt = tage_info.tgt;
        main_info.end_pc = tage_info.end_pc;
        main_info.taken_type = tage_info.taken_type;
        main_info.is_from_tage = true;
    } else if ((tage_info.prim_idx == -1U || !tage_info.taken) && bim_info.taken) {
        bool ubtb_taken = (ubtb_info.hit && ubtb_info.taken_pc <= bim_info.taken_pc);
        // use BIM if TAGE has no prediction and BIM has a prediction on this pc
        if (ubtb_taken) {
            main_info.is_from_ubtb = true;
        } else {
            main_info.is_from_bim = true;
            main_info.taken = true;
            main_info.taken_pc = bim_info.taken_pc;
            main_info.tgt = bim_info.tgt;
            main_info.end_pc = bim_info.end_pc;
            main_info.taken_type = bim_info.taken_type;
        }
    } else if (ubtb_info.hit) {
        main_info.is_from_ubtb = true;
    }
    if (main_info.taken) {
        main_info.end_pc_vld = true;
        switch (main_info.taken_type) {
            case BranchType::BLK_BR_RET:
                ASSERT(fb->stid < ras.size());
                main_info.tgt = ras[fb->stid].Predict(fb, utils.CalcPosInFB(fb->va));
                main_info.is_from_ras = true;
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F4) << "Main pred, RET, fbid=" << dec << fb->fbid
                        << ", taken_pc=0x" << hex << main_info.taken_pc << ", tgt=0x" << main_info.tgt;
                }
                logger.debug("Main Pred", NIL, "RET, fbid=%d, taken_pc=0x%x, tgt=0x%lx\n", fb->fbid, main_info.taken_pc, main_info.tgt);
                break;
            case BranchType::BLK_BR_IND:
            case BranchType::BLK_BR_ICALL:
                ibtb.Predict(fb, main_info.taken_pc);
                main_info.tgt = ibtb_info.tgt;
                main_info.is_from_ibtb = true;
                break;
            default:
                break;
        }
    }
}

void BFU::ArbitrateWithSP(PtrFB &fb, pos_t pos_s) {
    auto& sp_info = fb->sp_info;
    auto& ibtb_info = fb->ibtb_info;
    auto& main_info = fb->main_info;

    for (pos_t pos = pos_s; pos < BFU_BANDWIDTH; pos++) {
        if (!fb->machineInst[pos]) {
            break;
        }
        if (fb->machineInst[pos]->bfuInfo->concat) {
            fb->machineInst[pos]->bfuInfo->vld = false;
            continue;
        }
        PtrMachineInst &h = fb->machineInst[pos];
        if (pos_s != utils.CalcPosInFB(fb->va)) {
            fb->machineInst[pos]->bfuInfo->predict_forward = true;
        }

        bool sp_check = (!main_info.taken || (main_info.taken_pc > h->GetBundlePosPC() && main_info.taken));
        // check by static predictor
        switch (sp_info.attr[pos]) {
            case BranchType::BLK_BR_FALL:
                break;
            case BranchType::BLK_BR_RET:
                if (sp_check) {
                    ASSERT(fb->stid < ras.size());
                    main_info.tgt = ras[fb->stid].Predict(fb, pos_s);
                    main_info.is_from_ras = true;
                    main_info.end_pc_vld = h->bfuInfo->spInfo->bsizeVld;
                    main_info.end_pc = h->bfuInfo->spInfo->bsizeVld ? utils.NextBlockPC(h) : main_info.end_pc;
                }
                main_info.taken = true;
                main_info.pos = pos;
                main_info.taken_type = sp_info.attr[pos];
                main_info.taken_pc = h->GetBundlePosPC();
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F4) << "Main pred, RET, fbid=" << dec << fb->fbid
                        << ", hid=" << fb->machineInst[pos]->bfuInfo->hid << hex << ", tgt=0x" << main_info.tgt;
                }
                logger.debug("Main Pred", NIL, "RET, fbid=%d, hid=%d, tgt=0x%lx\n", fb->fbid, fb->machineInst[pos]->bfuInfo->hid, main_info.tgt);
                break;
            case BranchType::BLK_BR_DIRECT:
            case BranchType::BLK_BR_CALL:
                if (!main_info.taken || !utils.IsDirectOnly(sp_info.attr[pos]) ||
                    main_info.taken_pc != h->GetBundlePosPC() || !utils.IsCallOnly(main_info.taken_type)) {
                    main_info.taken_type = sp_info.attr[pos];
                }
                if (sp_check) {
                    main_info.tgt = sp_info.tgt[pos];
                    main_info.is_from_sp = true;
                    main_info.end_pc_vld = h->bfuInfo->spInfo->bsizeVld;
                    main_info.end_pc = h->bfuInfo->spInfo->bsizeVld ? utils.NextBlockPC(h) : main_info.end_pc;
                }
                main_info.taken = true;
                main_info.pos = pos;
                main_info.taken_pc = h->GetBundlePosPC();
                break;
            case BranchType::BLK_BR_COND:
                break;
            case BranchType::BLK_BR_IND:
            case BranchType::BLK_BR_ICALL:
                if (sp_check) {
                    ibtb.Predict(fb, h->GetBundlePosPC());
                    main_info.is_from_ibtb = true;
                    main_info.taken_pc = h->GetBundlePosPC();
                    main_info.end_pc_vld = h->bfuInfo->spInfo->bsizeVld;
                    main_info.end_pc = h->bfuInfo->spInfo->bsizeVld ? utils.NextBlockPC(h) : main_info.end_pc;
                }
                main_info.taken = true;
                main_info.pos = pos;
                main_info.taken_type = sp_info.attr[pos];
                main_info.tgt = ibtb_info.tgt;
                break;
            default:
                break;
        }

        // concat handling part2: mask off current concat block
        if (!fb->machineInst[pos]->bfuInfo->concat && fb->machineInst[pos]->bfuInfo->vld) {
            fb->machineInst[pos]->bfuInfo->vld = true; // from fb start to taken position
        } else {
            fb->machineInst[pos]->bfuInfo->vld = false;
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP mark concat fb, pc=0x" << hex << fb->machineInst[pos]->GetBundlePosPC()
                    << dec << ", fbid=" << fb->fbid << ", hid=" << fb->machineInst[pos]->bfuInfo->hid << ", pos=" << pos;
            }
            logger.debug("TOP", F4, "mark concat fb, pc=0x%x, fbid=%d, hid=%d, pos=%d\n",
                        fb->machineInst[pos]->GetBundlePosPC(), fb->fbid, fb->machineInst[pos]->bfuInfo->hid, pos);
        }

        // override by loop predictor
        // TODO: find a better way to handle the coupling of LP and BranchType::CONCAT
        if (fb->machineInst[pos]->bfuInfo->vld && !fb->machineInst[pos]->bfuInfo->spInfo->isInst) {
            lp.Predict(fb, pos);
            if (fb->loop_info[pos].hit) {
                main_info.is_from_loop = true;
                bool loop_pred_taken = fb->loop_info[pos].pred_taken;
                if (loop_pred_taken) {
                    main_info.taken = true;
                    main_info.pos = pos;
                    main_info.taken_pc = h->GetBundlePosPC();
                    main_info.tgt = sp_info.tgt[pos];
                    main_info.end_pc_vld = h->bfuInfo->spInfo->bsizeVld;
                    main_info.end_pc = h->bfuInfo->spInfo->bsizeVld ? utils.NextBlockPC(h) : main_info.end_pc;
                } else if (main_info.taken && main_info.taken_pc <= h->GetBundlePosPC()) {
                    main_info.taken = false;
                    main_info.end_pos = BFU_BANDWIDTH;
                    main_info.pos = BFU_BANDWIDTH; // unset prediction by TAGE or BIM
                }
            }
        }

        // set predict target
        fb->machineInst[pos]->bfuInfo->predict_taken = false;
        fb->machineInst[pos]->bfuInfo->predict_tgt = utils.NextBlockPC(fb->machineInst[pos]->GetBundlePosPC());
        if (main_info.taken && main_info.taken_pc == h->GetBundlePosPC()) {
            h->bfuInfo->predict_taken = main_info.taken;
            h->bfuInfo->predict_tgt = main_info.tgt;
            main_info.pos = pos;
        }

        // get bsize from btb/tage
        if (main_info.taken && main_info.end_pc_vld && h->GetBundlePosPC() == main_info.taken_pc && !h->bfuInfo->spInfo->bsizeVld) {
            utils.SetBsize(h, utils.NextBlockPC(h->GetBundlePosPC()), main_info.end_pc);
        }

        // fetch untill bend/next bstart
        if (main_info.taken && main_info.end_pc_vld && main_info.end_pc == utils.NextBlockPC(fb->machineInst[pos]->GetBundlePosPC())) {
            main_info.end_pos = pos;
            fb->setInvalid(pos+1);
            break;
        }
    }
    if (!main_info.taken) {
        main_info.tgt = utils.NextFBPC(fb->va);
    } else if (main_info.end_pc_vld) {
        fb->end = utils.IsFBRangeRightBorder(main_info.end_pc, fb);
    } else {
        fb->end = false;
    }
    addr_t hash_pc = main_info.taken ? main_info.taken_pc : fb->tag;
    main_info.path_hist.emplace_back(hash_pc, main_info.tgt, main_info.taken_type, main_info.taken);
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP do main prediction, tag=0x" << hex << fb->tag
            << dec << ", fbid=" << fb->fbid << ", taken=" << main_info.taken << ", taken_pos=" << main_info.pos
            << ", end_pos=" << main_info.end_pos << hex << ", taken_pc=0x" << main_info.taken_pc << ", tgt=0x" << main_info.tgt
            << ", end_pc_vld=" << main_info.end_pc_vld << ", end_pc=0x" << main_info.end_pc
            << ", ubtb=" <<  main_info.is_from_ubtb << ", bim=" << main_info.is_from_bim
            << ", tage=" << main_info.is_from_tage << ", loop=" << main_info.is_from_loop;
    }
    logger.debug("TOP", F4, "do main prediction, tag=0x%x, fbid=%d, taken=%d, taken_pos=%d, end_pos=%d, taken_pc=0x%x, tgt=0x%x, end_pc_vld=%d, end_pc=0x%x, ubtb=%d, bim=%d, tage=%d, loop=%d\n",
        fb->tag, fb->fbid, main_info.taken, main_info.pos, main_info.end_pos, main_info.taken_pc, main_info.tgt, main_info.end_pc_vld, main_info.end_pc, main_info.is_from_ubtb, main_info.is_from_bim, main_info.is_from_tage, main_info.is_from_loop);
}

void BFU::DoMainPrediction(PtrFB &fb, pos_t pos_s) {
    // generate main prediction information
    ArbitratePrediction(fb, pos_s);
    ArbitrateWithSP(fb, pos_s);
}

void BFU::ArbitrateForLocalFB(PtrFB &fb) {
    auto& sp_info = fb->sp_info;
    auto& ibtb_info = fb->ibtb_info;
    auto& main_info = fb->main_info;

    fb->main_info.pos = BFU_BANDWIDTH;
    fb->main_info.end_pos = BFU_BANDWIDTH;
    for (pos_t pos = utils.CalcPosInFB(fb->va); pos < BFU_BANDWIDTH; pos++) {
        PtrMachineInst &h = fb->machineInst[pos];
        auto &local = local_fu[fb->pipe_id];
        if (!h || !h->bfuInfo->vld || h->bfuInfo->concat) {
            continue;
        }
        h->bfuInfo->predict_taken = false;
        if (main_info.taken && utils.IsBendOrBstart(h->bfuInfo->spInfo)) {
            fb->end = true;
            utils.SetLast(h);
            main_info.end_pos = pos;
            fb->setInvalid(pos+1);
            break;
        }
        if (local.sizeGet) {
            if (!h->bfuInfo->spInfo->isInst && h->GetBundlePosPC() == local.taken_pc) {
                h->bfuInfo->predict_taken = true;
                h->bfuInfo->predict_tgt = local.tgt;
                main_info.pos = pos;
                if (!h->bfuInfo->spInfo->bsizeVld) {
                    utils.SetBsize(h, utils.NextBlockPC(h->GetBundlePosPC()), local.end_pc);
                }
            }
            if (utils.NextBlockPC(h->GetBundlePosPC()) == local.end_pc) {
                main_info.end_pos = pos;
                fb->setInvalid(pos+1);
                break;
            }
        } else if (utils.IsBendOrBstart(h->bfuInfo->spInfo)) {
            fb->end = true;
            utils.SetLast(h);
            main_info.pos = pos;
            main_info.end_pos = pos;
            fb->setInvalid(pos+1);
            break;
        }
        switch (sp_info.attr[pos]) {
            case BranchType::BLK_BR_FALL:
                break;
            case BranchType::BLK_BR_RET:
                main_info.taken_type = sp_info.attr[pos];
                if (h->GetBundlePosPC() != local.taken_pc) {
                    main_info.taken = true;
                    main_info.pos = pos;
                    ASSERT(fb->stid < ras.size());
                    main_info.tgt = ras[fb->stid].Predict(fb, utils.CalcPosInFB(fb->va));
                    main_info.is_from_ras = true;
                    main_info.end_pc = h->bfuInfo->spInfo->bsizeVld ? utils.NextBlockPC(h) : main_info.end_pc;
                    main_info.taken_pc = h->GetBundlePosPC();
                    if (cfg.debug_enable) {
                        LOG_INFO_M(Unit::BFU, Stage::F4) << "Main pred, RET, fbid=" << dec << fb->fbid
                            << ", hid=" << h->bfuInfo->hid << ", tgt=0x" << hex << main_info.tgt;
                    }
                    logger.debug("Main Pred", NIL, "RET, fbid=%d, hid=%d, tgt=0x%lx\n", fb->fbid, h->bfuInfo->hid, main_info.tgt);
                }
                break;
            case BranchType::BLK_BR_DIRECT:
            case BranchType::BLK_BR_CALL:
                if (h->GetBundlePosPC() != local.taken_pc || (h->GetBundlePosPC() == local.taken_pc && utils.IsCallOnly(sp_info.attr[pos]) &&
                    utils.IsDirectOnly(local.taken_type))) {
                    main_info.taken = true;
                    main_info.pos = pos;
                    main_info.tgt = sp_info.tgt[pos];
                    main_info.is_from_sp = true;
                    main_info.taken_pc = h->GetBundlePosPC();
                    main_info.end_pc_vld = h->bfuInfo->spInfo->bsizeVld;
                    main_info.end_pc = h->bfuInfo->spInfo->bsizeVld ? utils.NextBlockPC(h) : main_info.end_pc;
                    main_info.taken_type = sp_info.attr[pos];
                }
                break;
            case BranchType::BLK_BR_COND:
                break;
            case BranchType::BLK_BR_IND:
            case BranchType::BLK_BR_ICALL:
                ibtb.Predict(fb, h->GetBundlePosPC());
                main_info.tgt = ibtb_info.tgt;
                main_info.taken_type = sp_info.attr[pos];
                if (h->GetBundlePosPC() != local.taken_pc) {
                    main_info.taken = true;
                    main_info.pos = pos;
                    main_info.is_from_ibtb = true;
                    main_info.end_pc_vld = h->bfuInfo->spInfo->bsizeVld;
                    main_info.end_pc = h->bfuInfo->spInfo->bsizeVld ? utils.NextBlockPC(h) : main_info.end_pc;
                }
                break;
            default:
                break;
        }
        if (main_info.taken && main_info.taken_pc == h->GetBundlePosPC()) {
            h->bfuInfo->predict_taken = true;
            h->bfuInfo->predict_tgt = main_info.tgt;
            main_info.path_hist.emplace_back(h->GetBundlePosPC(), main_info.tgt, main_info.taken_type, main_info.taken);
        }
        if (main_info.taken && main_info.end_pc_vld && utils.NextBlockPC(h->GetBundlePosPC()) == main_info.end_pc) {
            fb->end = true;
            fb->main_info.end_pos = pos;
            fb->setInvalid(pos+1);
            break;
        }
    }
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP do main prediction, tag=0x" << hex << fb->tag << ", pc=" << fb->va
            << dec << ", fbid=" << fb->fbid << ", fbid_local=" << fb->fbid_local << ", taken_pos=" << main_info.pos
            << ", end_pos=" << main_info.end_pos << ", taken=" << main_info.taken << ", end_pc_vld=" << main_info.end_pc_vld
            << ", end_pc=0x" << hex << main_info.end_pc << ", taken_pc=0x" << main_info.taken_pc << ", tgt=0x" << main_info.tgt;
    }
    logger.debug("TOP", F3, "do main prediction, tag=0x%x, pc=0x%x, fbid=%d, fbid_local=%d, taken_pos=%d, end_pos=%d, taken=%d, end_pc_vld=%d, end_pc=0x%x, taken_pc=0x%x, tgt=0x%x\n",
                fb->tag, fb->va, fb->fbid, fb->fbid_local, main_info.pos, main_info.end_pos, main_info.taken, main_info.end_pc_vld, main_info.end_pc, main_info.taken_pc, main_info.tgt);
}

void BFU::DoMainPredictionLB(PtrFB &fb)
{
    auto& sp_info = fb->sp_info;
    auto& tage_info = fb->tage_info[FIR];
    auto& bim_info = fb->bim_info;
    auto& main_info = fb->main_info;
    auto& ibtb_info = fb->ibtb_info;
    auto& ubtb_info = fb->ubtb_info;
    auto& lb_info = fb->lb_info;

    // generate main prediction information
    main_info.taken = false;
    main_info.pos = BFU_BANDWIDTH;

    for (pos_t pos = utils.CalcPosInFB(fb->va); pos<BFU_BANDWIDTH; pos++) {
        if (fb->machineInst[pos] == nullptr) {
            continue;
        }

        switch (sp_info.attr[pos]) {
            case BranchType::BLK_BR_FALL:
                break;
            case BranchType::BLK_BR_RET:
                main_info.taken = true;
                main_info.pos = pos;
                ASSERT(fb->stid < ras.size());
                main_info.tgt = ras[fb->stid].Predict(fb, pos);
                logger.debug("Main Pred", NIL, "RET, fbid=%d, tgt=0x%lx\n", fb->fbid, main_info.tgt);
                main_info.is_from_ras = true;
                break;
            case BranchType::BLK_BR_DIRECT:
            case BranchType::BLK_BR_CALL:
                main_info.taken = true;
                main_info.pos = pos;
                main_info.tgt = sp_info.tgt[pos];
                main_info.is_from_sp = true;
                break;
            case BranchType::BLK_BR_COND:
                if (!cfg.lb_tage_enable && !fb->lb_info.checkBP) {
                    if (lb_info.pos == pos) {
                        main_info.taken = lb_info.taken;
                        main_info.pos = pos;
                        main_info.tgt = lb_info.tgt;
                        main_info.is_from_lb = true;
                    }
                    break;
                }
                // If TAGE gives a prediction, it should be updated
                if (tage_info.prim_idx != -1U) main_info.is_from_tage = true;
                // If TAGE misses or predicts NT for an earlier position, then BIM should also be involved
                if (tage_info.prim_idx == -1U) main_info.is_from_bim = true;

                if (tage_info.prim_idx != -1U && tage_info.taken) {
                    // use TAGE if TAGE has a pred taken on this pos
                    main_info.taken = tage_info.taken;
                    main_info.pos = pos;
                    main_info.tgt = sp_info.tgt[pos];
                } else if (tage_info.prim_idx == -1U) {
                    // use BIM if TAGE has no prediction and BIM has a prediction on this pos
                    bool use_ubtb_pred = cfg.bim_only_tk_noflush && tage_info.prim_idx == -1U &&
                                         ubtb_info.hit && !ubtb_info.taken;
                    if (use_ubtb_pred) {
                        // do not update ubtb if this is predicted by UBTB
                        main_info.is_from_ubtb = true;
                    } else {
                        assert(bim_info.taken);
                        main_info.taken = true;
                        main_info.pos = pos;
                        main_info.tgt = sp_info.tgt[pos];
                    }
                } else if (cfg.bim_only_nt_noflush && tage_info.prim_idx == -1U && !bim_info.taken && ubtb_info.hit && ubtb_info.taken) {
                    // use UBTB taken if TAGE miss and BIM predictes NT (only when pos matches)
                    assert(ubtb_info.taken_type == BranchType::BLK_BR_COND);
                    assert(ubtb_info.tgt == sp_info.tgt[pos]);
                    main_info.is_from_ubtb = true;
                    main_info.taken = true;
                    main_info.pos = pos;
                    main_info.tgt = sp_info.tgt[pos];
                }
                // else: from bim or tage, pos mismatch, consider it a not-taken
                logger.debug("TOP", F4, "bcond prediction, pc=0x%x, fbid=%d, taken=%d, pos=%d, prim=%d, set=%d, way=%d, ubtb=%d\n",
                             fb->va, fb->fbid, main_info.taken, main_info.pos, tage_info.prim_idx,
                             tage_info.prim_idx != -1U? tage_info.lkup_info[tage_info.prim_idx].set_idx : -1U,
                             tage_info.prim_idx != -1U? tage_info.lkup_info[tage_info.prim_idx].way_idx : -1U,
                             main_info.is_from_ubtb);
                break;
            case BranchType::BLK_BR_IND:
            case BranchType::BLK_BR_ICALL:
                ibtb.Predict(fb, fb->machineInst[pos]->GetBundlePosPC());
                main_info.taken = true;
                main_info.pos = pos;
                main_info.tgt = ibtb_info.tgt;
                main_info.is_from_ibtb = true;
                break;
            default:
                break;
        }

        fb->machineInst[pos]->bfuInfo->vld = false;
        logger.debug("TOP", F4, "mark concat fb, pc=0x%x, fbid=%d, pos=%d\n", fb->machineInst[pos]->GetBundlePosPC(), fb->fbid, pos);

        // override by loop predictor
        // TODO: find a better way to handle the coupling of LP and BranchType::CONCAT
        if (fb->machineInst[pos]->bfuInfo->vld) {
            lp.Predict(fb, pos);
            if (fb->loop_info[pos].hit) {
                bool loop_pred_taken = fb->loop_info[pos].pred_taken;
                main_info.taken = loop_pred_taken;
                main_info.is_from_loop = true;
                if (loop_pred_taken) {
                    main_info.pos = pos;
                    main_info.tgt = sp_info.tgt[pos];
                } else {
                    main_info.pos = BFU_BANDWIDTH; // unset prediction by TAGE or BIM
                }
            }
        }

        main_info.path_hist.emplace_back(utils.CalcPC(fb->va, pos), main_info.taken? main_info.tgt : utils.CalcPC(fb->va, pos+1),
                                         sp_info.attr[pos], main_info.taken);

        if (main_info.taken) break;
    }
    if (!main_info.taken) {
        main_info.tgt = utils.NextFBPC(fb->va);
    }
    // update pred_tgt for machine headers
    for (pos_t pos = utils.CalcPosInFB(fb->va); pos<BFU_BANDWIDTH; pos++) {
        if (fb->machineInst[pos] == nullptr) {
            continue;
        }
        if (pos == main_info.pos && fb->machineInst[pos]->bfuInfo->vld) {
            fb->machineInst[pos]->bfuInfo->predict_taken = main_info.taken;
            fb->machineInst[pos]->bfuInfo->predict_tgt = main_info.tgt;
            break;
        } else {
            fb->machineInst[pos]->bfuInfo->predict_taken = false;
            fb->machineInst[pos]->bfuInfo->predict_tgt = 0;
        }
    }

    if (main_info.taken && main_info.taken == lb_info.taken) {
        if (main_info.pos == lb_info.pos) {
            if (main_info.tgt == lb_info.tgt) {
                if (cfg.bcache_verbose) {
                    cout<<"[BFU LB]: main_info eaqual to lb_info  fib="<<dec<<fb->fbid<<endl;
                }
            } else {
                if (cfg.bcache_verbose) {
                    cout<<"[BFU LB]: main_info tgt not eaqual to lb_info  fib="<<dec<<fb->fbid<<endl;
                    cout<<"LB_tgt 0x"<<hex<<lb_info.tgt<<", main_info.tgt 0x"<<main_info.tgt<<endl;
                }
                lb.stopLBFetch();
                pipe[F0].Flush(fb->stid);
                CreateNewInfoToFetchQ(fb->main_info.tgt, fb->machineInst[fb->main_info.pos]->GetBundlePosPC(), fb->tag,
                    fb->stid, false);
                lb_info.miss = true;
            }
        } else if (main_info.pos < lb_info.pos) {
            if (cfg.bcache_verbose) {
                cout<<"[BFU LB]: main_info location earlier not eaqual to lb_info  fib="<<dec<<fb->fbid<<endl;
                cout<<"LB_pos "<<dec<<lb_info.pos<<", main_info.pos "<<main_info.pos;
                cout<<"LB_tgt 0x"<<hex<<lb_info.tgt<<", main_info.tgt 0x"<<main_info.tgt<<endl;
            }
            lb.stopLBFetch();
            pipe[F0].Flush(fb->stid);
            CreateNewInfoToFetchQ(fb->main_info.tgt, fb->machineInst[fb->main_info.pos]->GetBundlePosPC(), fb->tag,
                fb->stid, false);
            lb_info.miss = true;
        } else {
            if (cfg.bcache_verbose) {
                cout<<"[BFU LB]: main_info location later not eaqual to lb_info  fib="<<dec<<fb->fbid<<endl;
                cout<<"LB_pos "<<dec<<lb_info.pos<<", main_info.pos "<<main_info.pos;
                cout<<"LB_tgt 0x"<<hex<<lb_info.tgt<<", main_info.tgt 0x"<<main_info.tgt<<endl;
            }
            fb->machineInst[lb_info.pos]->bfuInfo->predict_taken = main_info.taken;
            fb->machineInst[lb_info.pos]->bfuInfo->predict_tgt = main_info.tgt;
            main_info.taken = lb_info.taken;
            main_info.pos = lb_info.pos;
            main_info.tgt = lb_info.tgt;
            main_info.is_from_lb = true;
        }
    } else {
        if (cfg.bcache_verbose) {
            cout<<"[BFU LB]: main_info bp_taken not eaqual to lb_info  fib="<<dec<<fb->fbid<<endl;
            cout<<"LB_pos "<<dec<<lb_info.pos<<", main_info.pos "<<main_info.pos;
            cout<<"LB_tgt 0x"<<hex<<lb_info.tgt<<", main_info.tgt 0x"<<main_info.tgt<<endl;
        }
        if (main_info.taken && !lb_info.taken) {
            lb_info.miss = true;
            lb.stopLBFetch();
            pipe[F0].Flush(fb->stid);
            CreateNewInfoToFetchQ(main_info.tgt, fb->machineInst[fb->main_info.pos]->GetBundlePosPC(), fb->tag,
                fb->stid, false);
        } else if (!main_info.taken && lb_info.taken) {
            main_info.pos = lb_info.pos;
            main_info.is_from_lb = true;
            fb->machineInst[main_info.pos]->bfuInfo->predict_taken = main_info.taken;
            fb->machineInst[main_info.pos]->bfuInfo->predict_tgt = 0x0;
            lb_info.miss = true;
            lb.stopLBFetch();
            pipe[F0].Flush(fb->stid);
            CreateNewInfoToFetchQ(utils.NextBlockPC(fb->machineInst[fb->main_info.pos]->GetBundlePosPC()),
                fb->machineInst[fb->main_info.pos]->GetBundlePosPC(), fb->tag, fb->stid, false);
        }
    }
}

void BFU::UpdateUBTB(PtrFB const& fb, pos_t pos) {
    pbtb.UpdateAtMain(fb, pos);
    ubtb.UpdateAtMain(fb, pos);
}

void BFU::updatePosbase(PtrFB const& fb, pos_t pos) {
    // update the predictors (except ubtb)
    TrainTAGE(fb, pos);
    bim.update(fb, pos);
    ibtb.update(fb, pos);
    ubtb.UpdateAtResolve(fb, pos);
}

void BFU::TrainTAGE(PtrFB const& fb, pos_t pos)
{
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        tage[i].update(fb, pos);
        // When conditional taken, allocate tage.
        // When conditional not taken, allocate utils the taken branch resolve
        bool isBcondMispredict = fb->rslv_info.mispredict && utils.IsCond(fb->sp_info.attr[fb->rslv_info.pos]);
        if (isBcondMispredict && tage[i].MisPrim(fb->tage_info[i].prim_idx)) {
            if (fb->rslv_info.taken) {
                tage[i].allocate(fb, pos);
            } else {
                rslvNtFB[i] = fb;
            }
        } else if (rslvNtFB[i]) {
            if (rslvNtFB[i]->tag == fb->tag && tage[i].MisPrim(fb->tage_info[i].prim_idx) &&
                fb->rslv_info.taken && fb->ghr_info[i].ghr[0] == rslvNtFB[i]->ghr_info[i].ghr[0]) {
                tage[i].allocate(fb, pos);
            }
        }
        if (fb->rslv_info.taken) {
            rslvNtFB[i] = nullptr;
        }
    }
}

void BFU::UpdatePredictors(PtrFB const& fb) {
    pos_t pos = utils.CalcPosInFB(fb->va);
    while (fb->sfb_info[pos].vld) {
        updatePosbase(fb, pos);
        pos = fb->sfb_info[pos].tgt_pos;
    }
    updatePosbase(fb, pos);
}

void BFU::RunF4() {
    auto state = pipe[F4].state;
    if (state != NS_CORE::PipeState::VALID) {
        return;
    }
    for (size_t i = 0; i < pipe[F4].fb.size(); i++) {
        auto fb = pipe[F4].fb[i];
        bool mispred = PredF4(fb);
        if (mispred) {
            break;
        }
    }
}

bool BFU::PredF4(PtrFB &fb) {
    if (fb == nullptr) {
        return false;
    }
    pos_t pos = utils.CalcPosInFB(fb->va);
    if (ShortForwardF4(fb, pos)) {
        stats->forward_flush++;
        return true;
    }
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        tage[i].Predict(fb, pos);
    }
    ArbitrateTAGE(fb);
    bim.Predict(fb, pos);
    DoMainPrediction(fb, pos);
    lb.runAtMainPred(fb);
    ASSERT(fb->stid < ras.size());
    ras[fb->stid].runAtMainPred(fb);
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        ghrq[i].runAtMainPred(fb);
    }
    return MisAtF4(fb);
}

void BFU::ArbitrateTAGE(PtrFB &fb)
{
    if (fb->tage_info[FIR].prim_idx == -1U && fb->tage_info[SEC].prim_idx == -1U) {
        return;
    }
    fb->cPHTInfo.hit = true;
    fb->cPHTInfo.selectID = SelectTageID(fb);
    fb->cPHTInfo.w = fb->tage_info[fb->cPHTInfo.selectID].w;
}

uint32_t BFU::SelectTageID(PtrFB &fb)
{
    if (fb->tage_info[FIR].prim_idx != -1U && fb->tage_info[FIR].w.isTaken()) {
        return static_cast<uint32_t>(FIR);
    }
    return static_cast<uint32_t>(SEC);
}

bool shortForwardMis(PtrFB &fb, pos_t pos) {
    auto& ubtb_info = fb->sfb_info[pos].ubtb_info;
    if ((fb->main_info.tgt != ubtb_info.tgt) || (fb->main_info.taken != ubtb_info.taken) ||
        (fb->main_info.taken && ubtb_info.taken)) {
        fb->sfb_info[pos].mispred = true;
        for (pos_t pos_s = pos; pos_s < BFU_BANDWIDTH; pos_s++) {
            fb->sfb_info[pos_s].vld = false;
        }
        auto& sfb_info =  fb->sfb_info[pos];
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            fb->ghr_info[i] = sfb_info.ghr_info[i];
            fb->tage_info[i] = sfb_info.tage_info[i];
        }
        fb->ubtb_info = sfb_info.ubtb_info;
        fb->pbtb_info = sfb_info.pbtb_info;
        fb->ibtb_info = sfb_info.ibtb_info;
        fb->bim_info = sfb_info.bim_info;
        fb->ras_info = sfb_info.ras_info;
        fb->va_prev = sfb_info.va_prev;
        return true;
    }
    return false;
}

bool BFU::ShortForwardF4(PtrFB &fb, pos_t& pos) {
    while (fb->sfb_info[pos].vld) {
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            tage[i].Predict(fb, pos);
        }
        bim.Predict(fb, pos);
        DoMainPrediction(fb, pos);
        ASSERT(fb->stid < ras.size());
        ras[fb->stid].runAtMainPred(fb);
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            ghrq[i].runAtMainPred(fb);
        }
        if (shortForwardMis(fb, pos)) {
            // mispred handling
            fb->recent_va = utils.CalcPC(fb->va, pos);
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP short forward mispred, fbid=" << dec << fb->fbid
                    << ", pos=" << pos << ", pc=0x" << dec << utils.CalcPC(fb->va, pos);
            }
            logger.debug("TOP", F4, "short forward misped, fbid=%d, pos=%d, pc=0x%x\n",
                        fb->fbid, pos, utils.CalcPC(fb->va, pos));
            return true;
        }
        fb->sfb_info[pos].main_info = fb->main_info;
        fb->main_info.Reset();
        pos = fb->sfb_info[pos].tgt_pos;
        for (pos_t p = pos; p < BFU_BANDWIDTH; ++p) {
            if (!fb->machineInst[p]) {
                continue;
            }
            if (fb->machineInst[p]->bfuInfo->concat) {
                if (p == BFU_BANDWIDTH - 1) {
                    sp.Reset(fb->stid);
                }
                continue;
            }
            fb->sp_info.attr[p] = BranchType::BLK_BR_FALL;
            fb->sp_info.tgt[p] = utils.NextBlockPC(fb->machineInst[p]->GetBundlePosPC());
            break;
        }
    }
    return false;
}

void BFU::RunF3() {
    // pre-decode, static predictor
    if (pipe[F3].state == NS_CORE::PipeState::VALID) {
        for (auto &fb : pipe[F3].fb) {
            if (fb == nullptr) {
                continue;
            }
            sp.Predict(fb);
            WakeupLocalPipe(fb);
        }
    }

    // local pre-decode
    for (auto &local : local_fu) {
        PtrFB &fb = local.pipe[F3].fb[FIR];
        if (local.pipe[F3].state != NS_CORE::PipeState::VALID || fb == nullptr) {
            continue;
        }
        sp.Predict(fb);
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            tage[i].Predict(fb, utils.CalcPosInFB(fb->va));
        }
        ArbitrateTAGE(fb);
        ArbitrateForLocalFB(fb);
    }
}

void BFU::RunF2() {
    // Global fetch
    auto state = pipe[F2].state;
    bool prefetch = true;
    if (state != NS_CORE::PipeState::INVALID && (globalL2Reurn || state != NS_CORE::PipeState::STALL)) {
        for (size_t i = 0; i < pipe[F2].fb.size() && !bhc.needStall(); i++) {
            // l2 return, continue fetching next fetch bundle
            if (state == NS_CORE::PipeState::STALL && pipe[F2].stallIdx >= i && globalL2Reurn) {
                continue;
            }
            auto& fb = pipe[F2].fb[i];
            if (fb == nullptr) {
                break;
            }
            fb->bhc_fetch_time = GetSim()->getCycles();
            bool hit = bhc.fetch(fb);
            hwpf.observeFetch(fb);
            prefetch = false;
            if (!hit) {
                pipe[F2].stallIdx = i;
                break;
            }
        }
    }
    globalL2Reurn = false;

    // Local fetch, select oldest FB at current cycle
    if (!select_info.vld && !bhc.LocalFetchStall()) {
        for (uint32_t i = 0; i < local_fu.size(); i++) {
            auto &local = local_fu[i];
            if (local.pipe[F2].state == NS_CORE::PipeState::INVALID || !local.ready ||
                !local.pipe[F2].fb[FIR]) {
                continue;
            }
            if (!select_info.vld || (select_info.vld &&
                utils.CheckOlder(local.pipe[F2].fb[FIR], local_fu[select_info.pipe_id].pipe[F2].fb[FIR]) &&
                local.pipe[F2].fb[FIR]->stid == local_fu[select_info.pipe_id].pipe[F2].fb[FIR]->stid)) {
                select_info.vld = true;
                select_info.pipe_id = i;
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F2) << "TOP select local fb to bhc at current cycle, fbid=" << dec
                        << local.pipe[F2].fb[FIR]->fbid << ", fbid_local=" << local.pipe[F2].fb[FIR]->fbid_local;
                }
                logger.debug("TOP", F2, "Select local fb to bhc at current cycle, fbid=%d, fbid_local=%d\n",
                            local.pipe[F2].fb[FIR]->fbid, local.pipe[F2].fb[FIR]->fbid_local);
            }
        }
    }
    if (select_info.vld && !bhc.LocalFetchStall() && !select_info.stall) {
        auto &local = local_fu[select_info.pipe_id];
        auto &local_fb = local.pipe[F2].fb[FIR];
        select_info.stall = !bhc.fetch(local_fb);
        local_fb->bhc_fetch_time = GetSim()->getCycles();
        hwpf.observeFetch(local_fb);
        prefetch = false;
    }

    // Prefetch
    if (prefetch && !cfg.bhc_perfect) {
        // try send prefetch if no fetch bundle is using BHC pipe slot
        PtrHWPFInfo pf_info = hwpf.getTarget();
        if (pf_info) {
            pf_info->pa = pf_info->va; // FIXME: use btlb
            bhc.prefetch(pf_info);
        }
    }
}

void BFU::RunF1() {
    auto state = pipe[F1].state;
    if (state != NS_CORE::PipeState::VALID) {
        return;
    }
    for (size_t i = 0; i < pipe[F1].fb.size(); i++) {
        if (i != 0) {
            ASSERT(pipe[F1].fb[i - 1]->stid < ras.size());
            if (ras[pipe[F1].fb[i - 1]->stid].needStall()) {
                break;
            }
            CreateFBContinue(i);
        }

        auto& fb = pipe[F1].fb[i];
        ASSERT(fb->stid != -1U);
        PredF1(fb);
    }
}

void BFU::PredF1(PtrFB &fb) {
    fb->f1_time = GetSim()->getCycles();
    pos_t start_pos = utils.CalcPosInFB(fb->va);
    pbtb.Predict(fb, start_pos);
    ubtb.Predict(fb, start_pos);
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        tage[i].Lookup(fb, start_pos);
    }
    bim.Lookup(fb, start_pos);

    ASSERT(fb->stid < ras.size());
    // update hist after ras (refactor this shit!)
    ras[fb->stid].runAt0CyclePred(fb, start_pos);
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        ghrq[i].runAt0CyclePred(fb);
    }
    stats->f1_two_pred ++;

    if (cfg.short_forward_enable) {
        ShortForwardF1(fb);
    }
    if (fb->ubtb_info.taken) {
        fb->end = utils.IsFBRangeRightBorder(fb->ubtb_info.end_pc, fb);
    }
}

bool BFU::CheckForwardMatch(PtrFB const& fb) {
    ASSERT(fb->stid < ras.size());
    if (!fb->ubtb_info.taken || ras[fb->stid].needStall()) {
        return false;
    }
    return utils.IsShortForward(fb->va, fb->ubtb_info.taken_pc, fb->ubtb_info.tgt);
}

void BFU::ShortForwardF1(PtrFB &fb) {
    pos_t pos = utils.CalcPosInFB(fb->va);
    pos_t tgt_pos = 0;
    while (pos < BFU_BANDWIDTH) {
        if (!CheckForwardMatch(fb)) {
            return;
        }
        tgt_pos = utils.CalcPosInFB(fb->ubtb_info.tgt, fb->va);
        // exceptional block
        if (tgt_pos >= BFU_BANDWIDTH) {
            break;
        }
        stats->f1_two_pred ++;
        stats->forward_pred ++;
        // record forward
        fb->sfb_info[pos].vld = true;
        fb->sfb_info[pos].tgt_pos = tgt_pos;
        fb->sfb_info[pos].va_prev = fb->va_prev;
        fb->sfb_info[tgt_pos].pre_pos = pos;
        fb->sfb_info[pos].va = utils.CalcPC(fb->va, pos);
        fb->recent_va = fb->ubtb_info.tgt;
        fb->va_prev = fb->sfb_info[pos].va;
        if (cfg.debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::F1) << "TOP short forward, fbid=" << dec << fb->fbid
                << ", pos=" << pos << hex << ", pc=0x" << utils.CalcPC(fb->va, pos)
                << dec << ", tgt_pos=" << tgt_pos << hex << ", tgt_pc=0x" << utils.CalcPC(fb->va, tgt_pos)
                << ", BTB_tgt=0x" << fb->ubtb_info.tgt;
        }
        logger.debug("TOP", F1, "short forward, fbid=%d, pos=%d, pc=0x%x, tgt_pos=%d, tgt_pc=%x, BTB_tgt=%x\n",
                        fb->fbid, pos, utils.CalcPC(fb->va, pos), tgt_pos, utils.CalcPC(fb->va, tgt_pos), fb->ubtb_info.tgt);
        // Predict forward
        pbtb.Predict(fb, tgt_pos);
        ubtb.Predict(fb, tgt_pos);
        // stash ghr
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            fb->sfb_info[pos].ghr_info[i] = fb->ghr_info[i];
            ghrq[i].getGHRInfo(fb, 0);
            tage[i].Lookup(fb, tgt_pos);
        }
        bim.Lookup(fb, tgt_pos);
        ASSERT(fb->stid < ras.size());
        ras[fb->stid].runAt0CyclePred(fb, tgt_pos);
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            ghrq[i].runAt0CyclePred(fb);
        }
        pos = tgt_pos;
    }
}

void BFU::RunF0()
{
    uint32_t stid = PickThread();
    if (fetchThQ.Empty(stid)) {
        if (cfg.debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::F0) << "BFU T" << dec << stid << " empty at F0";
        }
        return;
    }

    if (pipe[F0].state != NS_CORE::PipeState::INVALID) {
        return;
    }

    PtrFB fb = fetchThQ.Read(stid);
    fb->stid = stid;
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::F0) << "BFU T" << stid << dec << " is picked at BFU F0, fbid=" << dec << fb->fbid;
    }
    CreateNewF0(fb);
}

uint32_t BFU::PickThread()
{
    static uint32_t stid = 0;
    stid = (stid + 1) % GetSim()->core->configs.scalar_smt_thread;
    return stid;
}

void BFU::RunLoopBuffer()
{
    for (uint32_t i = 0; i < BFU_BANDWIDTH;) {
        if (!lb.useLoopBuffer()) {
            break;
        }
        if (brq.isStall(1, 0)) {
            stats->brq_stall++;
            break;
        }
        PtrFB fb = std::make_shared<FetchBundle>(0, 0, false, fbid_global, 0, GetSim()->getCycles());
        fb->stid = 0;
        ASSERT(fb->stid < ras.size());
        if (ras[fb->stid].needStall()) {
            stats->ras_stall++;
            break;
        }
        if (!lb.sendHFromLB(fb)) {
            break;
        }
        for (uint32_t j = 0; j < BFU_TAGE_NPAGE; j++) {
            ghrq[j].getGHRInfo(fb, 0);
        }
        pos_t pos = utils.CalcPosInFB(fb->va);
        pbtb.Predict(fb, pos);
        ubtb.Predict(fb, pos);
        ASSERT(fb->stid < ras.size());
        ras[fb->stid].runAtLB0CyclePred(fb);

        sp.PredictLB(fb);
        bim.Lookup(fb, pos);
        for (uint32_t j = 0; j < BFU_TAGE_NPAGE; j++) {
            tage[j].Lookup(fb, pos);
        }
        if (cfg.lb_tage_enable || fb->lb_info.checkBP) {
            bim.Predict(fb, pos);
            for (uint32_t j = 0; j < BFU_TAGE_NPAGE; j++) {
                tage[j].Predict(fb, pos);
            }
        }

        DoMainPredictionLB(fb);
        for (uint32_t j = 0; j < BFU_TAGE_NPAGE; i++) {
            ghrq[j].runAtUseLB(fb);
        }
        brq.push(fb);
        if (fb->lb_info.miss) {
            lb.runAtRedirect(fb->machineInst[fb->main_info.pos], fb);
            break;
        }
        for (uint32_t j = 0; j < BFU_BANDWIDTH; j++) {
            if (fb->machineInst[j] == nullptr) {
                continue;
            }
            i++;
            if (j == fb->main_info.pos) {
                break;
            }
        }
    }
}

void BFU::PipeCtrl() {
    // nuke flush
    if (NukeHandling()) {
        return;
    }
    // redirection flush
    RedirectHandling();
    // F4 flush
    FlushForF4();
    // Local fetch end flush
    FlushByLocal();
    // back-pressure
    PipeStall();
}

static uint32_t GetStid(LocalPipe &localP)
{
    for (auto &pipe : localP.pipe) {
        if (pipe.fb[FIR]) {
            return pipe.fb[FIR]->stid;
        }
    }

    return -1U;
}

bool BFU::NukeHandling() {
    if (intf.be_bfu_nuke) {
        auto nuke_mhdr = intf.be_bfu_nuke;
        ASSERT(nuke_mhdr->stid != -1U);
        fetchThQ.FlushIf(nuke_mhdr->stid);
        pipe[F0].Flush(nuke_mhdr->stid);
        pipe[F1].Flush(nuke_mhdr->stid);
        pipe[F2].Flush(nuke_mhdr->stid);
        pipe[F3].Flush(nuke_mhdr->stid);
        pipe[F4].Flush(nuke_mhdr->stid);
        for (auto &local : local_fu) {
            if (GetStid(local) != nuke_mhdr->stid) {
                continue;
            }

            local.pipe[F0].Flush();
            local.pipe[F2].Flush();
            local.pipe[F3].Flush();
            local.FreePipe();
        }
        select_info.Reset();
        bhc.flush(nuke_mhdr->stid);
        btlb.flush();
        hwpf.flush(nuke_mhdr->stid);
        sp.Reset(nuke_mhdr->stid);
        GetSim()->core->disableOffGPR();

        PtrMachineInst h = nuke_mhdr->bfuInfo->spInfo->isInst ? brq.getBStartMhdrByFbid(nuke_mhdr->bfuInfo->hid, nuke_mhdr->stid) : nuke_mhdr;
        PtrFB nuke_fb = brq.nukeflush(nuke_mhdr);
        PtrFB nuke_fb_bstart = nuke_mhdr->bfuInfo->spInfo->isInst ?
            (*brq.getFBStartByHid(h->bfuInfo->hid, h->stid)) : nuke_fb;
        ASSERT(h->stid < ras.size());
        ras[h->stid].runAtNuke(h, nuke_fb_bstart);
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            ghrq[i].runAtNuke(nuke_fb_bstart);
        }
        lp.runAtNuke(h, nuke_fb_bstart);
        lb.stopLBFetch();
        if (h->bfuInfo->predict_taken) {
            ASSERT(nuke_mhdr->stid == nuke_fb_bstart->stid);
            LOG_INFO_M(Unit::BFU, Stage::NA) << "nuke flush create FB1 stid " << nuke_fb_bstart->stid;
            CreateNewInfoToFetchQ(h->bfuInfo->predict_tgt, nuke_fb_bstart->va, h->bfuInfo->predict_tgt,
                nuke_fb_bstart->stid, true);
            uint32_t pipe_id = CreateLocalF0(nuke_fb->va, nuke_fb, nuke_fb->replay_info.pos);
            sp.SetHeaderStash(h, nuke_mhdr->bfuInfo->spInfo, false, pipe_id, nuke_fb->stid);
            local_fu[pipe_id].ready = true;
            local_fu[pipe_id].taken_pc = h->GetBundlePosPC();
            local_fu[pipe_id].tgt = h->bfuInfo->predict_tgt;
            local_fu[pipe_id].taken_type = h->GetBranchType();
        } else {
            ASSERT(nuke_mhdr->stid == nuke_fb->stid);
            LOG_INFO_M(Unit::BFU, Stage::NA) << "nuke flush create FB2 stid " << nuke_fb_bstart->stid;
            CreateNewInfoToFetchQ(utils.NextBlockPC(nuke_mhdr->GetBundlePosPC()), nuke_fb->va, nuke_fb->tag,
                nuke_fb->stid, true);
            sp.SetHeaderStash(h, nuke_mhdr->bfuInfo->spInfo, true, 0, nuke_fb->stid);
        }
        if (pipe[F0].fb[FIR]) {
            pipe[F0].fb[FIR]->first_after_nuke = true;
        }
        if (cfg.debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::NA) << "TOP do nuke flush, fbid=" << dec << nuke_mhdr->bfuInfo->fbid
                << ", fbid_local=" << nuke_mhdr->bfuInfo->fbid_local << ", hid=" << nuke_mhdr->bfuInfo->hid
                << hex << ", pc=0x" << nuke_mhdr->GetBundlePosPC();
        }
        logger.debug("TOP", NIL, "do nuke flush, fbid=%d, fbid_local=%d, hid=%d, pc=0x%x\n",
                    nuke_mhdr->bfuInfo->fbid, nuke_mhdr->bfuInfo->fbid_local, nuke_mhdr->bfuInfo->hid, nuke_mhdr->GetBundlePosPC());
        if (cfg.bcache_verbose) {
            cout << "[BFU]: do nuke flush, fbid=" << dec << nuke_mhdr->bfuInfo->fbid << ", hid="
                 << nuke_mhdr->bfuInfo->hid << ", pc=0x" << hex << nuke_mhdr->GetBundlePosPC() << endl;
        }
        stats->ooo_flush ++;
        return true;
    }
    return false;
}

void BFU::RedirectHandling() {
    PtrFB redir_fb = brq.tryRedirect();
    if (redir_fb) { // redirect from this FB
        auto& h = redir_fb->machineInst[redir_fb->redir_info.pos];
        // set pipe state
        ASSERT(h->stid != -1U);
        fetchThQ.FlushIf(h->stid);
        pipe[F0].Flush(h->stid);
        pipe[F1].Flush(h->stid);
        pipe[F2].Flush(h->stid);
        pipe[F3].Flush(h->stid);
        pipe[F4].Flush(h->stid);
        // set local pipe state
        for (auto &local : local_fu) {
            if (GetStid(local) != h->stid) {
                continue;
            }

            local.pipe[F0].Flush();
            local.pipe[F2].Flush();
            local.pipe[F3].Flush();
            local.FreePipe();
        }
        select_info.Reset();
        bhc.flush(h->stid);
        btlb.flush();
        hwpf.flush(h->stid);
        sp.Reset(h->stid);

        GetSim()->core->disableOffGPR();
        // restore and re-update RAS and GHRQ
        ASSERT(redir_fb->stid < ras.size());
        ras[redir_fb->stid].runAtRedirect(redir_fb);
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            ghrq[i].runAtRedirect(redir_fb);
        }
        lp.runAtRedirect(h, redir_fb);
        lb.runAtRedirect(h, redir_fb);
        if (lb.useLoopBuffer()) {
            logger.debug("TOP", NIL, "after missBP continue use loopBuffer fbid=:%d\n",redir_fb->fbid);
            if (cfg.bcache_verbose) {
                cout << "after missBP continue use loopBuffer"<<endl;
            }
        } else {
            addr_t tag = redir_fb->redir_info.resolve_taken ? redir_fb->redir_info.tgt : redir_fb->tag;
            LOG_INFO_M(Unit::BFU, Stage::NA) << "RedirectHandling create FB stid " << redir_fb->stid
                                             << ", addr: 0x" << hex << redir_fb->redir_info.tgt;
            CreateNewInfoToFetchQ(redir_fb->redir_info.tgt, redir_fb->va, tag, redir_fb->stid, true);
        }
        if (cfg.debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::NA) << "TOP do misprediction redirect, fbid=" << dec << h->bfuInfo->fbid
                << ", fbid_local=" << h->bfuInfo->fbid_local << ", hid=" << h->bfuInfo->hid
                << ", rslv_taken=" << redir_fb->redir_info.resolve_taken << hex
                << ", rslv_tgt=" << redir_fb->redir_info.tgt;
        }
        logger.debug("TOP", NIL, "do misprediction redirect, fbid=%d, fbid_local=%d, hid=%d, rslv_taken=%d,"
                     "rslv_tgt=0x%x\n", h->bfuInfo->fbid, h->bfuInfo->fbid_local, h->bfuInfo->hid,
                     redir_fb->redir_info.resolve_taken, redir_fb->redir_info.tgt);
        if (cfg.bcache_verbose) {
            cout << "[BFU]: do missbp redirect, fbid=" << dec << h->bfuInfo->fbid << ", hid="
                 << h->bfuInfo->hid << ", pc=0x" << hex << h->GetBundlePosPC() << ", tgt_pc=0x"
                 << redir_fb->redir_info.tgt << endl;
        }
        if (redir_fb->lb_info.hit && !redir_fb->lb_info.front) {
            stats->lb_redirect++;
        }
        stats->bru_flush ++;
    }
}

void BFU::FlushForF4() {
    if (pipe[F4].state != NS_CORE::PipeState::VALID) {
        return;
    }
    bool flush = false;
    bool working = false;
    PtrFB f4 = nullptr;
    for (size_t i = 0; i < pipe[F4].fb.size(); i++) {
        if (flush) {
            pipe[F4].fb[i] = nullptr;
            continue;
        }
        flush = MisAtF4(pipe[F4].fb[i]);
        working = pipe[F4].fb[i] != nullptr;
        f4 = pipe[F4].fb[i];
    }
    stats->f4_working += static_cast<uint64_t>(working);
    if (flush) {
        auto& main_info = f4->main_info;
        auto& ubtb_info = f4->ubtb_info;
        stats->intraFlushCntMap[f4->va]++;
        // set pipe state
        ASSERT(f4->stid != -1U);
        fetchThQ.FlushIf(f4->stid);
        pipe[F0].Flush(f4->stid);
        pipe[F1].Flush(f4->stid);
        pipe[F2].Flush(f4->stid);
        pipe[F3].Flush(f4->stid);
        bool local_flush = (ubtb_info.taken || main_info.taken);
        for (uint32_t i = 0; i < local_fu.size(); i++) {
            auto &local = local_fu[i];
            PtrFB fb = local.pipe[F2].fb[FIR];
            local.Flush(f4->fbid, local_flush, f4->stid);
            if (fb != nullptr && local.pipe[F2].fb[FIR] == nullptr) {
                bhc.flushLocal(fb);
            }
            if (fb && select_info.vld && i == select_info.pipe_id) {
                select_info.Reset();
            }
            if (!local.occupied) {
                sp.ResetPipe(false, i, f4->stid);
            }
        }
        // flush static predictor
        f4->dec_info.taken = main_info.taken;
        sp.RecoverGlobalDec(f4);
        // flush other modules
        bhc.flushGlobal(f4->stid);
        btlb.flush();
        hwpf.flush(f4->stid);
        // restore and re-update
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            ghrq[i].runAtMainPredFlush(f4);
        }
        // restore and update RAS (if 0cycle ras is enabled)
        ASSERT(f4->stid < ras.size());
        ras[f4->stid].runAtMainPredFlush(f4);
        // create new F0 bundle
        if (f4->lb_info.front) {
            logger.debug("TOP", NIL, "FB hit LoopBuffer %d, clear pipe", f4->lb_info.lbIdx);
        } else {
            addr_t tag = main_info.taken ? main_info.tgt : f4->tag;
            CreateNewInfoToFetchQ(main_info.tgt, f4->recent_va, tag, f4->stid);
            if (main_info.taken && !f4->end) {
                uint32_t pipe_id = CreateLocalF0(f4->va, f4, main_info.end_pos);
                sp.GlobalToLocal(pipe_id, f4->stid);
                local_fu[pipe_id].ready = true;
                local_fu[pipe_id].taken_pc = f4->main_info.taken_pc;
                local_fu[pipe_id].tgt = main_info.tgt;
                local_fu[pipe_id].taken_type = main_info.taken_type;
                if (main_info.end_pc_vld) {
                    stats->f4_use_local ++;
                    if (cfg.debug_enable) {
                        LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP" << " local fetch end pc=0x"
                                                         << hex << main_info.end_pc;
                    }
                    logger.debug("TOP", NIL, "Local fetch end pc=%x\n", main_info.end_pc);
                    SetLocalPipeFetchSize(pipe_id, main_info.end_pc);
                    f4->end_va = main_info.end_pc;
                    DecLocalPipeFetchSize(pipe_id, local_fu[pipe_id].pipe[F0].fb[FIR]);
                }
            }
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::NA) << "TOP create new F0, pc=0x" << hex << main_info.tgt
                    << ", stid=" << f4->stid;
            }
            logger.debug("TOP", NIL, "create new F0, pc=0x%x\n", main_info.tgt);
        }
        if (cfg.debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP do F4 flush, stid=" << dec << f4->stid
                << ", fbid=" << dec << f4->fbid << hex << ", va=0x" << f4->va << ", tag=0x"
                << f4->tag << ", taken=" << main_info.taken << ", taken_type="
                << GetBlockBranchTypeName(main_info.taken_type).c_str() << ", taken_pc=0x"
                << main_info.taken_pc << ", end_pc_vld=" << main_info.end_pc_vld << ", end_pc=0x" << main_info.end_pc
                << ", main_tgt=0x" << main_info.tgt << dec << ", main_pos=" << main_info.pos;
            LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP do F4 flush, stid=" << dec << f4->stid
                << ", fbid=" << dec << f4->fbid << hex << ", va=0x" << f4->va << ", tag=0x" << f4->tag
                << ", ras=" << main_info.is_from_ras << ", indirect=" << main_info.is_from_ibtb
                << ", tage=" << main_info.is_from_tage << ", bim=" << main_info.is_from_bim
                << ", loop=" << main_info.is_from_loop;
            LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP do F4 flush, stid=" << dec << f4->stid
                << ", fbid=" << dec << f4->fbid << ", ubtb: hit=" << ubtb_info.hit << ", taken=" << ubtb_info.taken
                << ", taken_type=" << GetBlockBranchTypeName(ubtb_info.taken_type).c_str() << hex << ", taken_pc=0x"
                << ubtb_info.taken_pc << ", end_pc=0x" << ubtb_info.end_pc << ", tgt=0x" << ubtb_info.tgt
                << dec << ", s=" << ubtb_info.set_idx << ", w=" << ubtb_info.way_idx << ", stid: " << f4->stid;
        }
        logger.debug("TOP", F4, "do F4 flush, fbid=%d, va=0x%x, tag=0x%x, taken=%d, taken_type=%d,"
                     "taken_pc=0x%x, end_pc_vld=%d, end_pc=0x%x, "
                     "main_tgt=0x%x, main_pos=%d\n",
                     f4->fbid, f4->va, f4->tag, main_info.taken, main_info.taken_type,
                     main_info.taken_pc, main_info.end_pc_vld, main_info.end_pc,
                     main_info.tgt, main_info.pos);
        logger.debug("TOP", F4, "do F4 flush, fbid=%d, va=0x%x, tag=0x%x, ras=%d, indirect=%d,"
                     "tage=%d, bim=%d, lp=%d\n", f4->fbid, f4->va, f4->tag, main_info.is_from_ras,
                     main_info.is_from_ibtb, main_info.is_from_tage, main_info.is_from_bim, main_info.is_from_loop);
        logger.debug("TOP", F4, "do F4 flush, fbid=%d, ubtb: hit=%d, taken=%d, taken_type=%d,"
                     "taken_pc=0x%x, end_pc=0x%x, tgt=0x%x, s=%d, w=%d\n", f4->fbid, ubtb_info.hit, ubtb_info.taken,
                     ubtb_info.taken_type, ubtb_info.taken_pc, ubtb_info.end_pc, ubtb_info.tgt,
                     ubtb_info.set_idx, ubtb_info.way_idx);

        stats->intra_flush ++;
        stats->intra_flush_global ++;
        if (f4->main_info.is_from_ibtb) {
            stats->intra_flush_ibtb ++;
        } else if (f4->main_info.is_from_ras) {
            stats->intra_flush_ras ++;
        } else {
            // not caused by indirection or return
            if (!f4->ubtb_info.hit) {
                stats->intra_flush_ubtb_miss ++;
                if (f4->main_info.taken &&
                    (f4->main_info.is_from_ibtb || f4->main_info.is_from_ras || f4->main_info.is_from_sp)) {
                    stats->intra_flush_ubtb_miss_uncond ++;
                }
            } else {
                if (!f4->ubtb_info.taken) {
                    stats->intra_flush_ubtb_nt ++;
                } else {
                    stats->intra_flush_ubtb_tk ++;
                    if (!f4->main_info.taken) {
                        stats->intra_flush_ubtb_tk_dir ++;
                    } else {
                        stats->intra_flush_ubtb_tk_tgt ++;
                    }
                }
                if (f4->main_info.is_from_bim && !f4->main_info.is_from_tage && !f4->main_info.is_from_loop) {
                    stats->intra_flush_ubtb_hit_bim_only ++;
                }
            }
        }
    }
}

BrType getCallType(BranchType type) {
    switch (type) {
        case BranchType::BLK_BR_CALL:
        case BranchType::BLK_BR_DIRECT:
            return BranchType::BLK_BR_CALL;
            break;
        case BranchType::BLK_BR_ICALL:
        case BranchType::BLK_BR_IND:
            return BranchType::BLK_BR_ICALL;
            break;
        default:
            break;
    }
    return type;
}

bool BFU::CheckRasMis(PtrFB const& fb_start, pos_t const& pos, PtrFB const& fb_addpc) {
    // Main prediction is not done yet
    PtrMachineInst &h = fb_start->machineInst[pos];
    if (!utils.IsCall(fb_start->sp_info.attr[pos])) {
        return true;
    }
    if (!fb_start->main_info.taken) {
        if (fb_start->ubtb_info.taken && fb_start->ubtb_info.taken_pc < h->GetBundlePosPC()) {
            return false;
        }
        bool call_match = fb_start->ubtb_info.taken_type != getCallType(h->GetBranchType()) ||
                        fb_start->ubtb_info.end_pc == fb_addpc->sp_info.return_tgt;
        return !(fb_start->ubtb_info.taken && call_match);
    }
    // Main prediction is done yet
    if (fb_start->main_info.taken_pc < h->GetBundlePosPC()) {
        return false;
    }
    if (fb_start->sp_info.attr[pos] != getCallType(h->GetBranchType())) {
        return true;
    }
    if (!h->bfuInfo->spInfo->bsizeVld) {
        return true;
    }
    return false;
}

void BFU::FlushByLocal() {
    for (uint32_t i = 0; i < local_fu.size(); i++) {
        FlushForIncond(i);
        FlushForCall(i);
        FlushForEnd(i);
    }
}

void BFU::FlushForEnd(uint32_t pipe_id) {
    // Local fetch end
    auto local_end = [this] (Pipe &local_pipe, uint32_t pipe_id) -> bool {
        return (local_pipe.state == NS_CORE::PipeState::VALID && local_pipe.fb[FIR] &&
                local_pipe.fb[FIR]->end && local_fu[pipe_id].occupied &&
                local_pipe.fb[FIR]->fbid == local_fu[pipe_id].occupied_fbid);
    };
    auto &local = local_fu[pipe_id];
    if (local_end(local.pipe[F3], pipe_id)) {
        auto &fb = local.pipe[F3].fb[FIR];
        bool fetch_size_vld = local.sizeGet;
        if (fetch_size_vld && utils.IsFBRangeRightBorder(local.end_pc, fb)) {
            pos_t pos = utils.CalcPosInFB(utils.PrePC(local.end_pc), fb->va);
            if (fb->machineInst[pos]) {
                fb->machineInst[pos]->bfuInfo->fetch_end = true;
            }
        }
        local.FreePipe();
        sp.ResetPipe(false, pipe_id, fb->stid);
        if (fetch_size_vld) {
            return;
        }
        local.pipe[F0].Flush();
        if (local.pipe[F2].state != NS_CORE::PipeState::INVALID) {
            bhc.flushLocal(local.pipe[F2].fb[FIR]);
        }
        if (select_info.vld && pipe_id == select_info.pipe_id) {
            select_info.Reset();
        }
        local.pipe[F2].Flush();
        if (fb->main_info.end_pos >= BFU_BANDWIDTH) {
            cerr<<dec<<"fbid="<<fb->fbid<<" fbid_local="<<fb->fbid_local<<" va=0x"<<hex<<fb->va<<endl;
            assert(0 && "Error block fetch size!");
        }
        stats->intra_flush_local_end ++;
        if (cfg.debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP local end flush, fbid=" << dec << fb->fbid
                                             << fb->fbid_local << ", fbid_local=" << fb->fbid_local
                                             << ", hid=" << fb->hid << ", pos=" << fb->main_info.end_pos
                                             << hex << ", pc=0x"
                                             << fb->machineInst[fb->main_info.end_pos]->GetBundlePosPC();
        }
        logger.debug("TOP", NIL, "Local end flush, fbid=%d, fbid_local=%d, hid=%d, pos=%d, pc=%x\n",
                     fb->fbid, fb->fbid_local, fb->hid, fb->main_info.end_pos,
                     fb->machineInst[fb->main_info.end_pos]->GetBundlePosPC());
    }
}

bool BFU::MisInLocalFB(PtrFB& fb, LocalPipe& local) {
    if (!fb->main_info.taken || !local.occupied) {
        return false;
    }
    return (fb->main_info.taken_type != local.taken_type || fb->main_info.taken_pc != local.taken_pc ||
            fb->main_info.tgt != local.tgt);
}

void BFU::FlushForIncond(uint32_t pipe_id) {
    // FIXME: static predictor predict taken before bim/tage
    auto& local = local_fu[pipe_id];
    auto &fb = local.pipe[F3].fb[FIR];
    if (local.pipe[F3].state != NS_CORE::PipeState::VALID || fb == nullptr) {
        return;
    }
    if (MisInLocalFB(fb, local)) {
        ASSERT(fb->stid != -1U);
        fetchThQ.FlushIf(fb->stid);
        pipe[F0].Flush(fb->stid);
        pipe[F1].Flush(fb->stid);
        pipe[F2].Flush(fb->stid);
        pipe[F3].FlushByFbidGlobal(fb->fbid, false, fb->stid);
        pipe[F4].FlushByFbidGlobal(fb->fbid, false, fb->stid);
        for (uint32_t i = 0; i < local_fu.size(); i++) {
            PtrFB fb_f2 = local_fu[i].pipe[F2].fb[FIR];
            if (i == pipe_id) {
                local_fu[i].pipe[F0].Flush();
                local_fu[i].pipe[F2].Flush();
            } else {
                local_fu[i].Flush(fb->fbid, false, fb->stid);
            }
            if (fb_f2 && local_fu[i].pipe[F2].fb[FIR] == nullptr) {
                bhc.flushLocal(fb_f2);
                if (select_info.vld && i == select_info.pipe_id) {
                    select_info.Reset();
                }
            }
            if (!local_fu[i].occupied) {
                sp.ResetPipe(false, i, fb->stid);
            }
        }
        sp.ResetPipe(true, 0, fb->stid);
        bhc.flushGlobal(fb->stid);
        btlb.flush();
        hwpf.flush(fb->stid);
        for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
            ghrq[i].runAtMainPredFlush(fb);
        }
        ASSERT(fb->stid < ras.size());
        ras[fb->stid].runAtMainPredFlush(fb);
        LOG_INFO_M(Unit::BFU, Stage::NA) << "FlushForIncond create FB stid " << fb->stid;
        CreateNewInfoToFetchQ(fb->main_info.tgt, fb->tag, fb->main_info.tgt, fb->stid);
        if (!fb->end) {
            uint32_t pipe_id = CreateLocalF0(fb->tag, fb, fb->main_info.end_pos);
            local.taken_type = fb->main_info.taken_type;
            local.taken_pc = fb->main_info.taken_pc;
            local.tgt = fb->main_info.tgt;
            local.sizeGet = fb->main_info.end_pc_vld;
            local.end_pc = fb->main_info.end_pc;
            if (fb->main_info.end_pc_vld) {
                SetLocalPipeFetchSize(pipe_id, fb->main_info.end_pc);
                DecLocalPipeFetchSize(pipe_id, local.pipe[F0].fb[FIR]);
            }
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP create new local F0 after inconditional flush, local tgt=0x"
                    << hex << local.pipe[F0].fb[FIR]->va;
            }
            logger.debug("TOP", NIL, "create new Local F0 after inconditional flush, local tgt=0x%x\n",
                         local.pipe[F0].fb[FIR]->va);
        }
        stats->intra_flush_local_incond ++;
        if (cfg.debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP create new F0 after inconditional flush, fbid=" << dec
                << fb->fbid << ", fbid_local=" << fb->fbid_local << ", tgt=0x" << hex << fb->main_info.tgt;
        }
        logger.debug("TOP", NIL, "create new F0 after inconditional flush , fbid=%d, fbid_local=%d, tgt=0x%x\n",
                     fb->fbid, fb->fbid_local, fb->main_info.tgt);
    }
}

void BFU::FlushForCall(uint32_t pipe_id) {
    // flush for addpc in call, reget ras pointer
    auto &local = local_fu[pipe_id];
    auto &fb = local.pipe[F3].fb[FIR];
    if (local.pipe[F3].state != NS_CORE::PipeState::VALID || fb == nullptr) {
        return;
    }
    if (!fb->sp_info.add_pc_vld || fb->sp_info.pos > fb->main_info.end_pos) {
        return;
    }
    PtrFB fb_bstart = GetStartFBByFbidHid(fb->fbid, fb->machineInst[fb->sp_info.pos]->bfuInfo->hid, pipe_id, fb->stid);
    assert(fb_bstart != nullptr);
    pos_t pos = utils.getHeaderPosByHid(fb_bstart, fb->machineInst[fb->sp_info.pos]->bfuInfo->hid);
    if (pos < utils.CalcPosInFB(fb_bstart->va) || pos >= BFU_BANDWIDTH) {
        return;
    }
    PtrMachineInst &h_bstart = fb_bstart->machineInst[pos];
    if (h_bstart==nullptr) {
        return;
    }
    // Change to call branch type
    bool needFlush = CheckRasMis(fb_bstart, pos, fb);
    utils.SetBsize(h_bstart, utils.NextBlockPC(h_bstart->GetBundlePosPC()), fb->sp_info.return_tgt);
    fb_bstart->sp_info.attr[pos] = getCallType(h_bstart->GetBranchType());
    h_bstart->bcmd->branchType = fb_bstart->sp_info.attr[pos];
    fb_bstart->sp_info.tgt[pos] = utils.CalcStaticTarget(h_bstart);
    if (!needFlush) {
        return;
    }
    // flush for direct to call
    ASSERT(fb->stid != -1U);
    fetchThQ.FlushIf(fb->stid);
    pipe[F0].Flush(fb->stid);
    pipe[F1].Flush(fb->stid);
    pipe[F2].Flush(fb->stid);
    pipe[F3].FlushByFbidGlobal(fb->fbid, false, fb->stid);
    pipe[F4].FlushByFbidGlobal(fb->fbid, false, fb->stid);
    bool local_flush = false;
    for (uint32_t i = 0; i < local_fu.size(); i++) {
        auto &local = local_fu[i];
        PtrFB fb_f2 = local.pipe[F2].fb[FIR];
        local.Flush(fb->fbid, local_flush, fb->stid);
        if (fb_f2 != nullptr && local.pipe[F2].fb[FIR] == nullptr && fb_f2->stid == fb->stid) {
            bhc.flushLocal(fb_f2);
            if (select_info.vld && i == select_info.pipe_id) {
                select_info.Reset();
            }
        }
        if (!local.occupied) {
            sp.ResetPipe(false, i, fb->stid);
        }
    }
    // flush static predictor
    fb_bstart->dec_info.taken = true;
    sp.RecoverGlobalDec(fb_bstart);
    // flush other modules
    bhc.flushGlobal(fb->stid);
    btlb.flush();
    hwpf.flush(fb->stid);
    // Update for call
    addr_t tgt = utils.IsCallOnly(h_bstart->GetBranchType()) ? fb_bstart->sp_info.tgt[pos] :
                                                               h_bstart->bfuInfo->predict_tgt;
    fb_bstart->sp_info.path_hist.clear();
    fb_bstart->sp_info.path_hist.emplace_back(h_bstart->GetBundlePosPC(), tgt, fb_bstart->sp_info.attr[pos], true);
    // restore and re-update GHRQ
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        ghrq[i].runAtCallFlush(fb_bstart);
    }
    // restore and update RAS (if 0cycle ras is enabled)
    ASSERT(fb_bstart->stid < ras.size());
    ras[fb_bstart->stid].runCallFlush(fb_bstart, pos);
    fb_bstart->ubtb_info.end_pc = fb->sp_info.return_tgt;
    fb_bstart->ubtb_info.taken_type = fb->sp_info.attr[fb->sp_info.pos];
    fb_bstart->ubtb_info.taken = true;
    fb_bstart->ubtb_info.taken_pc = h_bstart->GetBundlePosPC();
    LOG_INFO_M(Unit::BFU, Stage::NA) << "FlushForCall create FB stid " << fb_bstart->stid;
    CreateNewInfoToFetchQ(tgt, fb_bstart->va, tgt, fb_bstart->stid);
    stats->intra_flush_local_call ++;
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP create new F0 after call flush, fbid=" << dec
            << fb->fbid << ", fbid_local=" << fb->fbid_local << hex << ", tgt=0x" << hex << tgt;
    }
    logger.debug("TOP", NIL, "create new F0 after call flush, fbid=%d, fbid_local=%d, tgt=0x%x\n",
                 fb->fbid, fb->fbid_local, tgt);
}

PtrFB BFU::GetStartFBByFbidHid(seq_t fbid, seq_t hid, uint32_t pipe_id, uint32_t stid) {
    for (auto &fb : pipe[F4].fb) {
        if (fb == nullptr) {
            continue;
        }
        if (fb->fbid != fbid) {
            continue;
        }
        for (pos_t pos = utils.CalcPosInFB(fb->va); pos < BFU_BANDWIDTH; pos++) {
            PtrMachineInst &h = fb->machineInst[pos];
            if (utils.IsStartMHdr(h, hid)) {
                return fb;
            }
        }
    }
    for (auto &fb : pipe[F3].fb) {
        if (fb == nullptr) {
            continue;
        }
        if (fb->fbid != fbid) {
            continue;
        }
        for (pos_t pos = utils.CalcPosInFB(fb->va); pos < BFU_BANDWIDTH; pos++) {
            PtrMachineInst &h = fb->machineInst[pos];
            if (utils.IsStartMHdr(h, hid)) {
                return fb;
            }
        }
    }
    if (local_fu[pipe_id].pipe[F3].fb[FIR]) {
        PtrFB fb = local_fu[pipe_id].pipe[F3].fb[FIR];
        for (pos_t pos = utils.CalcPosInFB(fb->va); pos < BFU_BANDWIDTH; pos++) {
            PtrMachineInst &h = fb->machineInst[pos];
            if (utils.IsStartMHdr(h, hid)) {
                return fb;
            }
        }
    }

    return *brq.getFBStartByHid(hid, stid);
}

bool BFU::CheckOldest(PtrFB const& fb, bool *global_select, bool *local_select) {
    if (fb->global) {
        for (uint32_t i = 0; i < local_fu.size(); i++) {
            auto &local = local_fu[i];
            if (local.pipe[F0].state != NS_CORE::PipeState::INVALID &&
                local.pipe[F0].fb[FIR] &&
                (fb->stid == local.pipe[F0].fb[FIR]->stid && !utils.CheckOlder(fb, local.pipe[F0].fb[FIR]))) {
                return false;
            }
            if (local.pipe[F2].state != NS_CORE::PipeState::INVALID &&
                local.pipe[F2].fb[FIR] &&
                (fb->stid == local.pipe[F2].fb[FIR]->stid && !utils.CheckOlder(fb, local.pipe[F2].fb[FIR]))) {
                return false;
            }
            if (local.pipe[F3].state != NS_CORE::PipeState::INVALID && local.pipe[F3].fb[FIR] &&
                (fb->stid == local.pipe[F3].fb[FIR]->stid && !utils.CheckOlder(fb, local.pipe[F3].fb[FIR])) &&
                !local_select[i]) {
                return false;
            }
        }
        return true;
    }
    for (uint32_t i = 0; i <= F4; i++) {
        auto &p = pipe[i];
        if (p.state == NS_CORE::PipeState::INVALID) {
            continue;
        }
        for (uint32_t j = 0; j < p.fb.size(); j++) {
            auto &fb_g = p.fb[j];
            bool selected = (i == F4 && global_select[j]);
            if (fb_g && !utils.CheckOlder(fb, fb_g) && fb->stid == fb_g->stid && !selected) {
                return false;
            }
        }
    }
    for (uint32_t i = 0; i < local_fu.size(); i++) {
        auto &local = local_fu[i];
        if (local.pipe[F0].state != NS_CORE::PipeState::INVALID && local.pipe[F0].fb[FIR] &&
            !utils.CheckOlder(fb, local.pipe[F0].fb[FIR]) && fb->stid == local.pipe[F0].fb[FIR]->stid) {
            return false;
        }
        if (local.pipe[F2].state != NS_CORE::PipeState::INVALID && local.pipe[F2].fb[FIR] &&
            !utils.CheckOlder(fb, local.pipe[F2].fb[FIR]) && fb->stid == local.pipe[F2].fb[FIR]->stid) {
            return false;
        }
        if (local.pipe[F3].state != NS_CORE::PipeState::INVALID && local.pipe[F3].fb[FIR] &&
            !utils.CheckEqual(fb, local.pipe[F3].fb[FIR]) &&
            !utils.CheckOlder(fb, local.pipe[F3].fb[FIR]) &&
            fb->stid == local.pipe[F3].fb[FIR]->stid &&
            !local_select[i]) {
            return false;
        }
    }
    return true;
}

void BFU::DeliverStall(uint32_t stid) {
    // Selected multiple fetch bundle to back-end in order
    DeliverFBInfo selected_info = DeliverFBInfo();
    uint32_t global_idx = pipe[F4].GetFirstValidFBIdx();
    uint32_t fb_num = 0;
    std::unique_ptr<bool[]> select_pipe(new bool[cfg.local_pipe_num]());
    std::unique_ptr<bool[]> select_fb_global(new bool[cfg.bfu_ntaken]());
    if (pipe[F4].state != NS_CORE::PipeState::INVALID && pipe[F4].fb[global_idx] &&
        pipe[F4].fb[global_idx]->stid != stid) {
        return;
    }

    ASSERT(stid < intf.bfu_be_q.size());
    while (!brq.isStall(fb_num+1, stid) && !intf.bfu_be_q[stid]->toBeOverflow(BFU_BANDWIDTH)) {
        bool selected = false;
        // Select global fetch bundle
        if (pipe[F4].state != NS_CORE::PipeState::INVALID &&
            global_idx < cfg.bfu_ntaken && pipe[F4].fb[global_idx]) {
            auto &fb = pipe[F4].fb[global_idx];
            bool deliver = CheckOldest(fb, select_fb_global.get(), select_pipe.get());
            if (deliver) {
                // Selected
                selected_info.vld = true;
                selected_info.global = true;
                selected_info.idx = global_idx;
                selected_info.fbid = fb->fbid;
                selected_info.fbid_local = fb->fbid_local;
                selected = true;
                fb_num++;
                deliver_q.push_back(selected_info);
                select_fb_global[global_idx] = true;
                global_idx++;
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP select FB(global) to back end, fbid=" << dec
                        << fb->fbid << ", fbid_local=" << fb->fbid_local << "stid=" << stid;
                }
                logger.debug("TOP", F4, "Select FB(global) to back end, fbid=%d, fbid_local=%d\n",
                             fb->fbid, fb->fbid_local);
            } else {
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP global FB is not the oldest, fbid=" << dec
                        << fb->fbid << ", fbid_local=" << fb->fbid_local << ", stid=" << stid << ", addr: 0x" << hex
                        << fb->va;
                }
                logger.debug("TOP", F4, "Global FB is not the oldest, fbid=%d, fbid_local=%d\n",
                             fb->fbid, fb->fbid_local);
            }
        }
        ASSERT(stid < intf.bfu_be_q.size());
        if (brq.isStall(fb_num+1, stid) || intf.bfu_be_q[stid]->toBeOverflow(BFU_BANDWIDTH)) {
            break;
        }
        // Select local fetch bundle
        for (uint32_t i = 0; i < local_fu.size(); i++) {
            if (brq.isStall(fb_num+1, stid) || intf.bfu_be_q[stid]->toBeOverflow(BFU_BANDWIDTH)) {
                break;
            }
            auto &local = local_fu[i];
            auto &fb = local.pipe[F3].fb[FIR];
            if (fb == nullptr || select_pipe[i]) {
                continue;
            }
            bool deliver = CheckOldest(fb, select_fb_global.get(), select_pipe.get());
            if (!deliver) {
                continue;
            }
            selected = true;
            select_pipe[i] = true;
            selected_info.vld = true;
            selected_info.global = false;
            selected_info.pipe_id = i;
            selected_info.fbid = fb->fbid;
            selected_info.fbid_local = fb->fbid_local;
            fb_num++;
            deliver_q.push_back(selected_info);
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F4) << "TOP Select FB(local) to back end, fbid=" << dec
                    << fb->fbid << ", fbid_local=" << fb->fbid_local << "stid=" << stid;
            }
            logger.debug("TOP", F4, "Select FB(local) to back end, fbid=%d, fbid_local=%d\n", fb->fbid, fb->fbid_local);
            break;
        }
        if (!selected) {
            break;
        }
    }

     // set global pipe F4 state
    bool globalPipeEmpty = true;
    pipe[F4].state = NS_CORE::PipeState::VALID;
    for (size_t i = 0; i < pipe[F4].fb.size(); i++) {
        auto& fb = pipe[F4].fb[i];
        if (fb == nullptr) {
            continue;
        }
        globalPipeEmpty = false;
        if (!select_fb_global[i]) {
            pipe[F4].state = NS_CORE::PipeState::STALL;
            pipe[F4].stallIdx = i;
            break;
        }
    }
    if (globalPipeEmpty) {
        pipe[F4].state = NS_CORE::PipeState::INVALID;
        pipe[F4].stallIdx = 0;
    } else if (pipe[F4].state == NS_CORE::PipeState::VALID) {
        pipe[F4].stallIdx = 0;
    }

    stats->brq_stall += static_cast<uint64_t>(brq.isStall(fb_num+1, stid));

    // set global pipe F3 state
    if (pipe[F3].state == NS_CORE::PipeState::STALL) {
        pipe[F3].state = pipe[F3].Empty() ? NS_CORE::PipeState::INVALID : NS_CORE::PipeState::VALID;
    } else {
        pipe[F3].state = pipe[F3].Empty() ? NS_CORE::PipeState::INVALID : pipe[F3].state;
    }

    // set local pipe F3 state
    for (uint32_t i = 0; i < local_fu.size(); i++) {
        auto &local = local_fu[i];
        local.pipe[F3].state = select_pipe[i] ? NS_CORE::PipeState::VALID :
                               (local.pipe[F3].fb[FIR] ? NS_CORE::PipeState::STALL :
                                NS_CORE::PipeState::INVALID);
    }
}

void BFU::FetchStall() {
    // BHC stall
    // Global stall
    if (bhc.needStall()) {
        pipe[F2].state = NS_CORE::PipeState::STALL;
        if (pipe[F2].fb[pipe[F2].stallIdx]) {
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F2) << "TOP FB1 stalled due to L1BCache stall, fbid="
                    << dec << pipe[F2].fb[pipe[F2].stallIdx]->fbid << ", addr: 0x"
                    << hex << pipe[F2].fb[pipe[F2].stallIdx]->va;
            }
            logger.debug("TOP", F2, "FB1 stalled due to L1BCache stall, fbid=%d\n",
                         pipe[F2].fb[pipe[F2].stallIdx]->fbid);
        }
        stats->bhc_stall_global ++;
    } else if (pipe[F2].state == NS_CORE::PipeState::STALL) {
        // may be INVALID before stall!
        pipe[F2].state = pipe[F2].Empty() ? NS_CORE::PipeState::INVALID : NS_CORE::PipeState::VALID;
        pipe[F2].stallIdx = 0;
    }
    LocalFetchStall();
}

void BFU::LocalFetchStall() {
    // Local stall, select the oldest ready local FB to fetch
    bool select_move = false;
    uint32_t move_id = 0;
    if (select_info.vld) {
        for (uint32_t i = 0; i < local_fu.size(); i++) {
            auto &local = local_fu[i];
            if (local.pipe[F2].state == NS_CORE::PipeState::INVALID ||
                local.pipe[F2].fb[FIR] == nullptr) {
                local.pipe[F2].state = NS_CORE::PipeState::INVALID;
                continue;
            }
            local.pipe[F2].state = (select_info.vld && select_info.pipe_id == i &&
                                    !bhc.LocalFetchStall()) ? NS_CORE::PipeState::VALID : NS_CORE::PipeState::STALL;
            if (local.pipe[F2].state == NS_CORE::PipeState::VALID) {
                select_info.vld = false;
                select_move = true;
                move_id = select_info.pipe_id;
            }
            if (local.pipe[F2].state == NS_CORE::PipeState::STALL) {
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F2) << "TOP LocalFB stalled due to L1BCache stall, fbid=" << dec
                        << local.pipe[F2].fb[FIR]->fbid << ", fbid_local=" << local.pipe[F2].fb[FIR]->fbid_local
                        << ", stid=" << dec << local.pipe[F2].fb[FIR]->stid
                        << ", addr: 0x" << hex << local.pipe[F2].fb[FIR]->va;
                }
                logger.debug("TOP", F2, "LocalFB stalled due to L1BCache stall, fbid=%d, fbid_local=%d\n",
                            local.pipe[F2].fb[FIR]->fbid, local.pipe[F2].fb[FIR]->fbid_local);
            }
        }
    }
    if (!select_info.vld) {
        for (uint32_t i = 0; i < local_fu.size(); i++) {
            auto &local = local_fu[i];
            if (local.pipe[F2].state == NS_CORE::PipeState::INVALID || local.pipe[F2].fb[FIR] == nullptr) {
                local.pipe[F2].state = NS_CORE::PipeState::INVALID;
                continue;
            }
            local.pipe[F2].state = (select_move && i == move_id) ? NS_CORE::PipeState::VALID :
                                    NS_CORE::PipeState::STALL;
            if (bhc.LocalFetchStall() || !local.ready) {
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F2) << "TOP LocalFB stalled due to L1BCache stall, fbid=" << dec
                        << local.pipe[F2].fb[FIR]->fbid << ", fbid_local=" << local.pipe[F2].fb[FIR]->fbid_local
                        << ", stid=" << dec << local.pipe[F2].fb[FIR]->stid
                        << ", addr: 0x" << hex << local.pipe[F2].fb[FIR]->va;
                }
                logger.debug("TOP", F2, "LocalFB stalled due to L1BCache stall, fbid=%d, fbid_local=%d\n",
                            local.pipe[F2].fb[FIR]->fbid, local.pipe[F2].fb[FIR]->fbid_local);
                continue;
            }
            if (select_move && i == move_id) {
                continue;
            }
            if (!select_info.vld) {
                select_info.vld = true;
                select_info.pipe_id = i;
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F2) << "TOP select local fb to bhc at next cycle, fbid=" << dec
                    << local.pipe[F2].fb[FIR]->fbid << ", fbid_local=" << local.pipe[F2].fb[FIR]->fbid_local;
                }
                logger.debug("TOP", F2, "Select local fb to bhc at next cycle, fbid=%d, fbid_local=%d\n",
                            local.pipe[F2].fb[FIR]->fbid, local.pipe[F2].fb[FIR]->fbid_local);
            } else if (utils.CheckOlder(local.pipe[F2].fb[FIR], local_fu[select_info.pipe_id].pipe[F2].fb[FIR]) &&
                       local.pipe[F2].fb[FIR]->stid == local_fu[select_info.pipe_id].pipe[F2].fb[FIR]->stid) {
                auto &pre_local = local_fu[select_info.pipe_id];
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F2) << "TOP LocalFB stalled due to L1BCache stall, fbid=" << dec
                        << pre_local.pipe[F2].fb[FIR]->fbid << ", fbid_local=" << pre_local.pipe[F2].fb[FIR]->fbid_local
                        << ", addr: 0x" << hex << pre_local.pipe[F2].fb[FIR]->va;
                    LOG_INFO_M(Unit::BFU, Stage::F2) << "TOP select local fb to bhc at next cycle, fbid=" << dec
                        << local.pipe[F2].fb[FIR]->fbid << ", fbid_local=" << local.pipe[F2].fb[FIR]->fbid_local;
                }
                logger.debug("TOP", F2, "LocalFB stalled due to L1BCache stall, fbid=%d, fbid_local=%d\n",
                            pre_local.pipe[F2].fb[FIR]->fbid, pre_local.pipe[F2].fb[FIR]->fbid_local);
                logger.debug("TOP", F2, "Select local fb to bhc at next cycle, fbid=%d, fbid_local=%d\n",
                            local.pipe[F2].fb[FIR]->fbid, local.pipe[F2].fb[FIR]->fbid_local);
                select_info.vld = true;
                select_info.pipe_id = i;
            }
        }
    }
    stats->bhc_stall_local += static_cast<uint64_t>(bhc.LocalFetchStall());
}

void BFU::TlbStall() {
    if (btlb.isStall()) {
        pipe[F1].state = NS_CORE::PipeState::STALL;
        pipe[F1].stallIdx = 0;
        stats->btlb_stall++;
        logger.debug("TOP", F1, "%s stalled due to btlb stall\n", "FB1");
        return;
    }
    uint32_t takenNum = 1;
    for (size_t i = 0; i < pipe[F1].fb.size(); i++) {
        auto &fb = pipe[F1].fb[i];
        if (fb == nullptr) {
            continue;
        }
        if (fb->ubtb_info.taken && LocalPipeStall(takenNum) && fb->pipe_id == -1U) {
            pipe[F1].state = NS_CORE::PipeState::STALL;
            pipe[F1].stallIdx = i;
            logger.debug("TOP", F1, "FB1 stalled due to localFB stall, fbid=%d\n", fb->fbid);
            return;
        }
        takenNum += static_cast<uint32_t>(fb->ubtb_info.taken && !fb->end && fb->pipe_id == -1U);
    }
    if (pipe[F1].state == NS_CORE::PipeState::STALL) {
        pipe[F1].state = NS_CORE::PipeState::VALID;
        pipe[F1].stallIdx = 0;
    }

    for (uint32_t p = F2; p < NSTAGE; ++p) {
        if (pipe[p].state == NS_CORE::PipeState::STALL) {
            pipe[F1].state = NS_CORE::PipeState::STALL;
            pipe[F1].stallIdx = 0;
        }
    }
}

void BFU::CreateNewFB() {
    // generate new target using F1 prediction if current F0 is invalid, or let F0 retry
    PtrFB preFB = nullptr;
    if (pipe[F1].state != NS_CORE::PipeState::INVALID) {
        for (size_t i = 0; i < pipe[F1].fb.size(); i++) {
            if (pipe[F1].state == NS_CORE::PipeState::STALL && pipe[F1].stallIdx == i) {
                break;
            }
            auto &fb = pipe[F1].fb[i];
            if (fb == nullptr) {
                continue;
            }
            preFB = fb;
            if (!fb->ubtb_info.taken || fb->pipe_id != -1U) {
                continue;
            }
            stats->f1_pred_taken++;
            if (fb->end) {
                continue;
            }
            stats->f1_use_local++;
            uint32_t pipe_id = CreateLocalF0(fb->va, fb, BFU_BANDWIDTH);
            local_fu[pipe_id].taken_type = fb->ubtb_info.taken_type;
            local_fu[pipe_id].taken_pc = fb->ubtb_info.taken_pc;
            local_fu[pipe_id].tgt = fb->ubtb_info.tgt;
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F0) << "TOP local fetch taken pc=0x" << hex
                    << fb->ubtb_info.taken_pc << ", end_pc=0x" << fb->ubtb_info.end_pc;
            }
            logger.debug("TOP", F0, "Local fetch taken pc=%x, end pc=%x\n",
                         fb->ubtb_info.taken_pc, fb->ubtb_info.end_pc);
            SetLocalPipeFetchSize(pipe_id, fb->ubtb_info.end_pc);
            fb->end_va = fb->ubtb_info.end_pc;
            DecLocalPipeFetchSize(pipe_id, local_fu[pipe_id].pipe[F0].fb[FIR]);
        }
    }
    if (pipe[F1].state == NS_CORE::PipeState::VALID && preFB) {
        addr_t tag = preFB->ubtb_info.taken ? preFB->ubtb_info.tgt : preFB->tag;
        CreateNewInfoToFetchQ(preFB->ubtb_info.tgt, preFB->recent_va, tag, preFB->stid);
        if (cfg.debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::F0) << "TOP create new F0 FB1, fbid=" << dec << preFB->fbid
                << ", pc=0x" << hex << preFB->va << ", stid=" << dec << preFB->stid;
        }
        logger.debug("TOP", F0, "create new F0 FB1, fbid=%d, pc=0x%x\n", preFB->fbid, preFB->va);
    }

    // Local pipe fetch continue
    for (uint32_t i = 0; i < local_fu.size(); i++) {
        auto &local = local_fu[i];
        if (local.pipe[F2].state != NS_CORE::PipeState::INVALID && local.pipe[F2].fb[FIR] &&
            local.pipe[F0].state == NS_CORE::PipeState::INVALID) {
            auto &fb = local.pipe[F2].fb[FIR];
            if (!fb->end) {
                if (cfg.debug_enable) {
                    LOG_INFO_M(Unit::BFU, Stage::F2) << "TOP local fb fetch continue, fbid=" << dec
                        << fb->fbid << ", fbid_local=" << fb->fbid_local;
                }
                logger.debug("TOP", F2, "local fb fetch continue, fbid=%d, local fbid=%d\n", fb->fbid, fb->fbid_local);
                uint32_t pipe_id = CreateLocalF0(fb->va, fb, BFU_BANDWIDTH);
                DecLocalPipeFetchSize(pipe_id, local_fu[pipe_id].pipe[F0].fb[FIR]);
            }
        }
    }
}

void BFU::RasStall(uint32_t stid) {
    ASSERT(stid < ras.size());
    if (ras[stid].needStall()) {
        if (pipe[F0].state != NS_CORE::PipeState::INVALID && pipe[F0].fb[FIR]->stid == stid) {
            pipe[F0].state = NS_CORE::PipeState::STALL;
            if (cfg.debug_enable) {
                LOG_INFO_M(Unit::BFU, Stage::F0) << dec << "TOP FB stalled due to RAS stall, fbid="
                    << dec << pipe[F0].fb[FIR]->fbid;
            }
            logger.debug("TOP", F0, "FB stalled due to RAS stall, fbid=%d\n", pipe[F0].fb[FIR]->fbid);
        }
        stats->ras_stall ++;
    } else if (pipe[F0].state == NS_CORE::PipeState::STALL && pipe[F0].fb[FIR]->stid == stid) {
        pipe[F0].state = NS_CORE::PipeState::VALID;
    }
    for (auto &local : local_fu) {
        local.pipe[F0].state = local.pipe[F0].fb[FIR] ? NS_CORE::PipeState::VALID : NS_CORE::PipeState::INVALID;
    }
}

void BFU::PipeStall() {
    // FB deliver stall
    for (uint32_t i = 0; i < GetSim()->core->configs.scalar_smt_thread; ++i) {
        DeliverStall(i);
    }
    // Fectch stall
    FetchStall();
    // TLB stall
    TlbStall();
    // Fetch continue
    CreateNewFB();
    // RAS stall (spec table full)
    for (uint32_t i = 0; i < GetSim()->core->configs.scalar_smt_thread; ++i) {
        RasStall(i);
    }
}

void BFU::MoveOneStage(Pipe& src, Pipe& dst) {
    if (dst.state == NS_CORE::PipeState::STALL) {
        if (src.state != NS_CORE::PipeState::INVALID) {
            src.state = NS_CORE::PipeState::STALL;
        }
        return;
    }
    if (src.state == NS_CORE::PipeState::STALL) {
        dst.Reset();
        for (size_t i = 0; i < src.fb.size(); i++) {
            if (src.stallIdx <= i) {
                break;
            }
            if (src.fb[i] == nullptr) {
                continue;
            }
            dst.state = NS_CORE::PipeState::VALID;
            dst.fb[i] = src.fb[i];
            src.fb[i] = nullptr;
        }
        size_t j = 0;
        for (size_t i = src.stallIdx; i < src.fb.size() && i != j; i++, j++) {
            if (src.fb[i] == nullptr) {
                break;
            }
            src.fb[j] = src.fb[i];
            src.fb[i] = nullptr;
        }
        src.stallIdx = 0;
    } else {
        dst = src;
    }
}

void BFU::DeliverFB() {
    uint32_t num = 0;
    while (!deliver_q.empty()) {
        DeliverFBInfo info = deliver_q.front();
        deliver_q.pop_front();
        if (cfg.debug_enable) {
            LOG_INFO_M(Unit::BFU, Stage::NA) << "BFU DeliverInfo, global=" << boolalpha << info.global
                                             << ", idx=" << dec << info.idx << ", pipe_id=" << info.pipe_id;
        }
        if (!info.vld) {
            continue;
        }
        PtrFB &fb = info.global ? pipe[F4].fb[info.idx] : local_fu[info.pipe_id].pipe[F3].fb[FIR];
        if (fb == nullptr) {
            continue;
        }
        bool ret = brq.push(fb);
        if (!ret) {
            break;
        }
        num++;
        if (fb->end) {
            stats->fetch_fall_throgh ++;
            pos_t pos = fb->main_info.end_pos < BFU_BANDWIDTH ? fb->main_info.end_pos : BFU_BANDWIDTH - 1;
            stats->fetch_size_after_taken += (utils.NextBlockPC(utils.CalcPC(fb->va, pos)) - fb->tag);
        }
        fb = nullptr;
    }
    if (num != 0) {
        stats->exist_minst_cycle++;
    }
    if (num == 0) {
        stats->f4_bhq_non_fb ++;
    } else if (num == 1) {
        stats->f4_bhq_one_fb ++;
    } else if (num == 2) {
        stats->f4_bhq_two_fb ++;
    } else {
        stats->f4_bhq_multi_fb ++;
    }
}

void BFU::PipeMove() {
    RptLocalStat();
    DeliverFB();

    // Global pipe move
    MoveOneStage(pipe[F3], pipe[F4]);
    MoveOneStage(pipe[F2], pipe[F3]);
    MoveOneStage(pipe[F1], pipe[F2]);
    MoveOneStage(pipe[F0], pipe[F1]);
    if (pipe[F0].state != NS_CORE::PipeState::STALL) {
        pipe[F0].Reset();
    }

    // Local pipe moves, skip for F1/F4 stage
    for (auto &local : local_fu) {
        MoveOneStage(local.pipe[F2], local.pipe[F3]);
        MoveOneStage(local.pipe[F0], local.pipe[F2]);
        if (local.pipe[F0].state != NS_CORE::PipeState::STALL) {
            local.pipe[F0].Reset();
        }
    }
}

void BFU::CreateNewInfoToFetchQ(addr_t va, addr_t va_prev, addr_t tag, uint32_t stid, bool first_after_redirect) {
    fbid_global++;
    PtrFB fb = std::make_shared<FetchBundle>(va, va_prev, first_after_redirect, fbid_global, 0, GetSim()->getCycles());
    fb->tag = tag;
    fb->stid = stid;
    ASSERT(stid != -1U);
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::NA) << "Write to fetchQ, stid=" << dec << stid << ", va=0x" << hex << va
                                         << ", prev_va=0x" << va_prev << ", fbid=" << dec << fb->fbid;
    }
    fetchThQ.Write(fb, stid);
}

void BFU::CreateNewF0(PtrFB &fb)
{
    pipe[F0].state = NS_CORE::PipeState::VALID;
    pipe[F0].fb[FIR] = fb;
    pipe[F0].fb[FIR]->global = true;
    pipe[F0].fb[FIR]->tag = fb->tag;
    int num_vld_fb = GetValidFB(-1U);
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        ghrq[i].getGHRInfo(pipe[F0].fb[FIR], num_vld_fb);
    }
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::F0) << "TOP create new F0, fbid=" << dec << pipe[F0].fb[FIR]->fbid
            << ", pc=0x" << hex << pipe[F0].fb[FIR]->va;
    }
    logger.debug("TOP", NIL, "create new F0, fbid=%d, pc=0x%x\n", pipe[F0].fb[FIR]->fbid, pipe[F0].fb[FIR]->va);
    if (cfg.bcache_verbose) {
        cout<<"create new f0, fbid="<<dec<<pipe[F0].fb[FIR]->fbid<<" pc=0x"<<hex<<pipe[F0].fb[FIR]->va<<endl;
    }
}

uint32_t BFU::GetFreeLocalPipeID() {
    for (uint32_t id = 0; id < local_fu.size(); id++) {
        if (!local_fu[id].occupied) {
            return id;
        }
    }
    assert(0 && "Can't find free local fetch pipe");
}

uint32_t BFU::GetOldestLocalPipeID() {
    uint32_t id = -1U;
    bool found = false;
    seq_t fbid_g = 0;
    seq_t fbid_l = 0;
    for (uint32_t i = 0; i < local_fu.size(); i++) {
        auto &local = local_fu[i];
        if (local.pipe[F2].state == NS_CORE::PipeState::INVALID) {
            continue;
        }
        if (!found || (found && local.pipe[F2].fb[FIR] && fbid_g >= local.pipe[F2].fb[FIR]->fbid &&
            fbid_l >= local.pipe[F2].fb[FIR]->fbid_local)) {
            fbid_g = local.pipe[F2].fb[FIR]->fbid;
            fbid_g = local.pipe[F2].fb[FIR]->fbid_local;
            id = i;
        }
        found = true;
    }
    return id;
}

bool BFU::LocalPipeStall(uint32_t const& reserve) {
    uint32_t free_num = 0;
    for (uint32_t id = 0; id < local_fu.size(); id++) {
        auto &local = local_fu[id];
        if (!local.occupied) {
            free_num++;
        }
        if (free_num >= reserve) {
            return false;
        }
    }
    return free_num < reserve;
}

uint32_t BFU::CreateLocalF0(addr_t va_prev, PtrFB fb_prev, pos_t pos_end) {
    uint32_t pipe_id = fb_prev->global ? GetFreeLocalPipeID() : fb_prev->pipe_id;
    fb_prev->pipe_id = fb_prev->global ? pipe_id : fb_prev->pipe_id;
    auto &local = local_fu[pipe_id];
    local.occupied = true;
    local.occupied_fbid = fb_prev->fbid;
    local.occupiedStid = fb_prev->stid;
    local.pipe[F0].state = NS_CORE::PipeState::VALID;
    addr_t next_pc = pos_end<BFU_BANDWIDTH ? utils.NextBlockPC(utils.CalcPC(fb_prev->va, pos_end)) :
                                             utils.NextFBPC(fb_prev->va);
    local.pipe[F0].fb[FIR] = std::make_shared<FetchBundle>(next_pc, va_prev, false, fb_prev->fbid,
                                                           fb_prev->fbid_local+1, GetSim()->getCycles());
    local.pipe[F0].fb[FIR]->global = false;
    local.pipe[F0].fb[FIR]->tag = fb_prev->tag;
    local.pipe[F0].fb[FIR]->stid = fb_prev->stid;
    local.pipe[F0].fb[FIR]->pipe_id = pipe_id;
    local.pipe[F0].fb[FIR]->pbtb_info = fb_prev->pbtb_info;
    local.pipe[F0].fb[FIR]->ubtb_info = fb_prev->ubtb_info;
    local.pipe[F0].fb[FIR]->ras_info = fb_prev->ras_info;
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        local.pipe[F0].fb[FIR]->ghr_info[i] = fb_prev->ghr_info[i];
        local.pipe[F0].fb[FIR]->tage_info[i] = fb_prev->tage_info[i];
    }
    local.pipe[F0].fb[FIR]->bim_info = fb_prev->bim_info;
    local.pipe[F0].fb[FIR]->cPHTInfo = fb_prev->cPHTInfo;
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::F0) << "TOP crate new local F0, pipe=" << dec << pipe_id
            << ", fbid=" << local.pipe[F0].fb[FIR]->fbid << ", fbid_local="
            << local.pipe[F0].fb[FIR]->fbid_local << hex << ", px=0x" << local.pipe[F0].fb[FIR]->va;
    }
    logger.debug("TOP", NIL, "create new local F0, pipe=%d, fbid=%d, fbid_local=%d, pc=0x%x\n",
                    pipe_id, local.pipe[F0].fb[FIR]->fbid,
                    local.pipe[F0].fb[FIR]->fbid_local, local.pipe[F0].fb[FIR]->va);
    if (cfg.bcache_verbose) {
        cout <<"create new f0, fbid=" << dec <<local.pipe[F0].fb[FIR]->fbid
             <<" pc=0x" <<hex << local.pipe[F0].fb[FIR]->va << endl;
    }
    return pipe_id;
}

int BFU::GetValidFB(size_t idx)
{
    int num = 0;
    for (size_t fx = F1; fx < F4; fx++) {
        for (size_t i = 0; i < pipe[fx].fb.size(); i++) {
            if (fx == F1 && i == idx) {
                break;
            }
            num += static_cast<int>(pipe[fx].fb[i] != nullptr);
        }
    }
    return num;
}

void BFU::CreateFBContinue(size_t idx) {
    PtrFB &fbPrev = pipe[F1].fb[idx-1];
    // create new fb2
    addr_t va = fbPrev->ubtb_info.tgt;
    addr_t va_prev = fbPrev->va;
    fbid_global++;
    pipe[F1].fb[idx] = std::make_shared<FetchBundle>(va, va_prev, false, fbid_global, 0, GetSim()->getCycles());
    pipe[F1].fb[idx]->predict_at_once = true;
    pipe[F1].fb[idx]->stid = fbPrev->stid;
    int num_vld_fb = GetValidFB(idx);
    for (uint32_t i = 0; i < BFU_TAGE_NPAGE; i++) {
        ghrq[i].getGHRInfo(pipe[F1].fb[idx], num_vld_fb);
    }
    PtrFB &fb2 = pipe[F1].fb[idx];
    fb2->tag = fbPrev->ubtb_info.taken ? fb2->va : fbPrev->tag;
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::F1) << "TOP create new FB2, fbid=" << dec << fb2->fbid
            << ", pc=0x" << hex << fb2->va << ", tag=0x" << fb2->tag << ", stid=" << dec << fb2->stid;
    }
    logger.debug("TOP", F1, "create new FB2, fbid=%d, pc=0x%x, tag=0x%x\n", fb2->fbid, fb2->va, fb2->tag);
    if (cfg.bcache_verbose) {
        cout<<"create second FB, fbid="<<dec<<fb2->fbid<<" pc=0x"<<hex<<fb2->va<<endl;
    }
}

char BFU::PipeState2Char(PipeState s) {
    if (s == NS_CORE::PipeState::INVALID) {
        return 'I';
    }
    if (s == NS_CORE::PipeState::VALID) {
        return 'V';
    }
    if (s == NS_CORE::PipeState::STALL) {
        return 'S';
    }
    (assert(0));
}

void BFU::PrintPipeState() {
    if (cfg.bcache_verbose) {
        printf("PipeState: %c%c%c%c%c, LB: %c\n", PipeState2Char(pipe[F0].state),
                                                  PipeState2Char(pipe[F1].state),
                                                  PipeState2Char(pipe[F2].state),
                                                  PipeState2Char(pipe[F3].state),
                                                  PipeState2Char(pipe[F4].state),
                                                  (lb.useLoopBuffer() ? 'V' : 'I'));
    }
    if (cfg.debug_enable) {
        LOG_INFO_M(Unit::BFU, Stage::NA) << "TOP PipeState:" << PipeState2Char(pipe[F0].state)
            << PipeState2Char(pipe[F1].state) << PipeState2Char(pipe[F2].state)
            << PipeState2Char(pipe[F3].state) << PipeState2Char(pipe[F4].state)
            << ", LB:" << (lb.useLoopBuffer() ? 'V' : 'I');
    }
    logger.debug("TOP", NIL, "PipeState: %c%c%c%c%c, LB: %c\n", PipeState2Char(pipe[F0].state),
                                                        PipeState2Char(pipe[F1].state),
                                                        PipeState2Char(pipe[F2].state),
                                                        PipeState2Char(pipe[F3].state),
                                                        PipeState2Char(pipe[F4].state),
                                                        lb.useLoopBuffer() ? 'V' : 'I');
}

/** print additional stats when simulation ends */
void BFU::ReportStat() {
    if (cfg.dump_mispreds) {
        brq.dumpMispreds();
    }
    if (cfg.report_header_footprint) {
        GetSim()->getRpt()->ReportTitle("Header Footprint Count");
        GetSim()->getRpt()->ReportVal("Header footprint (cmt)", static_cast<uint64_t>(hdr_fp_cmt.size()));
        GetSim()->getRpt()->ReportVal("Header footprint (spec)", static_cast<uint64_t>(hdr_fp_spec.size()));
        GetSim()->getRpt()->ReportVal("Header footprint (prefetch)", static_cast<uint64_t>(hdr_fp_pref.size()));
    }
}

void BFU::OnWarmupFinish() {
    if (cfg.dump_mispreds) {
        brq.resetMispreds();
    }
    if (cfg.report_header_footprint) {
        hdr_fp_cmt.clear();
        hdr_fp_spec.clear();
        hdr_fp_pref.clear();
    }
    logger.setEnable();
}

bool BFU::PipeIdle()
{
    return pipe[F0].state == NS_CORE::PipeState::INVALID &&
           pipe[F1].state == NS_CORE::PipeState::INVALID &&
           pipe[F2].state == NS_CORE::PipeState::INVALID &&
           pipe[F3].state == NS_CORE::PipeState::INVALID &&
           pipe[F4].state == NS_CORE::PipeState::INVALID;
}

uint32_t BFU::GetLocalPipeID(seq_t const& fbid) {
    for (uint32_t i =0; i < local_fu.size(); i++) {
        auto &local = local_fu[i];
        if (!local.occupied) {
            continue;
        }
        if (local.occupied_fbid == fbid) {
            return i;
        }
    }
    cerr<<"fbid="<<dec<<fbid<<endl;
    assert(0 && "Can't find occupied local pipe by globlal fbid");
}

void BFU::WakeupLocalPipe(PtrFB const& fb) {
    if (fb->ubtb_info.taken && !utils.IsFBRangeRightBorder(fb->ubtb_info.end_pc, fb)) {
        uint32_t pipe_id = GetLocalPipeID(fb->fbid);
        local_fu[pipe_id].ready = true;
        sp.GlobalToLocal(pipe_id, fb->stid);
    }
}

void BFU::DecLocalPipeFetchSize(uint32_t pipe_id, PtrFB const& fb) {
    if (local_fu[pipe_id].sizeVld) {
        fb->end = utils.IsFBRange(local_fu[pipe_id].end_pc, fb);
        local_fu[pipe_id].sizeVld = !fb->end;
    }
}

void BFU::SetLocalPipeFetchSize(uint32_t pipe_id, addr_t end_pc) {
    local_fu[pipe_id].sizeVld = true;
    local_fu[pipe_id].sizeGet = true;
    local_fu[pipe_id].end_pc = end_pc;
}

void BFU::TerminateFlush(uint32_t stid)
{
    fetchThQ.FlushIf(stid);
    pipe[F0].Flush(stid);
    pipe[F1].Flush(stid);
    pipe[F2].Flush(stid);
    pipe[F3].Flush(stid);
    pipe[F4].Flush(stid);
    for (auto &localPipe : local_fu) {
        if (localPipe.occupied && localPipe.occupiedStid == stid) {
            for (auto &p : localPipe.pipe) {
                p.Flush();
            }
            localPipe.FreePipe();
        }
    }
    hwpf.flush(stid);
    bhc.flush(stid);
}

void BFU::RptLocalStat() {
    uint64_t num = 0;
    for (auto &local : local_fu) {
        num += static_cast<uint64_t>(local.occupied);
    }
    if (num != 0) {
        stats->local_occupied_cyc ++;
    }
    stats->local_occupied_num += num;
}

void BFU::DumpPipeStatus()
{
    auto stage2Str = [](BFUStage stage) {
        switch(stage) {
            case BFUStage::F0 : return "F0";
            case BFUStage::F1 : return "F1";
            case BFUStage::F2 : return "F2";
            case BFUStage::F3 : return "F3";
            case BFUStage::F4 : return "F4";
            case BFUStage::NIL : return "XX";
            default: assert(0);
        }
    };
    auto state2Str = [](PipeState state) {
        if (state == NS_CORE::PipeState::INVALID) {
            return "INVALID";
        }
        if (state == NS_CORE::PipeState::VALID) {
            return "VALID";
        }
        if (state == NS_CORE::PipeState::STALL) {
            return "STALL";
        }
        return "UNKNOWN";
    };
    cout << "========================== DUMP BFU STATUS ==========================" << endl;
    for (size_t i = 0; i < NSTAGE; ++i) {
        auto &p = pipe[i];
        cout << stage2Str(static_cast<BFUStage>(i)) << ", state: " << state2Str(p.state) << endl;
        if (p.state != NS_CORE::PipeState::INVALID) {
            auto &fbs = p.fb;
            size_t cnt = 0;
            for (auto &fb : fbs) {
                if (!fb) {
                    cout << "\tfb[" << dec << cnt++ << "] = nullptr" << endl;
                    continue;
                }
                cout << "\tfb[" << dec << cnt++ << "] = { stid: " << dec << fb->stid << ", fbid: "
                     << dec << fb->fbid << ", va: 0x" << hex << fb->va << " }" << endl;
            }
        }
    }
    cout << "========================== DUMP BFU END =============================" << endl;
}

}


} // namespace JCore
