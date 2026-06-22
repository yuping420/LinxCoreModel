#include "ResVerifyManager.h"

#include "include/SimSys.h"
#include "core/Core.h"

namespace JCore {

void ResVerifyManager::Init(uint64_t bRobDepth)
{
    config.overrideDefaultConfig(sim->getCfgs());
    mRefBlockInfo = RingQ<BlockVerifyPtr>(bRobDepth);
    bfuPerfectTrace = RingQ<BlockPerfectPtr>(bRobDepth);
    ifuPerfectTrace = RingQ<BlockPerfectPtr>(bRobDepth);
    mModelBlockInfo.clear();
    mModelBlockInfo.resize(bRobDepth);
    for (uint64_t i = 0; i < bRobDepth; i++) {
        mModelBlockInfo[i] = std::make_shared<BlockVerifyInfo>();
        mModelBlockInfo[i]->Reset(i);
    }

    blockRobDepth = bRobDepth;
}

void ResVerifyManager::SetBlockBlockStatus(uint64_t blockId, bool status)
{
    mModelBlockInfo[blockId]->modelCompleted = status;
}

void ResVerifyManager::ResetBlockInfo(uint64_t blockId, uint64_t bpc)
{
    mModelBlockInfo[blockId]->Reset(blockId);
    mModelBlockInfo[blockId]->bpc = bpc;
}

void ResVerifyManager::RecordLoopManagerLastBlock(uint64_t blockId)
{
    mModelBlockInfo[blockId]->isLastBlock = true;
}

void ResVerifyManager::RecordBlockCompleted(uint64_t blockId)
{
    mModelBlockInfo[blockId]->modelCompleted = true;
}

void ResVerifyManager::RecordBlockTileRegData(uint64_t blockId, std::shared_ptr<TileRegVerifyData> tileRegData)
{
    waitCycles = 0;
    mModelBlockInfo[blockId]->verifyTileRegData = true;
    mModelBlockInfo[blockId]->tileRegData.push_back(tileRegData);
}

void ResVerifyManager::RecordMinst(InstVerifyInfo &minst)
{
    waitCycles = 0;
    mModelBlockInfo[minst.bid]->instVerifyInfoList.push_back(minst);
    LOG_INFO_M(Unit::DFX, Stage::NA) << "record model minst bid " << std::dec << minst.bid << " tpc:0x" << std::hex
        << minst.tpc << " data:0x" << minst.data;
}

void ResVerifyManager::InitPARGroup(uint64_t blockId, uint64_t groupNum)
{
    mModelBlockInfo[blockId]->isParBlock = true;
    mModelBlockInfo[blockId]->groupNum = groupNum;
    mModelBlockInfo[blockId]->groupStatus.resize(groupNum);
    mModelBlockInfo[blockId]->groupInstVerifyInfoList.resize(groupNum);
}

void ResVerifyManager::RecordPARGroupCompleted(uint64_t blockId, uint64_t groupId)
{
    waitCycles = 0;
    mModelBlockInfo[blockId]->completedGroupNum++;
    mModelBlockInfo[blockId]->groupStatus[groupId].isCompleted = true;
}

void ResVerifyManager::RecordPARMinst(InstVerifyInfo &minst)
{
    waitCycles = 0;
    if (!config.verifyParBlockMinst) {
        return;
    }
    mModelBlockInfo[minst.bid]->groupInstVerifyInfoList[minst.lgid].push_back(minst);
}

bool ResVerifyManager::RefBlockInfoListFull()
{
    return mRefBlockInfo.Full();
}

void ResVerifyManager::RecordRefBlockInfo(BlockVerifyPtr info)
{
    mRefBlockInfo.Write(info);
    // LOG_INFO_M(Unit::DFX, Stage::NA) << "record ref core bpc:0x" << std::hex << info->bpc;
}

void ResVerifyManager::RecordPerfectBlockInfo(BlockPerfectPtr bfuInfo, BlockPerfectPtr ifuInfo)
{
    bfuPerfectTrace.Write(bfuInfo);
    ifuPerfectTrace.Write(ifuInfo);
}

bool ResVerifyManager::CheckBFUFrontLast()
{
    if (bfuPerfectTrace.Empty()) {
        return true;
    }
    BlockPerfectPtr front = bfuPerfectTrace.Front();
    return front->instPCList.size() == bfuInstPtr + 1;
}

bool ResVerifyManager::CheckBFUFrontIsFirst()
{
    if (bfuPerfectTrace.Empty()) {
        return true;
    }
    return bfuInstPtr == 0;
}

uint64_t ResVerifyManager::GetBFUFrontPC()
{
    if (bfuPerfectTrace.Empty()) {
        return 0;
    }
    BlockPerfectPtr front = bfuPerfectTrace.Front();
    ASSERT(bfuInstPtr < front->instPCList.size());
    uint64_t pc = front->instPCList[bfuInstPtr++];
    if (bfuInstPtr == front->instPCList.size()) {
        bfuPerfectTrace.Read();
        bfuInstPtr = 0;
    }
    return pc;
}

void ResVerifyManager::CompletedBlock(uint64_t blockId)
{
    sim->ReleaseRefCoreInfo(mModelBlockInfo[blockId]->isLastBlock);
    // TODO: stid may needed
    if (!mRefBlockInfo.Empty()) {
        mRefBlockInfo.Read();
    }
    if (sim->core->configs.bp_mode == static_cast<uint32_t>(BP_Mode::PERFECT_BP)) {
        ifuPerfectTrace.Read();
    }
    currentBlockId++;
    verifiedBlockCount++;
    mModelBlockInfo[blockId]->Reset(blockId);
}

bool ResVerifyManager::Verify(uint64_t width)
{
    for (uint64_t i = 0; i < width; i++) {
        if (verifiedBlockCount >= sim->maxBCount) {
            break;
        }
        uint64_t ptr = currentBlockId % blockRobDepth;
        if (!mModelBlockInfo[ptr]->modelCompleted) {
            waitCycles++;
            return !IsDeadLock();
        }
        waitCycles = 0;
        if (!config.resVerifyEnable) {
            LOG_INFO_M(Unit::DFX, Stage::NA) << "Verify Skip Block ptr" << ptr << " bpc:0x" << std::hex
                << mModelBlockInfo[ptr]->bpc << " total Verified Block:" << std::dec << verifiedBlockCount;
            CompletedBlock(ptr);
            continue;
        }
        if (VerifyBlockInfo(ptr)) {
            LOG_INFO_M(Unit::DFX, Stage::NA) << "Verify suaccelss Block ptr" << ptr << " bpc:0x" << std::hex
                << mModelBlockInfo[ptr]->bpc << " total Verified Block:" << std::dec << verifiedBlockCount;
            CompletedBlock(ptr);
            continue;
        } else {
            DumpErrorInfo(ptr);
            return false;
        }
    }
    return true;
}

bool ResVerifyManager::VerifyBlockInfo(uint64_t blockId)
{
    BlockVerifyPtr refInfo = mRefBlockInfo.Front();
    BlockVerifyPtr &modelInfo = mModelBlockInfo[blockId];
    if (refInfo->bpc != modelInfo->bpc) {
        errorType = VerifyErrorType::ERROR_BPC_NE;
        return false;
    }

    bool ret = modelInfo->isParBlock ? VerifyParBlockInfo(blockId) : VerifyScalarBlockInfo(blockId);
    if (!ret) {
        return ret;
    }

    if (modelInfo->verifyTileRegData) {
        if (refInfo->tileRegData.size() != modelInfo->tileRegData.size()) {
            return false;
        }
        for (uint32_t i = 0; i < refInfo->tileRegData.size(); i++) {
            if (!VerifyTileRegData(refInfo->tileRegData[i], modelInfo->tileRegData[i])) {
                std::cerr << "resverify i:" << std::dec << i << " size:" << refInfo->tileRegData.size() << " ";
                std::cerr << " bid: " << dec << blockId << " " << endl;

                std::cerr << " ref bpc: " << hex << refInfo->bpc << " " << endl;
                refInfo->tileRegData[i]->Dump();

                std::cerr << " model bpc: " << hex << modelInfo->bpc << " " << endl;
                modelInfo->tileRegData[i]->Dump();

                return false;
            }
        }
    }

    return true;
}

bool ResVerifyManager::VerifyBEndInst(uint64_t blockId)
{
    BlockVerifyPtr &modelInfo = mModelBlockInfo[blockId];
    if (modelInfo->instVerifyInfoList.size() > 1) {
        return false;
    }
    InstVerifyInfo modelInst = modelInfo->instVerifyInfoList.front();
    if (modelInst.terminate) {
        return true;
    } else {
        return false;
    }
    return true;
}

bool ResVerifyManager::VerifyScalarBlockInfo(uint64_t blockId)
{
    if (!config.verifyScalarBlockMinst) {
        return true;
    }
    BlockVerifyPtr refInfo = mRefBlockInfo.Front();
    BlockVerifyPtr &modelInfo = mModelBlockInfo[blockId];
    for (auto it = refInfo->instVerifyInfoList.cbegin(); it != refInfo->instVerifyInfoList.cend();) {
        if (!it->check) {
            it = refInfo->instVerifyInfoList.erase(it);
        } else {
            it++;
        }
    }
    for (auto it = modelInfo->instVerifyInfoList.cbegin(); it != modelInfo->instVerifyInfoList.cend();) {
        if (!it->check) {
            it = modelInfo->instVerifyInfoList.erase(it);
        } else {
            it++;
        }
    }
    LOG_INFO_M(Unit::BCC, Stage::NA) << "Verify Block " << dec << blockId;
    while (!refInfo->instVerifyInfoList.empty() || !modelInfo->instVerifyInfoList.empty()) {
        // check model inst list is terminate when ref inst list is empty
        if (refInfo->instVerifyInfoList.empty() && VerifyBEndInst(blockId)) {
            break;
        }
        if (refInfo->instVerifyInfoList.empty()) {
            InstVerifyInfo modelInst = modelInfo->instVerifyInfoList.front();
            LOG_INFO_M(Unit::DFX, Stage::NA) << "Model PC: 0x" << hex << modelInst.tpc << ", check: " << boolalpha
                                             << modelInst.check << ", opcode: " << GetOpcodeName(modelInst.opcode);
        }
        ASSERT(!refInfo->instVerifyInfoList.empty() && "refInfo->instVerifyInfoList is empty");
        InstVerifyInfo refInst = refInfo->instVerifyInfoList.front();
        LOG_INFO_M(Unit::DFX, Stage::NA) << "Ref PC: 0x" << hex << refInst.tpc << ", check: " << boolalpha
                                         << refInst.check << ", opcode: " << GetOpcodeName(refInst.opcode);
        if (modelInfo->instVerifyInfoList.empty()) {
            errorType = VerifyErrorType::ERROR_MINST_NUM_NE;
            return false;
        }
        InstVerifyInfo modelInst = modelInfo->instVerifyInfoList.front();
        LOG_INFO_M(Unit::DFX, Stage::NA) << "Model PC: 0x" << hex << modelInst.tpc << ", check: " << boolalpha
                                         << modelInst.check << ", opcode: " << GetOpcodeName(modelInst.opcode);
        if (VerifyMinst(refInst, modelInst)) {
            refInfo->instVerifyInfoList.pop_front();
            modelInfo->instVerifyInfoList.pop_front();
            verifiedMinstCount++;
        } else {
            return false;
        }
    }
    return true;
}

bool ResVerifyManager::VerifyParBlockInfo(uint64_t blockId)
{
    if (!config.verifyParBlockMinst) {
        return true;
    }

    BlockVerifyPtr refInfo = mRefBlockInfo.Front();
    BlockVerifyPtr &modelInfo = mModelBlockInfo[blockId];

    /* push model simt inst to inst list */
    for (auto instList : modelInfo->groupInstVerifyInfoList) {
        for (auto inst : instList) {
            modelInfo->instVerifyInfoList.push_back(inst);
        }
    }

    /* erase need not opcode at model/ref inst list */
    refInfo->instVerifyInfoList.erase(
        std::remove_if(refInfo->instVerifyInfoList.begin(),
                       refInfo->instVerifyInfoList.end(),
                       [](const InstVerifyInfo& inst) {
                           return OpcodeNotCheck(inst.opcode);
                       }),
        refInfo->instVerifyInfoList.end()
    );

    modelInfo->instVerifyInfoList.erase(
        std::remove_if(modelInfo->instVerifyInfoList.begin(),
                       modelInfo->instVerifyInfoList.end(),
                       [](const InstVerifyInfo& inst) {
                           return OpcodeNotCheck(inst.opcode);
                       }),
        modelInfo->instVerifyInfoList.end()
    );

    if (refInfo->instVerifyInfoList.size() != modelInfo->instVerifyInfoList.size()) {
        errorType = VerifyErrorType::ERROR_MINST_NUM_NE;
        return false;
    }

    if (refInfo->instVerifyInfoList.empty()) {
        return true;
    }

    while (!refInfo->instVerifyInfoList.empty() || !modelInfo->instVerifyInfoList.empty()) {
        // check model inst list is terminate when ref inst list is empty
        InstVerifyInfo refInst = refInfo->instVerifyInfoList.front();
        ASSERT(refInst.check);

        InstVerifyInfo modelInst = modelInfo->instVerifyInfoList.front();
        if (VerifyMinst(refInst, modelInst)) {
            refInfo->instVerifyInfoList.pop_front();
            modelInfo->instVerifyInfoList.pop_front();
            verifiedMinstCount++;
        } else {
            cerr << " verify error: ref lane " << dec << refInst.lane << " model lane: " << modelInst.lane << endl;
            return false;
        }
    }

    ASSERT(modelInfo->instVerifyInfoList.empty());
    return true;
}

bool ResVerifyManager::VerifyTileRegData(std::shared_ptr<TileRegVerifyData> refData,
                                         std::shared_ptr<TileRegVerifyData> modelData)
{
    if (!config.verifyParBlockTileReg) {
        return true;
    }
    ASSERT(refData != nullptr && modelData != nullptr);
    if ((*refData) == (*modelData)) {
        return true;
    }
    errorType = VerifyErrorType::ERROR_TILE_DATA_NE;
    return false;
}

bool ResVerifyManager::VerifyMinst(InstVerifyInfo &refInst, InstVerifyInfo &mInst)
{
    auto checkData = [](InstVerifyInfo &refInst, InstVerifyInfo &mInst) -> bool {
        if (OpcodeInInstGroup(mInst.opcode, InstGroup::REDUCE)) {
            return true;
        }
        if (!refInst.isSIMTMinst) {
            return refInst.data == mInst.data;
        } else if (refInst.isSIMTMinst) {
            uint64_t checkMask = 0xfff; /* Smallest size is 1kb */
            return (refInst.data & checkMask) == (mInst.data & checkMask);
        }
        return true;
    };
    if (refInst.tpc == mInst.tpc && checkData(refInst, mInst)) {
        return true;
    }
    if (refInst.tpc != mInst.tpc) {
        errorType = VerifyErrorType::ERROR_TPC_NE;
        return false;
    }
    errorType = VerifyErrorType::ERROR_RES_NE;
    return false;
}

bool ResVerifyManager::IsDeadLock()
{
    if (waitCycles >= config.deadLockThreshold) {
        errorType = VerifyErrorType::ERROR_DEADLOCK;
        DumpErrorInfo((currentBlockId % blockRobDepth));
        return true;
    }
    return false;
}

void ResVerifyManager::DumpDeadLockInfo()
{
    std::stringstream oss;
    oss << "DeadLock:";
    errorInfo += oss.str();
}

void ResVerifyManager::DumpErrorInfo(uint64_t blockId)
{
    BlockVerifyPtr refInfo = mRefBlockInfo.Empty() ? nullptr : mRefBlockInfo.Front();
    BlockVerifyPtr modelInfo = (blockId < mModelBlockInfo.size()) ? mModelBlockInfo[blockId] : nullptr;
    std::stringstream oss;
    oss << std::endl;
    oss << "---------------------------------------------------------------------" << std::endl;
    oss << "ERROR:" << GetErrorName(errorType) << " execution at:" << std::endl;
    if (errorType == VerifyErrorType::ERROR_DEADLOCK) {
        for (uint32_t stid = 0; stid < sim->core->configs.scalar_smt_thread; ++stid) {
            std::string oldestInfo = sim->core->bctrl->blockROB.PrintLastStatusAndReturnOldest(stid);
            oss << " thread: " << stid << oldestInfo << std::endl;
        }
    }
    oss << TAB_1 << "Cycle:" << std::dec << sim->getCycles() << std::endl;
    oss << TAB_1 << "Error BlockID:" << std::dec << blockId << std::endl;
    oss << TAB_1 << "Wait cycles:" << std::dec << waitCycles << std::endl;
    oss << TAB_1 << "Reference queue depth:" << std::dec << mRefBlockInfo.Size() << std::endl;
    if (refInfo != nullptr) {
        oss << TAB_1 << "Ref BPC: 0x" << std::hex << refInfo->bpc << " isPar: " << refInfo->isParBlock << std::endl;
    } else {
        oss << TAB_1 << "Ref BPC: unavailable (reference queue empty)" << std::endl;
    }
    if (modelInfo != nullptr) {
        oss << TAB_1 << "Model BPC: 0x" << std::hex << modelInfo->bpc << " isPar: " << modelInfo->isParBlock
            << " completed: " << std::boolalpha << modelInfo->modelCompleted << std::endl;
    } else {
        oss << TAB_1 << "Model BPC: unavailable (block id outside model info table)" << std::endl;
    }
    oss << TAB_1 << "Model Execute Minst:" << std::endl;
    if (modelInfo != nullptr && modelInfo->isParBlock) {
        ROBID robId;
        robId.val = blockId;
        for (uint32_t stid = 0; stid < sim->core->configs.scalar_smt_thread; ++stid) {
            BlockCommandPtr cmd = sim->core->bctrl->blockROB.GetBlockCMDPtr(robId, stid);
            if (cmd != nullptr) {
                oss << TAB_2 << " thread: " << stid << "BlockCmd:" << cmd->Dump() << std::endl;
            }
        }
        oss << TAB_2 << "Total Group Num:" << std::dec << modelInfo->groupNum << ", completed:" <<
            modelInfo->completedGroupNum << std::endl;
    }
    if (modelInfo != nullptr && !modelInfo->instVerifyInfoList.empty()) {
        InstVerifyInfo modelInst = modelInfo->instVerifyInfoList.front();
        sim->printInst(modelInfo->bpc, modelInst.tpc, oss);
        oss << TAB_3 << "TPC 0x" << std::hex << modelInst.tpc << ", opcode: " <<
            GetOpcodeName(modelInst.opcode) << ", data: 0x" << modelInst.data << std::endl;
    } else if (modelInfo == nullptr) {
        oss << TAB_3 << "Model block info unavailable" << std::endl;
    } else {
        oss << TAB_3 << "Model inst list empty" << std::endl;
    }
    oss << TAB_1 << "Reference Execute Minst:" << std::endl;
    if (refInfo != nullptr && !refInfo->instVerifyInfoList.empty()) {
        InstVerifyInfo refInst = refInfo->instVerifyInfoList.front();
        sim->printInst(refInfo->bpc, refInst.tpc, oss);
        oss << TAB_3 << "TPC 0x" << std::hex << refInst.tpc << ", opcode: " <<
            GetOpcodeName(refInst.opcode) << ", data: 0x" << refInst.data << std::endl;
    } else if (refInfo == nullptr) {
        oss << TAB_3 << "Reference block info unavailable" << std::endl;
    } else {
        oss << TAB_3 << "Reference inst list empty" << std::endl;
    }
    oss << TAB_1 << "Please check function model log at line: B" << std::dec << verifiedBlockCount << " M"
        << verifiedMinstCount << std::endl;
    oss << TAB_1 << dec << verifiedMinstCount << " instructions correctly executed so far" << endl;
    oss << "---------------------------------------------------------------------" << std::endl;
    errorInfo += oss.str();
}

std::string ResVerifyManager::GetErrorInfo()
{
    return errorInfo;
}

uint64_t ResVerifyManager::GetVerifiedBlockCount() const
{
    return verifiedBlockCount;
}

uint64_t ResVerifyManager::GetverifiedMinstCount() const
{
    return verifiedMinstCount;
}

void ResVerifyManager::ResetWaitCycle()
{
    waitCycles = 0;
}

void ResVerifyManager::Flush(const FlushBus &flushReq, uint64_t commitBid)
{
    (void)commitBid;
    uint64_t bid = flushReq.req.bid.GetVal();
    LOG_DEBUG_M(Unit::DFX, Stage::NA) << "DFX FLUSH bid: " << dec << bid;
    if (flushReq.req.fetchTPCVld && !flushReq.baseOnBid) {
        LOG_DEBUG_M(Unit::DFX, Stage::NA) << "Base on RID: "
                                          << flushReq.req.rid << ", fetchTPC: 0x" << hex << flushReq.req.fetchTPC;
        auto& instInfo = mModelBlockInfo[bid];
        ASSERT(flushReq.req.fetchTPCVld);
        uint64_t rid = flushReq.req.rid.GetVal();
        auto it = instInfo->instVerifyInfoList.begin();
        for (; it != instInfo->instVerifyInfoList.end(); ++it) {
            LOG_DEBUG_M(Unit::DFX, Stage::NA) << "Query: tpc: 0x" << hex << it->tpc << ", rid: " << dec << it->rid;
            if (it->tpc == flushReq.req.fetchTPC && it->rid == rid) {
                break;
            }
        }
        instInfo->instVerifyInfoList.erase(it, instInfo->instVerifyInfoList.end());
    } else {
        // Nuke flush 会等待当前块最老才做 flush，会从块内第一条微指令开始取值
        // 所以需要把 BPC 给保留下来，否则校验会有问题
        LOG_DEBUG_M(Unit::DFX, Stage::NA) << "Base on bid";
        ResetBlockInfo(bid, mModelBlockInfo[bid]->bpc);
    }
}

} // namespace JCore
