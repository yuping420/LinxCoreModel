#include <algorithm>

#include "core/Core.h"
#include "iex/pipe/iex_pipe.h"
#include "iex/iex_latency.h"

namespace JCore {

using namespace std;

constexpr uint32_t IEX_ALU_DEFAULT_EX_STAGE_NUM = 1;
constexpr uint32_t IEX_ALU_VEC_EX_STAGE_NUM = 5; // latency=6: E1-E5

void ALUPipe::Build(uint32_t id) {
    pipeid = id;

    if ((top->core->IsVectorIex(top->machineType)) || (top->id == MEM_IEX)) {
        ex_stage_num = IEX_ALU_VEC_EX_STAGE_NUM;
        iqDeallocOption = top->configs.simt_iex_iq_dealloc_option;
    } else {
        ex_stage_num = IEX_ALU_DEFAULT_EX_STAGE_NUM;
        iqDeallocOption = 0;
    }
    i1_inst = nullptr;
    i2_inst = nullptr;
    ex_inst.assign(ex_stage_num, vector<SimInst>(ex_stage_num));
    w1_inst.assign(ex_stage_num, nullptr);
    w2_inst.assign(ex_stage_num, nullptr);
    rf_wr_req.resize(ex_stage_num);
}

void ALUPipe::Reset() {
    i1_inst = nullptr;
    i2_inst = nullptr;
    ex_inst.assign(ex_stage_num, vector<SimInst>(ex_stage_num));
    w1_inst.assign(ex_stage_num, nullptr);
    w2_inst.assign(ex_stage_num, nullptr);
    sys_rd_req.Reset();
    sys_wr_req.Reset();
}

void ALUPipe::Work() {
    runW2();
    runW1();
    for (int32_t stage = static_cast<int32_t>(Stage::E1) + ex_stage_num - 1; stage >= static_cast<int32_t>(Stage::E1);
         --stage) {
        runEx(static_cast<Stage>(stage));
    }
    runE0();
    runI2();
    runI1();
    runP1();
    w1_flag.erase(sim->getCycles());
    LogPrint();
}

void ALUPipe::moveLpv() {
    auto pipeMoveLpv = [](SimInst &inst) {
        if (inst) {
            inst->MoveLpv();
        }
    };

    for (auto& inst : w2_inst) {
        pipeMoveLpv(inst);
    }
    for (auto& inst : w1_inst) {
        pipeMoveLpv(inst);
    }
    for (int32_t stg_i = ex_stage_num - 1; stg_i >= 0; --stg_i) {
        for (auto& inst : ex_inst.at(stg_i)) {
            pipeMoveLpv(inst);
        }
    }
    for (auto iter : e0_inst) {
        pipeMoveLpv(iter.second);
    }
    pipeMoveLpv(i2_inst);
    pipeMoveLpv(i1_inst);
}

void ALUPipe::move() {
    // writeback stage
    w2_inst = w1_inst;
    w1_inst.assign(ex_stage_num, nullptr);

    auto move_to_w1 = [this](const SimInst& inst) {
        // find w1_inst slot
        for (uint32_t w1_idx = 0; w1_idx < ex_stage_num; ++w1_idx) {
            if (w1_inst.at(w1_idx) == nullptr) {
                w1_inst.at(w1_idx) = inst;
                break;
            }
        }
    };

    // check any instr in execution stage ready for write back
    for (int32_t stg_i = ex_stage_num - 1; stg_i >= 0; --stg_i) {
        for (uint32_t idx = 0; idx < ex_inst[stg_i].size(); ++idx) {
            SimInst inst = ex_inst[stg_i][idx];
            if (!inst) {
                continue;
            }
            if ((inst->iexLatency - 1) != static_cast<uint32_t>(stg_i + 1)) {
                continue;
            }
            ex_inst[stg_i][idx] = nullptr;
            move_to_w1(inst);
        }
    }
    for (auto& inst : ex_inst[ex_stage_num - 1]) {
        if (inst) {
            LOG_ERROR_M(top->machineType, Stage::NA) << "Last EX slot should be empty: uid=" << dec
                << inst->uid << " inst->iexLatency=" << inst->iexLatency << " " << inst;
            fflush(stdout);
            ASSERT(false);
        }
    }

    // execution stage
    for (int32_t stg_i = ex_stage_num - 1; stg_i > 0; --stg_i) {
        ex_inst[stg_i].swap(ex_inst[stg_i - 1]);
    }
    ex_inst[0].assign(ex_inst[0].size(), nullptr);

    // iter cycles
    uint32_t e1_idx = 0;
    auto iter = e0_inst.begin();
    while (iter != e0_inst.end()) {
        if (iter->first > 0) {
            iter->first -= 1;
        }
        if (iter->first == 0) {
            auto& inst = iter->second;
            ex_inst[0][e1_idx++] = inst;
            iter = e0_inst.erase(iter);
            continue;
        }
        ++iter;
    }

    // new instructions into E0 or E1
    if (i2_inst) {
        uint32_t iter_cycles = getIterCycles(i2_inst);
        if (iter_cycles > 0) {
            // into iter cycles (E0), for 64+6
            e0_inst.emplace_back(iter_cycles, i2_inst);
            i2_inst->pipeCycle->e0Cycle = sim->getCycles() + 1;
        } else {
            ex_inst[0][e1_idx++] = i2_inst;
        }
        // release iq entry (option 0)
        if (iqDeallocOption == 0) {
            release_iq_inst.emplace_back(i2_inst);
            LOG_DEBUG_M(top->machineType, Stage::NA) << "IntoReleaseQueue @E0/E1 C:" << dec << GetSim()->getCycles()
                << " " << i2_inst;
        }
    }

    // update E0 exec unit list
    e0_unit.clear();
    for (auto& iter : e0_inst) {
        auto& inst = iter.second;
        if (!inst) {
            continue;
        }
        constexpr uint32_t E0_STALL_MIN_LATENCY = 3;
        if (iter.first <= E0_STALL_MIN_LATENCY) {
            continue;
        }
        auto iterLatency = IexLatency::VEC_LATENCY.find(inst->opcode);
        if (iterLatency != IexLatency::VEC_LATENCY.cend()) {
            e0_unit.emplace(iterLatency->second.unit);
        }
    }

    // fetch
    i2_inst = i1_inst;
    // release iq entry (option 1)
    if (i1_inst && (iqDeallocOption == 1U)) {
        release_iq_inst.emplace_back(i1_inst);
        LOG_DEBUG_M(top->machineType, Stage::NA) << "IntoReleaseQueue @I2 C:" << dec << GetSim()->getCycles() << " "
            << i1_inst;
    }
    i1_inst = *p1_inst;
    *p1_inst = nullptr;
}

void ALUPipe::flush(FlushBus &flushReq) {
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
    for (auto& iter : e0_inst) {
        flush_stage(iter.second);
    }
    for (uint32_t stg_i = 0; stg_i < ex_inst.size(); ++stg_i) {
        for (auto& inst : ex_inst[stg_i]) {
            flush_stage(inst);
        }
    }
    for (auto& inst : w1_inst) {
        flush_stage(inst);
    }
    for (auto& inst : w2_inst) {
        flush_stage(inst);
    }
}

bool ALUPipe::isPipeStallByScb(const SimInst& inst, uint32_t& noBusCfltCycles) const
{
    if (!inst) {
        return false;
    }
    if (inst->pdsts_.size() == 0) {
        return false;
    }
    constexpr uint64_t W1_LATENCY_OFFSET = 2; // I1, I2
    const uint64_t w1Time = sim->getCycles() + W1_LATENCY_OFFSET + getExLatency(inst) + getIterCycles(inst);
    if (noBusCfltCycles <= getExLatency(inst)) {
        noBusCfltCycles = 0;
        return (w1_flag.find(w1Time) != w1_flag.cend());
    }
    const uint32_t maxOffset = noBusCfltCycles - getExLatency(inst);
    for (uint32_t offset = 0; offset <= maxOffset; offset++) {
        if (w1_flag.find(w1Time + offset) == w1_flag.cend()) {
            noBusCfltCycles = offset;
            return false;
        }
    }
    return true;
}

bool ALUPipe::isPipeStallByIterCycles(const SimInst& inst) const
{
    if (!inst) {
        return false;
    }
    if (getIterCycles(inst) == 0) {
        return false;
    }
    auto exec_unit = getExecUnit(inst);
    if (e0_unit.find(exec_unit) != e0_unit.cend()) {
        return true;
    }
    if (getExecUnit(*p1_inst) == exec_unit) {
        return true;
    }
    if (getExecUnit(i1_inst) == exec_unit) {
        return true;
    }
    if (getExecUnit(i2_inst) == exec_unit) {
        return true;
    }
    return false;
}

void ALUPipe::runP1() {
    if ((p1_inst == nullptr) || (*p1_inst == nullptr)) {
        return;
    }
    auto& inst = *p1_inst;
    inst->pipeCycle->p1Cycle = sim->getCycles();
    inst->iexLatency = getExLatency(inst);

    auto getUpdateW1Cycle = [this](SimInst& inst, uint64_t oldW1Time) {
        uint64_t w1Time = oldW1Time;
        const uint32_t maxOffset = top->configs.simt_iex_max_ex_no_rslv_cflt - inst->iexLatency;
        for (uint32_t offset = 0; offset <= maxOffset; offset++) {
            if (w1_flag.find(w1Time + offset) == w1_flag.cend()) {
                w1Time += offset;
                inst->iexLatency += offset;
                inst->iexExtendedCycles = offset;
                break;
            }
        }
        return w1Time;
    };
    if (inst->pdsts_.size() > 0) {
        // add additional ex cycles for rslv bus cflt and record w1 time
        constexpr uint64_t W1_LATENCY_OFFSET = 2; // I1, I2
        uint64_t w1Time = sim->getCycles() + W1_LATENCY_OFFSET + inst->iexLatency + getIterCycles(inst);
        if (((top->core->IsVectorIex(top->machineType)) || (top->id == MEM_IEX)) && top->configs.simt_iex_vec_rslv_cflt
            && (top->configs.simt_iex_max_ex_no_rslv_cflt > inst->iexLatency)) {
            ASSERT(top->configs.simt_iex_max_ex_no_rslv_cflt <= (ex_stage_num + 1));
            w1Time = getUpdateW1Cycle(inst, w1Time);
        }
        w1_flag.insert(w1Time);
    }
}

void ALUPipe::runI1() {
    rf_rd_req.Reset();
    sys_rd_req.Reset();
    if (!i1_inst) {
        return;
    }
    rf_rd_req = i1_inst->GenRFReqBus(true);
    rf_rd_req.pipeid = pipeid;
    sys_rd_req = i1_inst->GenSYSReqBus();

    i1_inst->pipeCycle->i1Cycle = sim->getCycles();
    // wakeup
    const uint32_t latency = i1_inst->iexLatency + getIterCycles(i1_inst);
    constexpr uint32_t I1_WKUP_LATENCY = 2;
    if (latency == I1_WKUP_LATENCY) {
        PLpvInfo lpvInfo = i1_inst->GetLpv();
        for (auto pdst : i1_inst->pdsts_) {
            top->iq.WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled),
                lpvInfo, i1_inst->peID, i1_inst->tid, i1_inst->stid);
        }
    }
}

