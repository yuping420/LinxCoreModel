#include "iex/iex_iq.h"
#include <numeric>
#include "core/Core.h"
#include "iex/iex.h"

namespace JCore {

using namespace std;

WakeupInfo::WakeupInfo(OperandType typeVal, uint32_t ptagVal, bool recycledVal)
    : type(typeVal), ptag(ptagVal), recycled(recycledVal)
{}

WakeupInfo::~WakeupInfo() {}

bool IssueState::ssrsetCheck(SimInst inst)
{
    if (inst->opcode != Opcode::OP_SSRSET) {
        return false;
    }

    ROBID rid;
    if (top->GetSim()->core->IsVecPe(inst->peID)) {
        std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(inst->coreID);
        rid = pe->prob[inst->GetTid()]->getOldestRID();
    } else {
        std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->top->core->peArray[inst->peID]);
        rid = pe->prob[inst->GetTid()]->getOldestRID();
    }
    ROBID bid = top->top->core->bctrl->blockROB.getOldestBlockID(inst->stid);
    bool oldest = (bid == inst->bid) && (rid == inst->rid);

    return !oldest;
}

bool IssueState::CheckSysStateStall(const SimInst inst)
{
    if (!inst) {
        return true;
    }

    if (top->GetSim()->systemStatus.ecallStatus == EcallStatus::DECODE) {
        return LessEqual(top->GetSim()->systemStatus.ecallBlkId, inst->bid);
    }
    if (top->GetSim()->systemStatus.EcallRunning()) {
        return LessROBID(top->GetSim()->systemStatus.ecallBlkId, inst->bid);
    }

    return false;
}

bool IssueState::IsInLdWindow(SimInst &inst)
{
    if (top->top->core->IsVecPe(inst->peID)) {
        if (top->GetSim()->core->vectorTop->GetLdqCredit(inst->coreID) < 1) {
            return false;
        }

        bool in_window = false;
        if (top->GetSim()->core->IsVecPe(inst->peID)) {
            std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(inst->coreID);
            in_window = pe->prob[inst->GetTid()]->IsInLdWindow(inst);
        } else {
            std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->top->core->peArray[inst->peID]);
            in_window = pe->prob[inst->GetTid()]->IsInLdWindow(inst);
        }

        return in_window;
    } else {
        return true;
    }
}

bool IssueState::IsInStWindow(SimInst &inst)
{
    if (top->top->core->IsVecPe(inst->peID)) {
        bool in_window = false;
        if (top->GetSim()->core->IsVecPe(inst->peID)) {
            std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(inst->coreID);
            in_window = pe->prob[inst->GetTid()]->IsInStWindow(inst);
        } else {
            std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->top->core->peArray[inst->peID]);
            in_window = pe->prob[inst->GetTid()]->IsInStWindow(inst);
        }

        LOG_DEBUG_M(top->top->machineType, Stage::P1) << "tile store in window=" << in_window << ", " << inst->Dump();
        return in_window;
    } else {
        return true;
    }
}
uint32_t IssueState::ReadyInstCount()
{
    uint32_t readyCnt = 0;
    for (uint32_t i = 0; i < entry.size(); i++) {
        if (!entry[i]) {
            continue;
        }
        if (entry[i]->IsReady() && !entry[i]->issued) {
            readyCnt++;
        }
    }
    return readyCnt;
}

// 'ldqLimit' is Optimization for executing.
SimInst IssueState::Select(IssueState* next, bool& ldqLimit, uint32_t oldestOption,
                         const PipeStallFn_t& pipeStallFn, const vector<IssueChkFn>& cantIssue,
                         const std::vector<IssueChkFn>& cancelIssue, std::vector<IssueBlockReason>& reasons)
{
    SimInst inst = std::shared_ptr<SimInstInfo>(nullptr);
    uint32_t oldeIdx = -1U;
    uint32_t pickIdx = -1U;
    if (!size) {
        reasons.push_back(IssueBlockReason::IQ_EMPTY);
        return inst;
    }

    auto checkPrevIssued = [&](SimInst& inst)
    {
        if (!top->top->core->IsVectorIex(top->top->machineType) || top->order == IsqOrder::OOO) {
            // only block vector isq
            return true;
        }
        if ((inst->opcode == Opcode::OP_TLD || inst->opcode == Opcode::OP_TSD) &&
            (top->GetSim()->core->configs.mtc_tls_enable)) {
            return false;
        }

        uint32_t peid = inst->peID;
        uint32_t tid = inst->GetTid();
        ROBID rid = inst->rid;
        if (top->GetSim()->core->IsVecPe(peid)) {
            std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(inst->coreID);
            auto &prob = pe->prob[tid];
            return prob->CheckPrevIssued(rid);
        } else {
            std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->top->core->peArray[inst->peID]);
            auto &prob = pe->prob[tid];
            return prob->CheckPrevIssued(rid);
        }
        return false;
    };

    auto setRobIssued = [&](SimInst& inst) {
        if (((inst->opcode == Opcode::OP_TLD || inst->opcode == Opcode::OP_TSD)) &&
            (top->GetSim()->core->configs.mtc_tls_enable)) {
            return;
        }
        uint32_t peid = inst->peID;
        uint32_t tid = inst->GetTid();
        ROBID rid = inst->rid;
        if (top->GetSim()->core->IsVecPe(peid)) {
            auto &prob = dynamic_pointer_cast<VecPE>(top->GetSim()->core->vectorTop->GetPE(inst->coreID))->prob[tid];
            prob->SetIssued(rid);
        } else {
            auto &prob = dynamic_pointer_cast<SPE>(top->GetSim()->core->peArray[peid])->prob[inst->stid];
            prob->SetIssued(rid);
        }
    };

    auto canIssueInstruction = [&](uint32_t i) -> bool {
        if (!entry[i] || entry[i]->issued) {
            return false;
        }
        IssueBlockReason cantIssueReason = IssueBlockReason::UNDEF;
        for (auto& fn : cantIssue) {
            if (fn(entry[i], cantIssueReason)) {
                reasons.push_back(entry[i]->IsReady() ?
                    IssueBlockReason::CANT_ISSUE_BUT_READY : IssueBlockReason::CANT_ISSUE_NOT_READY);
                return false;
            }
        }

        // ssrset must become the oldest instruction before entering the pipe
        if (ssrsetCheck(entry[i])) {
            reasons.push_back(IssueBlockReason::SSRSET_NOT_OLDEST);
            return false;
        }

        // When in the system_state, block younger than the ecall block cannot be issued
        if (CheckSysStateStall(entry[i])) {
            reasons.push_back(IssueBlockReason::SYS_STATE_STALL);
            return false;
        }

        // check load is in window
        if (OpcodeIsLoad(entry[i]->opcode)) {
            if (!IsInLdWindow(entry[i])) {
                reasons.push_back(IssueBlockReason::LD_NOT_IN_WINDOW);
                return false;
            }
        }

        if (OpcodeIsStore(entry[i]->opcode)) {
            if (!IsInStWindow(entry[i])) {
                reasons.push_back(IssueBlockReason::ST_NOT_IN_WINDOW);
                return false;
            }
        }

        if (pAgeQueue && pAgeQueue->loadLimit(entry[i])) {
            if (next->entry[i]) {
                next->entry[i]->ldqLimit = true;
            }
            reasons.push_back(IssueBlockReason::AGE_QUEUE_LDQ_LIMIT);
            return false;
        }

        if (!checkPrevIssued(next->entry[i])) {
            reasons.push_back(IssueBlockReason::NOT_PREV_ISSUED);
            return false;
        }

        // check stall by SCB / iter cycles
        if (pipeStallFn && pipeStallFn(entry[i])) {
            reasons.push_back(IssueBlockReason::PIPE_STALL);
            return false;
        }

        return true;
    };

    auto handleLdqLimit = [&](uint32_t i) {
        if (next->entry[i]) {
            next->entry[i]->ldqLimit = false;
        }
        ldqLimit = false;
    };

    auto selectInstruction = [&](uint32_t i) {
        if (entry[i]->IsReady() && storeIDMatch(entry[i]) && !entry[i]->issued) {
            // Find the oldest ready inst(issued or not issued)
            if (oldeIdx == -1U) {
                oldeIdx = i;
            }

            // Find the ready inst with global reg
            if (pickIdx == -1U || (!top->GetSim()->core->IsVecPe(entry[i]->peID) && entry[i]->DstTypeContain(OperandType::OPD_GREG))) {
                pickIdx = i;
            }
            // First inst with global reg
            if (!top->GetSim()->core->IsVecPe(entry[i]->peID) && entry[pickIdx]->DstTypeContain(OperandType::OPD_GREG)) {
                return true;
            }
        }
        return false;
    };

    auto commitInstruction = [&]() {
        if (pickIdx != -1U) {
            inst = entry[pickIdx];
            entry[pickIdx]->issued = true;
            if (top->GetSim()->core->IsVecPe(inst->peID) &&
                OpcodeIsLoad(entry[pickIdx]->opcode)) {
                top->GetSim()->core->vectorTop->SubLdqCredit(inst->coreID, 1);
            } else if (top->GetSim()->core->IsVecPe(inst->peID) &&
                OpcodeIsStore(entry[pickIdx]->opcode)) {
                std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(inst->coreID);
                pe->prob[inst->GetTid()]->UpdateTileLsuIssueWindowPos(inst);
                top->GetSim()->core->vectorTop->SubStqCredit(inst->coreID, inst->GetTid(), 1);
            }
            --sizeNotIssued;
            next->entry[pickIdx]->issued = true;
            --next->sizeNotIssued;
            setRobIssued(next->entry[pickIdx]);

            if (pAgeQueue) {
                pAgeQueue->issued(entry[pickIdx], true);
            }
            if (next->pAgeQueue) {
                next->pAgeQueue->issued(next->entry[pickIdx], true);
            }
        }
    };

    vector<uint32_t> orderedIdx;
    for (uint32_t idx = 0; idx < entry.size(); idx++) {
        if (entry[idx]) {
            orderedIdx.push_back(idx);
        }
    }
    if (oldestOption == 1U) {
        sort(orderedIdx.begin(), orderedIdx.end(), [this](uint32_t a, uint32_t b) {
            ASSERT(entry[a] != nullptr);
            ASSERT(entry[b] != nullptr);
            return entry[a]->pipeCycle->iqCycle < entry[b]->pipeCycle->iqCycle;
        });
    }
    for (uint32_t idx = 0; idx < orderedIdx.size(); idx++) {
        const uint32_t entryIdx = orderedIdx[idx];
        if (!canIssueInstruction(entryIdx)) {
            if (top->top->core->IsVectorIex(top->top->machineType) && top->order == IsqOrder::STRICTLY_INORDER) {
                break;
            }
            continue;
        }

        if (next->entry[entryIdx]) {
            next->entry[entryIdx]->ldqCheck = true;
        }

        handleLdqLimit(entryIdx);

        if (selectInstruction(entryIdx)) {
            break;
        }
    }

    // The oldest need to issue.
    if ((oldeIdx != -1U) && !entry[oldeIdx]->issued) {
        pickIdx = oldeIdx;
    }

    if (pickIdx != -1U) {
        for (auto& fn : cancelIssue) {
            IssueBlockReason cancelReason = IssueBlockReason::UNDEF;
            if (fn(entry[pickIdx], cancelReason)) {
                reasons.push_back(cancelReason);
                ASSERT(inst == nullptr);
                return inst;
            }
        }
    }

    commitInstruction();
    return inst;
}

SimInst IssueState::Select_SC(IssueState* next, bool& ldqLimit, const PipeStallFn_t& pipeStallFn,
                            const IssueChkFn& cantIssue, const std::vector<IssueChkFn>& cancelIssue)
{
    SimInst inst = std::shared_ptr<SimInstInfo>(nullptr);
    uint32_t oldeIdx = -1U;
    uint32_t pickIdx = -1U;
    if (!size) {
        return inst;
    }

    auto canIssueInstruction = [&](uint32_t i) -> bool {
        IssueBlockReason reason = IssueBlockReason::UNDEF;
        if (!entry[i] || (cantIssue && cantIssue(entry[i], reason))) {
            return false;
        }

        // ssrset must become the oldest instruction before entering the pipe
        if (ssrsetCheck(entry[i])) {
            return false;
        }

        // When in the system_state, block younger than the ecall block cannot be issued
        if (CheckSysStateStall(entry[i])) {
            return false;
        }

        // check stall by SCB / iter cycles
        if (pipeStallFn && pipeStallFn(entry[i])) {
            return false;
        }

        return true;
    };

    auto selectInstruction = [&](uint32_t i) {
        if (entry[i]->IsReady() && !entry[i]->issued && storeIDMatch(entry[i])) {
            // Find the oldest ready inst(issued or not issued)
            if (oldeIdx == -1U) {
                oldeIdx = i;
            }

            // Find the ready inst with global reg
            if (pickIdx == -1U || entry[i]->DstTypeContain(OperandType::OPD_GREG)) {
                pickIdx = i;
            }

            // First inst with global reg
            if (entry[pickIdx]->DstTypeContain(OperandType::OPD_GREG)) {
                return true;
            }
        }
        return false;
    };

    auto commitInstruction = [&]() {
        if (pickIdx != -1U) {
            inst = entry[pickIdx];
            entry[pickIdx]->issued = true;
            --sizeNotIssued;
            next->entry[pickIdx]->issued = true;
            --next->sizeNotIssued;

            if (pAgeQueue) {
                pAgeQueue->issued(entry[pickIdx], true);
            }
            if (next->pAgeQueue) {
                next->pAgeQueue->issued(next->entry[pickIdx], true);
            }
        }
    };

    for (uint32_t i = 0; i < entry.size(); i++) {
        if (!canIssueInstruction(i)) {
            if (top->top->core->IsVectorIex(top->top->machineType) && top->order == IsqOrder::STRICTLY_INORDER) {
                break;
            }
            continue;
        }

        if (next->entry[i]) {
            next->entry[i]->ldqCheck = true;
        }

        if (selectInstruction(i)) {
            break;
        }
    }

    // The oldest need to issue.
    if (oldeIdx != -1U && !entry[oldeIdx]->issued) {
        pickIdx = oldeIdx;
    }

    if (pickIdx != -1U) {
        for (auto& fn : cancelIssue) {
            IssueBlockReason reason = IssueBlockReason::UNDEF;
            if (fn(entry[pickIdx], reason)) {
                ASSERT(inst == nullptr);
                return inst;
            }
        }
    }

    commitInstruction();
    return inst;
}

bool IssueState::storeIDMatch(SimInst &inst)
{
    if (inst->opcode == Opcode::OP_TSD) {
        return true;
    }
    if (!OpcodeIsLoad(inst->opcode) && !OpcodeIsStore(inst->opcode)) {
        return true;
    }
    if (top->top->core->IsVectorIex(top->top->machineType)) {
        return true;
    }

    if (top->top->id == SCALAR_IEX) {
        if (sliding_window_sid > 0) {
            return (inst->sid < sliding_window_sid);
        }
        if (sliding_window_load_id > 0) {
            return (inst->load_id < sliding_window_load_id);
        }

        return true;
    }

    ASSERT(top->top->core->IsVectorIex(top->top->machineType) || top->top->id == MEM_IEX);
    IDBus oldestBus;

    if (top->top->core->IsVectorIex(top->top->machineType)) {
        oldestBus = top->GetSim()->core->GetSimtOldestInfo();
    } else if (top->top->id == MEM_IEX) {
        oldestBus = top->GetSim()->core->GetSimtOldestInfo();
    }

    ASSERT(oldestBus.vld);
    bool last = top->top->IsLastLoadStore(inst, oldestBus);
    uint32_t simtLane = top->top->core->configs.simtLane;
    if (last) {
        // There must be space for oldest load request. but oldest store request may be in stq.
        if (inst->opcode == Opcode::OP_TLD) {
            return !top->top->GetLsucheckLoadCltStall(0, 1, inst->stid);
        }
        if (OpcodeIsLoad(inst->opcode)) {
            return !top->top->GetLsucheckLoadCltStall(0, simtLane, inst->stid);
        }
        if (OpcodeIsStore(inst->opcode)) {
            return !top->top->GetLsucheckStoreStall(simtLane, inst->stid);
        }
    }

    if (OpcodeIsLoad(inst->opcode)) {
        uint32_t reserverWidth = top->top->iexLdaIqCount * simtLane;
        if (top->top->core->IsVectorIex(top->top->machineType)) {
            reserverWidth = top->top->iexAguIqCount * top->top->iexAguPickCount * simtLane;
        }
        if (inst->opcode == Opcode::OP_TLD && (top->top->id == MEM_IEX)) {
            reserverWidth = top->top->iexLdaIqCount * top->top->iexLdaPickCount;
        }
        uint32_t acvtiveLdCnt = top->top->GetPipeActiveLoad();
        uint32_t acvtiveTLdCnt = top->top->GetPipeActiveTLoad();
        ASSERT(acvtiveTLdCnt == 0);
        if (top->top->id == MEM_IEX) {
            uint32_t currentSpace = inst->opcode == Opcode::OP_TLD ? 1 : simtLane;
            uint32_t oldestSpace = oldestBus.isTld ? 1 : simtLane;
            acvtiveLdCnt = top->top->GetPipeMtcActiveLoad();
            uint32_t checkWidth = currentSpace + oldestSpace + (acvtiveLdCnt - acvtiveTLdCnt) * simtLane +
                acvtiveTLdCnt;
            return !top->top->GetLsucheckLoadCltStall(0, checkWidth, inst->stid);
        }
        // Reserve space for lastinst: current entry, pipe entry, and 1 entry for oldest;
        uint32_t checkWidth = 2 * reserverWidth + (acvtiveLdCnt -  acvtiveTLdCnt) * simtLane +
            acvtiveTLdCnt;
        return !top->top->GetLsucheckLoadCltStall(0, checkWidth, inst->stid);
    }

    ASSERT(OpcodeIsStore(inst->opcode));
    uint32_t reserverWidth = top->top->iexStaIqCount * simtLane;
    if (top->top->core->IsVectorIex(top->top->machineType)) {
        // in agu both picker can pick load, but only one picker can pick store, so no need to multiply pick count
        reserverWidth = top->top->iexAguIqCount * simtLane;
    }
    uint32_t acvtiveStCnt = top->top->GetPipeActiveStore();
    uint32_t checkWidth = 2 * reserverWidth + acvtiveStCnt * simtLane;
    return !top->top->GetLsucheckStoreStall(checkWidth, inst->stid);
}

