#include "core/SRename.h"

#include <iostream>

#include "core/Core.h"

namespace JCore {

void StackRenameUnit::renameStack(StackRenameBus renameReq)
{
    if (!renameReq.vld)
        return;
    if (blockState[renameReq.bid.val].noNeedRename)
        return;
    if (blockState[renameReq.bid.val].preRename)
        return;
    // Recieve rename request from PE
    blockState[renameReq.bid.val].peID = renameReq.peID;
    if (renameReq.type != StackInstType::STACK_SET_WAIT && !blockState[renameReq.bid.val].newBlock && !renameReq.set_sp) {
        blockState[renameReq.bid.val].inputQ.Write(renameReq);
    }
    // Record block stack information
    BlockCommandPtr cmd = core->bctrl->blockROB.GetBlockCMDPtr(renameReq.bid, renameReq.stid);
    if (bStack.count(cmd->bpc) != 0 && !bStack[cmd->bpc].vld) { //&& !GetSim()->perfectLoadStore
        if (bStack[cmd->bpc].updating && bStack[cmd->bpc].bid == renameReq.bid) {
            if (renameReq.set_sp) {
                bStack[cmd->bpc].setSP = true;
            } else {
                bStack[cmd->bpc].stackReqQ.push_back(renameReq);
            }
            bStack[cmd->bpc].bid = renameReq.bid;
            if (renameReq.type == StackInstType::STACK_SET_WAIT) {
                bStack[cmd->bpc].waitCount++;
            } else if (renameReq.type == StackInstType::LAST) {
                bStack[cmd->bpc].decodeDone = true;
            }
            if (bStack[cmd->bpc].waitCount == 0 && bStack[cmd->bpc].decodeDone) {
                bStack[cmd->bpc].vld = true;
                bStack[cmd->bpc].updating = false;
            }
        }
    }
}

void StackRenameUnit::setHistoryTable(StackRenameBus req, uint64_t bpc) {
    bStack[bpc].stackReqQ.push_back(req);
}

void StackRenameUnit::setHistoryTableVld(uint64_t bpc) {
    StackRenameBus renameReq;
    renameReq.vld = true;
    renameReq.type = StackInstType::LAST;
    bStack[bpc].stackReqQ.push_back(renameReq);
    bStack[bpc].vld = true;
    bStack[bpc].decodeDone = true;
    bStack[bpc].updating = false;
}

void StackRenameUnit::setBlockRenameState(BlockCommandPtr &cmd)
{
    blockState[cmd->bid.val].Reset();
    blockState[cmd->bid.val].vld = true;
    blockState[cmd->bid.val].isHyper = false;
    blockState[cmd->bid.val].bid = cmd->bid;
    blockState[cmd->bid.val].bpc = cmd->bpc;
    blockState[cmd->bid.val].isTemplate = false;

    if (cmd->blockType == BlockType::BLK_TYPE_XB && cmd->branchType == BranchType::BLK_BR_CALL) {
        blockState[cmd->bid.val].bSetSP = true;
        blockState[cmd->bid.val].spOffset = false;
        return;
    }

    if (!historyEnable)
        return;
    if (bStack.count(cmd->bpc) == 0) {
        bStackState bt = bStackState();
        bt.Reset();
        bStack[cmd->bpc] = bt;
    }
    genTemplateStack(cmd);
    // The block has been excuted
    if (bStack[cmd->bpc].vld && !IsBlockTypeNeedVReg(cmd->blockType)) {
        if (cmd->opcode == Opcode::OP_FRET_RA || cmd->opcode == Opcode::OP_FRET_STK ||
            cmd->opcode == Opcode::OP_FENTRY || cmd->opcode == Opcode::OP_FEXIT)
            blockState[cmd->bid.val].isTemplate = true;
        deque<StackRenameBus> renameQ = bStack[cmd->bpc].stackReqQ;
        blockState[cmd->bid.val].end = true;
        blockState[cmd->bid.val].bSetSP = bStack[cmd->bpc].setSP;
        if (renameQ.size() <= 1) {
            blockState[cmd->bid.val].noNeedRename = !bStack[cmd->bpc].setSP;
            return;
        }
        blockState[cmd->bid.val].noNeedRename = false;
        blockState[cmd->bid.val].inputQ.Reset();
        if (false) {
            blockState[cmd->bid.val].newBlock = true;
            return;
        }
        while (!renameQ.empty()) {
            StackRenameBus renameReq = renameQ.front();
            renameReq.bid = cmd->bid;
            blockState[cmd->bid.val].inputQ.Write(renameReq);
            renameQ.pop_front();
        }
        blockState[cmd->bid.val].inputQ.Work();
        blockState[cmd->bid.val].preRename = true;
        LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: stack prerename block " << dec << cmd->bid;
    } else {
        if (!configs.rename_new_block) {
            blockState[cmd->bid.val].newBlock = true;
        }
        if (!bStack[cmd->bpc].updating) {
            bStack[cmd->bpc].updating = true;
            bStack[cmd->bpc].bid = cmd->bid;
        }
        blockState[cmd->bid.val].end = true;
    }
}

void StackRenameUnit::genTemplateStack(const BlockCommandPtr cmd) {
    if (bStack[cmd->bpc].vld)
        return;
    switch (cmd->opcode) {
        case Opcode::OP_FRET_RA:
        case Opcode::OP_FRET_STK:
        case Opcode::OP_FEXIT:
            genExitTemplate(cmd);
            break;
        case Opcode::OP_FENTRY:
            GenFEntryTemplate(cmd);
            break;
        default:
            break;
    }
    bStack[cmd->bpc].vld = cmd->bIsTemplate;
}

void StackRenameUnit::genExitTemplate(const BlockCommandPtr cmd) {
    StackRenameBus req = StackRenameBus();
    req.vld = true;
    int64_t imm = cmd->srcData[SRC0_IDX];
    // addi
    req.type = StackInstType::SET_SP;
    req.imm = -imm;
    bStack[cmd->bpc].stackReqQ.push_back(req);
    // load sp
    req.type = StackInstType::STACK_GET;
    int64_t reg_cnt = 0;
    for (uint32_t reg = 0; reg < static_cast<uint32_t>(GPR::GPR_COUNT); reg++) {
        if (reg == static_cast<uint32_t>(GPR::GPR_SP))
            continue;
        reg_cnt++;
        req.imm = ( - (reg_cnt * 8));
        bStack[cmd->bpc].stackReqQ.push_back(req);
    }
    // last flag
    req.type = StackInstType::LAST;
    bStack[cmd->bpc].stackReqQ.push_back(req);
    bStack[cmd->bpc].setSP = true;
    bStack[cmd->bpc].vld = true;
}

void StackRenameUnit::GenFEntryTemplate(const BlockCommandPtr cmd) {
    StackRenameBus req = StackRenameBus();
    req.vld = true;
    // subi
    req.type = StackInstType::SET_SP;
    req.imm = cmd->srcData[SRC0_IDX];
    bStack[cmd->bpc].stackReqQ.push_back(req);
    // store sp
    req.type = StackInstType::STACK_SET;
    int64_t reg_cnt = 0;
    uint32_t M = max((uint32_t)2, (uint32_t)cmd->srcData[SRC1_IDX]);
    uint32_t N = min((uint32_t)23, (uint32_t)cmd->srcData[SRC2_IDX]);
    if (M > N) N += 24;
    for (uint32_t i = M; i <= N; i++) {
        uint32_t reg = (i % static_cast<uint32_t>(GPR::GPR_COUNT));
        if (reg == static_cast<uint32_t>(GPR::GPR_ZERO) || reg == static_cast<uint32_t>(GPR::GPR_SP))
            continue;
        reg_cnt++;
        req.imm = (int64_t)(cmd->srcData[SRC0_IDX]) - (reg_cnt * 8);
        bStack[cmd->bpc].stackReqQ.push_back(req);
    }
    // last flag
    req.type = StackInstType::LAST;
    bStack[cmd->bpc].stackReqQ.push_back(req);
    bStack[cmd->bpc].setSP = true;
    bStack[cmd->bpc].vld = true;
}

void StackRenameUnit::setBlockRetire(ROBID bid)
{
    // Release for the whole block
    auto &setTable = blockState[bid.val].stackSetPRFTable;
    while (!setTable.empty()) {
        auto &stackSet = setTable.front();
        LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: rename release from table B" << dec << bid
            << ":T" << stackSet.rid << ":imm" << stackSet.imm;
        if (blockState[bid.val].bSetSP && (blockState[bid.val].totalSetCnt == blockState[bid.val].retireSetCnt)) {
            if (blockState[bid.val].spOffset) {
                cmapOffset(blockState[bid.val].imm);
            } else {
                resetCmap();
            }
            blockState[bid.val].bSetSP = false;
        }
        blockState[bid.val].retireSetCnt++;
        if (cmap.count(stackSet.imm) == 0) {
            cmap.emplace(stackSet.imm, stackSet.sptag);
        } else {
            releaseSptag(cmap[stackSet.imm]);
            cmap[stackSet.imm] = stackSet.sptag;
        }
        setTable.pop_front();
    }
    if (blockState[bid.val].bSetSP) {
        if (blockState[bid.val].spOffset) {
            cmapOffset(blockState[bid.val].imm);
        } else {
            resetCmap();
        }
    }
    blockState[bid.val].Reset();
}

bool StackRenameUnit::checkStall(StackRenameBus &renameReq)
{
    if (next.freeSize > 0) {
        return false;
    }
    if (renameReq.type == StackInstType::STACK_SET) {
        return true;
    }
    if (renameReq.type == StackInstType::STACK_GET) {
        if (smap.count(renameReq.imm) == 0) {
            return true;
        }
        if (!next.freeList[smap[renameReq.imm]]) {
            return true;
        }
        if (checkConfTable(renameReq.tpc)) {
            return true;
        }
    }
    return false;
}

bool StackRenameUnit::renameForReq(StackRenameBus &renameReq)
{
    StackRenameBus tempReq = renameReq;
    if (renameReq.type == StackInstType::SET_SP) {
        std::deque<StackRenameBus> readQ = blockState[renameReq.bid.val].inputQ.GetRawReadData();
        auto it = readQ.begin();
        it++;
        tempReq = *it;
    }
    if (checkStall(tempReq)) {
        LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: block rename stall : no free ptag";
        return true;
    }
    switch (renameReq.type) {
        case StackInstType::LAST:
            // Block rename complete, start next block
            blockState[renameReq.bid.val].noNeedRename = true;
            // When come across bset sp, initialize smap
            LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: block " << dec << renameReq.bid
                << " stack rename complete";
            if (blockState[renameReq.bid.val].preRename) {
                preRenameQ.push_back(renameReq.bid);
            }
            break;
        case StackInstType::STACK_GET:
            // Rename stack load
            if (!renameReq.calculated) {
                renameReq.imm += blockState[renameReq.bid.val].localImm;
            }
            renameStackLoad(renameReq);
            break;
        case StackInstType::STACK_SET:
            // Rename stack store
            if (!renameReq.calculated) {
                renameReq.imm += blockState[renameReq.bid.val].localImm;
            }
            renameStackStore(renameReq);
            break;
        case StackInstType::SET_SP:
            blockState[renameReq.bid.val].imm = renameReq.imm;
            blockState[renameReq.bid.val].spOffset = true;
            break;
        case StackInstType::LOCAL_SP:
            blockState[renameReq.bid.val].localImm += renameReq.imm;
            break;
        default:
            break;
    }
    blockState[renameReq.bid.val].inputQ.Pop();
    return false;
}

void StackRenameUnit::rptOcupiedPtag() {
    ++stats->cycles;
    uint64_t occupied_num = current.freeList.size() - current.freeSize;
    stats->occupied_ptag_toal += occupied_num;
    if (occupied_num < configs.sp_preg_count * 0.1) {
        stats->occupied_ptag_10++;
    } else if (occupied_num < configs.sp_preg_count * 0.25) {
        stats->occupied_ptag_25++;
    } else if (occupied_num < configs.sp_preg_count * 0.5) {
        stats->occupied_ptag_50++;
    } else if (occupied_num < configs.sp_preg_count * 0.75) {
        stats->occupied_ptag_75++;
    } else if (occupied_num < configs.sp_preg_count * 0.9) {
        stats->occupied_ptag_90++;
    } else {
        stats->occupied_ptag_100++;
    }
}

void StackRenameUnit::rptRenamedNum(uint64_t num) {
    stats->renamed_block += num;
    switch (num) {
        case 0:
            stats->renamed_block_0++;
            break;
        case 1:
            stats->renamed_block_1++;
            break;
        case 2:
            stats->renamed_block_2++;
            break;
        case 3:
            stats->renamed_block_3++;
            break;
        case 4:
            stats->renamed_block_4++;
            break;
        case 5:
            stats->renamed_block_5++;
            break;
        case 6:
            stats->renamed_block_6++;
            break;
        case 7:
            stats->renamed_block_7++;
            break;
        case 8:
            stats->renamed_block_8++;
            break;
        default:
            stats->renamed_block_more++;
            break;
    }
}

void StackRenameUnit::Work()
{
    rptOcupiedPtag();

    // mdb
    while (!stack_error_pc_q->Empty()) {
        uint64_t tpc = stack_error_pc_q->Read();
        if (tpc == 0)
            continue;
        if (pc_conf_table.count(tpc) == 0) {
            pc_conf_table[tpc] = configs.conflict_count;
        } else {
            pc_conf_table[tpc] += configs.conflict_count;
        }
    }

    ROBID renamePtr = current.renamePtr;
    uint32_t renameNum = 0;
    uint64_t renamedBlk = 0;
    while (renameNum < configs.stack_rename_block_num && StackRenameEnable()) {
        bool blockRenameComplete = false;
        bool stall = false;
        if (!blockState[renamePtr.val].vld || blockState[renamePtr.val].renamed)
            break;
        LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: rename ptr " << renamePtr;

        // Do stack rename
        blockState[renamePtr.val].inputQ.Work();
        bool isOffset = false;
        while (!blockState[renamePtr.val].inputQ.Empty()) {
            auto renameReq = blockState[renamePtr.val].inputQ.Front();
            stall = renameForReq(renameReq);
            if (!stall && renameReq.type == StackInstType::SET_SP) {
                if (blockState[next.renamePtr.val].spOffset) {
                    smapOffset(blockState[renamePtr.val].imm);
                } else {
                    setRenameStatus(true, 0);
                    LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: block  " << next.renamePtr
                        << " Reset smap " << blockState[next.renamePtr.val].totalSetCnt;
                }
                blockState[next.renamePtr.val].isCounting = false;
                isOffset = true;
            }
            if (!stall && blockState[next.renamePtr.val].isCounting) {
                blockState[next.renamePtr.val].totalSetCnt++;
            }
            if (renameReq.type == StackInstType::LAST) {
                // Block rename complete, start next block
                blockRenameComplete = true;
                renameNum++;
            } else if (stall) {
                stats->stall_count++;
                break;
            }
        }
        if (blockState[renamePtr.val].inputQ.Empty() && blockState[renamePtr.val].end) {
            blockRenameComplete = true;
        }
        if (blockRenameComplete && !isOffset && blockState[renamePtr.val].bSetSP &&
            !blockState[next.renamePtr.val].spOffset) {
            // 块内只有 一条 subi fp(s0) ->sp, 没有其他 (sdi  d, t#1)的情况, 但是inputQ里面有一条stop指令
            setRenameStatus(true, 0);
            LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: block  " << next.renamePtr << " Reset smap ";
        }
        ROBID oldestBID = core->bctrl->blockROB.getOldestBlockID(blockState[renamePtr.val].stid);
        if (stall && blockState[renamePtr.val].bid == oldestBID) {
            freeForOldestBlock(oldestBID);
            continue;
        }

        // Rename next block
        if (blockRenameComplete && !blockState[renamePtr.val].isHyper) {
            blockState[next.renamePtr.val].renamed = true;
            blockState[next.renamePtr.val].stackCheckPoint = smap;
            renamedBlk++;
            LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: block  " << next.renamePtr
                << " stack rename done ";
            IncROBID(next.renamePtr, core->configs.block_rob_depth);
        }
        for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
            if (!blockState[next.renamePtr.val].vld || blockState[next.renamePtr.val].renamed ||
                !blockState[next.renamePtr.val].end)
                break;
            if (!blockState[next.renamePtr.val].noNeedRename && !blockState[next.renamePtr.val].newBlock)
                break;
            if (blockState[next.renamePtr.val].bSetSP && !blockState[next.renamePtr.val].spOffset) {
                setRenameStatus(true, 0);
                LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: block  " << next.renamePtr
                    << " Reset smap ";
            }
            if (historyEnable && !blockState[next.renamePtr.val].noNeedRename) {
                BlockCommandPtr cmd = core->bctrl->blockROB.GetBlockCMDPtr(blockState[next.renamePtr.val].bid,
                    blockState[next.renamePtr.val].stid);
                if (cmd) {
                    for (auto store : bStack[cmd->bpc].stackReqQ) {
                        if (store.type != StackInstType::STACK_SET)
                            continue;
                        setRenameStatus(false, store.imm);
                    }
                }
            }
            LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: block  " << next.renamePtr
                << " stack rename skip ";
            blockState[next.renamePtr.val].renamed = true;
            blockState[next.renamePtr.val].stackCheckPoint = smap;
            blockRenameComplete = true;
            IncROBID(next.renamePtr, core->configs.block_rob_depth);
            renamedBlk++;
        }
        renamePtr = next.renamePtr;

        if (stall || !blockRenameComplete)
            break;
    }

