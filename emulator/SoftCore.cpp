#include "SoftCore.h"

namespace JCore {
// 未能理解的功能，
std::vector<struct adderss_region> stub_region;

void SoftCore::Init()
{
    if (!inputArgs.cfgs.empty()) {
        LOG_WARN("Override configurations:");
        for (auto& cfg : inputArgs.cfgs) {
            LOG_WARN << cfg;
        }
    }
    cfgs = std::make_shared<ConfigInput>();
    bool ret = cfgs->Init(inputArgs.cfgs);
    ASSERT(ret && "Repeated configs input");
    config.overrideDefaultConfig(cfgs);
    memory = new SoftMemory();
    threadStatus.resize(config.multiThreadNum);
    for (uint64_t thread = 0; thread < config.multiThreadNum; thread++) {
        auto &status = threadStatus[thread];
        SetSystemReg(static_cast<uint64_t>(SystemReg::SYS_LXLCID), thread, thread);
        SetSystemReg(static_cast<uint64_t>(SystemReg::SYS_TEMP_CORE_ID), thread, thread); // 临时方案，
        status.archStatus.tileReg.Init(config.tileRegMaxRelativeIndex, config.tileRegMaxOutputNum, config.maxTileSize,
                                       config.minTileSize);
    }
    instLogs.clear();
    instLogs.resize(config.laneNum);
}


void SoftCore::Step()
{
    bool fullThreadEnd = true;
    for (uint64_t thread = 0; thread < config.multiThreadNum; thread++) {
        for (uint64_t i = 0; i < config.execWidth; i++) {
            EmulatorBlock(thread);
        }
        fullThreadEnd &= threadStatus[thread].simEnd;
    }
    coreSimEnd = fullThreadEnd;
}

void SoftCore::EmulatorBlock(uint64_t threadID)
{
    auto &threadState = threadStatus[threadID];
    threadState.currentBlock = nullptr;
    instLogs.clear();
    instLogs.resize(config.laneNum);
    bool blkEnd = false;
    uint64_t fetchPc = threadState.pc;
    while (!blkEnd && !threadState.simEnd) {
        PreProcessBlock(threadState.currentBlock);
        MInstFuncPtr inst = FetchDecodeInst(fetchPc, threadID);
        ExecuteMinst(inst, threadState.currentBlock);

        PostProcessBlock(threadState.currentBlock);

        fetchPc = UpdateNextPC(inst, threadState.currentBlock);
        blkEnd = IsBlockEnd(threadState.currentBlock);
        threadState.simEnd = IsSimEnd(inst);
    }
    threadState.pc = fetchPc;
}

MInstFuncPtr SoftCore::FetchDecodeInst(uint64_t fetchPc, uint64_t threadID)
{
    MInstFuncPtr inst = std::make_shared<MInstFunc>();
    uint32_t size = 0;
    uint64_t bin = 0;

    // PreCheck inst Size
    uint64_t probe16 = memory->Load(fetchPc, WIDTH_16, false);
    size = CheckMInstSize(probe16);
    bin = memory->Load(fetchPc, size, false);

    inst->DecodeBin(bin, size);
    inst->pc = fetchPc;
    inst->threadId = threadID;
    inst->execSeq = threadStatus[threadID].minstId;
    inst->localExecSeq = threadStatus[threadID].currentBlock == nullptr ?
                         0 : threadStatus[threadID].currentBlock->commitInstNum;
    InstGroup grp = OpcodeManager::Inst().GetOpcodeGroup(inst->opcode);
    inst->check = (grp != InstGroup::BLOCK_SPLIT);
    switch (grp) {
        case InstGroup::CONVERT:
            inst->InitFRM(GetFRM(inst->threadId));
            break;
        default:
            break;
    }
    InstConstraintCheck(inst, threadStatus[threadID].currentBlock);
    return inst;
}

void SoftCore::InstConstraintCheck(MInstFuncPtr &inst, BlockFuncPtr &currentBlock)
{
    if (inst->opcode == Opcode::OP_SETRET) {
        // CALL或ICALL类型的块指令中，setret或 c.setret指令必须放在BSTART后面。否则硬件触发非法指令异常。
        // BSTART 指令COND 和CALL 在编码上有部分重合，因此DIRECT 分支类型也在。
        if (currentBlock && (currentBlock->branchType == BranchType::BLK_BR_CALL ||
            currentBlock->branchType == BranchType::BLK_BR_ICALL ||
            currentBlock->branchType == BranchType::BLK_BR_DIRECT) &&
            currentBlock->commitInstNum == 1) {
            return;
        }
        LOG_ERROR << inst->Dump();
        LOG_ERROR << (currentBlock ? currentBlock->Dump() : "Empty Block");
        ASSERT(false && "SET_RET illegal");
    }
    if (inst->accMemInfo && inst->accMemInfo->continuous) {
        // 对于continue 为true
        // LD: baseAddr, LCx, offset
        // ST：srcD, baseAddr, LCx, offset
        OperandPtr baseAddrPtr = OpcodeIsLoad(inst->opcode) ? inst->srcs[SRC0_IDX] : inst->srcs[SRC1_IDX];
        bool bassAddrlegal = (OperandTypeIsSGPR(baseAddrPtr->type) || OperandTypeIsLGPR(baseAddrPtr->type) ||
                             OperandTypeIsLTAR(baseAddrPtr->type));
        OperandPtr offsetOpd = OpcodeIsLoad(inst->opcode) ? inst->srcs[SRC2_IDX] : inst->srcs[SRC3_IDX];
        bool isLSOffsetReg = OpcodeIsLoadReg(inst->opcode) || OpcodeIsStoreReg(inst->opcode);
        bool offsetLegal = true;
        if (isLSOffsetReg) {
            offsetLegal = (OperandTypeIsSGPR(offsetOpd->type) || OperandTypeIsLGPR(offsetOpd->type) ||
                          OperandTypeIsZero(offsetOpd->type));
            if (OperandTypeIsLoopReg(offsetOpd->type)) {
                offsetLegal = !currentBlock->IsReduceDimension();
            }
        }
        if (!bassAddrlegal || !offsetLegal) {
            LOG_ERROR << inst->Dump();
            LOG_ERROR << (currentBlock ? currentBlock->Dump() : "Empty Block");
            ASSERT(false && "Continues LD/ST Base Addr Reg illegal") << "bassAddrlegal:" << bassAddrlegal
                << ", offsetLegal:" << offsetLegal;
        }
    }
}

void SoftCore::ExecuteMinst(MInstFuncPtr inst, BlockFuncPtr &currentBlock)
{
    if (currentBlock != nullptr && currentBlock->simt &&
        (inst->codeLen == EncodeLen::ENL_V)) {
        ExecuteSIMTMinst(inst, currentBlock);
    } else {
        ExecuteSTDMinst(inst, currentBlock);
    }
}

void SoftCore::ExecuteSTDMinst(MInstFuncPtr inst, BlockFuncPtr &currentBlock)
{
    GetSrcData(inst, currentBlock);
    Calculate(inst, currentBlock);
    if (!OpcodeManager::Inst().IsNeedAccumulateBlockInfo(inst->opcode)) {
        SetDstData(inst, currentBlock);
    } else {
        AccumulateBlockInfo(inst, currentBlock);
    }
    StatSTDMInst(inst, currentBlock, currentBlock->commitInstNum == 0);
}

void SoftCore::ExecuteSIMTMinst(MInstFuncPtr inst, BlockFuncPtr &currentBlock)
{
    bool bypassPredMask = (inst->opcode == Opcode::OP_PSEL || inst->opcode == Opcode::OP_MOV);
    bool dstHasPredMask = false;
    for (auto &dst : inst->dsts) {
        if (dst->type == OperandType::OPD_PREDMASK) {
            dstHasPredMask = true;
            break;
        }
    }
    for (uint64_t i = 0; i < 64; ++i) {
        inst->laneID = i;
        if (currentBlock->localArchStatus.predMask[i].GetMask() || bypassPredMask) {
            GetSrcData(inst, currentBlock, i);
            Calculate(inst, currentBlock);
            if (!OpcodeManager::Inst().IsNeedAccumulateBlockInfo(inst->opcode)) {
                SetDstData(inst, currentBlock, i);
            } else {
                ASSERT(false && "SIMT Model unsupport");
            }
            StatSIMTMInst(inst, currentBlock, i);
        } else {
            for (auto& dst : inst->dsts) {
                dst->data = 0;
            }
            if (dstHasPredMask) {
                currentBlock->localArchStatus.predMask[i].SetMask(false);
            }
            if (!OpcodeManager::Inst().IsNeedAccumulateBlockInfo(inst->opcode)) {
                SetDstData(inst, currentBlock, i);
            }
        }
    }
}


void SoftCore::StatSTDMInst(MInstFuncPtr inst, BlockFuncPtr &currentBlock, bool firstInst)
{
    if (firstInst) {
        firstInst = false;
        if (verbose) {
            LOG_INFO << "\n" << currentBlock->Dump();
        }
    }
    if (inst->skipCurrentMinst) {
        return;
    }
    threadStatus[currentBlock->threadId].minstId++;
    currentBlock->commitInstNum++;
    if (verbose) {
        LOG_INFO << inst->Dump();
    }
    if (recordMInst) {
        instLogs[0].push_back(*inst);
    }
}

void SoftCore::StatSIMTMInst(MInstFuncPtr inst, BlockFuncPtr &currentBlock, uint64_t laneID)
{
    if (laneID == 0) {
        threadStatus[currentBlock->threadId].minstId++;
        currentBlock->commitInstNum++;
    }
    if (verbose) {
        LOG_INFO << inst->Dump(laneID, laneID == 0);
    }
    if (recordMInst) {
        instLogs[0].push_back(*inst);
    }
}

void SoftCore::PreProcessBlock(BlockFuncPtr &block)
{
    if (block != nullptr && block->simt && block->simtFirst) {
        block->simtFirst = false;
        InitSIMTRegInfo(block);
    }
}

void SoftCore::PostProcessBlock(BlockFuncPtr &currentBlock)
{
    if (currentBlock != nullptr && currentBlock->bIsTemplate && currentBlock->bIsComplete) {
        ExecuteTemplate(currentBlock);
    }
    if (currentBlock != nullptr && currentBlock->acrcReqType != ACRC_REQUEST_TYPE::SCT_INVAL &&
        currentBlock->bIsComplete) {
        ExecuteSysCall(currentBlock);
    }
    if (IsBlockEnd(currentBlock)) {
        if (!currentBlock->barg.dstATag.empty()) {
            ASSERT(currentBlock->barg.dstATag.size() == currentBlock->dstData.size());
            auto &threadState = threadStatus[currentBlock->threadId];
            for (size_t i = 0; i < currentBlock->barg.dstATag.size(); i++) {
                threadState.archStatus.gpr[currentBlock->barg.dstATag[i]] = currentBlock->dstData[i];
            }
        }
        CommitCurrentBlock(currentBlock);
    }
}

void SoftCore::CommitCurrentBlock(BlockFuncPtr &currentBlock)
{
    threadStatus[currentBlock->threadId].blockId++;
}

void SoftCore::Calculate(MInstFuncPtr inst, BlockFuncPtr &currentBlock)
{
    // 需要区分 真正需要计算的指令、ld/st 类指令(需要计算但是也需要访问memory)、累积块头信息的指令
    // 需要特殊处理B.IOT类指令，单独做tileAlloc
    bool ret = JCore::Calculate::MInstCalculator::Inst().CalculateMinst(*inst);
    if (!ret) {
        ASSERT(false) << "Calculate error:\n" << inst->Dump();
    }
    InstGroup grp = OpcodeManager::Inst().GetOpcodeGroup(inst->opcode);
    switch (grp) {
        case InstGroup::LOAD:
            AaccelssMemoryLoad(inst);
            break;
        case InstGroup::STORE:
            AaccelssMemoryStore(inst);
            break;
        case InstGroup::ATOMIC:
            AtomicAaccelssMemory(inst);
            break;
        case InstGroup::CACHE_MAINTAIN:
            CacheMaintainAaccelssMemory(inst);
            break;
        default:
            break;
    }
}

void SoftCore::ExecuteTemplate(BlockFuncPtr &currentBlock)
{
    /* 1.FENTRY/FEXIT/MCOPY.. code gen
     * 2. CUBE/TMA template
     */
    if (currentBlock->opcode != Opcode::OP_INVALID) {
        GenCodeTemplate(currentBlock);
    } else {
        ExecutePTO(currentBlock);
        auto &threadState = threadStatus[currentBlock->threadId];
        auto &tileReg = threadState.archStatus.tileReg;
        // tileReg的提交
        for (auto &src : currentBlock->srcTile) {       // 检查是否reuse
            if (!src->reuse) {
                tileReg.SetReleased(src->type, src->baseAddr);
            }
        }
        for (size_t i = 0; i < currentBlock->dstTile.size(); i++) {
            tileReg.Commit(tileReg.GetTileType(currentBlock->dstTile[i]->baseAddr), currentBlock->dstTile[i]->baseAddr);
        }
    }
    currentBlock->bIsComplete = true;
}

void SoftCore::ExecutePTO(BlockFuncPtr block)
{
    if (!block->srcTile.empty()) {
        ASSERT(block->srcTile[0]->size > 0) << "src tile size cannot be 0";
    }
    switch (block->blockType) {
        case BlockType::BLK_TYPE_TMA:
            ExecuteTMA(block);
            break;
        case BlockType::BLK_TYPE_CUBE:
            ExecuteCUBE(block);
            break;
        case BlockType::BLK_TYPE_TEPL:
            ExecuteTEPL(block);
            break;
        default:
            ASSERT(false && "Unsupport block type template");
    }
}

uint64_t SoftCore::UpdateNextPC(MInstFuncPtr inst, BlockFuncPtr &currentBlock)
{
    // 需要特殊处理分离块。目前仅处理了进入分离块。
    if (inst->opcode == Opcode::OP_B_TEXT) {
        return currentBlock->bTextPC;
    }
    InstGroup grp = OpcodeManager::Inst().GetOpcodeGroup(inst->opcode);
    if (IsBlockEnd(currentBlock)) {
        switch (currentBlock->branchType) {
            case BranchType::BLK_BR_DIRECT:
            case BranchType::BLK_BR_CALL:
            case BranchType::BLK_BR_IND:
            case BranchType::BLK_BR_ICALL:
            case BranchType::BLK_BR_RET:
                ASSERT(currentBlock->barg.target != 0 && "Block BARG target");
                return currentBlock->barg.target;
            case BranchType::BLK_BR_COND: {
                if (currentBlock->barg.taken) {
                    return currentBlock->barg.target;
                }
            }
            default: {
                if (inst->skipCurrentMinst) {
                    return inst->pc;
                }
                if (currentBlock->bTextOffset != 0) {
                    return currentBlock->barg.localRetAddr;
                }
                break;
            }
        }
    } else if (currentBlock->simtFirst) {
        return currentBlock->bTextPC;
    } else if (grp == InstGroup::BRANCH && inst->brInfo->brTaken) {
        return inst->brInfo->targetPC;
    }
    return inst->pc + GetEncodeLenVal(inst->codeLen);
}

bool SoftCore::IsBlockEnd(BlockFuncPtr &currentBlock)
{
    return currentBlock->bIsComplete;
}

bool SoftCore::IsSimEnd(MInstFuncPtr inst)
{
    if (inst->opcode == Opcode::OP_ACRC) {
        return (inst->srcs[SRC0_IDX]->data == 1 && GetGPR(static_cast<uint64_t>(GPR::GPR_X1), inst->threadId) == 94);
    }
    return false;
}

void SoftCore::ReportStat()
{
    uint64_t totalBlockNum = 0;
    uint64_t totalInstNum = 0;
    LOG_ERROR << "";
    for (uint64_t i = 0; i < config.multiThreadNum; i++) {
        totalBlockNum += threadStatus[i].blockId;
        totalInstNum += threadStatus[i].minstId;
        LOG_ERROR << "Thread:" << std::dec << i << "Total Block number = " << threadStatus[i].blockId;
        LOG_ERROR << "Thread:" << std::dec << i << "Total Inst number = " << threadStatus[i].minstId;
    }
    LOG_ERROR << "\nTotal Block number = " << totalBlockNum;
    LOG_ERROR << "Total Inst number = " << totalInstNum;
}

void SoftCore::ResetPC(uint64_t newPc, uint64_t threadID)
{
    threadStatus[threadID].pc = newPc;
    threadStatus[threadID].blockId = 0;
    threadStatus[threadID].minstId = 0;
    threadStatus[threadID].simEnd = false;
    coreSimEnd = false;
}

uint64_t SoftCore::GetPC(uint64_t threadId)
{
    return threadStatus[threadId].pc;
}

void SoftCore::SetGPR(int idx, uint64_t value, uint64_t threadID)
{
    threadStatus[threadID].archStatus.gpr[idx] = value;
}

uint64_t SoftCore::GetGPR(int idx, uint64_t threadID)
{
    return threadStatus[threadID].archStatus.gpr[idx];
}

void SoftCore::SetSystemReg(uint64_t id, uint64_t value, uint64_t threadID)
{
    threadStatus[threadID].archStatus.sysreg[static_cast<SystemReg>(id)] = value;
}

uint64_t SoftCore::GetSystemReg(uint64_t id, uint64_t threadID)
{
    return threadStatus[threadID].archStatus.sysreg[static_cast<SystemReg>(id)];
}

FRMMode SoftCore::GetFRM(uint64_t threadID)
{
    uint64_t cState = threadStatus[threadID].archStatus.sysreg[SystemReg::SYS_CSTATE];
    uint64_t bits = GetBits(cState, FRM_BIT_END, FRM_BIT_BEGIN);
    if (bits >= static_cast<uint64_t>(FRMMode::FRM_RNA)) {
        return FRMMode::FRM_RNE;
    }
    return static_cast<FRMMode>(bits);
}

void SoftCore::SetBPC(uint64_t newBPC, uint64_t threadID)
{
    threadStatus[threadID].archStatus.bpc = newBPC;
}

uint64_t SoftCore::GetBPC(uint64_t threadID)
{
    return threadStatus[threadID].archStatus.bpc;
}

uint64_t SoftCore::GetTotalBlockNum()
{
    uint64_t totalBlockNum = 0;
    for (uint64_t i = 0; i < config.multiThreadNum; i++) {
        totalBlockNum += threadStatus[i].blockId;
    }
    return totalBlockNum;
}

void SoftCore::EnableTrace()
{
    verbose = true;
}

void SoftCore::CheckCkptRlt(const char* filename)
{
    std::ifstream is;
    is.open(filename, std::ios::binary);
    if (is.fail()) {
        assert(0 && "Input File cannot open when check checkpoint result!\n");
    }
    uint64_t asize;
    nlohmann::json in_trace = nlohmann::json::parse(is);
    for (auto root = in_trace.begin(); root != in_trace.end(); root++) {
        asize = (*root)["LastReg"].size();
        for (uint64_t i = 0; i < asize; i++) {
            if (i == 1) {
                continue;
            }
            uint64_t reg_value = (*root)["LastReg"][i];
            if (reg_value != threadStatus[0].archStatus.gpr[i]) {
                std::cout << std::hex << "bpc: " << threadStatus[0].pc << std::endl;
                printf("Register %ld comparison fail! check data: 0x%lx/%lu, register data: 0x%lx/%lu\n",
                    i, reg_value, reg_value, threadStatus[0].archStatus.gpr[i], threadStatus[0].archStatus.gpr[i]);
                assert(0);
            }
        }
        asize = (*root)["LastMemAcc"].size();
        for (uint64_t i = 0; i < asize; i++) {
            uint64_t addr = (*root)["LastMemAcc"][i]["addr"];
            if (!memory->LookupBank(addr)) {
                continue;
            }
            uint64_t data = (*root)["LastMemAcc"][i]["data"];
            uint64_t check_data = (uint64_t)memory->Load(addr, 8, false);
            if (data != check_data) {
                printf("Memory at 0x%lx Comparison fail! ckpt data: %lu register data: %lu\n", addr, data, check_data);
            }
        }
    }
    is.close();
}

SoftCore::~SoftCore()
{
    delete memory;
}

}
