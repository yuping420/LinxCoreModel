#pragma once

#ifndef SIM_INST_INFO_H
#define SIM_INST_INFO_H

#include "ISA.h"
#include "ROBID.h"
#include "CycleInfo.h"
#include "../common/DataStruct/VecData.h"
#include "LpvInfo.h"
#include "ShapeLoopInfo.h"
#include "ModelEnumDefines.h"
#include "ModelCommon/bus/VectorBridgeBus.h"

namespace JCore {
using addr_t = uint64_t;
using tag_t = uint64_t;

using time_t = uint32_t;
using seq_t = uint32_t;
using msize_t = uint32_t;

using set_t = uint32_t;
using way_t = uint32_t;
using bank_t = uint32_t;
using pos_t = uint32_t;

using idx_t = uint32_t;

class SPMInstInfo {
public:
    bool bsizeVld = false;
    uint32_t bsize = 0;
    uint32_t hsize = 0;
    bool isInst = false;
    bool isLast = false;
    bool afterBranch = false;
    bool cut = false;
    bool isBstart = false;
    bool hasBend = false;
};

using SPInfoPtr = std::shared_ptr<SPMInstInfo>;

class BFUMachineInfo {
public:
    // ID
    bool global {false};
    seq_t hid {0};
    seq_t fbid {0};
    seq_t fbid_local {0};
    static seq_t hid_global;

    // Static information
    addr_t pc {0};
    SPInfoPtr spInfo = nullptr;
    bool vld {false}; // is a valid block in a fetch bundle
    bool concat {false};
    bool fetch_end {false};
    bool needBstop {false};
    bool nuke_after_redirect {false};

    // Time stamps
    time_t create_time {0};
    time_t fetch_time {0};
    time_t f1_time {0};
    time_t bhc_fetch_time {0};
    time_t ib_time {0};
    time_t resolve_time {0};
    time_t commit_time {0};

    // Predict information
    bool after_branch {false};
    addr_t predict_tgt {0};
    bool predict_taken {false};

    // Path information for trace mode
    bool first_after_redirect {false};
    bool first_after_nuke {false};

    // Resolve information
    bool resolved {false};
    bool resolve_taken {false};
    bool resolve_mispredict {false};
    addr_t resolve_tgt {0};
    bool committed {false};
    // for statistcs
    bool predict_at_once {false};
    bool predict_forward {false};

public:
    BFUMachineInfo() = delete;
    BFUMachineInfo(uint64_t cycles);
    BFUMachineInfo(const BFUMachineInfo& minfo);
    BFUMachineInfo(seq_t fbid, addr_t pc, SPInfoPtr sp, time_t create_time, time_t fetch_time, time_t f1_time,
        time_t bhc_fetch_time, time_t ib_time);
    BFUMachineInfo& operator=(const BFUMachineInfo& minfo);
};
using BFUMachinePtr = std::shared_ptr<BFUMachineInfo>;

class BlockCommand;
using BlockCommandPtr = std::shared_ptr<BlockCommand>;

struct LpvInfo;
using PLpvInfo = std::shared_ptr<LpvInfo>;

constexpr uint64_t RANGE_8_BITS = 0xFF;

static inline bool FbsEightBitsRanged(uint64_t data)
{
    return (data <= RANGE_8_BITS || data > static_cast<uint64_t>(-RANGE_8_BITS));
}

static inline bool FbsEightBitsRanged(int64_t data)
{
    return (data <= static_cast<int64_t>(RANGE_8_BITS) || data > static_cast<int64_t>(-RANGE_8_BITS));
}

struct PhysicalOperand : public Operand {
public:
    bool                renamed = false;
    bool                ready = false;
    bool                innerDepend = false; // to delete
    /* \brief GREG\T\U\VT\VU\VM\VN use this ptag */
    uint64_t            ptag = -1U;
    bool                pair = false;

    uint64_t            wakeupTime = 0;

    bool                recycled = false;
    bool                released = false;

    bool                dataVld = false;
    bool                dataFromBypass = false;
    bool                vecDataVld = false;
    VecData             vecData;
    DataType            dataType;

    bool                reg_move = false;
    /* \brief Load position vector, use for load-to-use  */
    PLpvInfo            lpvInfo;

    /* 仅用于mtc-core */
    DataType            mtcDataType = DataType::INVALID;
    LayOut              mtcLayout = LayOut::LAYOUT_COUNT;
    uint64_t            mtcRealAddr = 0;
    uint64_t            mtcD1TR = 0;
    uint64_t            mtcD2TR = 0;
    uint64_t            mtcD1GM = 0;
    uint64_t            mtcD2GM = 0;
    uint64_t            mtcElementWidth = 0; // Bytes
    uint64_t            mtcStrideGM = 0;
    uint64_t            mtcShapeSize = 0;

    /* \brief the distance of rob when aaccelss localreg */
    // 并无功能作用，看起来只为计数。
    uint32_t                localRegRobDist = 0;