    rptRenamedNum(renamedBlk);

    // stack release
    for (uint32_t i = 0; i < GetSim()->core->configs.scalar_smt_thread; ++i) {
        stackRetire(i);
    }
}

void StackRenameUnit::stackRetire(uint32_t stid) {
    // Release stack ptag and set PRFTable
    ROBID oldestPtr = core->bctrl->blockROB.getOldestBlockID(stid);
    auto &setTable = blockState[oldestPtr.val].stackSetPRFTable;
    bool needFlush = core->bctrl->blockROB.needFlush(oldestPtr, stid);
    while (!setTable.empty() && !needFlush) {
        auto &stackSet = setTable.front();
        if (!stackSet.retired) {
            break;
        }
        LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: rename release from table B" << dec << oldestPtr
            << ":T" << stackSet.rid << ":imm" << stackSet.imm;
        if (blockState[oldestPtr.val].bSetSP && (blockState[oldestPtr.val].totalSetCnt == blockState[oldestPtr.val].retireSetCnt)) {
            if (blockState[oldestPtr.val].spOffset) {
                cmapOffset(blockState[oldestPtr.val].imm);
            } else {
                resetCmap();
            }
            blockState[oldestPtr.val].bSetSP = false;
        }
        blockState[oldestPtr.val].retireSetCnt++;

        if (cmap.count(stackSet.imm) == 0) {
            cmap.emplace(stackSet.imm, stackSet.sptag);
        } else {
            releaseSptag(cmap[stackSet.imm]);
            cmap[stackSet.imm] = stackSet.sptag;
        }
        setTable.pop_front();
        if (blockState[oldestPtr.val].rptr > 0) {
            blockState[oldestPtr.val].rptr--;
        }
    }
    auto &getTable = blockState[oldestPtr.val].stackGetPRFTable;
    while (!getTable.empty() && !needFlush) {
        auto &stackGet = getTable.front();
        if (!stackGet.retired) {
            break;
        }
        getTable.pop_front();
        if (blockState[oldestPtr.val].rptrG > 0) {
            blockState[oldestPtr.val].rptrG--;
        }
    }
}