void IssueState::ReleaseEntry(ROBID bid, ROBID rid, uint32_t stid)
{
    for (uint32_t i = 0; i < entry.size(); i++) {
        if (!entry[i]) {
            continue;
        }
        if (entry[i]->bid != bid || entry[i]->rid != rid || !entry[i]->issued) {
            continue;
        }
        if (!entry[i]->issued) {
            --sizeNotIssued;
            continue;
        }
        entry[i] = std::shared_ptr<SimInstInfo>(nullptr);
        ASSERT(size>0);
        size--;
        for (uint32_t k=i; k<entry.size(); k++) {
            if (k == entry.size()-1) {
                entry[k] = std::shared_ptr<SimInstInfo>(nullptr);
                break;
            }
            entry[k] = entry[k+1];
        }

        if (pAgeQueue) {
            pAgeQueue->Release(bid, rid, stid);
        }
        return;
    }
}

void IssueState::wakeupStackLoad(ROBID bid, ROBID rid, PLpvInfo &lpvInfo)
{
    for (uint32_t i = 0; i < entry.size(); i++) {
        if (!entry[i]) {
            continue;
        }
        if (entry[i]->bid != bid || entry[i]->rid != rid) {
            continue;
        }
        SimInst inst = std::make_shared<SimInstInfo>(*(entry[i]));
        inst->stack_type = StackInstType::STACK_LOAD;
        inst->stack_renamed = true;
        POperandPtr srcSP = inst->GetPSrcPtrByType(OperandType::STACK_POINTER);
        SetLpv(lpvInfo, srcSP->lpvInfo);
        srcSP->ready = true;
        srcSP->wakeupTime = top->GetSim()->getCycles();
        entry[i] = inst;
        return;
    }
}

void IssueState::Reset()
{
    for (auto &e : entry) {
        e = std::shared_ptr<SimInstInfo>(nullptr);
    }
    size = 0;
    sizeNotIssued = 0;
    insert_stall = false;
}

void IssueState::Build(uint32_t size)
{
    entry.resize(size);
    for (uint32_t i = 0; i < entry.size(); i++) {
        entry[i] = std::make_shared<SimInstInfo>();
    }
    Reset();
}

void IssueState::insert(SimInst inst) {
    ASSERT(size < entry.size());
    inst->iq_name = this->name;
    if (top->order == IsqOrder::STRICTLY_INORDER) {
        inst->insertIsqId = insertId;
        IncROBID(insertId, UINT32_MAX);
    }
    entry[size] = inst;
    size++;
    ++sizeNotIssued;
    insertEle = true;

    if (pAgeQueue) {
        pAgeQueue->insert(inst);
    }
}

bool IssueState::Wakeup(const WakeupInfo& wakeInfo, PLpvInfo lpvInfo, uint32_t peID, uint32_t tid, uint32_t stid)
{
    const OperandType type = wakeInfo.type;
    const uint32_t ptag = wakeInfo.ptag;
    const bool recycled = wakeInfo.recycled;
    // only VRF can be recycled
    ASSERT(!recycled || (recycled && (type == OperandType::OPD_VTLINK ||
        type == OperandType::OPD_VULINK ||
        type == OperandType::OPD_VMLINK ||
        type == OperandType::OPD_VNLINK)));
    bool wakeup = false;
    for (uint32_t i = 0; i < entry.size(); i++) {
        if (entry[i] == nullptr || (OperandTypeIsLocalReg(type) &&
                                   (entry[i]->peID != peID || entry[i]->tid != tid || entry[i]->stid != stid))) {
            continue;
        }
        for (size_t j = 0; j < entry[i]->psrcs_.size(); ++j) {
            auto psrc = entry[i]->psrcs_[j];
            if (psrc->type == type && psrc->ptag == ptag && psrc->recycled == recycled) {
                SimInst inst = std::make_shared<SimInstInfo>(*entry[i]);
                inst->psrcs_[j]->ready = true;
                inst->psrcs_[j]->wakeupTime = top->GetSim()->getCycles();
                SetLpv(lpvInfo, inst->psrcs_[j]->lpvInfo);
                entry[i] = inst;
                if (entry[i]->IsReady()) {
                    entry[i]->pipeCycle->rdyCycle = top->GetSim()->getCycles();
                }
                wakeup = true;
            }
        }
    }
    return wakeup;
}

// void IssueState::WakeupTmpSrcD(uint32_t dTag, uint32_t peid, PLpvInfo &lpvInfo, uint32_t tid)
// {
//     for (uint32_t i = 0; i < entry.size(); i++) {
//         if (!entry[i] || entry[i]->peID != peid || entry[i]->issued || entry[i]->tid != tid) {
//             continue;
//         }
//         if (entry[i]->tmpSrc.vld && entry[i]->tmpSrc.dTagVld &&
//             entry[i]->tmpSrc.dTag == dTag && entry[i]->tmpSrc.rdy == false) {
//             SimInst inst = std::make_shared<UInstInfo>(*entry[i]);
//             inst->tmpSrc.rdy = true;
//             SetLpv(lpvInfo, inst->tmpSrc.lpvInfo);
//             entry[i] = inst;
//             if (entry[i]->ready()) {
//                 entry[i]->pipeCycle->rdyCycle = top->GetSim()->getCycles();
//             }
//         }
//     }
// }

void IssueState::wakeupLda(uint64_t id)
{
    // auto check_store = [&id] (LoadStoreInfo &ldInfo) -> bool {
    //     for (auto it = ldInfo.depend_id.begin(); it != ldInfo.depend_id.end(); ) {
    //         if (*it == id) {
    //             it = ldInfo.depend_id.erase(it);
    //             return true;
    //         }
    //         it++;
    //     }
    //     return false;
    // };
    // for (uint32_t i = 0; i < entry.size(); i++) {
    //     if (!entry[i]) {
    //         continue;
    //     }
    //     bool match = check_store(entry[i]->ref.lsInfo);
    //     if (match && entry[i]->ref.lsInfo.depend_id.empty()) {
    //         SimInst inst = std::make_shared<UInstInfo>(*entry[i]);
    //         inst->addr_rdy = true;
    //         entry[i] = inst;
    //         if (entry[i]->ready()) {
    //             entry[i]->pipeCycle->rdyCycle = top->GetSim()->getCycles();
    //         }
    //     }
    // }
}

void IssueState::windowSlides(uint64_t distance, bool isLoad)
{
    if (isLoad) {
        sliding_window_load_id += distance;
        return;
    }
    sliding_window_sid += distance;
}

void IssueState::wakeupAddrSrc()
{
    for (uint32_t i = 0; i < top->GetSim()->core->configs.scalar_smt_thread; ++i) {
        wakeupAddrSrc(i);
    }
}

void IssueState::wakeupAddrSrc(uint32_t stid)
{
    if (!top->top->iexmdb.mdb_enable) {
        return;
    }
    ROBID oldestBID = top->top->core->bctrl->blockROB.getOldestBlockID(stid);
    for (uint32_t i = 0; i < entry.size(); i++) {
        if (!entry[i]) {
            continue;
        }
        for (auto it = entry[i]->psrcs_.begin(); it != entry[i]->psrcs_.end();) {
            auto src = *it;
            if (src->type == OperandType::LS_MDB_DEPENDENCY) {
                ROBIDBus oldestLSID = top->top->core->peArray[entry[i]->peID]->GetOldestLSID(entry[i]->GetTid());
                if (top->top->iexmdb.isStRdy(src->ptag, entry[i]->addrWakeuped) ||
                    (entry[i]->bid == oldestBID && oldestLSID.vld && entry[i]->lsID == oldestLSID.id)) {
                    top->top->iexmdb.release(src->ptag);
                    if (entry[i]->IsReady()) {
                        entry[i]->pipeCycle->rdyCycle = top->GetSim()->getCycles();
                    }
                    it = entry[i]->psrcs_.erase(it);
                } else {
                    ++it;
                }
            } else {
                ++it;
            }
        }
    }
}

void IssueState::flush(FlushBus flushReq)
{
    auto match_flush = [&flushReq](SimInst &inst)->bool {
        if (!inst) {
            return false;
        }
        if (flushReq.req.stid != inst->stid) {
            return false;
        }
        if (flushReq.baseOnPE && flushReq.req.peID != inst->peID) {
            return false;
        }
        if (flushReq.baseOnThread && flushReq.req.tid != inst->GetTid()) {
            return false;
        }
        return flushReq.baseOnBid ? LessEqual(flushReq.req.bid, inst->bid):
            LessEqual(flushReq.req.bid, flushReq.req.rid, inst->bid, inst->rid);
    };

    for (uint32_t i = 0; i < entry.size(); i++) {
        if (!entry[i]) {
            continue;
        }

        if (match_flush(entry[i])) {
            if (pAgeQueue) {
                pAgeQueue->Release(entry[i]->bid, entry[i]->rid, entry[i]->stid);
            }
            if (!entry[i]->issued) {
                --sizeNotIssued;
            }
            entry[i] = std::shared_ptr<SimInstInfo>(nullptr);
            ASSERT(size>0);
            size--;
        }
        if (!size) {
            break;
        }
    }

    // Remove bubbles in the queue
    for (int i = 0; i < static_cast<int>(entry.size() - 1); i++) {
        if (entry[i]) {
            continue;
        }
        for (uint32_t j=i+1; j<entry.size(); j++) {
            if (!entry[j]) {
                continue;
            }
            entry[i] = entry[j];
            entry[j] = std::shared_ptr<SimInstInfo>(nullptr);
            break;
        }
    }
}

void IssueState::setCancel(uint32_t pipe)
{
    for (uint32_t i = 0; i < entry.size(); i++) {
        if (entry[i] && entry[i]->CheckCancel(pipe)) {
            for (auto psrc : entry[i]->psrcs_) {
                if (psrc->lpvInfo != nullptr && psrc->lpvInfo->CheckCancel(pipe)) {// !entry[i]->ref.gsInfo.src_vld
                    psrc->ready = false;
                    psrc->lpvInfo->Reset();
                }
            }

            entry[i]->issued = false;
            if (pAgeQueue) {
                pAgeQueue->issued(entry[i], false);
            }
        }
    }
}

string RdPortControl::GetPipeSrcIdStr(PipeSrcId pipeSrcId)
{
    switch (pipeSrcId) {
        case PipeSrcId::ALU0_SRC0:
            return "ALU0_SRC0";
        case PipeSrcId::ALU0_SRC1:
            return "ALU0_SRC1";
        case PipeSrcId::ALU0_SRC2:
            return "ALU0_SRC2";
        case PipeSrcId::ALU1_SRC0:
            return "ALU1_SRC0";
        case PipeSrcId::ALU1_SRC1:
            return "ALU1_SRC1";
        case PipeSrcId::ALU1_SRC2:
            return "ALU1_SRC2";
        case PipeSrcId::ALU2_SRC0:
            return "ALU2_SRC0";
        case PipeSrcId::ALU2_SRC1:
            return "ALU2_SRC1";
        case PipeSrcId::ALU2_SRC2:
            return "ALU2_SRC2";
        case PipeSrcId::AGU0_SRC0:
            return "AGU0_SRC0";
        case PipeSrcId::AGU0_SRC1:
            return "AGU0_SRC1";
        case PipeSrcId::AGU0_SRC2:
            return "AGU0_SRC2";
        case PipeSrcId::AGU1_SRC0:
            return "AGU1_SRC0";
        case PipeSrcId::AGU1_SRC1:
            return "AGU1_SRC1";
        case PipeSrcId::AGU1_SRC2:
            return "AGU1_SRC2";
        default:
            ASSERT(false);
    }
    return string();
}

RdPortControl::RdPortControl(IEX* top)
    : mTop(top)
{}

RdPortControl::~RdPortControl() {}

void RdPortControl::Build()
{
//     std::vector<std::set<uint32_t>> vrfPipeSrcRdPorts; // PipeSrcId, ReadPort
//     std::vector<std::set<uint32_t>> vrfRdPortBanksInit; // ReadPort, Bank
    vector<uint32_t> banks(mTop->configs.simt_vrf_bank_num);
    iota(banks.begin(), banks.end(), 0);

    mVrfPipeSrcRdPorts.resize(static_cast<uint32_t>(PipeSrcId::NUM));
    mVrfRdPortBanksInit.assign(mTop->configs.simt_iex_read_port_num, set<uint32_t>(begin(banks), end(banks)));

    auto initSrcRdPorts = [this](const vector<uint64_t>& cfg, PipeSrcId pipeSrcId) {
        mVrfPipeSrcRdPorts.at(static_cast<uint32_t>(pipeSrcId)) = vector<uint32_t>(begin(cfg), end(cfg));
    };
    for (uint32_t portId = 0; portId < mTop->configs.simt_iex_read_port_num; portId++) {
        initSrcRdPorts(mTop->configs.simt_iex_alu0_src0_rd_port, PipeSrcId::ALU0_SRC0);
        initSrcRdPorts(mTop->configs.simt_iex_alu0_src1_rd_port, PipeSrcId::ALU0_SRC1);
        initSrcRdPorts(mTop->configs.simt_iex_alu0_src2_rd_port, PipeSrcId::ALU0_SRC2);
        initSrcRdPorts(mTop->configs.simt_iex_alu1_src0_rd_port, PipeSrcId::ALU1_SRC0);
        initSrcRdPorts(mTop->configs.simt_iex_alu1_src1_rd_port, PipeSrcId::ALU1_SRC1);
        initSrcRdPorts(mTop->configs.simt_iex_alu1_src2_rd_port, PipeSrcId::ALU1_SRC2);
        initSrcRdPorts(mTop->configs.simt_iex_alu2_src0_rd_port, PipeSrcId::ALU2_SRC0);
        initSrcRdPorts(mTop->configs.simt_iex_alu2_src1_rd_port, PipeSrcId::ALU2_SRC1);
        initSrcRdPorts(mTop->configs.simt_iex_alu2_src2_rd_port, PipeSrcId::ALU2_SRC2);
        initSrcRdPorts(mTop->configs.simt_iex_agu0_src0_rd_port, PipeSrcId::AGU0_SRC0);
        initSrcRdPorts(mTop->configs.simt_iex_agu0_src1_rd_port, PipeSrcId::AGU0_SRC1);
        initSrcRdPorts(mTop->configs.simt_iex_agu0_src2_rd_port, PipeSrcId::AGU0_SRC2);
        initSrcRdPorts(mTop->configs.simt_iex_agu1_src0_rd_port, PipeSrcId::AGU1_SRC0);
        initSrcRdPorts(mTop->configs.simt_iex_agu1_src1_rd_port, PipeSrcId::AGU1_SRC1);
        initSrcRdPorts(mTop->configs.simt_iex_agu1_src2_rd_port, PipeSrcId::AGU1_SRC2);
    }
}

void RdPortControl::Reset()
{
    mVrfRdPortBanksAvailable = mVrfRdPortBanksInit;
}

bool RdPortControl::IssueAndUpdate(PipeId pipe, const SimInst& inst, bool update)
{
    PipeSrcId pipeSrcId = PipeSrcId::UNDEF;
    switch (pipe) {
        case PipeId::ALU0:
            pipeSrcId = PipeSrcId::ALU0_SRC0;
            break;
        case PipeId::ALU1:
            pipeSrcId = PipeSrcId::ALU1_SRC0;
            break;
        case PipeId::ALU2:
            pipeSrcId = PipeSrcId::ALU2_SRC0;
            break;
        case PipeId::AGU0:
            pipeSrcId = PipeSrcId::AGU0_SRC0;
            break;
        case PipeId::AGU1:
            pipeSrcId = PipeSrcId::AGU1_SRC0;
            break;
        default:
            ASSERT(false);
            return true;
    }
    const uint32_t noPort = -1U;
    vector<set<uint32_t>> vrfRdPortBanksAvailableCopy = mVrfRdPortBanksAvailable;
    vector<pair<uint32_t, uint32_t>> pipeSrcUsedPorts;
    auto getBankId = [this] (uint32_t vrfPtag) {
        return vrfPtag % mTop->configs.simt_vrf_bank_num;
    };
    auto getAvailablePort = [&vrfRdPortBanksAvailableCopy, noPort, this] (const vector<uint32_t>& rdPorts,
        uint32_t bankId) {
        ASSERT(!rdPorts.empty());
        for (auto& port : rdPorts) {
            ASSERT(port < mTop->configs.simt_iex_read_port_num);
            if (vrfRdPortBanksAvailableCopy.at(port).find(bankId) != vrfRdPortBanksAvailableCopy.at(port).cend()) {
                return port;
            }
        }
        return noPort;
    };
    const uint64_t currentCycle = mTop->GetSim()->getCycles();
    uint32_t srcOffset = 0;
    for (auto &psrc : inst->psrcs_) {
        if (OperandTypeIsVReg(psrc->type) && !psrc->CanBypass(currentCycle)) {
            const uint32_t bankId = getBankId(psrc->ptag);
            uint32_t port = getAvailablePort(mVrfPipeSrcRdPorts.at(static_cast<uint32_t>(pipeSrcId) + srcOffset), bankId);
            ASSERT(!update || (port != noPort));
            if (port == noPort) {
                return false;
            }
            vrfRdPortBanksAvailableCopy.at(port).erase(bankId);
            pipeSrcUsedPorts.emplace_back(make_pair(static_cast<uint32_t>(pipeSrcId) + srcOffset, port));
            ++srcOffset;
        }
    }
    if (update) {
        mVrfRdPortBanksAvailable = vrfRdPortBanksAvailableCopy;
        for (auto& it : pipeSrcUsedPorts) {
            mTop->stats->vectorRdPortUsedCnt.at(it.second) += 1;
            mTop->stats->vectorPipeSrcRdPortUsedCnt.at(it.first).at(it.second) += 1;
        }
    }
    return true;
}

