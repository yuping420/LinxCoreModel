#include "vectorcore/GroupBuffer.h"

#include "core/Core.h"
#include "vectorcore/LastMaskFile.h"

namespace JCore {

void GroupBuffer::Work()
{
    Dispatch();
    Alloc();
    Stats();
}

void GroupBuffer::Build(uint32_t thdNum, uint32_t depth, uint32_t fetchWidth)
{
    m_thdN = thdNum;
    for (uint32_t tid = 0; tid < m_thdN; ++tid) {
        GBufferState gstate(tid, depth, tid * depth);
        gstate.Build();
        gstate.SetSim(m_sim);
        m_buffers.emplace_back(gstate);
    }

    m_stall = false;
    m_fetchWidth = fetchWidth;
}

void GroupBuffer::Reset()
{
    for (uint8_t i = 0; i < m_thdN; i++) {
        m_buffers[i].Reset();
    }

    m_stall = false;
}

void GroupBuffer::Xfer() {}

void GroupBuffer::Alloc()
{
    if (!curReq.vld && m_lm2GBufferQ->Empty()) {
        return;
    }
    VCore::GBufferAllocReq req;
    if (curReq.vld) {
        req = curReq;
    } else {
        req = m_lm2GBufferQ->Read();
        if (req.blockCmd->biqType == BIQType::VEC_IQ) {
            m_maskFile->Alloc(req.blockCmd->bid, req.shapelpinfo.lastGroupNum, req.blockCmd->stid);
        }
        leftGroups = req.shapelpinfo.groupNum;
    }
    GBufferEntry entry;
    entry.vld = true;
    entry.bid = req.blockCmd->bid;
    entry.tpc = req.blockCmd->bTextPC;
    entry.blockCmd = req.blockCmd;

    if (entry.blockCmd->biqType == BIQType::MCALL_IQ) {
        entry.offset = req.blockCmd->gid.val;
        entry.logicalGID = req.blockCmd->gid.val;
        entry.shapelpinfo = req.shapelpinfo;
        entry.totalCnt = 1;
        entry.tid = m_thdWPtr;
        if (!m_buffers[m_thdWPtr].Write(entry)) {
            curReq = req;
            curReq.vld = 1;
        }
        m_thdWPtr = (m_thdWPtr + 1) % m_thdN;
        return;
    }

    entry.logicalGID = req.logicalGID;
    entry.shapelpinfo = req.shapelpinfo;
    uint32_t avgGroupNum = entry.shapelpinfo.avgGroupNum;
    bool groupOver = false;
    bool ruled = true;
    if (req.shapelpinfo.groupNum > m_thdN && req.shapelpinfo.groupNum % m_thdN != 0) {
        ruled = false;
    }

    for (uint32_t i = 0; i < m_thdN && i < entry.shapelpinfo.groupNum; ++i) {
        entry.offset = req.dispBufferCnt;
        entry.totalCnt = avgGroupNum;
        entry.tid = m_thdWPtr;
        // if the total groups are 2/3: ruled is true, the 2nd/3rd thread has the group.
        // if the total groups are 8: ruled is true, the 4th group has the group.
        // if the total groups are 5: ruled is false, the 1st group has the group.
        // if the total groups are 6: ruled is false, the 2nd group has the group.
        if (ruled) {
            if (avgGroupNum * (entry.offset + 1) == req.shapelpinfo.groupNum) {
                entry.hasLast = true;
            }
        } else {
            uint32_t tryOffset = avgGroupNum * m_thdN + (entry.offset + 1);
            if (tryOffset < req.shapelpinfo.groupNum) {
                ++entry.totalCnt;
            } else if (avgGroupNum * m_thdN + (req.dispBufferCnt + 1) == req.shapelpinfo.groupNum) {
                ++entry.totalCnt;
                entry.hasLast = true;
            }
        }

        if (!m_buffers[m_thdWPtr].Write(entry)) {
            LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Vector GBuffer Manager]: Thread is " << m_thdWPtr
                << ". Alloc bid" << entry.bid << ". but stall.";
            break;
        }

        LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Vector GBuffer Manager]: Thread is " << m_thdWPtr
            << ". Alloc bid" << entry.bid << " gnum is " << entry.totalCnt;
        m_thdWPtr = (m_thdWPtr + 1) % m_thdN;
        ++req.dispBufferCnt;
        leftGroups -= entry.totalCnt;
        if (leftGroups == 0) {
            groupOver = true;
            break;
        }
    }
    curReq = req;
    curReq.vld = !groupOver;
}