void ALUPipe::runI2() {
    // do bypass from all pipes
    if (!i2_inst) {
        return;
    }
    i2_inst->pipeCycle->i2Cycle = sim->getCycles();
    // E1/E2/E3/E4/E5 to I2
    // W1 to I2
    // W2 to I2
    if (!rf_data_ret.vld) {
        return;
    }

    i2_inst->RFRetSetData(rf_data_ret);
    // wakeup
    const uint32_t latency = i2_inst->iexLatency + getIterCycles(i2_inst);
    constexpr uint32_t I2_WKUP_LATENCY = 3;
    if (latency == I2_WKUP_LATENCY) {
        PLpvInfo lpvInfo = i2_inst->GetLpv();
        for (auto pdst : i2_inst->pdsts_) {
            top->iq.WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled),
                lpvInfo, i2_inst->peID, i2_inst->tid, i2_inst->stid);
        }
    }

    if (!sys_data_ret.vld) {
        return;
    }
    i2_inst->RecSYSReqRet(sys_data_ret);
}

void ALUPipe::runE0()
{
    for (auto& iter : e0_inst) {
        uint32_t iter_cycles = iter.first;
        SimInst inst = iter.second;
        const uint32_t latency = inst->iexLatency + iter_cycles;
        constexpr uint32_t E0_WKUP_LATENCY = 4;
        if (latency == E0_WKUP_LATENCY) {
            PLpvInfo lpvInfo = inst->GetLpv();
            for (auto pdst : inst->pdsts_) {
                top->iq.WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled),
                    lpvInfo, inst->peID, inst->tid, inst->stid);
            }
        }
    }
}