bool RdPortControl::CanIssue(PipeId pipe, const SimInst& inst)
{
    return IssueAndUpdate(pipe, inst, false);
}

void RdPortControl::PostIssueUpdate(PipeId pipe, const SimInst& inst)
{
    (void)IssueAndUpdate(pipe, inst, true);
}

void IssueQueue::setCancel(const SimInst& inst, uint32_t pip)
{
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].setCancel(inst, pip);
    }
}

void IssueState::setCancel(const SimInst& inst, uint32_t pipe)
{
    PLpvInfo lpvInfo;
    PEResolveBus peResolve;
    peResolve.peid = inst->peID;
    peResolve.bid = inst->bid;
    peResolve.rid = inst->rid;
    peResolve.stid = inst->stid;
    peResolve.isLDCancel = true;
    uint32_t tid = 0;
    if (top->GetSim()->core->IsVectorIex(top->top->machineType)) {
        tid = inst->GetTid();
    } else {
        tid = inst->stid;
    }
    top->top->iex_pe_rslv_array[inst->peID][tid]->Write(peResolve);
    for (uint32_t i = 0; i < entry.size() && entry[i]; i++) {
        if (entry[i]->bid != inst->bid || entry[i]->rid != inst->rid || !entry[i]->issued) {
            continue;
        }
        for (auto psrc : entry[i]->psrcs_) {
            if (psrc->lpvInfo != nullptr && psrc->lpvInfo->CheckCancel(pipe)) {// !entry[i]->ref.gsInfo.src_vld
                psrc->ready = false;
                psrc->lpvInfo->Reset();
            }
        }
        entry[i]->issued = false;
        if (pAgeQueue) {
            pAgeQueue->issued(entry[i], false);
        }
    }
}

bool IssueState::full()
{
    return (size >= entry.size());
}

bool IssueState::reservefull()
{
    return (size >= (entry.size() - reserveSize));
}

void IssueState::moveLpv()
{
    for (uint32_t i = 0; i < entry.size(); i++) {
        if (entry[i]) {
            entry[i]->MoveLpv();
        }
    }
}

IssueState& IssueState::operator=(const IssueState &rhs)
{
    if (this != &rhs) {
        this->entry = rhs.entry;
        this->insert_stall = rhs.insert_stall;
        this->size = rhs.size;
        this->sizeNotIssued = rhs.sizeNotIssued;
        this->reserveSize = rhs.reserveSize;
        this->selected_biot_id = rhs.selected_biot_id;
        this->sliding_window_vld = rhs.sliding_window_vld;
        this->sliding_window_sid = rhs.sliding_window_sid;
        this->sliding_window_load_id = rhs.sliding_window_load_id;
    }
    return *this;
}

void IssueQueue::Build()
{
    configs.overrideDefaultConfig(GetSim()->getCfgs());
    cmdIQStall = false;
    aluIQStall = false;
    aguIQStall = false;
    ldaIQStall = false;
    ldaSCIQStall = false;
    staIQStall = false;
    stdIQStall = false;
    bruIQStall = false;
    scaIQStall = false;

    if (configs.isq_order >= static_cast<uint64_t>(IsqOrder::COUNT)) {
        ASSERT(0 && "isq parameter is invalid");
    }
    order = static_cast<IsqOrder>(configs.isq_order);

    // Build agu-age queue
    if (top->core->IsVectorIex(top->machineType)) {
        uint32_t size = top->iexAguIqDepth * top->iexAguIqDepth * top->iexAguPickCount;
        current.aguAgeQueue.configs = &top->configs;
        next.aguAgeQueue.configs = &top->configs;
        current.aguAgeQueue.top = top;
        next.aguAgeQueue.top = top;
        current.aguAgeQueue.Build(size);
        next.aguAgeQueue.Build(size);

         // scalperQ
        current.aguSCAgeQueue.configs = &top->configs;
        next.aguSCAgeQueue.configs = &top->configs;
        current.aguSCAgeQueue.top = top;
        next.aguSCAgeQueue.top = top;
        current.aguSCAgeQueue.Build(size);
        next.aguSCAgeQueue.Build(size);
    } else {
        uint32_t loadSize = top->iexLdaIqDepth * top->iexLdaIqCount;
        uint32_t storeSize = top->iexStaIqDepth * top->iexStaIqCount;
        uint32_t size = loadSize + storeSize;
        current.aguAgeQueue.configs = &top->configs;
        next.aguAgeQueue.configs = &top->configs;
        current.aguAgeQueue.top = top;
        next.aguAgeQueue.top = top;
        current.aguAgeQueue.Build(size);
        next.aguAgeQueue.Build(size);

        // scalperQ
        current.aguSCAgeQueue.configs = &top->configs;
        next.aguSCAgeQueue.configs = &top->configs;
        current.aguSCAgeQueue.top = top;
        next.aguSCAgeQueue.top = top;
        current.aguSCAgeQueue.Build(size);
        next.aguSCAgeQueue.Build(size);
    }

    // Build issue queue
    for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
        current.cmdIQ.emplace_back();
        next.cmdIQ.emplace_back();
        current.cmdIQ[i].top = this;
        next.cmdIQ[i].top = this;
        current.cmdIQ[i].Build(top->iexCmdIqDepth);
        next.cmdIQ[i].Build(top->iexCmdIqDepth);
        current.cmdIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        next.cmdIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        current.cmdIQ[i].name = "CMD "+ std::to_string(i);
        next.cmdIQ[i].name = "CMD "+ std::to_string(i);
    }
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        current.aluIQ.emplace_back();
        next.aluIQ.emplace_back();
        current.aluIQ[i].top = this;
        next.aluIQ[i].top = this;
        current.aluIQ[i].Build(top->iexAluIqDepth);
        next.aluIQ[i].Build(top->iexAluIqDepth);
        current.aluIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        next.aluIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        current.aluIQ[i].name = "ALU "+ std::to_string(i);
        next.aluIQ[i].name = "ALU "+ std::to_string(i);
    }
    LSUConfig lsuConfig;
    lsuConfig.overrideDefaultConfig(GetSim()->getCfgs());
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        current.aguIQ.emplace_back();
        next.aguIQ.emplace_back();
        current.aguIQ[i].top = this;
        next.aguIQ[i].top = this;
        current.aguIQ[i].Build(top->iexAguIqDepth);
        next.aguIQ[i].Build(top->iexAguIqDepth);
        uint32_t count = lsuConfig.lu_clusters_depth;
        if (top->id == SCALAR_IEX) {
            count = top->core->configs.scalar_lsu_load_windows;
        } else if (top->id == MEM_IEX) {
            count = top->core->configs.mtc_lsu_load_windows;
        }
        current.aguIQ[i].sliding_window_load_id = count - top->iexAguIqCount * top->iexAguPickCount;
        next.aguIQ[i].sliding_window_load_id = count - top->iexAguIqCount * top->iexAguPickCount;
        current.aguIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        next.aguIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        current.aguIQ[i].name = "AGU "+ std::to_string(i);
        next.aguIQ[i].name = "AGU "+ std::to_string(i);
        current.aguIQ[i].pAgeQueue = &current.aguAgeQueue;
        next.aguIQ[i].pAgeQueue = &next.aguAgeQueue;

        if (top->core->IsVectorIex(top->machineType)) {
            current.aguIQ[i].sliding_window_sid = lsuConfig.stq_depth;
            next.aguIQ[i].sliding_window_sid = lsuConfig.stq_depth;
        }
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        current.ldaIQ.emplace_back();
        next.ldaIQ.emplace_back();
        current.ldaIQ[i].top = this;
        next.ldaIQ[i].top = this;
        current.ldaIQ[i].Build(top->iexLdaIqDepth);
        next.ldaIQ[i].Build(top->iexLdaIqDepth);
        uint32_t count = lsuConfig.lu_clusters_depth;
        if (top->id == SCALAR_IEX) {
            count = top->core->configs.scalar_lsu_load_windows;
        } else if (top->id == MEM_IEX) {
            count = top->core->configs.mtc_lsu_load_windows;
        }
        current.ldaIQ[i].sliding_window_load_id = count - top->iexLdaIqCount;
        next.ldaIQ[i].sliding_window_load_id = count - top->iexLdaIqCount;
        current.ldaIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        next.ldaIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        current.ldaIQ[i].name = "LDA "+ std::to_string(i);
        next.ldaIQ[i].name = "LDA "+ std::to_string(i);
        current.ldaIQ[i].pAgeQueue = &current.aguAgeQueue;
        next.ldaIQ[i].pAgeQueue = &next.aguAgeQueue;
    }

    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        current.ldaScIQ.emplace_back();
        next.ldaScIQ.emplace_back();
        current.ldaScIQ[i].top = this;
        next.ldaScIQ[i].top = this;
        current.ldaScIQ[i].Build(top->iexLdaIqDepth);
        next.ldaScIQ[i].Build(top->iexLdaIqDepth);
        uint32_t count = lsuConfig.lu_clusters_depth;
        if (top->id == SCALAR_IEX) {
            count = lsuConfig.scalar_lu_clusters_depth;
        }
        current.ldaScIQ[i].sliding_window_load_id = count - top->iexLdaIqCount;
        next.ldaScIQ[i].sliding_window_load_id = count - top->iexLdaIqCount;
        current.ldaScIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        next.ldaScIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        current.ldaScIQ[i].name = "LDA_SC "+ std::to_string(i);
        next.ldaScIQ[i].name = "LDA_SC "+ std::to_string(i);
        current.ldaScIQ[i].pAgeQueue = &current.aguSCAgeQueue;
        next.ldaScIQ[i].pAgeQueue = &next.aguSCAgeQueue;
    }
    if (!top->core->IsVectorIex(top->machineType)) {
        // LSUConfig lsuConfig;
        for (uint32_t i = 0; i < top->iexStaIqCount; i++) {
            current.staIQ.emplace_back();
            next.staIQ.emplace_back();
            current.staIQ[i].top = this;
            next.staIQ[i].top = this;
            current.staIQ[i].Build(top->iexStaIqDepth);
            next.staIQ[i].Build(top->iexStaIqDepth);
            current.staIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
            next.staIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
            if (top->core->configs.BSB_enable) {
                next.staIQ[i].sliding_window_vld = true;
                current.staIQ[i].sliding_window_vld = true;
            }
            uint64_t count = lsuConfig.stq_depth;
            if (top->id == SCALAR_IEX) {
                count = top->core->configs.scalar_lsu_store_windows;
            } else if (top->id == MEM_IEX) {
                count = top->core->configs.mtc_lsu_store_windows;
            }
            next.staIQ[i].sliding_window_sid = count;
            current.staIQ[i].sliding_window_sid = count;
            current.staIQ[i].name = "STA "+ std::to_string(i);
            next.staIQ[i].name = "STA "+ std::to_string(i);
            current.staIQ[i].pAgeQueue = &current.aguAgeQueue;
            next.staIQ[i].pAgeQueue = &next.aguAgeQueue;
        }
    }
    for (uint32_t i = 0; i < top->iexStdIqCount; i++) {
        current.stdIQ.emplace_back();
        next.stdIQ.emplace_back();
        current.stdIQ[i].top = this;
        next.stdIQ[i].top = this;
        current.stdIQ[i].Build(top->iexStdIqDepth);
        next.stdIQ[i].Build(top->iexStdIqDepth);
        current.stdIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        next.stdIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        if (top->core->configs.BSB_enable) {
            next.stdIQ[i].sliding_window_vld = true;
            current.stdIQ[i].sliding_window_vld = true;
        }
        uint64_t count = lsuConfig.stq_depth;
        if (top->id == SCALAR_IEX) {
            count = top->core->configs.scalar_lsu_store_windows;
        } else if (top->id == MEM_IEX) {
            count = top->core->configs.mtc_lsu_store_windows;
        }
        next.stdIQ[i].sliding_window_sid = count;
        current.stdIQ[i].sliding_window_sid = count;
        current.stdIQ[i].name = "STD "+ std::to_string(i);
        next.stdIQ[i].name = "STD "+ std::to_string(i);
    }
    for (uint32_t i = 0; i < top->iexBruIqCount; i++) {
        current.bruIQ.emplace_back();
        next.bruIQ.emplace_back();
        current.bruIQ[i].top = this;
        next.bruIQ[i].top = this;
        current.bruIQ[i].Build(top->iexBruIqDepth);
        next.bruIQ[i].Build(top->iexBruIqDepth);
        current.bruIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        next.bruIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        current.bruIQ[i].name = "BRU "+ std::to_string(i);
        next.bruIQ[i].name = "BRU "+ std::to_string(i);
    }
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        current.scaIQ.emplace_back();
        next.scaIQ.emplace_back();
        current.scaIQ[i].top = this;
        next.scaIQ[i].top = this;
        current.scaIQ[i].Build(top->iexScaIqDepth);
        next.scaIQ[i].Build(top->iexScaIqDepth);
        current.scaIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        next.scaIQ[i].reserveSize =  configs.iex_iq_reserve_entry;
        current.scaIQ[i].name = "VecSca "+ std::to_string(i);
        next.scaIQ[i].name = "VecSca "+ std::to_string(i);
    }

    // Build internal pipeline
    selectCmdInst.assign(top->iexCmdIqCount * top->iexCmdPickCount, nullptr);
    selectAluInst.assign(top->iexAluIqCount * top->iexAluPickCount, nullptr);
    selectAguInst.assign(top->iexAguIqCount * top->iexAguPickCount, nullptr);
    selectLdaInst.assign(top->iexLdaIqCount * top->iexLdaPickCount, nullptr);
    selectSCLdaInst.assign(top->iexLdaIqCount * top->iexLdaPickCount, nullptr);
    selectStaInst.assign(top->iexStaIqCount * top->iexStaPickCount, nullptr);
    selectStdInst.assign(top->iexStdIqCount * top->iexStdPickCount, nullptr);
    selectBruInst.assign(top->iexBruIqCount * top->iexBruPickCount, nullptr);
    aluPipeStallFn.assign(top->iexAluIqCount * top->iexAluPickCount, nullptr);

    vab = std::make_shared<VAB>();
    vab->iex_iq_top = this;
    vab->sim = GetSim();
    vab->Build(GetSim()->core->configs.simtLane);

    for (uint32_t i = 0; i < top->iexScaIqCount * top->iexScaPickCount; i++) {
        selectScaInst.emplace_back(std::make_shared<SimInstInfo>());
    }
    scaPipeStallFn.assign(top->iexScaIqCount * top->iexScaPickCount, nullptr);

    if (top->core->IsVectorIex(top->machineType)) {
        InitVecALUHelperFnAndStruct();
        // for Read Port Limit
        if (top->configs.simt_iex_read_port_num > 0) {
            rdPortCtrl = make_shared<RdPortControl>(top);
            rdPortCtrl->Build();
        }
    }
}

void IssueQueue::Reset() {
    for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
        current.cmdIQ[i].Reset();
        next.cmdIQ[i].Reset();
    }
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        current.aluIQ[i].Reset();
        next.aluIQ[i].Reset();
    }
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        current.aguIQ[i].Reset();
        next.aguIQ[i].Reset();
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        current.ldaIQ[i].Reset();
        next.ldaIQ[i].Reset();
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        current.ldaScIQ[i].Reset();
        next.ldaScIQ[i].Reset();
    }
    for (uint32_t i = 0; i < top->iexStaIqCount; i++) {
        current.staIQ[i].Reset();
        next.staIQ[i].Reset();
    }
    for (uint32_t i = 0; i < top->iexStdIqCount; i++) {
        current.stdIQ[i].Reset();
        next.stdIQ[i].Reset();
    }
    for (uint32_t i = 0; i < top->iexBruIqCount; i++) {
        current.bruIQ[i].Reset();
        next.bruIQ[i].Reset();
    }
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        current.scaIQ[i].Reset();
        next.scaIQ[i].Reset();
    }

    selectCmdInst.assign(top->iexCmdIqCount * top->iexCmdPickCount, nullptr);
    selectAluInst.assign(top->iexAluIqCount * top->iexAluPickCount, nullptr);
    selectAguInst.assign(top->iexAguIqCount * top->iexAguPickCount, nullptr);
    selectLdaInst.assign(top->iexLdaIqCount * top->iexLdaPickCount, nullptr);
    selectSCLdaInst.assign(top->iexLdaIqCount * top->iexLdaPickCount, nullptr);
    selectStaInst.assign(top->iexStaIqCount * top->iexStaPickCount, nullptr);
    selectStdInst.assign(top->iexStdIqCount * top->iexStdPickCount, nullptr);
    selectBruInst.assign(top->iexBruIqCount * top->iexBruPickCount, nullptr);
    selectScaInst.assign(top->iexScaIqCount * top->iexScaPickCount, nullptr);
}