    PhysicalOperand() = default;
    ~PhysicalOperand() = default;
    explicit PhysicalOperand(Operand &opd);
    explicit PhysicalOperand(OperandPtr &ptr);
    explicit PhysicalOperand(OperandType typ, uint64_t val);
    uint64_t GetAllocRegSize(uint64_t lanes = 64);
    bool MatchRegType(OperandType rtype)
    {
        return type == rtype;
    }
    bool IsSP();
    std::string Dump(uint64_t idx = 0);

    bool IsLocalReg()
    {
        return type == OperandType::OPD_TLINK || type == OperandType::OPD_ULINK
               || type == OperandType::OPD_VTLINK || type == OperandType::OPD_VULINK
               || type == OperandType::OPD_VMLINK || type == OperandType::OPD_VNLINK
               || type == OperandType::OPD_PREDMASK;
    }
    bool DataRanged()
    {
        return ready && dataVld && FbsEightBitsRanged(data<<shamt);
    }
    bool IsVRFReg()
    {
        return type == OperandType::OPD_VTLINK || type == OperandType::OPD_VULINK
               || type == OperandType::OPD_VMLINK || type == OperandType::OPD_VNLINK;
    }
    void InitVecData(uint32_t width, uint32_t lanes)
    {
        if (type == OperandType::OPD_PREDMASK) {
            width = 1;
        }
        vecDataVld = true;
        vecData.Init(width, lanes);
    }
    bool CanBypass(uint64_t pickTime) const;
};
using POperandPtr = std::shared_ptr<PhysicalOperand>;

struct InnerBPInfo {
    bool predTaken = true;
    uint64_t predTarget = 0;
};

/*  */
struct GatherInfo {
    uint64_t                gather_stall_cycle_begin = 0;
    uint64_t                gather_stall_cycle_end = 0;
    uint64_t                gather_issue_cycle_begin = 0;
    uint64_t                gather_issue_cycle_end = 0;
    uint64_t                gather_issue_cycle_Retire = 0;
    bool                    gather_stall = false;
};

struct RFReqBus;
struct MemReqBus;
struct PEResolveBus;
class SimInstInfo : public JCore::MInst {
public:
    ROBID bid;
    ROBID rid;
    ROBID gid;
    uint64_t tid = 0;
    uint64_t peID = 0;
    uint64_t coreID = 0;
    uint64_t bpc = 0;
    uint32_t stid = -1U;
    BIQType biqType = BIQType::NONE_IQ;
    ExecEngineTyp iexType = SCALAR_IEX;
    BFUMachinePtr bfuInfo = nullptr;
    CycleBus pipeCycle = nullptr;
    /* \brief execution latency */
    uint32_t                iexLatency = 0;
    uint32_t                iexExtendedCycles = 0;
    /* \brief indicates whether this instruction is the first instruction of block */
    bool                    first = false; // 暂不清楚作用，应该仅作用于分离块
    bool isLastInBlock = false;
    bool terminate = false;
    bool autogen = false;
    /* \brief loadId, storeId */
    uint64_t                startSID = 0;
    uint64_t                startLoadID = 0;
    uint64_t                sid = 0; // store-ID
    ROBID                   vcSid;
    uint64_t                load_id = 0;
    ROBID                   lsID;
    bool                    storeSplit = true;
    bool                    dwDstType = false;
    /* from memcopyin/memcopyout */
    bool                    memInst = false;
    bool                    fromSC = false;
    bool                    vecTileLoad = false;
    bool                    isSysStateInst = false;
    bool                    backToCodeTemplate = false; // IEX 需要向CT 返回寄存器值
    uint64_t                debugId = UINT64_MAX;

    BlockCommandPtr         bcmd = nullptr; // 仅用于bfu 内部，sp 相关信息赋值。

    std::shared_ptr<InnerBPInfo> innerBPInfo = nullptr;

    std::vector<POperandPtr> psrcs_;
    std::vector<POperandPtr> pdsts_;
    /* status info */
    bool issued = false;
    bool renamed = false;
    bool innerBranch = false;
    /* \brief whether stack rename is finished */
    bool stack_renamed = false;
    bool loadWakeuped = false;
    /* \brief whether it carries data or address for store inst */
    uint32_t                type = 0;
    uint32_t                iqid = -1U;
    uint64_t                isqId = 0;
    uint64_t                isqPickerId = 0;
    bool                    ldqCheck = false;
    bool                    ldqLimit = false; // Optimization for software.

    /* unique ID */
    uint64_t                uid = 0; // git 记录添加人 张永安 00537502
    /* used in pipeview */
    std::string             iq_name;
    // isq status
    ROBID                   insertIsqId;
    bool                    picked = false;
    /* \pipe s1*/
    bool                    hasIqStall = false;

    /* \brief local reg allocated position, Work for flushing/rtable */
    ROBID                   tSeq;
    ROBID                   uSeq;
    ROBID                   predSeq;

    /* 主线上功能关闭，最初添加人 00899536 */
    bool                    gatherLd = false;
    bool                    scatterSt = false;
    GatherInfo              gatherInfo;

