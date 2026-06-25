#pragma once

#include <queue>
#include <set>
#include <stack>
#include <vector>

#include "bctrl/bfu/bfu.h"
#include "bctrl/bfu/bfu_interface.h"
#include "core/Bus.h"
#include "GFUSim.h"
#include "ModelSpec.h"
#include "ModelCommon/SimInstInfo.h"

namespace JCore {

class BCtrlUnit;
class SoftMemory;

struct PerfectFetchInfo {
    bool last = false;
    bool first = false;
    bool isInst = false;
    bool canSkip = false;
    uint64_t pc = 0;
};

class BlockIFU : public SimObj {
private:
public:
    BCtrlUnit                                   *top;
    BlockIFU();
    BFUInterface                                bfuIntf;
    std::shared_ptr<NS_CORE::BFU>               bfu;
    BHeader                                     oldestMispred;
    uint64_t                                    lastRetireBPC = 0;
    /* \brief BlockType for minst in perfect bp mode */
    BlockType                                   bType = BlockType::BLK_TYPE_INVAL;

    std::vector<SimQueue<NS_CORE::PtrMachineInst>>     bfu_be_q;
    std::vector<SimQueue<NS_CORE::PtrMachineInst>>     be_bfu_rslv_q;
    std::vector<SimQueue<NS_CORE::PtrMachineInst>>     be_bfu_cmt_q;
    SimQueue<NS_CORE::NukeInfo>                        be_bfu_nuke_info_q;
    // 从 gfrun 拿到的 PC Trace 会暂存在这里，需要 flush 的话会从这开始重填到 flushedPCQ
    std::deque<PerfectFetchInfo>                fetchedQ;
    std::deque<PerfectFetchInfo>                flushedPCQ;
    void                                        Reset();
    void                                        Work();
    void                                        Xfer();
    void                                        Build();
    SimSys                                      *GetSim();
    void ReportStat() override {}
    /* \brief Tells the BFU to jump to a new block (external request) */
    void                                        jumpTo(uint64_t bpc, uint32_t stid);

    /* \brief Flush the BFU and header FIFO */
    void                                        flush(FlushBus flushReq);
    void                                        FlushFullBP(FlushBus flushReq);
    void                                        FlushNoBFU(FlushBus flushReq);
    /* \brief Tells the BFU the block has been resolved or mispredicted */
    void                                        resolveBlock(BHeader &header);
    /* \brief Tells the BFU the block has been retired */
    void                                        retireBlock(BHeader &header);

    SimInst                                       GetInst(uint32_t stid);
    SimInst                                       GetInstFromTrace(uint32_t stid);
    SimInst                                       GetInstFromBFU(uint32_t stid);

    void                                        TerminateFlush(uint32_t stid);
};

} // namespace JCore