static bool checkInstOptMatch (const SimInst& inst) {
    if (OpcodeIsLoad(inst->opcode) || OpcodeIsStore(inst->opcode)) {
        return false;
    }
    return inst->RangedDataReady();
}

void IssueQueue::Work()
{
    if (rdPortCtrl) {
        rdPortCtrl->Reset();
    }
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].wakeupAddrSrc();
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaIQ[i].wakeupAddrSrc();
    }
    recieveWakeReq();
    // issue & set outputs
    issueCMD();
    issueALU();
    if (top->core->IsVectorIex(top->machineType) && configs.VAB_EN) {
        issueGatherAGU();
    }
    issueVecAGU();
    issueAGU();
    issueSCAGU();
    issueSTA();
    issueSTD();
    issueBRU();
    issueVecSca();

    if (top->core->IsVectorIex(top->machineType)) {
        vector<uint64_t> bankRdCnt;
        bankRdCnt.assign(top->configs.simt_vrf_bank_num, 0);
        const auto updateRdCnt = [&bankRdCnt, this](const SimInst& inst) {
            if (!inst) {
                return;
            }
            const uint64_t currentCycle = GetSim()->getCycles();
            for (auto &psrc : inst->psrcs_) {
                if (OperandTypeIsVReg(psrc->type) && !psrc->CanBypass(currentCycle)) {
                    ASSERT(psrc->ptag != -1U);
                    bankRdCnt[psrc->ptag % top->configs.simt_vrf_bank_num] += 1;
                }
            }
        };
        for (auto& inst : selectAluInst) {
            updateRdCnt(inst);
        }
        for (auto& inst : selectAguInst) {
            updateRdCnt(inst);
        }
        for (uint32_t bankId = 0; bankId < top->configs.simt_vrf_bank_num; bankId++) {
            const uint64_t rdCnt = bankRdCnt.at(bankId);
            top->stats->vectorVrfBankRdPortUsedCnt[bankId][rdCnt] += 1;
        }
    }
}

void sortIQ (IssueQ &iq, bool keepThreadOrder) {
    auto isqCmp = [keepThreadOrder](SimInst &a, SimInst &b) {
        if (a && b) {
            if (keepThreadOrder) {
                return LessROBID(a->insertIsqId, b->insertIsqId);
            }
            return LessROBID(a->bid, a->rid, b->bid, b->rid);
        }

        if (a) {
            return true;
        }

        return false;
    };
    auto sortIQArray = [isqCmp](vector<IssueState> &iqArray) {
        for (auto &iq : iqArray) {
            if (iq.size == 0 || !iq.insertEle) {
                continue;
            }
            iq.insertEle = false;
            sort(iq.entry.begin(), iq.entry.end(), isqCmp);
        }
    };

    sortIQArray(iq.cmdIQ);
    sortIQArray(iq.aluIQ);
    sortIQArray(iq.aguIQ);
    sortIQArray(iq.ldaIQ);
    sortIQArray(iq.ldaScIQ);
    sortIQArray(iq.staIQ);
    sortIQArray(iq.stdIQ);
    sortIQArray(iq.bruIQ);
    sortIQArray(iq.scaIQ);
    iq.aguAgeQueue.sortByAge();
};

void IssueQueue::Dump()
{
    auto printIQInst = [](std::vector<IssueState> &iqArray) {
        uint32_t i = 0;
        for (auto &iq : iqArray) {
            cout << "lane " << dec << (i++) << "(" << iq.size << ")" << endl;
            for (auto &inst : iq.entry) {
                if (inst) {
                    cout << inst->Dump() << endl;
                }
            }
        }
    };

    if (false) {
        cout << "IEX (" << (top->id == SCALAR_IEX ? "SCALAR" : " SIMT") << ")" << endl;
        cout << "ALU IQ: " << endl;
        printIQInst(current.aluIQ);
        cout << "BRU IQ: " << endl;
        printIQInst(current.bruIQ);
        cout << "AGU IQ: " << endl;
        printIQInst(current.aguIQ);
        cout << "LDA IQ: " << endl;
        printIQInst(current.ldaIQ);
        cout << "STA IQ: " << endl;
        printIQInst(current.staIQ);
        cout << "STD IQ: " << endl;
        printIQInst(current.stdIQ);
        cout << "CMD IQ: " << endl;
        printIQInst(current.cmdIQ);
    }
}

void IssueQueue::Xfer()
{
    auto iqArrayMoveLpv = [](std::vector<IssueState> &iqArray) {
        for (auto &iq : iqArray) {
            if (iq.size == 0) {
                continue;
            }
            iq.moveLpv();
        }
    };

    iqArrayMoveLpv(next.cmdIQ);
    iqArrayMoveLpv(next.aluIQ);
    iqArrayMoveLpv(next.bruIQ);
    iqArrayMoveLpv(next.aguIQ);
    iqArrayMoveLpv(next.ldaIQ);
    iqArrayMoveLpv(next.ldaScIQ);
    iqArrayMoveLpv(next.staIQ);
    iqArrayMoveLpv(next.stdIQ);
    iqArrayMoveLpv(next.scaIQ);
    cmdIQ.Work();
    sortIQ(next, (top->core->IsVectorIex(top->machineType) && order == IsqOrder::STRICTLY_INORDER));
    current = next;
    isq_wake_q.Work();
}

void IssueQueue::recieveWakeReq()
{
    while (!isq_wake_q.Empty()) {
        auto inst = isq_wake_q.Read();
        if (!inst) {
            continue;
        }
        PLpvInfo lpv = inst->GetLpv();
        const std::unordered_set<OperandType> WAKEUP_TYPE {
            OperandType::OPD_GREG, OperandType::OPD_TLINK, OperandType::OPD_ULINK,
            OperandType::OPD_VTLINK, OperandType::OPD_VULINK, OperandType::OPD_VMLINK,
            OperandType::OPD_VNLINK, OperandType::OPD_PREDMASK,
        };
        for (auto pdst : inst->pdsts_) {
            if (WAKEUP_TYPE.count(pdst->type) != 0) {
                WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled), lpv, inst->peID,
                    inst->GetTid(), inst->stid);
            }
        }
    }
}

void IssueQueue::deliveryData(uint32_t ptag, uint64_t data) {
    for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
        next.cmdIQ[i].deliveryData(ptag, data);
    }
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        next.aluIQ[i].deliveryData(ptag, data);
    }
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].deliveryData(ptag, data);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaIQ[i].deliveryData(ptag, data);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaScIQ[i].deliveryData(ptag, data);
    }
    for (uint32_t i = 0; i < top->iexStaIqCount; i++) {
        next.staIQ[i].deliveryData(ptag, data);
    }
    for (uint32_t i = 0; i < top->iexStdIqCount; i++) {
        next.stdIQ[i].deliveryData(ptag, data);
    }
    for (uint32_t i = 0; i < top->iexBruIqCount; i++) {
        next.bruIQ[i].deliveryData(ptag, data);
    }
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        next.scaIQ[i].deliveryData(ptag, data);
    }
}

void IssueState::deliveryData(uint32_t ptag, uint64_t data)
{
    for (uint32_t i = 0; i < entry.size(); i++) {
        if (!entry[i]) {
            continue;
        }
        for (auto psrc : entry[i]->psrcs_) { // && !entry[i]->ref.gsInfo.src_vld
            if (psrc->type == OperandType::OPD_GREG && psrc->ptag == ptag && psrc->dataVld) {
                psrc->ready = true;
                psrc->dataVld = true;
                psrc->data = data;
            }
        }
    }
}

void IssueQueue::RptIQStats(const SimInst& inst, IssueState &iq, IQStats &iq_stat, uint32_t id)
{
    // Issue stall, no instruction is selected from issuq
    if (inst != nullptr && iq.size != 0) {
        iq_stat.pick_stall_count[id]++;
    }
    uint32_t iq_depth = iq.entry.size();
    iq_stat.depth[id] += iq.size;
    if (iq.size <= (iq_depth * 0.25)) {
        iq_stat.depth_25[id] += 1;
    } else if (iq.size <= (iq_depth * 0.5)) {
        iq_stat.depth_50[id] += 1;
    } else if (iq.size <= (iq_depth * 0.75)) {
        iq_stat.depth_75[id] += 1;
    } else {
        iq_stat.depth_100[id] += 1;
    }
}

void IssueQueue::CoutStat(uint32_t readyCycle)
{
    uint64_t latency = GetSim()->getCycles() - readyCycle;
    if (latency <= 5) {
        top->stats->alu_iq.valuStats->readyNotPickLat0++;
    } else if (latency <= 10) {
        top->stats->alu_iq.valuStats->readyNotPickLat1++;
    } else if (latency <= 20) {
        top->stats->alu_iq.valuStats->readyNotPickLat2++;
    } else if (latency <= 30) {
        top->stats->alu_iq.valuStats->readyNotPickLat3++;
    } else {
        top->stats->alu_iq.valuStats->readyNotPickLat4++;
    }
}

static bool CheckEmpty(std::vector<IssueState> &iqArr)
{
    for (auto &iq : iqArr) {
        if (iq.size > 0) {
            return false;
        }
    }

    return true;
}

void IssueQueue::SetCorePEBound(BIQType biqType, uint64_t cycle) const
{
    top->core->bctrl->stats->SetPeBound(cycle);
    switch (biqType) {
        case BIQType::VEC_IQ:
        case BIQType::VET_IQ:
            top->core->bctrl->stats->SetPeBoundVec(cycle);
            break;
        case BIQType::CUBE_IQ:
            top->core->bctrl->stats->SetPeBoundCube(cycle);
            break;
        case BIQType::MTC_IQ:
            break;
        case BIQType::TMA_IQ:
            top->core->bctrl->stats->SetPeBoundTma(cycle);
            break;
        case BIQType::TAU_IQ:
            top->core->bctrl->stats->SetPeBoundTau(cycle);
            break;
        default:
            top->core->bctrl->stats->SetBccBound(cycle);
            break;
    }
}

void IssueQueue::SetCorePEBoundBrobStall(BIQType biqType, uint64_t cycle) const
{
    switch (biqType) {
        case BIQType::VEC_IQ:
        case BIQType::VET_IQ:
            top->core->bctrl->stats->SetPeBoundVecBrobStall(cycle);
            break;
        case BIQType::CUBE_IQ:
            top->core->bctrl->stats->SetPeBoundCubeBrobStall(cycle);
            break;
        case BIQType::MTC_IQ:
            break;
        case BIQType::TMA_IQ:
            top->core->bctrl->stats->SetPeBoundTmaBrobStall(cycle);
            break;
        case BIQType::TAU_IQ:
            top->core->bctrl->stats->SetPeBoundTauBrobStall(cycle);
            break;
        default:
            break;
    }
}

void IssueQueue::SetCorePEBoundBiqStall(BIQType biqType, uint64_t cycle) const
{
    switch (biqType) {
        case BIQType::VEC_IQ:
        case BIQType::VET_IQ:
            top->core->bctrl->stats->SetPeBoundVecBiqStall(cycle);
            break;
        case BIQType::CUBE_IQ:
            top->core->bctrl->stats->SetPeBoundCubeBiqStall(cycle);
            break;
        case BIQType::MTC_IQ:
            break;
        case BIQType::TMA_IQ:
            top->core->bctrl->stats->SetPeBoundTmaBiqStall(cycle);
            break;
        case BIQType::TAU_IQ:
            top->core->bctrl->stats->SetPeBoundTauBiqStall(cycle);
            break;
        default:
            break;
    }
}

void IssueQueue::SetCorePEBoundTileTagStall(BIQType biqType, uint64_t cycle) const
{
    switch (biqType) {
        case BIQType::VEC_IQ:
        case BIQType::VET_IQ:
            top->core->bctrl->stats->SetPeBoundVecTileTagStall(cycle);
            break;
        case BIQType::CUBE_IQ:
            top->core->bctrl->stats->SetPeBoundCubeTileTagStall(cycle);
            break;
        case BIQType::MTC_IQ:
            break;
        case BIQType::TMA_IQ:
            top->core->bctrl->stats->SetPeBoundTmaTileTagStall(cycle);
            break;
        case BIQType::TAU_IQ:
            top->core->bctrl->stats->SetPeBoundTauTileTagStall(cycle);
            break;
        default:
            break;
    }
}

void IssueQueue::SetCoreTopDownBound(bool efficient, SimQueue<SimInst> &isq, uint64_t cycle) const
{
    if (top->machineType != MachineType::SIEX || top->GetSim()->systemStatus.EcallRunning()) {
        return;
    }

    if (efficient) {
        static uint64_t efficent = 0;
        ++efficent;
        top->core->bctrl->stats->SetEfficient(cycle);
        return;
    }

    SimInst inst = isq.Empty() ? nullptr : isq.Front();

    if (inst && top->core->bctrl->blockROB.needStall(1, inst->stid)) {
        for (uint32_t i = 0; i < top->GetSim()->core->configs.scalar_smt_thread; ++i) {
            ROBID bid = top->core->bctrl->blockROB.getOldestBlockID(i);
            BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(bid, i);
            SetCorePEBound(cmd->biqType, cycle);
            SetCorePEBoundBrobStall(cmd->biqType, cycle);
        }
        return;
    }

    if (inst && top->core->bctrl->blockIssueQueueUnit.CheckBIQStall(inst->biqType, inst)) {
        SetCorePEBound(inst->biqType, cycle);
        SetCorePEBoundBiqStall(inst->biqType, cycle);
        return;
    }

    if (inst && top->core->bctrl->blockIssueQueueUnit.CheckTileRegStall(inst)) {
        for (uint32_t i = 0; i < top->GetSim()->core->configs.scalar_smt_thread; ++i) {
            ROBID bid = top->core->bctrl->blockROB.getOldestBlockID(i);
            BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(bid, i);
            SetCorePEBound(cmd->biqType, cycle);
            SetCorePEBoundTileTagStall(cmd->biqType, cycle);
        }
        return;
    }

    if (inst == nullptr) {
        bool hasRobEntry = false;
        for (uint32_t i = 0; i < top->GetSim()->core->configs.scalar_smt_thread; ++i) {
            if (top->core->bctrl->blockROB.getBROBSize(i) > 0) {
                hasRobEntry = true;
                ROBID bid = top->core->bctrl->blockROB.getOldestBlockID(i);
                BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(bid, i);
                SetCorePEBound(cmd->biqType, cycle);
                SetCorePEBoundBrobStall(cmd->biqType, cycle);
            }
        }
        if (hasRobEntry) {
            return;
        }
    }

    top->core->bctrl->stats->SetBccBound(cycle);
}

bool IssueQueue::CheckBCCBound()
{
    if (top->GetSim()->systemStatus.EcallRunning()) {
        return false;
    }

    if (cmdIQ.Empty()) {
        return true;
    }

    auto &readQ = cmdIQ.GetRawReadData();
    uint32_t selectCnt = 0;
    for (auto it = readQ.begin(); it != readQ.end() && selectCnt < top->iexCmdIqCount * top->iexCmdPickCount; ++it) {
        top->rtable.checkReady(*(it));
        if (GetSim()->core->bctrl->blockIssueQueueUnit.CheckBIQStall((*it)->biqType, *it) ||
            GetSim()->core->bctrl->blockIssueQueueUnit.CheckTileRegStall(*it) ||
            (*it)->picked || !(*it)->IsReady() || !(*it)->CheckNoCancel()) {
            break;
        }
        ++selectCnt;
    }

    if (selectCnt != 0) {
        return false;
    }

    SimInst inst = cmdIQ.Front();
    if (inst && (top->core->bctrl->blockROB.needStall(1, inst->stid) ||
        top->core->bctrl->blockIssueQueueUnit.CheckTileRegStall(inst))) {
        ROBID bid = top->core->bctrl->blockROB.getOldestBlockID(inst->stid);
        BlockCommandPtr cmd = top->core->bctrl->blockROB.GetBlockCMDPtr(bid, inst->stid);
        if (cmd->biqType != BIQType::VEC_IQ && cmd->biqType != BIQType::CUBE_IQ &&
            cmd->biqType != BIQType::MTC_IQ && cmd->biqType != BIQType::TMA_IQ &&
            cmd->biqType != BIQType::TAU_IQ) {
            return true;
        }
    }

    return !top->core->bctrl->blockROB.needStall(1, inst->stid) &&
        !top->core->bctrl->blockIssueQueueUnit.CheckBIQStall(inst->biqType, inst) &&
        !top->core->bctrl->blockIssueQueueUnit.CheckTileRegStall(inst);
}