void StackRenameUnit::smapOffset(int64_t imm) {
    LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: smap move offset " << dec << imm;
    unordered_map<int64_t, uint32_t> new_smap;
    for (auto &tag : smap) {
        new_smap.emplace(tag.first + imm, tag.second);
    }
    smap = new_smap;
}

void StackRenameUnit::cmapOffset(int64_t imm) {
    LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: cmap move offset " << dec << imm;
    unordered_map<int64_t, uint32_t> new_cmap;
    for (auto &tag : cmap) {
        new_cmap.emplace(tag.first + imm, tag.second);
    }
    cmap = new_cmap;
}

void StackRenameUnit::freeForOldestBlock(ROBID bid) {
    smap.clear();
    unordered_map<uint32_t, bool> tag_map;
    auto &getTable = blockState[bid.val].stackGetPRFTable;
    for (auto &info : getTable) {
        if (!info.stack)
            continue;
        if (info.get) {
            tag_map.emplace(info.sptag, true);
            smap[info.imm] = info.sptag;
        }
    }

    // 因为smap可能进行了一次偏移，所以要camp和smap已经不对应了
    auto &setTable = blockState[bid.val].stackSetPRFTable;
    for (auto &info : setTable) {
        if (!info.stack)
            continue;
        smap[info.imm] = info.sptag;
    }
    for (auto it = cmap.begin(); it != cmap.end(); ) {
        if (tag_map.count(it->second) != 0) {
            it++;
            continue;
        }
        releaseSptag(it->second);
        cmap.erase(it++);
    }
}