void ALUPipe::runEx(Stage stage)
{
    uint32_t idx = static_cast<uint32_t>(stage) - static_cast<uint32_t>(Stage::E1);
    for (auto& inst : ex_inst.at(idx)) {
        if (!inst) {
            continue;
        }
        switch (stage) {
            case Stage::E1:
                ++top->stats->vALUCycles;
                top->stats->vAluExtendCycles[inst->iexExtendedCycles] += 1;
                inst->pipeCycle->e1Cycle = sim->getCycles();
                // if (sim->perfectGetSet) {
                //     if (inst->ref.gsInfo.src0_vld) {
                //         inst->src0.data = inst->ref.gsInfo.src0_data;
                //     }
                //     if (inst->ref.gsInfo.src1_vld) {
                //         inst->src1.data = inst->ref.gsInfo.src1_data;
                //     }
                //     if (inst->ref.gsInfo.src2_vld) {
                //         inst->src2.data = inst->ref.gsInfo.src2_data;
                //     }
                //     if (inst->ref.gsInfo.src3_vld) {
                //         inst->src3.data = inst->ref.gsInfo.src3_data;
                //     }
                // }
                break;
            case Stage::E2:
                if ((inst->iexLatency - 1) >= idx + 1) {
                    inst->pipeCycle->e2Cycle = sim->getCycles();
                }
                break;
            case Stage::E3:
                if ((inst->iexLatency - 1) >= idx + 1) {
                    inst->pipeCycle->e3Cycle = sim->getCycles();
                }
                break;
            case Stage::E4:
                if ((inst->iexLatency - 1) >= idx + 1) {
                    inst->pipeCycle->e4Cycle = sim->getCycles();
                }
                break;
            case Stage::E5:
                if ((inst->iexLatency - 1) >= idx + 1) {
                    inst->pipeCycle->e5Cycle = sim->getCycles();
                }
                break;
            default:
                break;
        }

        // wakeup
        const uint32_t latency = inst->iexLatency;
        constexpr uint32_t EX_WKUP_LATENCY_OFST = 4;
        if (latency == (idx + EX_WKUP_LATENCY_OFST)) {
            PLpvInfo lpvInfo = inst->GetLpv();
            for (auto pdst : inst->pdsts_) {
                top->iq.WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled),
                    lpvInfo, inst->peID, inst->tid, inst->stid);
            }
        }

        if ((inst->iexLatency - 1) == (idx + 1)) {
            executeInstr(inst, stage);
        }
    }
}