void IssueQueue::issueCMD() {
    uint32_t selectCnt = 0;
    bool empty = cmdIQ.Empty();
    std::vector<uint32_t> cntCMDPerLaneArr(configs.iexCmdIqCount, 0);

    size_t idx = 0;
    auto findValidPos = [this]() -> size_t {
        for (size_t i = 0; i < top->iexCmdIqCount * top->iexCmdPickCount; ++i) {
            if (selectCmdInst[i] == nullptr) {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    };
    while (!cmdIQ.Empty()) {
        SimInst oriinst = cmdIQ.Front();
        SimInst inst = std::make_shared<SimInstInfo>(*oriinst);
        idx = findValidPos();
        if (idx == static_cast<size_t>(-1)) {
            break;
        }
        if (GetSim()->core->bctrl->blockIssueQueueUnit.CheckBlockCmdStall(inst->biqType, inst)) {
            break;
        }
        if (inst->picked) {
            break;
        }
        top->rtable.checkReady(inst);
        if (inst->IsReady()) {
            if (!inst->CheckNoCancel()) {
                LOG_INFO_M(top->machineType, Stage::S1) << "cmd iq lpv is not ready, inst is " << inst->Dump();
                break;
            }
            cmdIQ.Read();
            selectCnt++;
            selectCmdInst[idx] = inst;
            if (idx >= top->iexCmdIqCount * top->iexCmdPickCount) {
                break;
            }
        } else {
            LOG_INFO_M(top->machineType, Stage::S1) << "cmd iq is not ready, inst is " << inst->Dump();
            break;
        }

        LOG_INFO_M(top->machineType, Stage::S1) << "cmd iq is suaccelss, inst is " << inst->Dump();
    }

    SetCoreTopDownBound(selectCnt != 0, cmdIQ);
    if (selectCnt == 0) {
        if (!empty) {
            top->stats->cmd_iq.all_pick_stall++;
        }
        cmdIQStall = true;
        return;
    }
    cmdIQStall = false;

    for (uint32_t i = 0; i < top->iexCmdIqCount * top->iexCmdPickCount; i++) {
        if (selectCmdInst[i] == nullptr) {
            continue;
        }

        SimInst &inst = selectCmdInst[i];
        PLpvInfo lpvInfo = inst->GetLpv();

        PEResolveBus peResolve;
        peResolve.bid = inst->bid;
        peResolve.rid = inst->rid;
        peResolve.peid = inst->peID;
        peResolve.isIssue = true;
        peResolve.pipe_cycle = inst->pipeCycle;
        // CMD Pipe 只有 BCC
        top->iex_pe_rslv_array[inst->peID][inst->stid]->Write(peResolve);
    }
}

void IssueQueue::InitVecALUHelperFnAndStruct()
{
    // Vector ALU Picker Can't Issue Function - per picker
    vecAluIqPickerCantIssueFn.reserve(top->iexAluPickCount);
    for (uint32_t idx = 0; idx < top->iexAluPickCount; idx++) {
        vecAluIqPickerCantIssueFn.push_back([this, idx](const SimInst& inst, IssueBlockReason& reason) {
            vector<uint64_t> vPicker;
            auto iterLatency = IexLatency::VEC_LATENCY.find(inst->opcode);
            if (iterLatency != IexLatency::VEC_LATENCY.cend()) {
                auto unit = iterLatency->second.unit;
                switch (unit) {
                    case IexExecUnit::IALU:
                        vPicker = configs.simt_iex_alu_iq_ialu_picker;
                        break;
                    case IexExecUnit::FCVT:
                        vPicker = configs.simt_iex_alu_iq_cvt_picker;
                        break;
                    case IexExecUnit::FDIV:
                        vPicker = configs.simt_iex_alu_iq_div_picker;
                        break;
                    case IexExecUnit::FMLA:
                        vPicker = configs.simt_iex_alu_iq_fmla_picker;
                        break;
                    case IexExecUnit::IMAC:
                        vPicker = configs.simt_iex_alu_iq_mac_picker;
                        break;
                    case IexExecUnit::PERMUTE:
                        vPicker = configs.simt_iex_alu_iq_perm_picker;
                        break;
                    default:
                        LOG_WARN_M(top->machineType, Stage::NA) << "Unhandled ExecUnit: " << dec
                            << static_cast<uint32_t>(unit) << " OpCode: " << GetOpcodeName(inst->opcode);
                        break;
                }
            } else {
                LOG_WARN_M(top->machineType, Stage::NA) << "Unhandled OpCode: " << GetOpcodeName(inst->opcode);
            }
            if (vPicker.size() == 0) {
                // non-blocking by default for unhandled cases
                return false;
            }
            if (find(vPicker.cbegin(), vPicker.cend(), idx) != vPicker.cend()) {
                return false;
            }
            reason = IssueBlockReason::CANT_ISSUE_EXEC_UNIT;
            return true;
        });
    }
    // vecAluIqTotalIssueLimit - per IexExecUnit
    vecAluIqTotalIssueLimit.assign(static_cast<uint32_t>(IexExecUnit::UNIT_NUM), 0);
    vecAluIqTotalIssueLimit.at(static_cast<uint32_t>(IexExecUnit::IALU)) = configs.simt_iex_alu_iq_ialu_picker.size();
    vecAluIqTotalIssueLimit.at(static_cast<uint32_t>(IexExecUnit::FCVT)) = configs.simt_iex_alu_iq_cvt_picker.size();
    vecAluIqTotalIssueLimit.at(static_cast<uint32_t>(IexExecUnit::FDIV)) = configs.simt_iex_alu_iq_div_picker.size();
    vecAluIqTotalIssueLimit.at(static_cast<uint32_t>(IexExecUnit::FMLA)) = configs.simt_iex_alu_iq_fmla_picker.size();
    vecAluIqTotalIssueLimit.at(static_cast<uint32_t>(IexExecUnit::IMAC)) = configs.simt_iex_alu_iq_mac_picker.size();
    vecAluIqTotalIssueLimit.at(static_cast<uint32_t>(IexExecUnit::PERMUTE)) =
        configs.simt_iex_alu_iq_perm_picker.size();
    for (auto& limitVal : vecAluIqTotalIssueLimit) {
        ASSERT(limitVal > 0);
    }
}

void IssueQueue::RecordVecIssueBlockReasons(std::vector<IssueBlockReason> blockReasons) const
{
    if (!top->core->IsVectorIex(top->machineType)) {
        return;
    }
    for (auto& reason : blockReasons) {
        top->stats->vectorIssueBlock[reason] += 1;
    }
}

void IssueQueue::RecordVecAluPickerIssue(uint32_t pickId, const SimInst& inst) const
{
    if (!top->core->IsVectorIex(top->machineType)) {
        return;
    }
    ASSERT(inst != nullptr);
    IexExecUnit unit = IexExecUnit::UNDEF;
    auto iterLatency = IexLatency::VEC_LATENCY.find(inst->opcode);
    if (iterLatency != IexLatency::VEC_LATENCY.cend()) {
        unit = iterLatency->second.unit;
    }
    if (unit == IexExecUnit::UNDEF) {
        unit = IexExecUnit::IALU;
        LOG_WARN_M(top->machineType, Stage::NA) << "Unhandled OpCode: " << GetOpcodeName(inst->opcode);
    }
    top->stats->vectorALUPickerIssueCnt.at(pickId).at(static_cast<uint32_t>(unit)) += 1;
}

void IssueQueue::IssueALUIqUpdateStats(uint32_t iqId, uint32_t pickId, const SimInst& issueInst,
    vector<uint32_t>& iqExecIssueLimit, IssueStats& issStats)
{
    (void)iqId;
    if (rdPortCtrl) {
        rdPortCtrl->PostIssueUpdate(static_cast<RdPortControl::PipeId>(
            static_cast<uint32_t>(RdPortControl::PipeId::ALU0) + pickId), issueInst);
    }
    RecordVecAluPickerIssue(pickId, issueInst);
    LOG_DEBUG_M(top->machineType, Stage::NA) << "IssueEntry C:" << dec << GetSim()->getCycles() << " "
        << issueInst;
    if (top->core->IsVectorIex(top->machineType)) {
        CoutStat(issueInst->pipeCycle->rdyCycle);
        auto execUnitIdx = static_cast<uint32_t>(GetIssueExecUnit(issueInst->opcode));
        ASSERT(execUnitIdx < iqExecIssueLimit.size());
        ASSERT(iqExecIssueLimit[execUnitIdx] > 0);
        iqExecIssueLimit[execUnitIdx] -= 1;
    }
    if (OpcodeIsVCvt(issueInst->opcode)) {
        ++issStats.vcvt;
    } else if (OpcodeIsVFp(issueInst->opcode)) {
        ++issStats.vfp;
    } else if (OpcodeIsShf(issueInst->opcode)) {
        ++issStats.shfl;
    } else if (OpcodeIsDivSqrt(issueInst->opcode)) {
        ++issStats.divSqrt;
    } else {
        ++issStats.vint;
    }
    ++top->stats->issued_cnt;
    top->stats->alu_iq.total_inst_num++;
    if (checkInstOptMatch(issueInst)) {
        top->stats->can_be_optimized_inst++;
    }
    top->stats->alu_iq.total_wait +=
        (GetSim()->getCycles() - issueInst->pipeCycle->dispatchCycle);
    ++issStats.selectCnt;
}

SimInst IssueQueue::IssueALUIqPick(uint32_t iqId, uint32_t pickId, const vector<IssueChkFn>& cantIssueFn,
    const vector<IssueChkFn>& cancelIssueFnVec, std::vector<IssueBlockReason>& blockReasons)
{
    uint32_t oldestOption = 0;
    if (top->core->IsVectorIex(top->machineType)) {
        oldestOption = top->configs.simt_iex_alu_oldest_option;
    }
    const uint32_t select = iqId * top->iexAluPickCount + pickId;
    bool ldqLimit = false;
    selectAluInst[select] = current.aluIQ[iqId].Select(&next.aluIQ[iqId], ldqLimit, oldestOption,
        aluPipeStallFn[select], cantIssueFn, cancelIssueFnVec, blockReasons);
    RptIQStats(selectAluInst[select], current.aluIQ[iqId], top->stats->alu_iq, iqId);
    if (!selectAluInst[select]) {
        return nullptr;
    }
    auto &issueInst = selectAluInst[select];
    issueInst->isqId = iqId;
    issueInst->isqPickerId = pickId;
    return issueInst;
}

void IssueQueue::IssueALUIqGetCheckFn(uint32_t pickId, uint32_t nonHeterPickCount,
    const vector<uint32_t>& iqExecIssueLimit, vector<IssueChkFn>& cantIssueFn, vector<IssueChkFn>& cancelIssueFnVec)
{
    if (!top->core->IsVectorIex(top->machineType)) {
        return;
    }
    if ((top->iexAluHeterPickCount == 0) || (pickId < nonHeterPickCount)) {
        // Normal picker - can't issue
        cantIssueFn.push_back(vecAluIqPickerCantIssueFn.at(pickId));
    } else if (configs.simt_iex_alu_iq_heter_picker_option == 0) {
        // Heter picker - cancel
        cancelIssueFnVec.push_back([this, &iqExecIssueLimit](const SimInst& inst,
            IssueBlockReason& reason) {
            auto execUnitIdx = static_cast<uint32_t>(GetIssueExecUnit(inst->opcode));
            ASSERT(execUnitIdx < iqExecIssueLimit.size());
            if (iqExecIssueLimit[execUnitIdx] == 0) {
                reason = IssueBlockReason::CANCEL_ISSUE_EXEC_UNIT;
                return true;
            }
            return false;
        });
    } else if (configs.simt_iex_alu_iq_heter_picker_option == 1U) {
        // Heter picker - can't issue
        cantIssueFn.push_back([this, &iqExecIssueLimit](const SimInst& inst, IssueBlockReason& reason) {
            auto execUnitIdx = static_cast<uint32_t>(GetIssueExecUnit(inst->opcode));
            ASSERT(execUnitIdx < iqExecIssueLimit.size());
            if (iqExecIssueLimit[execUnitIdx] == 0) {
                reason = IssueBlockReason::CANT_ISSUE_EXEC_UNIT;
                return true;
            }
            return false;
        });
    } else {
        ASSERT(false);
    }

    if (rdPortCtrl) {
        cancelIssueFnVec.push_back([this, pickId](const SimInst& inst, IssueBlockReason& reason) {
            RdPortControl::PipeId pipe = static_cast<RdPortControl::PipeId>(
                static_cast<uint32_t>(RdPortControl::PipeId::ALU0) + pickId);
            if (!rdPortCtrl->CanIssue(pipe, inst)) {
                reason = IssueBlockReason::CANCEL_ISSUE_ALU_RD_PORT;
                return true;
            }
            return false;
        });
    }
}

void IssueQueue::IssueALUIq(uint32_t iqId, IssueStats& issStats)
{
    vector<uint32_t> iqExecIssueLimit;
    bool pickerRR = false;
    if (top->core->IsVectorIex(top->machineType)) {
        iqExecIssueLimit = vecAluIqTotalIssueLimit;
        pickerRR = top->configs.simt_iex_alu_iq_picker_rr;
    }
    const uint32_t nonHeterPickCount = top->iexAluPickCount - top->iexAluHeterPickCount;
    for (uint32_t idx = 0; idx < top->iexAluPickCount; ++idx) {
        uint32_t pickId = idx;
        if (pickerRR && idx < nonHeterPickCount) {
            pickId = (pickId + aluIqPickerRRIndex) % nonHeterPickCount;
        }
        vector<IssueChkFn> cantIssueFn;
        vector<IssueChkFn> cancelIssueFnVec;
        IssueALUIqGetCheckFn(pickId, nonHeterPickCount, iqExecIssueLimit, cantIssueFn, cancelIssueFnVec);
        if (top->core->IsVectorIex(top->machineType)) {
            issStats.readyNeedPickCnt += current.aluIQ[iqId].ReadyInstCount();
        }
        // pick
        std::vector<IssueBlockReason> blockReasons;
        SimInst issueInst = IssueALUIqPick(iqId, pickId, cantIssueFn, cancelIssueFnVec, blockReasons);
        if (!issueInst) {
            RecordVecIssueBlockReasons(blockReasons);
            continue;
        }
        if (configs.iex_dispatch_mode == 2) {
            top->dispatchUnit.aluInfo.count[issueInst->peID]--;
        }
        // For loading balancing
        if (next.aluIQ[iqId].size < next.aluIQ[top->dispatchUnit.aluInfo.dispatchID].size) {
            top->dispatchUnit.aluInfo.dispatchID = iqId;
        }
        if (top->GetSim()->core->IsVecPe(issueInst->peID)) {
            std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(issueInst->coreID);
            pe->prob[issueInst->GetTid()]->SetIsqId(issueInst->rid, iqId);
            pe->prob[issueInst->GetTid()]->SetIsqPicker(issueInst->rid, pickId);
        } else {
            std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->GetSim()->core->peArray[issueInst->peID]);
            pe->prob[issueInst->stid]->SetIsqId(issueInst->rid, iqId);
            pe->prob[issueInst->stid]->SetIsqPicker(issueInst->rid, pickId);
        }
        // Update stats
        IssueALUIqUpdateStats(iqId, pickId, issueInst, iqExecIssueLimit, issStats);
    }
    if (pickerRR) {
        aluIqPickerRRIndex = (aluIqPickerRRIndex + 1) % nonHeterPickCount;
    }
}

void IssueQueue::issueALU()
{
    bool empty = CheckEmpty(current.aluIQ);
    selectAluInst.assign(top->iexAluIqCount * top->iexAluPickCount, nullptr);

    IssueStats issStats;
    for (uint32_t iqId = 0; iqId < top->iexAluIqCount; ++iqId) {
        if (current.aluIQ[iqId].size <= 0) {
            continue;
        }
        IssueALUIq(iqId, issStats);
    }

    auto updateVAluPMU = [this, &issStats]() {
        if (top->stats->alu_iq.valuStats == nullptr) {
            return;
        }
        constexpr uint64_t IS_CNT_0 = 0;
        constexpr uint64_t IS_CNT_1 = 1;
        constexpr uint64_t IS_CNT_2 = 2;
        constexpr uint64_t IS_CNT_3 = 3;
        constexpr uint64_t IS_CNT_4 = 4;

        switch (issStats.selectCnt) {
            case IS_CNT_0:
                top->stats->alu_iq.valuStats->issue0++;
                break;
            case IS_CNT_1:
                top->stats->alu_iq.valuStats->issue1++;
                break;
            case IS_CNT_2:
                top->stats->alu_iq.valuStats->issue2++;
                break;
            case IS_CNT_3:
                top->stats->alu_iq.valuStats->issue3++;
                break;
            case IS_CNT_4:
                top->stats->alu_iq.valuStats->issue4++;
                break;
            default:
                break;
        }

        switch (issStats.vint) {
            case IS_CNT_1:
                top->stats->alu_iq.valuStats->issue1VInt++;
                break;
            case IS_CNT_2:
                top->stats->alu_iq.valuStats->issue2VInt++;
                break;
            case IS_CNT_3:
                top->stats->alu_iq.valuStats->issue3VInt++;
                break;
            case IS_CNT_4:
                top->stats->alu_iq.valuStats->issue4VInt++;
                break;
            default:
                break;
        }

        switch (issStats.vcvt) {
            case IS_CNT_1:
                top->stats->alu_iq.valuStats->issue1VCvt++;
                break;
            case IS_CNT_2:
                top->stats->alu_iq.valuStats->issue2VCvt++;
                break;
            case IS_CNT_3:
                top->stats->alu_iq.valuStats->issue3VCvt++;
                break;
            case IS_CNT_4:
                top->stats->alu_iq.valuStats->issue4VCvt++;
                break;
            default:
                break;
        }

        switch (issStats.vfp) {
            case IS_CNT_1:
                top->stats->alu_iq.valuStats->issue1VFp++;
                break;
            case IS_CNT_2:
                top->stats->alu_iq.valuStats->issue2VFp++;
                break;
            case IS_CNT_3:
                top->stats->alu_iq.valuStats->issue3VFp++;
                break;
            case IS_CNT_4:
                top->stats->alu_iq.valuStats->issue4VFp++;
                break;
            default:
                break;
        }

        switch (issStats.shfl) {
            case IS_CNT_1:
                top->stats->alu_iq.valuStats->issue1Shf++;
                break;
            case IS_CNT_2:
                top->stats->alu_iq.valuStats->issue2Shf++;
                break;
            case IS_CNT_3:
                top->stats->alu_iq.valuStats->issue3Shf++;
                break;
            case IS_CNT_4:
                top->stats->alu_iq.valuStats->issue4Shf++;
                break;
            default:
                break;
        }

        switch (issStats.divSqrt) {
            case IS_CNT_1:
                top->stats->alu_iq.valuStats->issue1DivSqrt++;
                break;
            case IS_CNT_2:
                top->stats->alu_iq.valuStats->issue2DivSqrt++;
                break;
            case IS_CNT_3:
                top->stats->alu_iq.valuStats->issue3DivSqrt++;
                break;
            case IS_CNT_4:
                top->stats->alu_iq.valuStats->issue4DivSqrt++;
                break;
            default:
                break;
        }
    };

    if (issStats.readyNeedPickCnt > 0 && (top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX)) {
        // pmu
        updateVAluPMU();
    }

    if (issStats.selectCnt == 0) {
        if (!empty) {
            top->stats->alu_iq.all_pick_stall++;
        }
        aluIQStall = true;
        return;
    }
    aluIQStall = false;

    for (uint32_t iqId = 0; iqId < top->iexAluIqCount * top->iexAluPickCount; iqId++) {
        if (selectAluInst[iqId] == nullptr) {
            continue;
        }

        SimInst &inst = selectAluInst[iqId];
        PLpvInfo lpvInfo = inst->GetLpv();

        // Stack get wake up stack load check
        POperandPtr srcSP = inst->GetPSrcPtrByType(OperandType::STACK_POINTER);
        if (srcSP != nullptr) {
            wakeupStackLoad(inst->bid, inst->rid, lpvInfo);
            top->rtable.setROBReadyTable(inst->peID, inst->rid.val, lpvInfo, true);
        }

        PEResolveBus peResolve;
        peResolve.bid = inst->bid;
        peResolve.rid = inst->rid;
        peResolve.peid = inst->peID;
        peResolve.isIssue = true;
        peResolve.pipe_cycle = inst->pipeCycle;
        uint32_t tid = 0;
        if (top->GetSim()->core->IsVectorIex(top->machineType)) {
            tid = inst->GetTid();
        } else {
            tid = inst->stid;
        }
        top->iex_pe_rslv_array[inst->peID][tid]->Write(peResolve);
    }
}

void IssueQueue::issueGatherAGU()
{
    // vab没有请求
    if (!vab->Stall) {
        return;
    }

    // vab的请求发完了，在等待resp
    if (vab->Pending) {
        return;
    }
    SimInst inst = vab->popVab();
    MemRequest *memReq = vab->selectOne();
    assert(memReq);
    top->iexVcoreReqQ->Write(*memReq);
}

void IssueQueue::issueAGU()
{
    if (configs.VAB_EN) {
        if (vab->Stall && !vab->Pending) {
            return;
        }
    }
    uint32_t selectCnt = 0;
    uint32_t noSelectByLimit = 0;
    bool empty = CheckEmpty(current.ldaIQ);
    std::vector<uint32_t> cntPerLaneArr(configs.iexLdaIqCount, 0);
    for (uint32_t i = 0; i < top->iexLdaIqCount; ++i) {
        if (current.ldaIQ[i].size <= 0) {
            continue;
        }

        uint32_t select = i * top->iexLdaPickCount;
        for (uint32_t j = 0; j < top->iexLdaPickCount; ++j) {
            uint32_t issuedPerIqLane = top->id == SCALAR_IEX ?
                configs.issuedPerIqLane : configs.simt_issued_per_iq_lane;
            if (top->id == MEM_IEX) {
                issuedPerIqLane = configs.mtc_issued_per_iq_lane;
            }

            if (cntPerLaneArr[i] >= issuedPerIqLane) {
                break;
            }

            // Pick not stall load inst
            empty = false;
            while (select < (i + 1) * top->iexLdaPickCount && selectLdaInst[select]) {
                ++select;
            }
            if (select >= (i + 1) * top->iexLdaPickCount) {
                break;
            }

            // Pick inst
            bool ldqLimit = true;
            std::vector<IssueBlockReason> blockReasons;
            selectLdaInst[select] = current.ldaIQ[i].Select(&next.ldaIQ[i], ldqLimit, 0, nullptr,
                {}, {}, blockReasons);
            RptIQStats(selectLdaInst[select], current.ldaIQ[i], top->stats->lda_iq, i);
            if (!selectLdaInst[select]) {
                noSelectByLimit += (ldqLimit) ? 1 : 0;
                RecordVecIssueBlockReasons(blockReasons);
                break;
            }
            selectLdaInst[select]->isqId = i;
            selectLdaInst[select]->isqPickerId = j;
            ++top->stats->issued_cnt;
            top->stats->lda_iq.total_inst_num++;
            top->stats->lda_iq.total_wait +=
                (GetSim()->getCycles() - selectLdaInst[select]->pipeCycle->dispatchCycle);
            if (configs.iex_dispatch_mode == 2) {
                top->dispatchUnit.ldaInfo.count[selectLdaInst[select]->peID]--;
            }
            if (next.ldaIQ[i].size < next.ldaIQ[top->dispatchUnit.ldaInfo.dispatchID].size) {
                top->dispatchUnit.ldaInfo.dispatchID = i;
            }
            selectLdaInst[select]->iqid = select;
            SimInst &issueInst = selectLdaInst[select];
            if (top->GetSim()->core->IsVecPe(issueInst->peID)) {
                std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(issueInst->coreID);
                pe->prob[issueInst->GetTid()]->setPipeid(issueInst->rid, select);
                pe->prob[issueInst->GetTid()]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->GetTid()]->SetIsqPicker(issueInst->rid, j);
            } else {
                std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->GetSim()->core->peArray[issueInst->peID]);
                pe->prob[issueInst->stid]->setPipeid(issueInst->rid, select);
                pe->prob[issueInst->stid]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->stid]->SetIsqPicker(issueInst->rid, j);
            }
            ++select;
            ++selectCnt;
            ++cntPerLaneArr[i];
        }
    }

    if (noSelectByLimit == top->iexLdaIqCount * top->iexLdaPickCount) {
        // No load inst ready, reset the update flag.
        next.aguAgeQueue.update = false;
    }

    if (selectCnt == 0) {
        if (!empty) {
            top->stats->lda_iq.all_pick_stall++;
        }
        ldaIQStall = true;
        return;
    }
    ldaIQStall = false;

    for (uint32_t i = 0; i < top->iexLdaIqCount * top->iexLdaPickCount; i++) {
        if (selectLdaInst[i] == nullptr) {
            continue;
        }
        SimInst &inst = selectLdaInst[i];
        POperandPtr srcSP = inst->GetPSrcPtrByType(OperandType::STACK_POINTER);
        if (srcSP != nullptr) {
            PEResolveBus peResolve;
            peResolve.bid = inst->bid;
            peResolve.rid = inst->rid;
            peResolve.peid = inst->peID;
            peResolve.isIssue = true;
            peResolve.pipe_cycle = inst->pipeCycle;
            uint32_t tid = 0;
            if (top->GetSim()->core->IsVectorIex(top->machineType)) {
                tid = inst->GetTid();
            } else {
                tid = inst->stid;
            }
            top->iex_pe_rslv_array[inst->peID][tid]->Write(peResolve);
        }
    }
    for (auto &inst : selectLdaInst) {
        if (inst == nullptr) {
            continue;
        }
        LOG_INFO_M(top->machineType, Stage::S1) << "aguIQ " << std::dec << inst->iqid << "Selected non spec mem inst:"
            << inst->Dump();
    }
}