void StackRenameUnit::resetCmap() {
    for (auto it = cmap.begin(); it != cmap.end(); it++) {
        releaseSptag(it->second);
        if (smap.count(it->first) == 0) {
            continue;
        }
        if (smap[it->first] == it->second) {
            smap.erase(it->first);
        }
    }
    cmap.clear();
}

bool StackRenameUnit::checkConfTable(uint64_t tpc) {
    if (tpc != 0 && pc_conf_table.count(tpc) != 0) {
        if (pc_conf_table[tpc] > 0)
            return true;
    }
    return false;
}

void StackRenameUnit::decConfTableCount(uint64_t tpc) {
    if (tpc != 0 && pc_conf_table.count(tpc) != 0) {
        if (pc_conf_table[tpc] > 0)
            pc_conf_table[tpc]--;
    }
}

bool StackRenameUnit::checkSmapVld(int64_t imm) {
    if (smap.count(imm) == 0) {
        return true;
    }
    if (next.freeList[smap[imm]]) {
        return true;
    }
    return false;
}

void StackRenameUnit::renameStackLoad(StackRenameBus &renameReq)
{
    if (!checkSmapVld(renameReq.imm) && !checkConfTable(renameReq.tpc)) {
        // stack get
        renameReq.sptag = smap[renameReq.imm];
        renameReq.ready = current.stackPtagReady[renameReq.sptag].ready;
        std::string renInfo;
        if (!blockState[renameReq.bid.val].preRename) {
            renInfo += " PE" + std::to_string(renameReq.peID);
        }
        renInfo += " Rename stack get B" + std::to_string(renameReq.bid.val);
        if (!blockState[renameReq.bid.val].preRename) {
            renInfo += ":T" + std::to_string(renameReq.rid.val);
        }
        renInfo += ":imm" + std::to_string(renameReq.imm) + ":SP" + std::to_string(renameReq.sptag);
        LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename]:" << renInfo;
    } else {
        // stack load, don't do stack rename
        renameReq.vld = false;
        renameReq.type = StackInstType::STACK_SET;
    }
    decConfTableCount(renameReq.tpc);

    if (blockState[renameReq.bid.val].preRename) {
        if (renameReq.type == StackInstType::STACK_GET) {
            insertSatckGetTable(renameReq);
            stats->stack_get++;
            BlockCommandPtr cmd = core->bctrl->blockROB.GetBlockCMDPtr(renameReq.bid, renameReq.stid);
            if (cmd->opcode == Opcode::OP_FEXIT || cmd->opcode == Opcode::OP_FRET_RA || cmd->opcode == Opcode::OP_FRET_STK) {
            }
        } else {
            renameReq.vld = true;
            renameReq.sptag = allocSptag(renameReq.imm);
            // For look up
            insertSatckGetTable(renameReq);
            // For flush and recover
            insertSatckSetTable(renameReq);
            stats->init_load_set++;
            stats->stack_set++;
            std::string renInfo;
            if (!blockState[renameReq.bid.val].preRename) {
                renInfo += " PE" + std::to_string(renameReq.peID);
            }
            renInfo += " Rename stack set B" + std::to_string(renameReq.bid.val);
            if (!blockState[renameReq.bid.val].preRename) {
                renInfo += ":T" + std::to_string(renameReq.rid.val);
            }
            renInfo += ":imm" + std::to_string(renameReq.imm) + ":SP" + std::to_string(renameReq.sptag);
            LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename]:" << renInfo;
        }
    }
}

void StackRenameUnit::insertSatckGetTable(StackRenameBus &spRegReq)
{
    StackPRFTable tableInfo = StackPRFTable();
    tableInfo.vld = true;
    tableInfo.stack = spRegReq.vld;
    if (spRegReq.type == StackInstType::STACK_GET || !tableInfo.stack) {
        tableInfo.get = true;
    } else {
        tableInfo.get = false;
    }
    tableInfo.lsID = spRegReq.lsID;
    tableInfo.sptag = spRegReq.sptag;
    tableInfo.imm = spRegReq.imm;
    tableInfo.tpc = spRegReq.tpc;
    tableInfo.stackCheckPoint = smap;
    blockState[spRegReq.bid.val].stackGetPRFTable.push_back(tableInfo);
}

void StackRenameUnit::renameStackStore(StackRenameBus &renameReq)
{
    renameReq.sptag = allocSptag(renameReq.imm);
    insertSatckSetTable(renameReq);
    stats->stack_set++;
    std::string renInfo;
    if (!blockState[renameReq.bid.val].preRename) {
        renInfo += " PE" + std::to_string(renameReq.peID);
    }
    renInfo += " Rename stack set B" + std::to_string(renameReq.bid.val);
    if (!blockState[renameReq.bid.val].preRename) {
        renInfo += ":T" + std::to_string(renameReq.rid.val);
    }
    renInfo += ":imm" + std::to_string(renameReq.imm) + ":SP" + std::to_string(renameReq.sptag);
    LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename]:" << renInfo;
}

void StackRenameUnit::insertSatckSetTable(StackRenameBus &spRegReq)
{
    StackPRFTable tableInfo = StackPRFTable();
    tableInfo.vld = true;
    tableInfo.stack = true;
    tableInfo.lsID = spRegReq.lsID;
    tableInfo.sptag = spRegReq.sptag;
    tableInfo.imm = spRegReq.imm;
    tableInfo.tpc = spRegReq.tpc;
    tableInfo.stackCheckPoint = smap;
    blockState[spRegReq.bid.val].stackSetPRFTable.push_back(tableInfo);
}