bool GroupBuffer::IsEmpty() const
{
    for (uint8_t i = 0; i < m_thdN; ++i) {
        if (!m_buffers[i].IsEmpty()) {
            return false;
        }
    }
    return true;
}

bool GroupBuffer::CanDispatch()
{
    for (uint8_t tid = 0; tid < m_thdN; ++tid) {
        if (m_buffers[tid].CanDispatch()) {
            return true;
        }
    }
    return false;
}

void GroupBuffer::CheckRelease(uint32_t pickThead)
{
    if (m_buffers[pickThead].CheckOver()) {
        m_buffers[pickThead].Read();
    }
}

void GroupBuffer::SetGroupComplete(ROBID &gid, uint32_t tid)
{
    LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Vector GBuffer Manager]:  Commit: " << "T" << tid << "-G" << gid;
    m_buffers[tid].SetStatus(gid, BlockStatus::COMPLETED);
}

SimSys* GroupBuffer::GetSim()
{
    return m_sim;
}

void GroupBuffer::SetSim(SimSys *sim)
{
    m_sim = sim;
}

bool GroupBuffer::PickThread(uint32_t &pickThead)
{
    for (uint8_t tid = 0; tid < m_thdN; ++tid) {
        pickThead = (m_thdRPtr + tid) % m_thdN;
        if (m_buffers[pickThead].CanDispatch()) {
            return true;
        }
    }
    return false;
}

const GBufferEntry& GroupBuffer::PickGroup(uint32_t pickThead)
{
    ASSERT(m_buffers[pickThead].CanDispatch());
    m_buffers[pickThead].ReadyToDisp(m_thdN);
    return m_buffers[pickThead].Front();
}

void GroupBuffer::Dispatch()
{
    for (uint32_t i = 0; i < m_fetchWidth; ++i) {
        uint32_t pickThead = 0;
        if (!PickThread(pickThead)) {
            return;
        }

        if (m_workLoadQ[pickThead]->Full()) {
            break;
        }

        const GBufferEntry &entry = PickGroup(pickThead);
        ASSERT(entry.vld);
        if (entry.blockCmd->biqType == BIQType::VEC_IQ && !top->m_grob->CheckAlloc(entry.bid, entry.blockCmd->stid)) {
            break;
        }
        m_thdRPtr = (pickThead + 1) % m_thdN;

        uint32_t validLaneNum = entry.shapelpinfo.validLaneNum;
        if (entry.blockCmd->biqType != BIQType::MCALL_IQ && entry.shapelpinfo.dimReduction) {
            if (entry.CheckLastGroup()) {
                validLaneNum = m_maskFile->GetLastGroupNum(entry.bid, entry.blockCmd->stid);
            }
        } else if (entry.blockCmd->biqType != BIQType::MCALL_IQ && !entry.shapelpinfo.dimReduction) {
            /* How many groups does lb0 need? */
            uint64_t lb0GroupNum = (entry.shapelpinfo.lb0 + entry.shapelpinfo.m_lanes - 1) / \
                entry.shapelpinfo.m_lanes;
            /* The initial lc0 of the current group */
            uint64_t currGroupStartLC0 = (entry.gid.GetVal() % lb0GroupNum) * entry.shapelpinfo.m_lanes;
            uint64_t remainLC0 = entry.shapelpinfo.lb0 - currGroupStartLC0;
            validLaneNum = (remainLC0 > entry.shapelpinfo.m_lanes) ? entry.shapelpinfo.validLaneNum : remainLC0;
        }
        LOG_INFO_M(Unit::VECTOR, Stage::NA) << "[Vector GBuffer Manager]: Thread is " << pickThead << ". .* "
            << entry.blockCmd->Dump() << "|" << "B" << entry.bid << "-T" << entry.tid << "-G" << entry.gid << "-lanes:"
            << validLaneNum << (entry.CheckLastGroup() ? "L" : "");
        GroupIssueInfo iInfo;
        iInfo.bid = entry.bid;
        iInfo.gid = entry.gid;
        iInfo.tid = entry.tid;
        iInfo.stid = entry.blockCmd->stid;
        iInfo.p1Cycle = GetSim()->getCycles();
        if (entry.blockCmd->biqType == BIQType::MCALL_IQ) {
            top->m_vectorGS->SetSwimlineStart(entry.bid, entry.gid, entry.tid);
        }
        if (entry.blockCmd->blockType != BlockType::BLK_TYPE_VPAR &&
            entry.blockCmd->blockType != BlockType::BLK_TYPE_VSEQ &&
            entry.blockCmd->biqType != BIQType::MCALL_IQ) {          /* TileOp */
            ASSERT(false && "Unsupport vector block type.") << GetBlockTypeName(entry.blockCmd->blockType);
        } else {   /* Block */
            if (m_peIfuQ->Full(entry.tid)) {
                break;
            }
            BlkBodyFetchState req;
            req.first = true;
            req.fetchTPC = entry.tpc;
            req.vld = true;
            req.bid = entry.bid;
            req.stid = entry.blockCmd->stid;
            req.startTPC = entry.tpc;
            req.endTPC = INST_MAX_PC;
            req.blkSrcType = BlkSrcType::FLUSH_BYPASS;
            req.isTemplate = false;
            req.isize = WIDTH_64; // SIMT
            req.noBranch = false;
            req.btype = BlockType::BLK_TYPE_MPAR;
            req.biqType = entry.blockCmd->biqType;
            req.gid = entry.gid;
            req.logicalGID = entry.logicalGID;
            req.shapelpinfo = entry.shapelpinfo;
            req.shapelpinfo.validLaneNum = validLaneNum;
            ASSERT(req.shapelpinfo.validLaneNum != 0);
            m_peIfuQ->Write(req, entry.tid);
            if (entry.blockCmd->biqType != BIQType::MCALL_IQ) {
                top->m_grob->SetGroupStartCycle(entry.bid, GetSim()->getCycles(), entry.blockCmd->stid);
            }
            BlockRunInfo blkInfo;
            blkInfo.vld = true;
            blkInfo.bid = entry.bid;
            blkInfo.gid = entry.gid;
            blkInfo.stid = entry.blockCmd->stid;
            blkInfo.logicalGID = entry.logicalGID;
            blkInfo.shapelpinfo = entry.shapelpinfo;
            blkInfo.blkNoBranch = true;
            blkInfo.blkRenamed = true;
            blkInfo.isTemplate = false;
            blkInfo.blockCmd = entry.blockCmd;
            blkInfo.shapelpinfo.validLaneNum = validLaneNum;
            ASSERT(blkInfo.shapelpinfo.validLaneNum != 0);
            // TODO: use sim queue
            if (entry.blockCmd->biqType != BIQType::MCALL_IQ) {
                m_GBuffer2GROB->Write(iInfo);
            }
            m_workLoadQ[entry.tid]->Write(blkInfo);
        }
        CheckRelease(pickThead);
    }
}