// 此函数用于将指令发射到scalperQ中，非正式的issue流程。区别在于 只要满足src ready就可以将指令送scalperQ中
void IssueQueue::issueSCAGU()
{
    uint32_t selectCnt = 0;
    uint32_t noSelectByLimit = 0;
    bool empty = CheckEmpty(current.ldaScIQ);
    std::vector<uint32_t> cntPerLaneArr =
    std::vector<uint32_t>(configs.iexLdaIqCount);
    std::fill(cntPerLaneArr.begin(), cntPerLaneArr.end(), 0);
    for (uint32_t i = 0; i < top->iexLdaIqCount; ++i) {
        if (current.ldaScIQ[i].size <= 0) {
            continue;
        }

        uint32_t select = i * top->iexLdaPickCount;
        for (uint32_t j = 0; j < top->iexLdaPickCount; ++j) {
            uint32_t issuedPerIqLane = top->id == SCALAR_IEX ?
                configs.issuedPerIqLane : configs.simt_issued_per_iq_lane;
            if (top->id == MEM_IEX) {
                issuedPerIqLane = configs.mtc_issued_per_iq_lane;
            }

            if (cntPerLaneArr[i] >= issuedPerIqLane) {
                break;
            }

            // Pick not stall load inst
            empty = false;
            while (select < (i + 1) * top->iexLdaPickCount && selectSCLdaInst[select]) {
                ++select;
            }
            if (select >= (i + 1) * top->iexLdaPickCount) {
                break;
            }

            // Pick inst
            bool ldqLimit = true;
            selectSCLdaInst[select] = current.ldaScIQ[i].Select_SC(&next.ldaScIQ[i], ldqLimit, nullptr, nullptr,
                {});
            if (!selectSCLdaInst[select]) {
                noSelectByLimit += (ldqLimit) ? 1 : 0;
                break;
            }
            ASSERT(GetSim()->core->configs.scalper_enable);
            selectSCLdaInst[select]->isqPickerId = j;

            ++top->stats->issued_cnt;
            top->stats->lda_sc_iq.total_inst_num++;
            top->stats->lda_sc_iq.total_wait +=
                (GetSim()->getCycles() - selectSCLdaInst[select]->pipeCycle->dispatchCycle);
            if (configs.iex_dispatch_mode == 2) {
                top->dispatchUnit.ldascInfo.count[selectSCLdaInst[select]->peID]--;
            }
            if (next.ldaScIQ[i].size < next.ldaScIQ[top->dispatchUnit.ldaInfo.dispatchID].size) {
                top->dispatchUnit.ldascInfo.dispatchID = i;
            }
            selectSCLdaInst[select]->iqid = select;
            SimInst &issueInst = selectSCLdaInst[select];
            if (top->GetSim()->core->IsVecPe(issueInst->peID)) {
                std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(issueInst->coreID);
                pe->prob[issueInst->GetTid()]->setPipeid(issueInst->rid, select);
            } else {
                std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->GetSim()->core->peArray[issueInst->peID]);
                pe->prob[issueInst->GetTid()]->setPipeid(issueInst->rid, select);
            }
            ++select;
            ++selectCnt;
            ++cntPerLaneArr[i];
        }
    }

    if (noSelectByLimit == top->iexLdaIqCount * top->iexLdaPickCount) {
        // No load inst ready, reset the update flag.
        next.aguSCAgeQueue.update = false;
    }

    if (selectCnt == 0) {
        if (!empty)
            top->stats->lda_sc_iq.all_pick_stall++;
        ldaSCIQStall = true;
        return;
    }
    ldaSCIQStall = false;

    for (uint32_t i = 0; i < top->iexLdaIqCount * top->iexLdaPickCount; i++) {
        if (selectSCLdaInst[i]) {
            LOG_INFO_M(top->machineType, Stage::P1) << "SC guIQ " <<selectSCLdaInst[i]->iqid
                << " Selected non spec mem | " << selectSCLdaInst[i]->Dump() << " | (C)";
        }
    }
}

void IssueQueue::issueVecAGU()
{
    if (configs.VAB_EN) {
        if (vab->Stall && !vab->Pending) {
            return;
        }
    }
    uint32_t selectCnt = 0;
    uint32_t noSelectByLimit = 0;
    bool empty = CheckEmpty(current.aguIQ);
    std::vector<uint32_t> cntPerLaneArr(configs.simt_iex_agu_iq_count, 0);
    for (uint32_t i = 0; i < top->iexAguIqCount * top->iexAguPickCount; ++i) {
        auto &inst = selectAguInst[i];
        if (inst && OpcodeIsStore(inst->opcode)) {
            selectAguInst[i] = std::shared_ptr<SimInstInfo>(nullptr);
        }
    }
    auto isStore = [](const SimInst& inst, IssueBlockReason& reason) {
        (void)reason;
        return OpcodeIsStore(inst->opcode);
    };
    auto isLoad = [](const SimInst& inst, IssueBlockReason& reason) {
        (void)reason;
        return OpcodeIsLoad(inst->opcode);
    };

    auto pickInst = [this, &isStore, &isLoad](bool separateLdSt, uint32_t i, uint32_t j, uint32_t &select, bool& ldqLimit,
                                         const uint32_t oldestOption, std::vector<IssueBlockReason>& reasons) {
        vector<IssueChkFn> cancelIssueFnVec;
        if (rdPortCtrl) {
            cancelIssueFnVec.push_back([this, j](const SimInst& inst, IssueBlockReason& reason) {
                RdPortControl::PipeId pipe = static_cast<RdPortControl::PipeId>(
                    static_cast<uint32_t>(RdPortControl::PipeId::AGU0) + j);
                if (!rdPortCtrl->CanIssue(pipe, inst)) {
                    reason = IssueBlockReason::CANCEL_ISSUE_AGU_RD_PORT;
                    return true;
                }
                return false;
            });
        }
        if (separateLdSt) {
            if (j == top->iexAguPickCount - 1) {
                selectAguInst[select] = current.aguIQ[i].Select(&next.aguIQ[i], ldqLimit, oldestOption, nullptr,
                                                                { isLoad }, cancelIssueFnVec, reasons);
            } else {
                selectAguInst[select] = current.aguIQ[i].Select(&next.aguIQ[i], ldqLimit, oldestOption, nullptr,
                                                                { isStore }, cancelIssueFnVec, reasons);
            }
        } else {
            if (j == 0) {
                selectAguInst[select] = current.aguIQ[i].Select(&next.aguIQ[i], ldqLimit, oldestOption, nullptr,
                                                                {}, cancelIssueFnVec, reasons);
            } else {
                selectAguInst[select] = current.aguIQ[i].Select(&next.aguIQ[i], ldqLimit, oldestOption, nullptr,
                                                                { isStore }, cancelIssueFnVec, reasons);
            }
        }
    };
    bool separateLdSt = top->configs.simt_separate_ld_st_pipe;

    for (uint32_t i = 0; i < top->iexAguIqCount; ++i) {
        if (current.aguIQ[i].size <= 0) {
            continue;
        }

        uint32_t select = i * top->iexAguPickCount;
        for (uint32_t j = 0; j < top->iexAguPickCount; ++j) {
            // Pick not stall load inst
            empty = false;
            while (select < (i + 1) * top->iexAguPickCount && selectAguInst[select]) {
                ++select;
            }
            if (select >= (i + 1) * top->iexAguPickCount) {
                break;
            }
            bool ldqLimit = true;
            std::vector<IssueBlockReason> blockReasons;
            const uint32_t oldestOption = top->configs.simt_iex_agu_oldest_option;
            pickInst(separateLdSt, i, j, select, ldqLimit, oldestOption, blockReasons);

            RptIQStats(selectAguInst[select], current.aguIQ[i], top->stats->agu_iq, i);
            if (!selectAguInst[select]) {
                noSelectByLimit += (ldqLimit) ? 1 : 0;
                RecordVecIssueBlockReasons(blockReasons);
                continue;
            }
            selectAguInst[select]->isqId = i;
            selectAguInst[select]->isqPickerId = j;
            if (rdPortCtrl) {
                rdPortCtrl->PostIssueUpdate(static_cast<RdPortControl::PipeId>(
                    static_cast<uint32_t>(RdPortControl::PipeId::AGU0) + j), selectAguInst[select]);
            }

            ++top->stats->issued_cnt;
            top->stats->agu_iq.total_inst_num++;
            top->stats->agu_iq.total_wait +=
                (GetSim()->getCycles() - selectAguInst[select]->pipeCycle->dispatchCycle);
            if (configs.iex_dispatch_mode == 2) {
                top->dispatchUnit.aguInfo.count[selectAguInst[select]->peID]--;
            }
            if (next.aguIQ[i].size < next.aguIQ[top->dispatchUnit.aguInfo.dispatchID].size) {
                top->dispatchUnit.aguInfo.dispatchID = i;
            }
            selectAguInst[select]->iqid = select;
            SimInst &inst = selectAguInst[select];
            std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(inst->coreID);
            pe->prob[selectAguInst[select]->GetTid()]->setPipeid(selectAguInst[select]->rid, select);
            pe->prob[selectAguInst[select]->GetTid()]->SetIsqId(selectAguInst[select]->rid, i);
            pe->prob[selectAguInst[select]->GetTid()]->SetIsqPicker(selectAguInst[select]->rid, j);
            ++select;
            ++selectCnt;
            ++cntPerLaneArr[i];
        }
    }

    if (noSelectByLimit == top->iexAguIqCount * top->iexAguPickCount) {
        // No load inst ready, reset the update flag.
        next.aguAgeQueue.update = false;
    }

    if (selectCnt == 0) {
        if (!empty) {
            top->stats->agu_iq.all_pick_stall++;
        }
        aguIQStall = true;
        return;
    }
    aguIQStall = false;

    for (uint32_t i = 0; i < top->iexAguIqCount * top->iexAguPickCount; i++) {
        if (selectAguInst[i] == nullptr) {
            continue;
        }
        SimInst &inst = selectAguInst[i];
        POperandPtr srcSP = inst->GetPSrcPtrByType(OperandType::STACK_POINTER);
        if (srcSP != nullptr) {
            PEResolveBus peResolve;
            peResolve.bid = inst->bid;
            peResolve.rid = inst->rid;
            peResolve.peid = inst->peID;
            peResolve.isIssue = true;
            peResolve.pipe_cycle = inst->pipeCycle;
            uint32_t tid = 0;
            if (top->GetSim()->core->IsVectorIex(top->machineType)) {
                tid = inst->GetTid();
            } else {
                tid = inst->stid;
            }
            top->iex_pe_rslv_array[inst->peID][tid]->Write(peResolve);
        }
    }
}