uint32_t StackRenameUnit::allocSptag(int64_t imm)
{
    for (uint32_t i = 0; i < next.freeList.size(); i++) {
        if (!next.freeList[i])
            continue;
        if (smap.count(imm) == 0) {
            smap.emplace(imm, i);
        } else {
            smap[imm] = i;
        }
        ASSERT(next.freeSize > 0);
        next.freeSize--;
        next.stackPtagReady[i].ready = false;
        next.freeList[i] = false;
        return i;
    }
    ASSERT(0 && "Could not find free stack ptag");
    return 0;
}

void StackRenameUnit::releaseSptag(uint32_t sptag)
{
    next.freeList[sptag] = true;
    next.freeSize++;
    next.stackPtagReady[sptag].ready = false;
    LOG_INFO_M(Unit::BCC, Stage::D2) << "[Stack Rename   Stage]: relaese SPtag " << dec << sptag;
}

void StackRenameUnit::setSpPtagReady(uint32_t sptag, bool ready, PLpvInfo &lpvInfo)
{
    next.stackPtagReady[sptag].ready = ready;
    SetLpv(lpvInfo, next.stackPtagReady[sptag].lpvInfo);
}

void StackRenameUnit::stackRetire(ROBID bid, ROBID rid, ROBID lsID)
{
    for (auto &stackSet : blockState[bid.val].stackSetPRFTable) {
        if (!stackSet.lsid_vld) break;
        if (stackSet.lsid_vld && !stackSet.retired) {
            if (stackSet.lsID == lsID) {
                stackSet.retired = true;
                break;
            }
            if (LessEqual(lsID, stackSet.lsID)) {
                break;
            }
        }
    }
    for (auto&stackGet : blockState[bid.val].stackGetPRFTable) {
        if (!stackGet.lsid_vld) break;
        if (stackGet.lsid_vld && !stackGet.retired) {
            if (stackGet.lsID == lsID) {
                stackGet.retired = true;
                break;
            }
            if (LessEqual(lsID, stackGet.lsID)) {
                break;
            }
        }
    }
}

void StackRenameUnit::Xfer()
{
    for (auto &tag : next.stackPtagReady) {
        if (tag.lpvInfo) {
            tag.lpvInfo->Move();
        }
    }
    current = next;
    blockState[current.renamePtr.val].inputQ.Work();
    stack_error_pc_q->Work();
}

void StackRenameUnit::Build()
{
    configs.overrideDefaultConfig(GetSim()->getCfgs());
    BlockStackState bstate = BlockStackState();
    // Build block rename state
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        blockState.emplace_back(bstate);
    }
    // Build stack physical register
    StackRenameTableInfo tInfo;
    for (uint32_t i = 0; i < configs.sp_preg_count; i++) {
        current.freeList.push_back(true);
        current.stackPtagReady.emplace_back(tInfo);
        next.freeList.push_back(true);
        next.stackPtagReady.emplace_back(tInfo);
    }
    historyEnable = configs.stack_history_enable;
    Reset();
    stack_error_pc_q = new SimQueue<uint64_t>();
    stats = new StackStats(GetSim()->getRpt());
    stats->preg_count = configs.sp_preg_count;
}

void BlockStackState::Reset()
{
    vld = false;
    noNeedRename = false;
    newBlock = false;
    preRename = false;
    isHyper = false;
    bSetSP = false;
    spOffset = false;
    renamed = false;
    inputQ.Reset();
    rptr = 0;
    rptrG = 0;
    peID = 0;
    imm = 0;
    localImm = 0;
    isTemplate = false;
    stackSetPRFTable.clear();
    stackGetPRFTable.clear();
    retireSetCnt = 0;
    totalSetCnt = 0;
    isCounting = true;
}

void StackRenameUnit::Reset()
{
    ROBID id;
    id.val = 0;
    id.wrap = false;
    for (uint32_t i = 0; i < blockState.size(); i++) {
        blockState[i].Reset();
    }

    for (uint32_t i = 0; i < configs.sp_preg_count; i++) {
        current.freeList[i] = true;
        current.stackPtagReady[i].ready = false;
        next.freeList[i] = true;
        next.stackPtagReady[i].ready = false;
    }

    current.freeSize = current.freeList.size();
    next.freeSize = next.freeList.size();
    current.renamePtr = id;
    next.renamePtr = id;

    for (auto &map : bStack) {
        map.second.vld = false;
        map.second.updating = false;
        map.second.decodeDone = false;
        map.second.waitCount = 0;
    }
}

void StackRenameUnit::replay(FlushBus flushReq)
{
    if (!flushReq.req.vld)
        return;
    if (!flushReq.baseOnBid) {
        replayByRid(flushReq);
    } else {
        replayByBid(flushReq);
    }
}

void StackRenameUnit::replayByBid(FlushBus flushReq) {
    ROBID replayPtr = flushReq.req.bid;
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        if (!blockState[replayPtr.val].vld) {
            break;
        }

        if (!LessEqual(flushReq.req.bid, blockState[replayPtr.val].bid))
            break;

        BlockStatus status = core->bctrl->blockROB.getBlockStatus(blockState[replayPtr.val].bid,
            blockState[replayPtr.val].stid);
        if (status == BlockStatus::FREE || status == BlockStatus::FLUSHED) {
            break;
        }

        if (flushReq.baseOnPE && blockState[replayPtr.val].peID != flushReq.req.peID) {
            IncROBID(replayPtr, core->configs.block_rob_depth);
            continue;
        }

        // Replay for stack set table
        for (auto &tableInfo : blockState[replayPtr.val].stackSetPRFTable) {
            next.stackPtagReady[tableInfo.sptag].ready = false;
            tableInfo.retired = false;
            tableInfo.lsid_vld = false;
        }
        blockState[replayPtr.val].rptr = 0;

        // Replay for  stack get table
        for (auto &tableInfo : blockState[replayPtr.val].stackGetPRFTable) {
            tableInfo.retired = false;
            tableInfo.lsid_vld = false;
        }
        blockState[replayPtr.val].rptrG = 0;

        // Recover block stack history table
        if (!blockState[replayPtr.val].preRename) {
            if (!bStack[blockState[replayPtr.val].bpc].vld && bStack[blockState[replayPtr.val].bpc].updating &&
                bStack[blockState[replayPtr.val].bpc].bid == blockState[replayPtr.val].bid) {
                bStack[blockState[replayPtr.val].bpc].stackReqQ.clear();
                bStack[blockState[replayPtr.val].bpc].waitCount = 0;
                bStack[blockState[replayPtr.val].bpc].decodeDone = false;
            }
        }

        IncROBID(replayPtr, core->configs.block_rob_depth);
    }
}

