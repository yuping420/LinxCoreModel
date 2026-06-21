#pragma once

#ifndef BLOCK_STRUCT
#define BLOCK_STRUCT

#include "MInst.h"
#include "ISACommon/BlockType.h"
#include "ISACommon/BARG.h"
#include "ISACommon/BlockAttribute.h"
#include "ISACommon/BlockHint.h"

namespace JCore {
class Block {
public:
    virtual ~Block() = default;

    bool                                    bIsComplete = false; // The block is complete, not just executed.
    bool                                    bIsTemplate = false;
    bool                                    iotLast = false;   // 用于b.iot/b.ioti结束检查

    Opcode                                  opcode = Opcode::OP_INVALID;    // 为Fentry等模板块准备

    BlockType                               blockType = BlockType::BLK_TYPE_INVAL;
    BranchType                              branchType = BranchType::BLK_BR_INVAL;

    BARG                                    barg;
    TileOp                                  tileOp = TileOp::TINVALID;
    DataType                                dataType = DataType::INVALID;
    VREGMode                                vregMode = VREGMode::VS16;
    std::shared_ptr<BlockAttribute>         blockAttr = nullptr;
    std::shared_ptr<BlockHintInfo>          blockHint = nullptr;

    ACRC_REQUEST_TYPE                       acrcReqType = ACRC_REQUEST_TYPE::SCT_INVAL;

    std::vector<uint64_t>                   srcATag;
    std::vector<uint64_t>                   srcData;
    std::vector<uint64_t>                   dstData;

    uint64_t                                bTextOffset = 0;
    uint64_t                                bTextPC = 0;
    uint64_t                                hSize = 0;
    uint64_t                                lb0 = 1;
    uint64_t                                lb1 = 1;
    uint64_t                                lb2 = 1;

    // tileReg管理
    std::vector<OperandPtr>                 srcTile;
    std::vector<OperandPtr>                 dstTile;

    uint64_t                                totalBodyIters = 0; // Number of block Body Iterations
    uint64_t                                totalGroupNum = 0;

    std::string Dump();
    std::string DumpCT();

    void SetBlockType(BlockType blkType);
    void SetBranchType(BranchType brType);
    void HandleBSTARTBlockType(MInst &inst);
    void HandleBSTARTBranchType(MInst &inst);
    void HandleBSTARTTMA(uint64_t function);
    void HandleBSTARTCUBE(uint64_t function);
    void HandleBSTARTTEPL(uint64_t function, uint64_t mode);
    void HandleBSTARTParallel(MInst &inst);
    void HandleBSTART(MInst &inst);
    void HandleBSTOP(MInst &inst);
    void HandleBIOR(MInst &inst);
    void HandleBIOT(MInst &inst);
    void HandleBDIM(MInst &inst);
    void HandleBCATR(MInst &inst);
    void HandleBDATR(MInst &inst);
    void HandleBTEXT(MInst &inst);
    void HandleBHINT(MInst &inst);
    void HandleMCOPY(MInst &inst);
    void HandleMSET(MInst &inst);
    void HandleFENTRY(MInst &inst);
    void HandleFEXIT(MInst &inst);
    void HandleFRETRA(MInst &inst);
    void HandleFRETSTK(MInst &inst);

    bool IsReduceDimension();
    uint64_t GetGroupNum(uint64_t laneNum);
    uint64_t GetCurrentGroupID(uint64_t completedIters, uint64_t laneNum);
    uint64_t GetCurrentGroupIters(uint64_t completedIters, uint64_t laneNum);
    uint64_t GetFlatIter(uint64_t groupId, uint64_t landNum, uint64_t lane);
    uint64_t GetLC0(uint64_t groupId, uint64_t landNum, uint64_t lane);
    uint64_t GetLC1(uint64_t groupId, uint64_t landNum, uint64_t lane);
    uint64_t GetLC2(uint64_t groupId, uint64_t landNum, uint64_t lane);

    std::string GetBStartStr();
    std::string GetSTDTemplateStr();
    std::string GetBTextStr();
    std::string GetBAttrStr();
    std::string GetHintStr();

    void SetBlockIsComplete();
    TileInfoPtr GetDstTileInfo(uint64_t tileIdx = 0) const;
    virtual void UpdateDstTileInfo() const;
};
inline uint64_t CeilDiv(uint64_t dividen, uint64_t divisor)
{
    return divisor == 0 ? dividen + divisor - 1 : (dividen + divisor - 1) / divisor;
}
}
#endif