void IssueQueue::issueSTA()
{
    uint32_t selectCnt = 0;
    bool empty = CheckEmpty(current.staIQ);
    std::vector<uint32_t> cntPerLaneArr;
    if (top->id == MEM_IEX) {
        cntPerLaneArr.assign(max(configs.mtc_iex_sta_iq_count, configs.iexStaIqCount), 0);
    } else {
        cntPerLaneArr.assign(configs.iexStaIqCount, 0);
    }

    selectStaInst.assign(top->iexStaIqCount * top->iexStaPickCount, nullptr);

    for (uint32_t i = 0; i < top->iexStaIqCount; ++i) {
        if (current.staIQ[i].size <= 0) {
            continue;
        }
        uint32_t select = i * top->iexStaPickCount;
        for (uint32_t j = 0; j < top->iexStaPickCount; ++j) {
            uint32_t issuedPerIqLane = top->id == SCALAR_IEX ?
                configs.issuedPerIqLane : configs.simt_issued_per_iq_lane;
            if (top->id == MEM_IEX) {
                issuedPerIqLane = configs.mtc_issued_per_iq_lane;
            }

            if (cntPerLaneArr[i] >= issuedPerIqLane) {
                break;
            }

            empty = false;
            bool ldqLimit = false;
            std::vector<IssueBlockReason> blockReasons;
            selectStaInst[select] = current.staIQ[i].Select(&next.staIQ[i], ldqLimit, 0, nullptr,
                {}, {}, blockReasons);
            RptIQStats(selectStaInst[select], current.staIQ[i], top->stats->sta_iq, i);
            if (!selectStaInst[select]) {
                RecordVecIssueBlockReasons(blockReasons);
                break;
            }
            selectStaInst[select]->isqId = i;
            selectStaInst[select]->isqPickerId = j;

            ++top->stats->issued_cnt;
            top->stats->sta_iq.total_inst_num++;
            top->stats->sta_iq.total_wait +=
                (GetSim()->getCycles() - selectStaInst[select]->pipeCycle->dispatchCycle);
            if (configs.iex_dispatch_mode == 2) {
                top->dispatchUnit.staInfo.count[selectStaInst[select]->peID]--;
            }
            if (next.staIQ[i].size < next.staIQ[top->dispatchUnit.staInfo.dispatchID].size) {
                top->dispatchUnit.staInfo.dispatchID = i;
            }
            SimInst &issueInst = selectStaInst[select];
            if (top->GetSim()->core->IsVecPe(issueInst->peID)) {
                std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(issueInst->coreID);
                pe->prob[issueInst->GetTid()]->setPipeid(issueInst->rid, select);
                pe->prob[issueInst->GetTid()]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->GetTid()]->SetIsqPicker(issueInst->rid, j);
            } else {
                std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->GetSim()->core->peArray[issueInst->peID]);
                pe->prob[issueInst->stid]->setPipeid(issueInst->rid, select);
                pe->prob[issueInst->stid]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->stid]->SetIsqPicker(issueInst->rid, j);
            }
            ++select;
            ++selectCnt;
            ++cntPerLaneArr[i];
        }
    }

    if (selectCnt == 0) {
        if (!empty) {
            top->stats->sta_iq.all_pick_stall++;
        }
        staIQStall = true;
        return;
    }
    staIQStall = false;
    for (uint32_t i = 0; i < top->iexStaIqCount * top->iexStaPickCount; i++) {
        if (selectStaInst[i] == nullptr) {
            continue;
        }
        SimInst &inst = selectStaInst[i];
        PLpvInfo lpvInfo = inst->GetLpv();
        PEResolveBus peResolve;
        peResolve.bid = inst->bid;
        peResolve.rid = inst->rid;
        peResolve.peid = inst->peID;
        peResolve.isIssue = true;
        peResolve.pipe_cycle = inst->pipeCycle;
        uint32_t tid = 0;
        if (top->GetSim()->core->IsVectorIex(top->machineType)) {
            tid = inst->GetTid();
        } else {
            tid = inst->stid;
        }
        top->iex_pe_rslv_array[inst->peID][tid]->Write(peResolve);

        for (auto pdst : inst->pdsts_) {
            if (pdst->type == OperandType::OPD_GREG) {
                WakeupIQTag(WakeupInfo(OperandType::OPD_GREG, pdst->ptag, pdst->recycled), lpvInfo,
                    inst->peID, inst->GetTid(), inst->stid);
            } else if (pdst->type == OperandType::LS_MDB_DEPENDENCY) {
                (top->iexmdb).setStRdy(pdst->ptag);
            } else {
                WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled), lpvInfo,
                            inst->peID, inst->GetTid(), inst->stid);
            }
        }
    }
}

void IssueQueue::issueSTD()
{
    uint32_t selectCnt = 0;
    bool empty = CheckEmpty(current.stdIQ);
    std::vector<uint32_t> cntPerLaneArr;
    if (top->id == MEM_IEX) {
        cntPerLaneArr.assign(max(configs.mtc_iex_std_iq_count, configs.iexStdIqCount), 0);
    } else {
        cntPerLaneArr.assign(max(configs.simt_iex_std_iq_count, configs.iexStdIqCount), 0);
    }

    selectStdInst.assign(top->iexStdIqCount * top->iexStdPickCount, nullptr);

    for (uint32_t i = 0; i < top->iexStdIqCount; ++i) {
        if (current.stdIQ[i].size <= 0) {
            continue;
        }

        uint32_t select = i * top->iexStdPickCount;
        for (uint32_t j = 0; j < top->iexStdPickCount; ++j) {
            uint32_t issuedPerIqLane = top->id == SCALAR_IEX ?
                configs.issuedPerIqLane : configs.simt_issued_per_iq_lane;
            if (top->id == MEM_IEX) {
                issuedPerIqLane = configs.mtc_issued_per_iq_lane;
            }
            if (cntPerLaneArr[i] >= issuedPerIqLane) {
                break;
            }

            empty = false;
            bool ldqLimit = false;
            std::vector<IssueBlockReason> blockReasons;
            selectStdInst[select] = current.stdIQ[i].Select(&next.stdIQ[i], ldqLimit, 0, nullptr,
                {}, {}, blockReasons);
            RptIQStats(selectStdInst[select], current.stdIQ[i], top->stats->std_iq, i);
            if (!selectStdInst[select]) {
                RecordVecIssueBlockReasons(blockReasons);
                break;
            }
            selectStdInst[select]->isqId = i;
            selectStdInst[select]->isqPickerId = j;

            ++top->stats->issued_cnt;
            top->stats->std_iq.total_inst_num++;
            top->stats->std_iq.total_wait +=
                (GetSim()->getCycles() - selectStdInst[select]->pipeCycle->dispatchCycle);
            if (configs.iex_dispatch_mode == 2) {
                top->dispatchUnit.stdInfo.count[selectStdInst[select]->peID]--;
            }
            if (next.stdIQ[i].size < next.stdIQ[top->dispatchUnit.stdInfo.dispatchID].size) {
                top->dispatchUnit.stdInfo.dispatchID = i;
            }
            SimInst &issueInst = selectStdInst[select];
            if (top->GetSim()->core->IsVecPe(issueInst->peID)) {
                std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(issueInst->coreID);
                pe->prob[issueInst->GetTid()]->setPipeid(issueInst->rid, select);
                pe->prob[issueInst->GetTid()]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->GetTid()]->SetIsqPicker(issueInst->rid, j);
            } else {
                std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->GetSim()->core->peArray[issueInst->peID]);
                pe->prob[issueInst->stid]->setPipeid(issueInst->rid, select);
                pe->prob[issueInst->stid]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->stid]->SetIsqPicker(issueInst->rid, j);
            }

            ++select;
            ++selectCnt;
            ++cntPerLaneArr[i];
        }
    }

    if (selectCnt == 0) {
        if (!empty) {
            top->stats->std_iq.all_pick_stall++;
        }
        stdIQStall = true;
        return;
    }
    stdIQStall = false;
    for (uint32_t i = 0; i < top->iexStdIqCount * top->iexStdPickCount; i++) {
        if (selectStdInst[i] == nullptr) {
            continue;
        }
        SimInst &inst = selectStdInst[i];
        PEResolveBus peResolve;
        peResolve.bid = inst->bid;
        peResolve.rid = inst->rid;
        peResolve.peid = inst->peID;
        peResolve.isIssue = true;
        peResolve.pipe_cycle = inst->pipeCycle;
        uint32_t tid = 0;
        if (top->GetSim()->core->IsVectorIex(top->machineType)) {
            tid = inst->GetTid();
        } else {
            tid = inst->stid;
        }
        top->iex_pe_rslv_array[inst->peID][tid]->Write(peResolve);

        PLpvInfo lpvInfo = inst->GetLpv();
        POperandPtr dstSP = inst->GetPDstPtrByType(OperandType::STACK_POINTER);
        if (dstSP != nullptr) {
            wakeupSptag(dstSP->ptag, dstSP->recycled, lpvInfo);
        }
    }
}

void IssueQueue::issueBRU()
{
    uint32_t selectCnt = 0;
    bool empty = CheckEmpty(current.bruIQ);
    std::vector<uint32_t> cntPerLaneArr;
    if (top->id == MEM_IEX) {
        cntPerLaneArr.assign(max(configs.mtc_iex_bru_iq_count, configs.iexBruIqCount), 0);
    } else {
        cntPerLaneArr.assign(configs.iexBruIqCount, 0);
    }

    selectBruInst.assign(top->iexBruIqCount * top->iexBruPickCount, nullptr);

    for (uint32_t i = 0; i < top->iexBruIqCount; ++i) {
        if (current.bruIQ[i].size <= 0) {
            continue;
        }

        uint32_t select = i * top->iexBruPickCount;
        for (uint32_t j = 0; j < top->iexBruPickCount; ++j) {
            uint32_t issuedPerIqLane = top->id == SCALAR_IEX ?
                configs.issuedPerIqLane : configs.simt_issued_per_iq_lane;
            if (top->id == MEM_IEX) {
                issuedPerIqLane = configs.mtc_issued_per_iq_lane;
            }
            if (cntPerLaneArr[i] >= issuedPerIqLane) {
                break;
            }

            empty = false;
            bool ldqLimit = false;
            std::vector<IssueBlockReason> blockReasons;
            selectBruInst[select] = current.bruIQ[i].Select(&next.bruIQ[i], ldqLimit, 0, nullptr,
                {}, {}, blockReasons);
            RptIQStats(selectBruInst[select], current.bruIQ[i], top->stats->bru_iq, i);
            if (!selectBruInst[select]) {
                RecordVecIssueBlockReasons(blockReasons);
                break;
            }
            selectBruInst[select]->isqId = i;
            selectBruInst[select]->isqPickerId = j;

            ++top->stats->issued_cnt;
            top->stats->bru_iq.total_inst_num++;
            if (checkInstOptMatch(selectBruInst[select])) {
                top->stats->can_be_optimized_inst++;
            }
            top->stats->bru_iq.total_wait +=
                (GetSim()->getCycles() - selectBruInst[select]->pipeCycle->dispatchCycle);
            if (configs.iex_dispatch_mode == 2) {
                top->dispatchUnit.bruInfo.count[selectBruInst[select]->peID]--;
            }
            if (next.bruIQ[i].size < next.bruIQ[top->dispatchUnit.bruInfo.dispatchID].size) {
                top->dispatchUnit.bruInfo.dispatchID = i;
            }
            SimInst &issueInst = selectBruInst[select];
            if (top->GetSim()->core->IsVecPe(issueInst->peID)) {
                std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(issueInst->coreID);
                pe->prob[issueInst->GetTid()]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->GetTid()]->SetIsqPicker(issueInst->rid, j);
            } else {
                std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->GetSim()->core->peArray[issueInst->peID]);
                pe->prob[issueInst->stid]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->stid]->SetIsqPicker(issueInst->rid, j);
            }
            ++select;
            ++selectCnt;
            ++cntPerLaneArr[i];
        }
    }

    if (selectCnt == 0) {
        if (!empty) {
            top->stats->bru_iq.all_pick_stall++;
        }
        bruIQStall = true;
        return;
    }
    bruIQStall = false;

    for (uint32_t i = 0; i < top->iexBruIqCount * top->iexBruPickCount; i++) {
        if (selectBruInst[i] == nullptr) {
            continue;
        }

        SimInst &inst = selectBruInst[i];

        PEResolveBus peResolve;
        peResolve.bid = inst->bid;
        peResolve.rid = inst->rid;
        peResolve.peid = inst->peID;
        peResolve.isIssue = true;
        peResolve.pipe_cycle = inst->pipeCycle;
        uint32_t tid = 0;
        if (top->GetSim()->core->IsVectorIex(top->machineType)) {
            tid = inst->GetTid();
        } else {
            tid = inst->stid;
        }
        top->iex_pe_rslv_array[inst->peID][tid]->Write(peResolve);
    }
}

void IssueQueue::issueVecSca()
{
    uint32_t selectCnt = 0;
    bool empty = CheckEmpty(current.scaIQ);
    std::vector<uint32_t> cntPerLaneArr(top->iexScaIqCount, 0);
    for (uint32_t i = 0; i < top->iexScaIqCount * top->iexScaPickCount; ++i) {
        selectScaInst[i] = std::shared_ptr<SimInstInfo>(nullptr);
    }

    auto noBr = [](const SimInst& inst, IssueBlockReason& reason) {
        (void)reason;
        return OpcodeIsSetc(inst->opcode) ||
               OpcodeIsCondInnerJump(inst->opcode) || OpcodeIsInnerJump(inst->opcode);
    };

    for (uint32_t i = 0; i < top->iexScaIqCount; ++i) {
        if (current.scaIQ[i].size <= 0) {
            continue;
        }

        uint32_t select = i * top->iexScaPickCount;
        for (uint32_t j = 0; j < top->iexScaPickCount; ++j) {
            uint32_t issuedPerIqLane = top->id == SCALAR_IEX ?
                configs.issuedPerIqLane : configs.simt_issued_per_iq_lane;
            if (top->id == MEM_IEX) {
                issuedPerIqLane = configs.mtc_issued_per_iq_lane;
            }
            if (cntPerLaneArr[i] >= issuedPerIqLane) {
                break;
            }

            empty = false;
            bool ldqLimit = false;
            std::vector<IssueBlockReason> blockReasons;
            if (j == 0) {
                selectScaInst[select] = current.scaIQ[i].Select(&next.scaIQ[i], ldqLimit, 0,
                    scaPipeStallFn[select], {}, {}, blockReasons);
            } else if (j == 1) {
                selectScaInst[select] = current.scaIQ[i].Select(&next.scaIQ[i], ldqLimit, 0,
                    scaPipeStallFn[select], { noBr }, {}, blockReasons);
            } else {
                ASSERT(false);
            }
            RptIQStats(selectScaInst[select], current.scaIQ[i], top->stats->sca_iq, i);
            if (!selectScaInst[select]) {
                RecordVecIssueBlockReasons(blockReasons);
                continue;
            }
            selectScaInst[select]->isqId = i;
            selectScaInst[select]->isqPickerId = j;

            ++top->stats->issued_cnt;
            top->stats->sca_iq.total_inst_num++;
            if (checkInstOptMatch(selectScaInst[select])) {
                top->stats->can_be_optimized_inst++;
            }
            top->stats->sca_iq.total_wait +=
                (GetSim()->getCycles() - selectScaInst[select]->pipeCycle->dispatchCycle);
            if (configs.iex_dispatch_mode == 2) {
                top->dispatchUnit.scaInfo.count[selectScaInst[select]->peID]--;
            }
            if (next.scaIQ[i].size < next.scaIQ[top->dispatchUnit.bruInfo.dispatchID].size) {
                top->dispatchUnit.bruInfo.dispatchID = i;
            }
            SimInst &issueInst = selectScaInst[select];
            if (top->GetSim()->core->IsVecPe(issueInst->peID)) {
                std::shared_ptr<VecPE> pe = top->GetSim()->core->vectorTop->GetPE(issueInst->coreID);
                pe->prob[issueInst->GetTid()]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->GetTid()]->SetIsqPicker(issueInst->rid, j);
            } else {
                std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(top->GetSim()->core->peArray[issueInst->peID]);
                pe->prob[issueInst->stid]->SetIsqId(issueInst->rid, i);
                pe->prob[issueInst->stid]->SetIsqPicker(issueInst->rid, j);
            }
            ++select;
            ++selectCnt;
            ++cntPerLaneArr[i];
        }
    }

    if (selectCnt == 0) {
        if (!empty) {
            top->stats->sca_iq.all_pick_stall++;
        }
        scaIQStall = true;
        return;
    }
    scaIQStall = false;

    for (uint32_t i = 0; i < top->iexScaIqCount * top->iexScaPickCount; i++) {
        if (selectScaInst[i] == nullptr) {
            continue;
        }

        SimInst &inst = selectScaInst[i];
        PLpvInfo lpvInfo = inst->GetLpv();

        // Stack get wake up stack load check
        if (inst->SrcTypeContain(OperandType::STACK_POINTER)) {
            wakeupStackLoad(inst->bid, inst->rid, lpvInfo);
            top->rtable.setROBReadyTable(inst->peID, inst->rid.val, lpvInfo, true);
        }

        PEResolveBus peResolve;
        peResolve.bid = inst->bid;
        peResolve.rid = inst->rid;
        peResolve.peid = inst->peID;
        peResolve.isIssue = true;
        peResolve.pipe_cycle = inst->pipeCycle;
        uint32_t tid = 0;
        if (top->GetSim()->core->IsVectorIex(top->machineType)) {
            tid = inst->GetTid();
        } else {
            tid = inst->stid;
        }
        top->iex_pe_rslv_array[inst->peID][tid]->Write(peResolve);
    }
}