void StackRenameUnit::replayByRid(FlushBus flushReq) {
    ROBID replayPtr = flushReq.req.bid;
    ROBID oldestBID = core->bctrl->blockROB.getOldestBlockID(flushReq.req.stid);
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        if (!blockState[replayPtr.val].vld) {
            break;
        }

        if (!LessEqual(flushReq.req.bid, blockState[replayPtr.val].bid))
            break;

        BlockStatus status = core->bctrl->blockROB.getBlockStatus(blockState[replayPtr.val].bid,
            blockState[replayPtr.val].stid);
        if (status == BlockStatus::FREE || status == BlockStatus::FLUSHED) {
            break;
        }

        if (flushReq.baseOnPE && blockState[replayPtr.val].peID != flushReq.req.peID) {
            IncROBID(replayPtr, core->configs.block_rob_depth);
            continue;
        }

        // Replay for stack set table
        auto &setTable = blockState[replayPtr.val].stackSetPRFTable;
        uint32_t count = 0;
        for (auto it = setTable.begin(); it != setTable.end(); it++) {
            if ((it->lsid_vld &&
                !LessEqual(flushReq.req.bid, flushReq.req.lsID, blockState[replayPtr.val].bid, it->lsID)) ||
                (it->retired && oldestBID == blockState[replayPtr.val].bid)) {
                count++;
                continue;
            }
            it->lsid_vld = false;
            it->retired = false;
            next.stackPtagReady[it->sptag].ready = false;
        }
        if (blockState[replayPtr.val].rptr > count) {
            blockState[replayPtr.val].rptr = count;
        }

        // Replay for  stack get table
        auto &getTable = blockState[replayPtr.val].stackGetPRFTable;
        count = 0;
        for (auto it = getTable.begin(); it != getTable.end(); it++) {
            if ((it->lsid_vld &&
                !LessEqual(flushReq.req.bid, flushReq.req.lsID, blockState[replayPtr.val].bid, it->lsID)) ||
                (it->retired && oldestBID == blockState[replayPtr.val].bid)) {
                count++;
                continue;
            }
            it->lsid_vld = false;
            it->retired = false;
        }
        if (blockState[replayPtr.val].rptrG > count) {
            blockState[replayPtr.val].rptrG = count;
        }

        // Recover block stack history table
        if (!blockState[replayPtr.val].preRename) {
            if (!bStack[blockState[replayPtr.val].bpc].vld && bStack[blockState[replayPtr.val].bpc].updating &&
                bStack[blockState[replayPtr.val].bpc].bid == blockState[replayPtr.val].bid) {
                while (!bStack[blockState[replayPtr.val].bpc].stackReqQ.empty() &&
                       LessEqual(flushReq.req.bid, flushReq.req.lsID, blockState[replayPtr.val].bid,
                                 bStack[blockState[replayPtr.val].bpc].stackReqQ.back().lsID)) {
                    if (bStack[blockState[replayPtr.val].bpc].stackReqQ.back().type == StackInstType::STACK_SET_WAIT) {
                        bStack[blockState[replayPtr.val].bpc].waitCount--;
                    }
                    bStack[blockState[replayPtr.val].bpc].stackReqQ.pop_back();
                }
                bStack[blockState[replayPtr.val].bpc].decodeDone = false;
            }
        }

        IncROBID(replayPtr, core->configs.block_rob_depth);
    }
}

void StackRenameUnit::flush(FlushBus flushReq)
{
    if (!flushReq.req.vld)
        return;

    if (!flushReq.baseOnBid) {
        flushByRid(flushReq);
    } else {
        flushByBid(flushReq);
    }

    // Reset rename ptr
    next.renamePtr = flushReq.req.bid;
    ROBID ptr = next.renamePtr;
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        SubROBID(ptr, 1, core->configs.block_rob_depth);
        if ((blockState[ptr.val].vld && blockState[ptr.val].renamed) || !blockState[ptr.val].vld)
            break;
        next.renamePtr = ptr;
    }
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        if (!blockState[next.renamePtr.val].vld || !blockState[next.renamePtr.val].renamed)
            break;
        IncROBID(next.renamePtr, core->configs.block_rob_depth);
    }

    // disable cmap sptag commit
    for (auto it = cmap.begin(); it != cmap.end(); ) {
        if (next.freeList[it->second]) {
            next.stackPtagReady[it->second].ready = false;
            cmap.erase(it++);
        } else {
            it++;
        }
    }

    // Reset smap
    for (auto it = smap.begin(); it != smap.end(); ) {
        if (next.freeList[it->second]) {
            smap.erase(it++);
        } else {
            it++;
        }
    }

    // Flush pre renamed queue
    while (!preRenameQ.empty() && LessEqual(flushReq.req.bid, preRenameQ.back())) {
        preRenameQ.pop_back();
    }
}

