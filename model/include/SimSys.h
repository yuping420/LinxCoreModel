#pragma once
#include <chrono>
#include <deque>
#include <list>
#include <queue>
#include <set>
#include <string>
#include <vector>
#include "../emulator/Memory.h"
#include "../emulator/SoftCore.h"
#include "../tools/trace_logger/TraceLogger.h"
#include "core/Bus.h"
#include "DFX/ViewManager.h"
#include "DFX/ResVerifyManager.h"
#include "GFUSim.h"
#include "ISA.h"
#include "ModelSpec.h"
#include "utils/reporter.h"
#include "utils/util.h"

#ifndef MODEL_TOP_H
#define MODEL_TOP_H

namespace JCore {
constexpr int ESL_PORT_WIDTH = 128;

enum class BP_Mode {
    FULL_BP = 0,
    PERFECT_BP = 1,
    NO_BP = 2,
};

class Core;

enum class EcallStatus {
    NONE = 0,
    DECODE,     // finished decoding, but it's not the oldest
    EXECUTE,    // it's the oldest, but it's not committed yet
    COMMITTED,  // committed, but the memory aaccelss is not complete
};

enum class TraceMode {
    STANDARD    = 1,
    MODE2       = 2,
    ROBEXTRA    = 3,
    MODE4       = 4,
    PIPEVIEW    = 5,
    PIPEVIEW2   = 6,
    DEBUGLOGGER = 7,
};

struct SystemStatus {
    EcallStatus                     ecallStatus = EcallStatus::NONE;
    ROBID                           ecallBlkId;
    bool                            EcallRunning();
};

class SimSys {
public:
    uint64_t                cycles = 0;
    bool                    fakeJcore = false;
    bool                    l2Free = false;
    bool                    stqFree = false;
    bool                    cubeRunning = false;
    bool                    mtcRunning = false;
    bool                    vecRunning = false;
    SystemStatus            systemStatus;
    bool                    remainingRefVld = false;
    int64_t                 remainingRefCnt = 0;
    /* \brief Number of simulatable objects */
    std::vector<std::shared_ptr<SimObj>> modules;
    std::shared_ptr<Core>   core;
    /* \brief Memory for Checkpoint */
    SoftMemory              memory;
    bool                    ckpt_file = false;
    /* \brief Overrided configurations */
    std::shared_ptr<ConfigInput> cfgs;
    /* \brief Reporter */
    NS_PLAT::Reporter       rpt;

    /* \brief Level 1 versbose mode */
    bool                    verboseON = false;
    std::set<StageID>       verboseSwitch;

    /* \brief Level 2 versbose mode */
    bool                    verboseON2 = false;
    std::set<StageID>       verboseSwitch2;

    std::string                  binaryFilename;

    /* \brief DebugLogger versbose switch */
    bool                    DebugVerboseON = false;

    /* \brief Perfect simulation mode: load/store or get/set */
    bool                    perfectSimON = false;
    /* \brief Perfect load/store mode */
    bool                    perfectLoadStore = false;
    /* \brief Perfect stack rename mode */
    bool                    perfectStackRename = false;
    /* \brief Perfect get/set mode */
    bool                    perfectGetSet = false;
    /* \brief Function model for golden reference */
    SoftCore                refCore;
    /* \brief Golden reference information from function model trace */
    ReferenceInfo           refInfo;
    /* \bool check if src is global sp */
    bool                    refSpGlobal = false;

    uint64_t                maxBCount = 0;
    uint64_t                correctBCount = 0;
    uint64_t                correctICount = 0;
    uint64_t                totalBROBSize = 0;
    uint64_t                lastCommitedBID = -1;
    uint64_t                head_dfx_index = 0;

    uint64_t                lastCycInstCnt = 0;
    uint64_t                nsPECorrectBCnt = 0;
    uint64_t                nsPECorrectICnt = 0;
    /* \brief Simulate memory aaccelsses externally */
    bool                    externalMode = false;
    bool                    testFinisherSeen = false;
    bool                    testFinisherFailed = false;
    uint64_t                testFinisherValue = 0;
    uint32_t                testFinisherByteMask = 0;
    std::string             uartLineBuffer;

    std::vector<bool>                    terminate;

    std::chrono::system_clock::time_point simStartTime;

    std::string swimLaneFile = "gfsim.swimlane.json";
    std::shared_ptr<TraceLog::TraceLogger> swimLogger = nullptr;

    std::vector<std::shared_ptr<ViewManager>>            viewManager;
    std::vector<std::shared_ptr<ResVerifyManager>>       resVerifyManager;

    SimSys() {
        cycles = 0;
        ckpt_file = false;
        correctBCount = 0;
        correctICount = 0;

        verboseON                    = false;
        verboseON2                   = false;
        simStartTime = std::chrono::system_clock::now();
        cfgs = std::make_shared<ConfigInput>();
    }
    ~SimSys();
    bool                    buildSystem();
    void                    Reset();
    void                    setGPR(uint32_t id, uint64_t data, uint32_t stid);
    void                    setSysreg(uint32_t id, uint64_t data);
    FRMMode                 GetFRM();
    uint64_t                getGPR(uint32_t id, uint32_t stid);
    bool                    needTerminate();
    void                    step();

    void                    setVerbose(StageID stageID);
    void                    setVerbose2(StageID stageID);

    void                    unsetVerbose(StageID stageID);
    void                    unsetVerbose2(StageID stageID);

    void                    addModule(std::shared_ptr<SimObj> m);