void ALUPipe::executeInstr(const SimInst& inst, Stage stage)
{
    sys_wr_req.Reset();
    // simt or scalor
    if (IsScalarInst(inst->codeLen)) {
        bool ret = inst->Execute();
        if (!ret) {
            LOG_INFO_M(top->machineType, stage) << "[ALU  Pipe      Line" << dec << pipeid <<
                "]: report exception. Inst is " << inst;
            top->core->bctrl->blockROB.reportException(inst->bid, inst->stid, "ALU excution exception(scalar)");
        }
    } else {
        auto predSrc = inst->GetPSrcPtrByType(OperandType::OPD_PREDMASK);
        uint64_t curMask = predSrc == nullptr ? inst->predMask : predSrc->data;
        if (inst->dwDstType && (inst->pdsts_.size() > 0) && inst->pdsts_[0]->vecDataVld) {
            inst->pdsts_[0]->vecData.Init(static_cast<uint64_t>(OperandWidth::OPDW_D), inst->pdsts_[0]->vecData.m_lane);
        }
        for (uint32_t lane = 0; lane < inst->lanes; ++lane) {
            if (curMask & (1ULL << lane)) {
                bool ret = inst->Execute(lane, inst->pc);
                if (!ret) {
                    LOG_INFO_M(top->machineType, stage) << "[ALU  Pipe      Line" << dec << pipeid <<
                        "]: report exception. Lane is " << lane << " Inst is " << inst;
                    top->core->bctrl->blockROB.reportException(inst->bid, inst->stid,
                                                               "ALU excution exception(SIMT)");
                    break;
                }
            }
        }
    }

    if (inst->backToCodeTemplate) {
        rf_ct_q[inst->peID]->Write(inst);
    }

    // sysset write
    // if (OpcodeIsSysset(inst->opcode) || OpcodeIsLoopAux(inst->opcode)) {
    if (inst->opcode == Opcode::OP_SSRSET) {
        sys_wr_req = inst->GenSYSWFBus();
    }

    // ready table wtire
    /* FIXME: The conditions for "reduce" and "simt" might be able to be combined together. */
    // if (inst->checkAlldstRanged() && top->core->configs.reno_dynamic_enable && !OpcodeInInstGroup(inst->opcode, InstGroup::REDUCE) &&
    //     !top->core->IsVectorIex(inst->iexType) && inst->iexType !=MEM_IEX) {
    //     RFReqBus req = inst->genWRTableBus();
    //     if (req.vld) {
    //         iex_rt_wr_q->Write(req);
    //     }
    // }
}