void StackRenameUnit::flushByRid(FlushBus flushReq)
{
    // flush rename table
    ROBID flushPtr = flushReq.req.bid;
    ROBID oldestBID = core->bctrl->blockROB.getOldestBlockID(flushReq.req.stid);
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        if (!blockState[flushPtr.val].vld)
            break;

        if (!LessEqual(flushReq.req.bid, blockState[flushPtr.val].bid))
            break;

        // Flush stack set table
        auto &setTable = blockState[flushPtr.val].stackSetPRFTable;
        if (flushReq.req.bid != blockState[flushPtr.val].bid || !blockState[flushPtr.val].preRename) {
            while (!setTable.empty()) {
                auto &info = setTable.back();
                releaseSptag(info.sptag);
                setTable.pop_back();
            }
        } else {
            uint32_t count = 0;
            for (auto it = setTable.begin(); it != setTable.end(); it++) {
                if ((it->lsid_vld && !LessEqual(flushReq.req.lsID, it->lsID)) ||
                    (it->retired && oldestBID == blockState[flushPtr.val].bid)) {
                    count++;
                    continue;
                }
                it->lsid_vld = false;
                it->retired = false;
                next.stackPtagReady[it->sptag].ready = false;
            }
            if (blockState[flushPtr.val].rptr > count) {
                blockState[flushPtr.val].rptr = count;
            }
        }
        if (blockState[flushPtr.val].rptr > setTable.size()) {
            blockState[flushPtr.val].rptr = setTable.size();
        }

        // Flush stack get table
        auto &getTable = blockState[flushPtr.val].stackGetPRFTable;
        if (flushReq.req.bid != blockState[flushPtr.val].bid || !blockState[flushPtr.val].preRename) {
            // Younger block or equal block rename in order, get table rename Reset
            getTable.clear();
        } else {
            uint32_t count = 0;
            for (auto it = getTable.begin(); it != getTable.end(); it++) {
                if ((it->lsid_vld && !LessEqual(flushReq.req.lsID, it->lsID)) ||
                    (it->retired && oldestBID == blockState[flushPtr.val].bid)) {
                    count++;
                    continue;
                }
                if (flushReq.req.lsID == it->lsID && it->lsid_vld && it->get) {
                    it->stack = false;
                }
                it->lsid_vld = false;
                it->retired = false;
            }
            if (blockState[flushPtr.val].rptrG > count) {
                blockState[flushPtr.val].rptrG = count;
            }
        }
        if (blockState[flushPtr.val].rptrG > getTable.size()) {
            blockState[flushPtr.val].rptrG = getTable.size();
        }

        // Recover block stack history table
        if (!blockState[flushPtr.val].preRename) {
            if (!bStack[blockState[flushPtr.val].bpc].vld && bStack[blockState[flushPtr.val].bpc].updating &&
                bStack[blockState[flushPtr.val].bpc].bid == blockState[flushPtr.val].bid) {
                while (!bStack[blockState[flushPtr.val].bpc].stackReqQ.empty() &&
                    LessEqual(flushReq.req.bid, flushReq.req.lsID, blockState[flushPtr.val].bid,
                        bStack[blockState[flushPtr.val].bpc].stackReqQ.back().lsID)) {
                    if (bStack[blockState[flushPtr.val].bpc].stackReqQ.back().type == StackInstType::STACK_SET_WAIT) {
                        bStack[blockState[flushPtr.val].bpc].waitCount--;
                    }
                    bStack[blockState[flushPtr.val].bpc].stackReqQ.pop_back();
                }
                bStack[blockState[flushPtr.val].bpc].decodeDone = false;
            }
        }

        if (flushReq.req.bid != blockState[flushPtr.val].bid) {
            if (!bStack[blockState[flushPtr.val].bpc].vld && bStack[blockState[flushPtr.val].bpc].updating &&
                bStack[blockState[flushPtr.val].bpc].bid == blockState[flushPtr.val].bid) {
                bStack[blockState[flushPtr.val].bpc].vld = false;
                bStack[blockState[flushPtr.val].bpc].updating = false;
            }
            blockState[flushPtr.val].Reset();
        }

        IncROBID(flushPtr, core->configs.block_rob_depth);
    }

    // recover smap
    ROBID recoverPtr = flushReq.req.bid;
    bool fromCheckPoint = false;
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        if (!blockState[recoverPtr.val].vld || (LessEqual(flushReq.req.bid, blockState[recoverPtr.val].bid) &&
            blockState[recoverPtr.val].bid != flushReq.req.bid)) {
            break;
        }
        if (blockState[recoverPtr.val].renamed) {
            smap = blockState[recoverPtr.val].stackCheckPoint;
            fromCheckPoint = true;
            break;
        }
        if (!blockState[recoverPtr.val].stackSetPRFTable.empty()) {
            uint32_t ptr = blockState[recoverPtr.val].stackSetPRFTable.size() - 1;
            smap = blockState[recoverPtr.val].stackSetPRFTable[ptr].stackCheckPoint;
            fromCheckPoint = true;
            break;
        }
        SubROBID(recoverPtr, 1, core->configs.block_rob_depth);
    }
    if (!fromCheckPoint) {
        smap = cmap;
    }
    for (auto &info : blockState[flushReq.req.bid.val].stackGetPRFTable) {
        int64_t imm = info.imm;
        if (blockState[flushReq.req.bid.val].spOffset) {
            imm += blockState[flushReq.req.bid.val].imm;
        }
        if (flushReq.req.lsID == info.lsID && info.lsid_vld && smap.count(imm) != 0) {
            smap.erase(imm);
        }
    }
}

void StackRenameUnit::flushByBid(FlushBus flushReq)
{
    ROBID flushPtr = flushReq.req.bid;
    // flush rename table
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        if (!blockState[flushPtr.val].vld)
            break;

        if (!LessEqual(flushReq.req.bid, blockState[flushPtr.val].bid))
            break;

        bool replay = (flushReq.req.fetchTPCVld && flushReq.req.bid == blockState[flushPtr.val].bid);

        // Flush stack set table
        auto &setTable = blockState[flushPtr.val].stackSetPRFTable;
        for (auto it = setTable.begin(); it != setTable.end(); it++) {
            if (replay) {
                it->retired = false;
                it->lsid_vld = false;
                next.stackPtagReady[it->sptag].ready = false;
            } else {
                releaseSptag(it->sptag);
            }
        }
        if (!replay) {
            setTable.clear();
        }
        blockState[flushPtr.val].rptr = 0;

        // Flush stack get table
        if (!replay) {
            blockState[flushPtr.val].stackGetPRFTable.clear();
        } else {
            for (auto &info : blockState[flushPtr.val].stackGetPRFTable) {
                info.lsid_vld = false;
                info.retired = false;
            }
        }
        blockState[flushPtr.val].rptrG = 0;

        // Recover block stack history table
        if (!blockState[flushPtr.val].preRename) {
            if (!bStack[blockState[flushPtr.val].bpc].vld && bStack[blockState[flushPtr.val].bpc].updating &&
                bStack[blockState[flushPtr.val].bpc].bid == blockState[flushPtr.val].bid) {
                bStack[blockState[flushPtr.val].bpc].stackReqQ.clear();
                bStack[blockState[flushPtr.val].bpc].vld = false;
                bStack[blockState[flushPtr.val].bpc].updating = false;
                bStack[blockState[flushPtr.val].bpc].decodeDone = false;
                bStack[blockState[flushPtr.val].bpc].waitCount = 0;
            }
        }

        if (!replay) {
            blockState[flushPtr.val].Reset();
        }

        IncROBID(flushPtr, core->configs.block_rob_depth);
    }

    // recover smap
    ROBID recoverPtr = flushReq.req.bid;
    if (!flushReq.req.fetchTPCVld) {
        SubROBID(recoverPtr, 1, core->configs.block_rob_depth);
    }
    bool fromCheckPoint = false;
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        if (!blockState[recoverPtr.val].vld) {
            break;
        }
        if ((flushReq.req.fetchTPCVld && LessROBID(flushReq.req.bid, blockState[recoverPtr.val].bid)) ||
            (!flushReq.req.fetchTPCVld && LessEqual(flushReq.req.bid, blockState[recoverPtr.val].bid))) {
            break;
        }
        if (blockState[recoverPtr.val].renamed) {
            smap = blockState[recoverPtr.val].stackCheckPoint;
            fromCheckPoint = true;
            break;
        }
        if (!blockState[recoverPtr.val].stackSetPRFTable.empty()) {
            uint32_t ptr = blockState[recoverPtr.val].stackSetPRFTable.size() - 1;
            smap = blockState[recoverPtr.val].stackSetPRFTable[ptr].stackCheckPoint;
            fromCheckPoint = true;
            break;
        }
        SubROBID(recoverPtr, 1, core->configs.block_rob_depth);
    }
    if (!fromCheckPoint) {
        smap = cmap;
    }
}

void replayForSingleBlk(ROBID bid) {

}

bool StackRenameUnit::getRenameStatus(int64_t imm)
{
    return (smap.count(imm) != 0);
}