    int64_t                 SetRefInfo(int64_t recordCnt);
    void                    RunReference(uint64_t stopPC);
    void                    RecordTrace(uint64_t threadId);
    void                    RecordPerectBPInfo(uint64_t threadId);
    void                    updateRefStq(LoadStoreInfo &lsInfo);
    void                    lookupRefStq(LoadStoreInfo &ldInfo);
    bool                    checkRefLoadReady(LoadStoreInfo &ldInfo);
    bool                    checkStackRename(std::vector<MInst> minsts, uint32_t index, uint64_t bpc);
    bool                    lookupRefStqID(uint64_t id);
    void                    dequeRefStq(uint64_t bid);
    void                    unsetRefStq(uint64_t id);
    void                    recoverBlockRefStq(uint64_t bid);
    void                    recoverRefStq(uint64_t id);
    void                    DeleteInfoQ(uint64_t sid);
    /* \brief Check all buffers in the simulator to

     * verify the correctness of the simulation.
     * The reference is the function model */
    void                    ReleaseRefCoreInfo(bool isLastHeader);
    void                    setData(MInst &inst, BlockType type);
    void                    printMInst(uint64_t tpc, std::ostream &out, BlockType type);
    void                    printInst(uint64_t bpc, uint64_t tpc, std::ostream &out);
    void                    ReportVectorCore();
    void                    ReportStat();
    void                    resetStats();

    void                    enableTrace(uint64_t trace_pc);

    // Unified memory interface for aaccelssing memory
    uint64_t                fetchData(uint64_t address, int width);
    uint64_t                loadData(uint64_t address, int width, bool signedLoad);
    void                    storeData(uint64_t address, uint64_t data, int width);
    void                    observeTestFinisher(uint64_t address, uint64_t data, int width);
    void                    observeUartWrite(uint64_t address, uint64_t data, int width);

    // utilities
    uint64_t                getCycles() { return cycles; }
    NS_PLAT::Reporter*      getRpt() { return &rpt; }
    std::queue<FetchInfo>*    getHeaderTrace() { return &(refInfo.headerTrace); }
    std::shared_ptr<ConfigInput> getCfgs() { return cfgs; }
    void                    innerTraceFlush();
    Core                    *GetCore();

    void                                    InitSwimLaneLogger(const std::string &fileName);
    std::shared_ptr<TraceLog::TraceLogger>  GetSwimLogger();
    void                                    DumpSwimLaneToJson();
    void                                    ObjRegisterLogInfo(std::shared_ptr<SimObj> obj, size_t nameSeq,
                                                               size_t globalSeq, std::string threadName = "");
    void                                    RegisterMultiThread(std::shared_ptr<SimObj> obj, uint64_t num, size_t seq,
                                                                std::unordered_map<uint64_t, std::string> threadNameMap = {});

    void AddDependency(uint64_t blockId, int eventId, uint64_t scalarThreadId);
    void AddEventBegin(std::string name, Pid pid, Tid tid, std::string hint);
    void AddEventEnd(Pid pid, Tid tid);
    void AddDuration(SwimLogData &logData);
    int GetEventId();

    void BuildViewManager(uint64_t depth, uint64_t threadNum);
    std::shared_ptr<ViewManager> GetViewManager(uint64_t threadId);
    void BuildVerifyManager(uint64_t depth, uint64_t threadNum);
    std::shared_ptr<ResVerifyManager> GetVerifyManager(uint64_t threadId);
    void ResetWaitCycle();

    void ResVerify();
    void PrintPipeView();
};

enum class LogStatus {
    LOGS_NORMAL = 0,
    LOGS_WAIT_START,
    LOGS_WAIT_END
};

struct CommandLineArgs {
    std::string filename;
    int traceMode = 0;
    std::string traceFileName = "";
    int filterEnable = 0;
    std::vector<std::string> filterModule;
    uint64_t blockNum = static_cast<uint64_t>(-1);
    uint64_t blockWarmup = 0;
    uint64_t minstWarmup = 0;

    uint64_t stopPC = static_cast<uint64_t>(-1);
    uint64_t stopCycles = static_cast<uint64_t>(-1);
    uint64_t debugLogStartPC = static_cast<uint64_t>(-1);
    uint64_t debugLogStartCycle = 0;
    uint64_t debugLogEndCycle = 0;

    std::vector<std::string> cfgs;
    uint64_t pipeViewMode = 0;
    std::string pipeFileName = "gfsim.pipeview.log";
    bool pipeFilterGroupInfo = false;

    uint64_t swimLaneMode = 0;
    std::string swimLaneFile = "gfsim.swimlane.json";

    int32_t seedMode = 0;
    std::vector<std::string> configFiles;

    LogStatus GetLogStatus()
    {
        if (!IsEnableLog()) {
            return LogStatus::LOGS_NORMAL;
        }
        if (DelayStartTrace()) {
            return LogStatus::LOGS_WAIT_START;
        } else if (NeedEndTrace()) {
            return LogStatus::LOGS_WAIT_END;
        }
        return LogStatus::LOGS_NORMAL;
    }

    bool DelayStartTrace() const
    {
        return debugLogStartCycle != 0 || debugLogStartPC != static_cast<uint64_t>(-1);
    }

    bool IsEnableLog() const
    {
        return (traceMode == static_cast<int>(TraceMode::STANDARD) ||
                traceMode == static_cast<int>(TraceMode::ROBEXTRA));
    }

    bool NeedEndTrace() const
    {
        return debugLogEndCycle != 0;
    }
};

} // namespace JCore

#endif