void IssueQueue::WakeupIQTag(const WakeupInfo& wakeInfo, PLpvInfo lpvInfo, uint32_t peID,
                             uint32_t tid, uint32_t stid)
{
    bool wakeup = false;
    for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
        wakeup |= next.cmdIQ[i].Wakeup(wakeInfo, lpvInfo, peID, tid, stid);
    }
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        wakeup |= next.aluIQ[i].Wakeup(wakeInfo, lpvInfo, peID, tid, stid);
    }
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        wakeup |= next.aguIQ[i].Wakeup(wakeInfo, lpvInfo, peID, tid, stid);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        wakeup |= next.ldaIQ[i].Wakeup(wakeInfo, lpvInfo, peID, tid, stid);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        wakeup |= next.ldaScIQ[i].Wakeup(wakeInfo, lpvInfo, peID, tid, stid);
    }
    for (uint32_t i = 0; i < top->iexStaIqCount; i++) {
        wakeup |= next.staIQ[i].Wakeup(wakeInfo, lpvInfo, peID, tid, stid);
    }
    for (uint32_t i = 0; i < top->iexStdIqCount; i++) {
        wakeup |= next.stdIQ[i].Wakeup(wakeInfo, lpvInfo, peID, tid, stid);
    }
    for (uint32_t i = 0; i < top->iexBruIqCount; i++) {
        wakeup |= next.bruIQ[i].Wakeup(wakeInfo, lpvInfo, peID, tid, stid);
    }
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        wakeup |= next.scaIQ[i].Wakeup(wakeInfo, lpvInfo, peID, tid, stid);
    }
    top->SetRegReadyTable(wakeInfo.type, wakeInfo.ptag, true, lpvInfo, peID, tid);
    if (wakeInfo.type == OperandType::OPD_GREG) {
        ++top->stats->total_wakeup;
        if (!wakeup) {
            ++top->stats->no_wakeup;
            if (top->stats->no_wakeup_ptag_map.count(wakeInfo.ptag) != 0) {
                ++top->stats->no_wakeup_ptag_map[wakeInfo.ptag];
            } else {
                top->stats->no_wakeup_ptag_map.insert(make_pair(wakeInfo.ptag, 1));
            }
        }
    }
}

// void IssueQueue::WakeupDummy(uint32_t peID, uint32_t dTag, PLpvInfo &lpvInfo, uint32_t tid)
// {
//     for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
//         next.cmdIQ[i].WakeupTmpSrcD(dTag, peID, lpvInfo, tid);
//     }
// }

void IssueQueue::wakeupLda(uint64_t id)
{
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].wakeupLda(id);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaIQ[i].wakeupLda(id);
    }
    GetSim()->unsetRefStq(id);
}

void IssueQueue::windowSlides(uint64_t distance, bool isLoad)
{
    if (isLoad) {
        if (top->core->IsVectorIex(top->machineType)) {
            LOG_DEBUG_M(top->machineType, Stage::NA) << "load window slides(ptr+1) to " << std::dec
                << (next.aguIQ[0].sliding_window_load_id + distance);
        }
        for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
            next.aguIQ[i].windowSlides(distance, isLoad);
        }
        for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
            next.ldaIQ[i].windowSlides(distance, isLoad);
        }
        return;
    }
    for (uint32_t i = 0; i < top->iexStaIqCount; i++) {
        next.staIQ[i].windowSlides(distance);
    }
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].windowSlides(distance);
    }
    for (uint32_t i = 0; i < top->iexStdIqCount; i++) {
        next.stdIQ[i].windowSlides(distance);
    }
}

void IssueQueue::wakeupSptag(uint32_t sptag, bool recycled, PLpvInfo &lpvInfo)
{
    for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
        next.cmdIQ[i].Wakeup(WakeupInfo(OperandType::STACK_POINTER, sptag, recycled), lpvInfo);
    }
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        next.aluIQ[i].Wakeup(WakeupInfo(OperandType::STACK_POINTER, sptag, recycled), lpvInfo);
    }
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        next.scaIQ[i].Wakeup(WakeupInfo(OperandType::STACK_POINTER, sptag, recycled), lpvInfo);
    }
    top->core->sRenameUnit->setSpPtagReady(sptag, true, lpvInfo); // TODO : move stack ready table to iex
}

void IssueQueue::ReleaseEntry(ROBID bid, ROBID rid, uint32_t stid) {
    for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
        next.cmdIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        next.aluIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        next.scaIQ[i].ReleaseEntry(bid, rid, stid);
    }
    top->dispatchUnit.ReleaseEntry(bid, rid, stid);
}

void IssueQueue::wakeupStackLoad(ROBID bid, ROBID rid, PLpvInfo &lpvInfo)
{
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].wakeupStackLoad(bid, rid, lpvInfo);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaIQ[i].wakeupStackLoad(bid, rid, lpvInfo);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaScIQ[i].wakeupStackLoad(bid, rid, lpvInfo);
    }
}

void IssueQueue::flush(FlushBus flushReq)
{
    if (!flushReq.req.vld) {
        return;
    }

    auto match_flush = [&flushReq](SimInst &inst)->bool {
        if (inst->stid != flushReq.req.stid) {
            return false;
        }
        if (flushReq.baseOnPE && flushReq.req.peID != inst->peID) {
            return false;
        }
        if (flushReq.baseOnThread && flushReq.req.tid != inst->GetTid()) {
            return false;
        }
        return flushReq.baseOnBid ? LessEqual(flushReq.req.bid, inst->bid):
            LessEqual(flushReq.req.bid, flushReq.req.rid, inst->bid, inst->rid);
    };
    cmdIQ.FlushIf(match_flush);
    for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
        next.cmdIQ[i].flush(flushReq);
    }
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        next.aluIQ[i].flush(flushReq);
    }
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].flush(flushReq);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaIQ[i].flush(flushReq);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaScIQ[i].flush(flushReq);
    }
    for (uint32_t i = 0; i < top->iexStaIqCount; i++) {
        next.staIQ[i].flush(flushReq);
    }
    for (uint32_t i = 0; i < top->iexStdIqCount; i++) {
        next.stdIQ[i].flush(flushReq);
    }
    for (uint32_t i = 0; i < top->iexBruIqCount; i++) {
        next.bruIQ[i].flush(flushReq);
    }
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        next.scaIQ[i].flush(flushReq);
    }
    auto flush_stage = [&flushReq, this] (SimInst &inst) {
        if (!inst) {
            return;
        }
        if (flushReq.req.stid != inst->stid) {
            return;
        }
        if (flushReq.baseOnPE && flushReq.req.peID != inst->peID) {
            return;
        }
        if (flushReq.baseOnThread && flushReq.req.tid != inst->GetTid()) {
            return;
        }
        bool lessEq = flushReq.baseOnBid ? LessEqual(flushReq.req.bid, inst->bid):
            LessEqual(flushReq.req.bid, flushReq.req.rid, inst->bid, inst->rid);
        if (lessEq) {
            inst = std::shared_ptr<SimInstInfo>(nullptr);
        }
    };
    for (auto &inst : selectLdaInst) {
        flush_stage(inst);
    }
    for (auto &inst : selectSCLdaInst) {
        flush_stage(inst);
    }
    for (auto &inst : selectAguInst) {
        flush_stage(inst);
    }
    for (auto &inst : selectCmdInst) {
        flush_stage(inst);
    }
    for (auto &inst : selectAluInst) {
        flush_stage(inst);
    }
    for (auto &inst : selectStaInst) {
        flush_stage(inst);
    }
    for (auto &inst : selectStdInst) {
        flush_stage(inst);
    }
    for (auto &inst : selectBruInst) {
        flush_stage(inst);
    }
    for (auto &inst : selectScaInst) {
        flush_stage(inst);
    }
    auto flush_inst =[&flushReq] (SimInst inst) {
        if (flushReq.req.stid != inst->stid) {
            return false;
        }
        if (flushReq.baseOnPE && flushReq.req.peID != inst->peID) {
            return false;
        }
        if (flushReq.baseOnThread && flushReq.req.tid != inst->GetTid()) {
            return false;
        }
        return (flushReq.baseOnBid ? LessEqual(flushReq.req.bid, inst->bid):
            LessEqual(flushReq.req.bid, flushReq.req.rid, inst->bid, inst->rid));
    };
    isq_wake_q.FlushIf(flush_inst);
}

static void CancelInstLpv(SimInst &inst, uint32_t pipe)
{
    if (inst && inst->CheckCancel(pipe)) {
        for (auto psrc : inst->psrcs_) {
            if (psrc->lpvInfo != nullptr && psrc->lpvInfo->CheckCancel(pipe)) { //  && !inst->ref.gsInfo.src_vld
                psrc->ready = false;
                psrc->lpvInfo->Reset();
            }
        }

        inst->issued = false;
    }
}

void IssueQueue::setIQCancel(uint32_t pipe) {
    for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
        next.cmdIQ[i].setCancel(pipe);
    }
    for (auto &inst : cmdIQ.GetRawReadData()) {
        CancelInstLpv(inst, pipe);
    }
    for (auto &inst : cmdIQ.GetRawWriteData()) {
        CancelInstLpv(inst, pipe);
    }
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        next.aluIQ[i].setCancel(pipe);
    }
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].setCancel(pipe);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaIQ[i].setCancel(pipe);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaScIQ[i].setCancel(pipe);
    }
    for (uint32_t i = 0; i < top->iexStaIqCount; i++) {
        next.staIQ[i].setCancel(pipe);
    }
    for (uint32_t i = 0; i < top->iexStdIqCount; i++) {
        next.stdIQ[i].setCancel(pipe);
    }
    for (uint32_t i = 0; i < top->iexBruIqCount; i++) {
        next.bruIQ[i].setCancel(pipe);
    }
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        next.scaIQ[i].setCancel(pipe);
    }
}

void IssueQueue::releaseCMDEntry(ROBID bid, ROBID rid, uint32_t stid) {
    if (cmdIQ.Empty()) {
        return;
    }
    SimInst inst = cmdIQ.Front();
    if (inst->bid == bid && inst->rid == rid && inst->stid == stid) {
        cmdIQ.Read();
    }
}

void IssueQueue::releaseALUEntry(ROBID bid, ROBID rid, uint32_t stid) {
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        next.aluIQ[i].ReleaseEntry(bid, rid, stid);
    }
}

void IssueQueue::releaseAGUEntry(ROBID bid, ROBID rid, uint32_t stid)
{
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].ReleaseEntry(bid, rid, stid);
    }
}

void IssueQueue::ReleaseLDAEntry(ROBID bid, ROBID rid, uint32_t stid)
{
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaIQ[i].ReleaseEntry(bid, rid, stid);
    }
}

void IssueQueue::ReleaseLDASCEntry(ROBID bid, ROBID rid, uint32_t stid)
{
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++)
        next.ldaScIQ[i].ReleaseEntry(bid, rid, stid);
}

void IssueQueue::releaseSTAEntry(ROBID bid, ROBID rid, uint32_t stid)
{
    if (top->core->IsVectorIex(top->machineType)) {
        return;
    }
    for (uint32_t i = 0; i < top->iexStaIqCount; i++) {
        next.staIQ[i].ReleaseEntry(bid, rid, stid);
    }
}

void IssueQueue::releaseSTDEntry(ROBID bid, ROBID rid, uint32_t stid)
{
    for (uint32_t i = 0; i < top->iexStdIqCount; i++) {
        next.stdIQ[i].ReleaseEntry(bid, rid, stid);
    }
}

void IssueQueue::releaseBRUEntry(ROBID bid, ROBID rid, uint32_t stid)
{
    for (uint32_t i = 0; i < top->iexBruIqCount; i++) {
        next.bruIQ[i].ReleaseEntry(bid, rid, stid);
    }
}

void IssueQueue::ReleaseVecScaEntry(ROBID bid, ROBID rid, uint32_t stid)
{
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        next.scaIQ[i].ReleaseEntry(bid, rid, stid);
    }
}

void IssueQueue::releaseIQEntry(ROBID bid, ROBID rid, uint32_t stid) {
    for (uint32_t i = 0; i < top->iexCmdIqCount; i++) {
        next.cmdIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexAluIqCount; i++) {
        next.aluIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexAguIqCount; i++) {
        next.aguIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexLdaIqCount; i++) {
        next.ldaScIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexStaIqCount; i++) {
        next.staIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexStdIqCount; i++) {
        next.stdIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexBruIqCount; i++) {
        next.bruIQ[i].ReleaseEntry(bid, rid, stid);
    }
    for (uint32_t i = 0; i < top->iexScaIqCount; i++) {
        next.scaIQ[i].ReleaseEntry(bid, rid, stid);
    }
}

SimSys *IssueQueue::GetSim()
{
    return top->GetSim();
}

IexExecUnit IssueQueue::GetIssueExecUnit(Opcode opcode) const
{
    IexExecUnit unit = IexExecUnit::UNDEF;
    auto iterLatency = IexLatency::VEC_LATENCY.find(opcode);
    if (iterLatency != IexLatency::VEC_LATENCY.cend()) {
        unit = iterLatency->second.unit;
    }
    if (unit == IexExecUnit::UNDEF) {
        unit = IexExecUnit::IALU;
        LOG_WARN_M(top->machineType, Stage::NA) << "Unhandled OpCode: " << GetOpcodeName(opcode);
    }
    return unit;
}

void AgeQueue::Build(uint32_t size)
{
    if (configs->store_cnt_to_limit_load == 0 || top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        return;
    }

    entry.resize(size);
    for (auto &e : entry) {
        e = std::make_shared<SimInstInfo>();
    }
    Reset();
}

void AgeQueue::Reset()
{
    if (configs->store_cnt_to_limit_load == 0 || top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        return;
    }

    for (auto &e : entry) {
        e = std::shared_ptr<SimInstInfo>(nullptr);
    }
    size = 0;
    storeCnt = 0;
}

void AgeQueue::insert(SimInst &inst)
{
    if (configs->store_cnt_to_limit_load == 0 || top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        return;
    }

    ASSERT(size < entry.size());
    if (OpcodeIsLoad(inst->opcode) || (OpcodeIsStore(inst->opcode) && inst->type == ST_ADDR)) {
        entry[size] = inst;
        ++size;
        if (OpcodeIsStore(inst->opcode)) {
            ++storeCnt;
        }
    }
}

bool AgeQueue::loadLimit(SimInst &inst)
{
    if (configs->store_cnt_to_limit_load == 0 || top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        return false;
    }

    if (!OpcodeIsLoad(inst->opcode)) {
        return false;
    }

    if (size < configs->store_cnt_to_limit_load || storeCnt < configs->store_cnt_to_limit_load) {
        return false;
    }

    if (inst->ldqCheck) {
        if (!inst->ldqLimit) {
            return false;
        }

        if (!update) {
            return true;
        }
    }

    uint32_t cnt = 0;
    for (auto &e : entry) {
        if (e) {
            if (e->bid == inst->bid && e->rid == inst->rid && e->stid == inst->stid) {
                return (cnt >= configs->store_cnt_to_limit_load);
            }

            if (!e->issued && OpcodeIsStore(e->opcode)) {
                ++cnt;
            }
        }
    }

    return false;
}

void AgeQueue::issued(SimInst &inst, bool state)
{
    if (configs->store_cnt_to_limit_load == 0 || top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        return;
    }

    if (!OpcodeIsLoad(inst->opcode) && !OpcodeIsStore(inst->opcode)) {
        return;
    }

    for (auto &e : entry) {
        if (e && e->bid == inst->bid && e->rid == inst->rid && e->stid == inst->stid) {
            e->issued = state;
            if (OpcodeIsStore(inst->opcode)) {
                update = true;
                storeCnt = (state) ? storeCnt - 1 : storeCnt + 1;
            }
            return;
        }
    }

    return;
}

void AgeQueue::sortByAge()
{
    auto cmp = [](SimInst &a, SimInst &b) {
        if (a && b) {
            return LessROBID(a->bid, a->rid, b->bid, b->rid);
        }

        if (a) {
            return true;
        }

        return false;
    };

    if (configs->store_cnt_to_limit_load == 0 || top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        return;
    }

    if (entry.empty()) {
        return;
    }
    sort(entry.begin(), entry.end(), cmp);
}

void AgeQueue::Release(ROBID bid, ROBID rid, uint32_t stid)
{
    if (configs->store_cnt_to_limit_load == 0 || top->core->IsVectorIex(top->machineType) || top->id == MEM_IEX) {
        return;
    }

    for (auto &e : entry) {
        if (e && e->bid == bid && e->rid == rid && e->stid == stid) {
            e = std::shared_ptr<SimInstInfo>(nullptr);
            ASSERT(size > 0);
            --size;
            return;
        }
    }
}

bool checkEmpty(std::vector<IssueState> &iqArray)
{
    for (IssueState &iq : iqArray) {
        if (iq.sizeNotIssued != 0) {
            return false;
        }
    }

    return true;
}

bool IssueQ::isEmpty()
{
    if (!checkEmpty(ldaIQ)) {
        return false;
    }

    if (!checkEmpty(aguIQ)) {
        return false;
    }

    if (!checkEmpty(aluIQ)) {
        return false;
    }

    if (!checkEmpty(bruIQ)) {
        return false;
    }

    if (!checkEmpty(cmdIQ)) {
        return false;
    }

    if (!checkEmpty(staIQ)) {
        return false;
    }

    if (!checkEmpty(stdIQ)) {
        return false;
    }

    if (!checkEmpty(scaIQ)) {
        return false;
    }

    return true;
}

} // namespace JCore