void ALUPipe::runW1() {
    // 1) generate rd_wr/rslv
    // 2) handle cancel
    for (auto& inst : w1_inst) {
        if (!inst) {
            continue;
        }
        inst->pipeCycle->w1Cycle = sim->getCycles();
    }
}

void ALUPipe::runW2() {
    // send output bus
    for (auto& req : rf_wr_req) {
        req.Reset();
    }
    uint32_t req_cnt = 0;
    for (auto& inst : w2_inst) {
        if (!inst) {
            continue;
        }
        inst->pipeCycle->w2Cycle = sim->getCycles();

        // Generate readfile write bus
        rf_wr_req.at(req_cnt) = inst->GenRFReqBus(false);
        if (OpcodeInInstGroup(inst->opcode, InstGroup::REDUCE) && inst->SrcTypeContain(OperandType::OPD_GREG)) {
            rf_wr_req.at(req_cnt).vld = false;
        }
        // Generate PE resolved bus
        PEResolveBus rslv = inst->GenRslvBus();
        uint32_t tid = 0;
        if (top->GetSim()->core->IsVectorIex(top->machineType)) {
            tid = inst->tid;
        } else {
            tid = inst->stid;
        }
        rslv_array[inst->peID][tid]->Write(rslv);

        req_cnt += 1;
    }
}