void StackRenameUnit::setRenameStatus(bool all, int64_t imm)
{
    if (all) {
        smap.clear();
    } else if (smap.count(imm) != 0) {
        smap.erase(imm);
    }
}

void StackRenameUnit::setBlockHyper(ROBID bid)
{
    blockState[bid.val].isHyper = true;
}

bool StackRenameUnit::getBlockStatus(ROBID bid)
{
    return blockState[bid.val].preRename;
}

bool StackRenameUnit::isStackInst(ROBID bid, uint32_t tpc, bool load) {
    if (!blockState[bid.val].preRename)
        return false;
    if (blockState[bid.val].isTemplate)
        return true;
    if (load) {
        uint32_t &ptr = blockState[bid.val].rptrG;
        if (ptr < blockState[bid.val].stackGetPRFTable.size() &&
            blockState[bid.val].stackGetPRFTable[ptr].tpc == tpc) {
            return true;
        }
    } else {
        uint32_t &ptr = blockState[bid.val].rptr;
        if (ptr < blockState[bid.val].stackSetPRFTable.size() &&
            blockState[bid.val].stackSetPRFTable[ptr].tpc == tpc) {
            return true;
        }
    }
    auto &renameReqQ = blockState[bid.val].inputQ;
    auto &readQ = renameReqQ.GetRawReadData();
    for (auto &req : readQ) {
        if (req.tpc == tpc) {
            return true;
        }
    }
    auto &writeQ = renameReqQ.GetRawWriteData();
    for (auto &req : writeQ) {
        if (req.tpc == tpc) {
            return true;
        }
    }
    return false;
}

StackRenameBus StackRenameUnit::lookupStackPRFTable(StackRenameBus &renameReq)
{
    StackRenameBus renameRet = renameReq;
    blockState[renameReq.bid.val].peID = renameReq.peID;
    // cout<<renameReq<<endl;
    if (renameReq.type == StackInstType::STACK_GET) {
        // Stack load lookup
        uint32_t ptr = blockState[renameReq.bid.val].rptrG;
        // cout<<"check get size "<<dec<<blockState[renameReq.bid.val].stackGetPRFTable.size()<<" ptr:"<<ptr<<" B"<<renameReq.bid.val<<endl;
        if (ptr >= blockState[renameReq.bid.val].stackGetPRFTable.size()) {
            renameRet.stall = true;
            return renameRet;
        }
        blockState[renameReq.bid.val].rptrG++;
        StackPRFTable &stackGet = blockState[renameReq.bid.val].stackGetPRFTable[ptr];
        stackGet.lsID = renameRet.lsID;
        stackGet.lsid_vld = true;
        if (!stackGet.stack) {
            renameRet.vld = false;
            return renameRet;
        }
        renameRet.sptag = stackGet.sptag;
        if (!stackGet.get) {
            renameRet.type = StackInstType::STACK_SET;
            uint32_t ptrS = blockState[renameReq.bid.val].rptr;
            // cout<<"check load set size "<<dec<<blockState[renameReq.bid.val].stackSetPRFTable.size()<<" ptr"<<ptrS<<" B"<<renameReq.bid.val<<endl;
            if (ptrS >= blockState[renameReq.bid.val].stackSetPRFTable.size()) {
                renameRet.stall = true;
                return renameRet;
            }
            blockState[renameReq.bid.val].stackSetPRFTable[ptrS].lsid_vld = true;
            blockState[renameReq.bid.val].stackSetPRFTable[ptrS].lsID = renameRet.lsID;
            blockState[renameReq.bid.val].rptr++;
        }
    } else {
        // Stack store lookup
        uint32_t ptr = blockState[renameReq.bid.val].rptr;
        // cout<<"check set size "<<dec<<blockState[renameReq.bid.val].stackSetPRFTable.size()<<" ptr"<<ptr<<" B"<<renameReq.bid.val<<endl;
        if (ptr >= blockState[renameReq.bid.val].stackSetPRFTable.size()) {
            renameRet.stall = true;
            return renameRet;
        }
        blockState[renameReq.bid.val].rptr++;
        StackPRFTable &stackSet = blockState[renameReq.bid.val].stackSetPRFTable[ptr];
        stackSet.lsID = renameRet.lsID;
        stackSet.lsid_vld = true;
        renameRet.sptag = stackSet.sptag;
    }
    return renameRet;
}

bool StackRenameUnit::getPreRenamedStatus()
{
    bool renamed = false;
    if (!preRenameQ.empty()) {
        renamed = true;
    }
    return renamed;
}

void StackRenameUnit::popPreRenamedBID()
{
    if (preRenameQ.empty())
        return;
    preRenameQ.pop_front();
}

void StackRenameUnit::renewLongOffset(ROBID bid, ROBID lsID, int64_t imm)
{
    BlockCommandPtr cmd = core->bctrl->blockROB.GetBlockCMDPtr(bid, 0);
    for (auto it = bStack[cmd->bpc].stackReqQ.begin(); it != bStack[cmd->bpc].stackReqQ.end(); it++) {
        if (bid == it->bid && lsID == it->lsID && it->type == StackInstType::STACK_SET_WAIT) {
            it->type = StackInstType::STACK_SET;
            it->imm = imm;
            if (bStack[cmd->bpc].bid == bid && bStack[cmd->bpc].updating && !bStack[cmd->bpc].vld) {
                bStack[cmd->bpc].waitCount--;
                if (bStack[cmd->bpc].waitCount == 0 && bStack[cmd->bpc].decodeDone) {
                    bStack[cmd->bpc].vld = true;
                    bStack[cmd->bpc].updating = false;
                }
            }
            break;
        }
    }
}

bool StackRenameUnit::checkSpPtagReady(uint32_t sptag) {
    return current.stackPtagReady[sptag].ready;
}

PLpvInfo StackRenameUnit::getSpPtagLpvInfo(uint32_t sptag) {
    return current.stackPtagReady[sptag].lpvInfo;
}

bool StackRenameUnit::checkBlockRecorded(uint64_t bpc) {
    return bStack[bpc].vld;
}

bool StackRenameUnit::CheckRename(ROBID bid) {
    return (!blockState[bid.val].vld || blockState[bid.val].renamed);
}

void StackRenameUnit::setSPStat(ROBID bid) {
    blockState[bid.val].noNeedRename = false;
    blockState[bid.val].bSetSP = true;
}

void StackRenameUnit::setBend(ROBID bid) {
    blockState[bid.val].end = true;
}

StackRenameUnit::~StackRenameUnit()
{
    DeletePtr(stats);
    DeletePtr(stack_error_pc_q);
}

bool StackRenameUnit::StackRenameEnable(uint32_t peid)
{
    (void)peid;
    return GetSim()->core->bctrl->configs.stack_rename_mode != 0;
}

} // namespace JCore