void GroupBuffer::PrintEntryStatus(const GBufferEntry& entry, uint32_t idx)
{
    if (!entry.vld) {
        return;
    }

    std::cout << "\tB" << std::dec << entry.bid.val << "-T" << entry.tid << "-G" << entry.gid.val;
    if (entry.status == BlockStatus::ALLOCATED) {
        std::cout<<" allocated ";
    } else if (entry.status == BlockStatus::DISPATCHED) {
        std::cout<<" dispatched to VIFU/CT for execution";
    } else if (entry.status == BlockStatus::COMPLETED) {
        std::cout << " completed ";
    } else if (entry.status == BlockStatus::RETIRED) {
        std::cout << " retired ";
    } else {
        /* TODO: add me */
        std::cout << " unknown ";
    }
    if (idx == 0) {
        std::cout << "<- oldest";
    }
    std::cout << std::endl;
}

void GroupBuffer::UpdateReadPtr(ROBID &gid, uint32_t tid)
{
    m_buffers[tid].UpdateReadPtr(gid);
}

void GroupBuffer::SetFlush(FlushBus &bus)
{
    m_maskFile->SetFlush(bus);
    for (auto &buffer : m_buffers) {
        buffer.SetFlush(bus, m_thdN);
    }

    if (curReq.vld && LessEqual(bus.req.bid, curReq.blockCmd->bid)) {
        curReq.vld = false;
        leftGroups = 0;
    }
}

void GroupBuffer::Stats()
{
    bool bussy = false;
    for (uint8_t tid = 0; tid < m_thdN; ++tid) {
        if (m_buffers[tid].CanDispatch()) {
            bussy = true;
            ++top->m_stats.gBufferBussyGroupNum;
        }
    }

    if (bussy) {
        ++top->m_stats.gBufferBussyCyc;
    }
}

} // namespace JCore