IexExecUnit ALUPipe::getExecUnit(const SimInst& inst) const
{
    if (!inst) {
        return IexExecUnit::UNDEF;
    }
    auto iterLatency = IexLatency::VEC_LATENCY.find(inst->opcode);
    if (iterLatency != IexLatency::VEC_LATENCY.cend()) {
        return iterLatency->second.unit;
    }
    return IexExecUnit::UNDEF;
}

uint32_t ALUPipe::getExLatency(const SimInst& inst) const
{
    uint32_t latency = ex_stage_num + 1; // default
    if (!inst) {
        return latency;
    }
    if (top->id == SCALAR_IEX) {
        // TODO
    } else if ((top->core->IsVectorIex(top->machineType)) || (top->id == MEM_IEX)) {
        if (IexLatency::GetInstExLatency(inst, latency)) {
            return latency;
        }
    }
    return latency;
}

uint32_t ALUPipe::getIterCycles(const SimInst& inst) const
{
    if (!inst) {
        return 0;
    }
    if (top->core->IsVectorIex(top->machineType)) {
        auto exec_unit = getExecUnit(inst);
        if (exec_unit == IexExecUnit::FDIV) {
            // override by config
            return top->configs.simt_iex_fdiv_iter_cycles;
        }
        if (IexLatency::VEC_ITER_CYCLES.find(exec_unit) != IexLatency::VEC_ITER_CYCLES.cend()) {
            return IexLatency::VEC_ITER_CYCLES.at(exec_unit);
        }
    }
    return 0;
}

void ALUPipe::LogPrint()
{
    bool logEnable = ((p1_inst != nullptr) && (*p1_inst != nullptr)) || i1_inst || i2_inst ||
        any_of(w1_inst.cbegin(), w1_inst.cend(), [](const SimInst& inst) {
            return inst != nullptr;
        }) || any_of(w2_inst.cbegin(), w2_inst.cend(), [](const SimInst& inst) {
            return inst != nullptr;
        }) || any_of(ex_inst.cbegin(), ex_inst.cend(), [](const vector<SimInst>& v_inst) {
            return any_of(v_inst.cbegin(), v_inst.cend(), [](const SimInst& inst) {
                return inst != nullptr;
            });
        });
    if (!logEnable) {
        return;
    }
    if ((p1_inst != nullptr) && (*p1_inst != nullptr)) {
        LOG_DEBUG_M(top->machineType, Stage::P1) << "[ALU Line" << dec << pipeid << "]: " << (*p1_inst)->Dump();
    }
    if (i1_inst) {
        LOG_DEBUG_M(top->machineType, Stage::I1) << "[ALU Line" << dec << pipeid << "]: " << i1_inst->Dump();
    }
    if (i2_inst) {
        LOG_DEBUG_M(top->machineType, Stage::I2) << "[ALU Line" << dec << pipeid << "]: " << i2_inst->Dump();
    }
    for (auto& iter : e0_inst) {
        if (iter.second) {
            LOG_DEBUG_M(top->machineType, Stage::E0) << "[ALU Line" << dec << pipeid << "]: " << iter.second->Dump();
        }
    }
    for (uint32_t stg_i = 0; stg_i < ex_inst.size(); ++stg_i) {
        for (auto& inst : ex_inst[stg_i]) {
            if (inst) {
                LOG_DEBUG_M(top->machineType, static_cast<Stage>(static_cast<uint32_t>(Stage::E1) + stg_i)) <<
                    "[ALU Line" << dec << pipeid << "]: " << inst->Dump();
            }
        }
    }
    for (auto& inst : w1_inst) {
        if (inst) {
            LOG_DEBUG_M(top->machineType, Stage::WB) << "[ALU Line" << dec << pipeid << "]: " << inst->Dump();
        }
    }
    for (auto& inst : w2_inst) {
        if (inst) {
            LOG_DEBUG_M(top->machineType, Stage::W2) << "[ALU Line" << dec << pipeid << "]: " << inst->Dump();
        }
    }
}

} // namespace JCore