    /* 主线上功能关闭，mtccore 使用，最初添加人 贾贺飞 00513579 肖权 00600317 */
    bool                    isGatherLd = false;
    bool                    isScatterSt = false;
    bool                    haveSend = false;
    ROBID                   subrid;
    bool                    subissued = false;
    bool                    sentFromSc = false;

    uint32_t                realReqCnt = 0; // vab 使用。
    uint32_t                GetSrcMSIMTMask() { return 0;} // vab 使用。
    uint64_t                predMask = -1UL;

    /* SIMT 相关 */
    /* \brief The id of the group within the block, with each block starting from 0 */
    uint32_t                    logicalGID = 0;
    ShapeLoopInfo               shapelpinfo;
    uint32_t lanes = 1;
    uint32_t                wakeupCnt = 0; // git 记录添加人 石晓强 wx1151379
    uint32_t                retLane = 0; // git 记录添加人 石晓强 wx1151379，不清楚作用
    /* \brief Load Check or not for stack rename */
    bool                    stack = false;
    bool                    stack_check = false; // 貌似属于stack-rename 暂不支持。
    /* \brief for stack rename */
    bool                    isLoadReturn = false;
    StackInstType           stack_type = StackInstType::NORMAL;
    uint32_t                subInstCnt = 0; // git 记录添加人 肖权 00600317

    /* \for load/store conflict */
    int                    wait_store = -1;
    bool                   addrWakeuped = false;
    bool                   intercept = false;

    // for reduce
    bool                   isLastHeader = false;

    bool                   needSimtResolve = false;

    // SIMT MCALL mode
    bool                   threadSwitchEnd = false;

    bool                   isVgather = false;
    bool                   isVscatter = false;
    uint32_t               transactionId = 0;
    uint32_t               groupSlotId = 0;

    SimInstInfo();
    explicit SimInstInfo(const CycleBus& bundleCycleInfo);

    BranchType GetBranchType();
    bool IsFallthrough();
    bool IsCond();
    bool IsDirect();
    bool IsIndirect();
    bool IsCall();
    bool IsReturn();
    bool IsTerminateFlag();
    bool IsSetRet();
    uint64_t GetSetRetDst();
    uint64_t GetBundlePosPC();
    bool IsVgather() const;
    VectorBridgeReq GenVgatherReq(uint64_t addr);
    bool IsVscatter() const;
    VectorBridgeReq GenVscatterReq(uint32_t stqIndex);

    std::string Dump();
    std::string DumpPipeViewInfo();
    std::shared_ptr<SimInstInfo> GenNextBSTOP();
    uint64_t GetNextPC() const;

    void Decode(uint64_t tpc, uint64_t bin, uint32_t sizeByte, bool &is64Dst, uint32_t laneNum = 1);
    void ConvertPOperand(uint32_t laneNum = 1);

    std::vector<POperandPtr>& GetrcArray();
    std::vector<POperandPtr>& GetPDstArray();
    POperandPtr GetPSrcPtr(size_t index) const;
    POperandPtr GetPDstPtr(size_t index) const;
    POperandPtr GetPSrcPtrByType(OperandType type);
    POperandPtr GetPDstPtrByType(OperandType type);
    size_t GetSrcIdxByType(OperandType type);
    size_t GetDstIdxByType(OperandType type);
    OperandType GetDstTileType();
    uint64_t GetDstTileSize();
    POperandPtr GetFirstValidTileSrc() const;
    bool IsReady();
    bool CheckCancel(uint32_t mask);
    bool CheckNoCancel();
    bool IsSrcGPRRenamed();
    bool IsDstGPRRenamed();

    bool SrcTypeContain(OperandType type);
    bool DstTypeContain(OperandType type);
    bool SrcContainSP();
    bool DstContainSP();
    void MoveLpv();
    PLpvInfo GetLpv();

    bool Execute(uint32_t lane = 0, uint64_t tpc = 0);
    void PrePareSrc(uint32_t lane = 0, uint64_t tpc = 0);
    void SetLOOPRegPOperandVld(POperandPtr &operand, uint32_t lane);
    void ProcessDst(uint32_t lane = 0, uint64_t tpc = 0);
    void GetSrcData(POperandPtr &src);
    bool Calculate();

    // for register file
    RFReqBus GenRFReqBus(bool isRead);
    void RFRetSetData(RFReqBus const& rfRet);
    RFReqBus GenSYSReqBus();
    void RecSYSReqRet(RFReqBus const& sysRet);
    RFReqBus GenSYSWFBus();
    // memory
    MemReqBus GenMemReq(uint32_t peCount, uint32_t lane = 0);
    // resolve
    PEResolveBus GenRslvBus();
    bool CheckOOOLoad();

    // RENO
    bool RangedDataReady();
    void ResetLpv();

    bool IsMcallLoadGlobal();
    // get tid by iex type
    uint64_t GetTid() const
    {
        if (iexType == SCALAR_IEX) {
            return stid;
        }
        return tid;
    }
};
using SimInst = std::shared_ptr<SimInstInfo>;
std::ostream& operator<<(std::ostream& out, SimInst const& inst);
}
#endif
