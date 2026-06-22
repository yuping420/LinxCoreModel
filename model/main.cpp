#include "include/SimSys.h"
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <iomanip>
#include <iostream>
#include <sysexits.h>
#include <unistd.h>

#include "utils/ParseArgs.h"
#include "../configs/softcore_config.cpp"
#include "core/Core.h"
#include "ISA.h"
#include "plat/CLI11.hpp"
#include "plat/conf_parser.h"
#include "mtccore/lsu/MtcLoadStoreUnit.h"
#include "soc/RandomModelSoC.h"

using namespace std;
using namespace JCore;

namespace {

void PrintFatalSignalBacktrace(int signal)
{
    void *frames[64];
    int frameCount = backtrace(frames, 64);
    dprintf(STDERR_FILENO, "\nFATAL: gfsim received signal %d\n", signal);
    backtrace_symbols_fd(frames, frameCount, STDERR_FILENO);
    std::signal(signal, SIG_DFL);
    raise(signal);
}

void InstallFatalSignalHandlers()
{
    std::signal(SIGSEGV, PrintFatalSignalBacktrace);
    std::signal(SIGBUS, PrintFatalSignalBacktrace);
    std::signal(SIGILL, PrintFatalSignalBacktrace);
}

}

static int ParseCommandLine(int argc, char **argv, struct CommandLineArgs& args)
{
    JCore::ParseArgs argsPrase = JCore::ParseArgs("gfsim");
    argsPrase.RegisterParam("-f", args.filename, "Executable to load (binary or ELF)");

    // 1: basic info; 3: rob debug info;
    argsPrase.RegisterParam("-t", args.traceMode, "1: Basic log information;" \
                            "3: Detailed debug log information;");
    argsPrase.RegisterParam("--tracefile", args.traceFileName, "Debug Log to file");
    argsPrase.RegisterParam("--trace_filter", args.filterEnable, "Debug Log to file");
    argsPrase.RegisterParam("--trace_filter_set", args.filterModule, "Debug Log to file");
    argsPrase.RegisterParam("-m", args.blockNum, "Number of blocks executed when simulation terminates");
    argsPrase.RegisterParam("-w", args.blockWarmup, "Number of blocks for warmup");
    argsPrase.RegisterParam("-i", args.minstWarmup, "Number of minsts for warmup");
    argsPrase.RegisterParam("-s", args.cfgs, "Override default configs");

    argsPrase.RegisterParam("--stop_pc", args.stopPC, "Stop at this Block PC Address.");
    argsPrase.RegisterParam("--stop_cycle", args.stopCycles, "Stop execution after this cycle.");
    argsPrase.RegisterParam("--debug_spc", args.debugLogStartPC, "Start Debug logging from this PC address[not ready]");
    argsPrase.RegisterParam("--debug_scycle", args.debugLogStartCycle, "Start Debug logging this from cycles");
    argsPrase.RegisterParam("--debug_ecycle", args.debugLogEndCycle, "End Debug logging this from cycles");
    argsPrase.RegisterParam("-p", args.pipeViewMode, "Pipe view mode. 0: No pipevie; 1:Full PipeView; 2:Only"\
                                                     "Block PipeView");
    argsPrase.RegisterParam("--pipefile", args.pipeFileName, "Pipeview output filename");
    argsPrase.RegisterParam("--pipe_filter_group", args.pipeFilterGroupInfo, "Pipeview output filename");
    argsPrase.RegisterParam("--swimlane", args.swimLaneMode, "Swim lane mode");
    argsPrase.RegisterParam("--swimfile", args.swimLaneFile, "Swimlane output file");
    argsPrase.RegisterParam("--seed", args.seedMode, "Random seed mode. <0:time; 0:bypass(unset); >0:fixed");
    argsPrase.RegisterParam("--conf", args.configFiles, "Random seed mode. <0:time; 0:bypass(unset); >0:fixed");
    argsPrase.Parse(argc, argv);

    // Print the execution command to the screen
    for (int i = 0; i < argc; ++i) {
        std::cout << argv[i] << " ";
    }
    std::cout << std::endl;

    return 0;
}

static void ProcessConfigs(std::vector<std::string>& configFiles, std::vector<std::string>& cfgs)
{
    JCore::ParseArgs parser;
    for (auto &path : configFiles) {
        std::vector<std::string> conf;
        if (path.find(".json") != std::string::npos) {
            parser.ParseJsonConfig(path, conf);
        } else {
            if (path.find(".conf") == std::string::npos) {
                path += ".conf";
            }
            if (path.find("configs") == std::string::npos) {
                path = "configs/" + path;
            }
            parser.ParseConfig(path, conf);
        }
        cfgs.insert(cfgs.end(), conf.begin(), conf.end());
    }
    if (!cfgs.empty()) {
        LOG_ERROR("Override configurations:");
        for (auto& cfg : cfgs) {
            LOG_ERROR << cfg;
        }
    }
}

static void ProcessSeedMode(int32_t seedMode)
{
    if (seedMode < 0) {
        srand(static_cast<uint32_t>(time(nullptr)));
    } else if (seedMode > 0) {
        srand(static_cast<uint32_t>(seedMode));
    }
}

static void redirectQueue(SimSys *sim);
static struct addr LoadELF(const shared_ptr<SimSys>& sim, vector<uint64_t>& regs, const char *filename,
                           map<uint64_t, uint64_t>& sysregs)
{
    JsonElement jsonElement;
    jsonElement.regs = regs.data();
    jsonElement.filename = filename;

    struct addr addr_ret = sim->memory.LoadElf(jsonElement, sysregs);

    return addr_ret;
}

static void EnableLog(const shared_ptr<SimSys>& sim, const CommandLineArgs& args)
{
    if (args.traceMode == static_cast<int>(TraceMode::STANDARD)) {
        LoggerManager::GetManager().ResetLevel(LoggerLevel::INFO);
        if (!args.filterEnable) {
            sim->verboseON = true;
            sim->setVerbose(StageID::BCC_ALL);
            sim->setVerbose(StageID::OPE_ALL);
            sim->setVerbose(StageID::LSU_ALL);
            sim->setVerbose(StageID::IFU_ALL);
        }
    } else if (args.traceMode == static_cast<int>(TraceMode::ROBEXTRA)) {
        LoggerManager::GetManager().ResetLevel(LoggerLevel::DETAIL);
        if (!args.filterEnable) {
            sim->verboseON = true;
            sim->verboseON2 = true;
            sim->setVerbose(StageID::BCC_ALL);
            sim->setVerbose(StageID::OPE_ALL);
            sim->setVerbose(StageID::LSU_ALL);
            sim->setVerbose(StageID::IFU_ALL);
            sim->setVerbose2(StageID::OPE_ROB_FULL);
            sim->setVerbose2(StageID::BROB_FULL);
        }
    }
}

static void DisEnableLog(const shared_ptr<SimSys>& sim, const CommandLineArgs& args)
{
    if (args.traceMode == static_cast<int>(TraceMode::STANDARD)) {
        LoggerManager::GetManager().ResetLevel(LoggerLevel::WARN);
        sim->verboseON = false;
        sim->unsetVerbose(StageID::BCC_ALL);
        sim->unsetVerbose(StageID::OPE_ALL);
        sim->unsetVerbose(StageID::LSU_ALL);
        sim->unsetVerbose(StageID::IFU_ALL);
    } else if (args.traceMode == static_cast<int>(TraceMode::ROBEXTRA)) {
        LoggerManager::GetManager().ResetLevel(LoggerLevel::DEBUG);
        sim->verboseON = false;
        sim->verboseON2 = false;
        sim->unsetVerbose(StageID::BCC_ALL);
        sim->unsetVerbose(StageID::OPE_ALL);
        sim->unsetVerbose(StageID::LSU_ALL);
        sim->unsetVerbose(StageID::IFU_ALL);
        sim->unsetVerbose2(StageID::OPE_ROB_FULL);
        sim->unsetVerbose2(StageID::BROB_FULL);
    }
}

static void InitConfigurations(const shared_ptr<SimSys>& sim, CommandLineArgs& args)
{
    if (!args.DelayStartTrace()) {
        EnableLog(sim, args);
    }

    if (!args.traceFileName.empty()) {
        LoggerManager::GetManager().FileLoggerRegister(args.traceFileName, false);
    }

    if (args.filterEnable) {
        LoggerManager::GetManager().EnableFilter();
        LoggerManager::GetManager().InitFilterSet(args.filterModule);
    }

    if (args.pipeViewMode != 0) {
        sim->setVerbose(StageID::BCC_ALL);
        sim->setVerbose(StageID::OPE_ALL);
        sim->setVerbose(StageID::LSU_ALL);
        args.cfgs.push_back("dfx.printPipeView=true");
        args.cfgs.push_back("dfx.pipeViewOutFile=" + args.pipeFileName);
    }

    if (args.swimLaneMode != 0) {
        args.cfgs.push_back("dfx.recordDepency=true");
        sim->InitSwimLaneLogger(args.swimLaneFile);
    }
}

static void InitRegisters(const shared_ptr<SimSys>& sim, const vector<uint64_t>& regs,
                          const map<uint64_t, uint64_t>& sysregs, uint64_t sp_addr, const CoreConfig &configs)
{
    uint64_t sp_val;

    // Reset SP register
    for (uint64_t thread = 0; thread < configs.scalar_smt_thread; thread++) {
        if (sp_addr == static_cast<uint64_t>(-1)) {
            for (int i = 0; i < static_cast<int>(GPR::GPR_COUNT); i++) {
                sim->setGPR(i, regs[i], thread);
                sim->refCore.SetGPR(i, regs[i], 0);
            }
            for (auto& it: sysregs) {
                sim->setSysreg(it.first, it.second);
                sim->refCore.SetSystemReg(it.first, it.second, thread);
            }
        } else {
            sp_val = sp_addr;
            sim->setGPR(static_cast<uint32_t>(GPR::GPR_SP), sp_val, thread);
            sim->refCore.SetGPR(static_cast<uint32_t>(GPR::GPR_SP), sp_val, thread);
        }
        sim->setSysreg(static_cast<uint64_t>(SystemReg::SYS_LXLCID), thread);
        sim->refCore.SetSystemReg(static_cast<uint64_t>(SystemReg::SYS_LXLCID), thread, thread);
        sim->setSysreg(static_cast<uint64_t>(SystemReg::SYS_TEMP_CORE_ID), thread);
        sim->refCore.SetSystemReg(static_cast<uint64_t>(SystemReg::SYS_TEMP_CORE_ID), thread, thread);
    }
}

static void InitSimulatorSystem(const shared_ptr<SimSys>& sim, CommandLineArgs& args)
{
    uint64_t start_addr;
    vector<uint64_t> regs = vector<uint64_t>(static_cast<uint64_t>(GPR::GPR_COUNT));
    std::map<uint64_t, uint64_t> sysregs;

    // enable the debug mode
    if (args.traceMode == static_cast<int>(TraceMode::DEBUGLOGGER)) {
        sim->DebugVerboseON = true;
    }

    // Build GFU System
    bool ret = sim->cfgs->Init(args.cfgs);
    ASSERT(ret && "Repeated configs input");
    sim->buildSystem();

    // Connect SOC
    if (sim->core->configs.soc_enable) {
        assert(sim->core->soc != nullptr);
        sim->core->soc->Init();
        sim->core->soc->socFreqConv = sim->core->configs.socFreqConv;
        sim->core->soc->l2SocReqQ = &sim->core->coreInterface.l2SocReqQ;
        sim->core->soc->socL2RspQ = &sim->core->coreInterface.socL2RspQ;
        sim->core->soc->bridgeSocReqQ = &sim->core->memReqQ[static_cast<int>(LSUType::BRIDGE_TABLE)];
        sim->core->soc->socBridgeRspQ = &sim->core->memRetQ[static_cast<int>(LSUType::BRIDGE_TABLE)];
        sim->core->soc->Build(LSUType::BRIDGE_TABLE, sim.get());
        for (auto &lsu : sim->core->memIntf) {
            sim->core->soc->Build(lsu->typeId, lsu);
        }
        for (auto &mtclsu : sim->core->MtcmemIntf) {
            sim->core->soc->Build(mtclsu->typeId, mtclsu);
        }
    }

    if (sim->core->configs.soc_random) {
        sim->core->socRandomModel.Build(sim.get());
    }
    // Load ELF image into data memory
    struct addr addr_ret = LoadELF(sim, regs, args.filename.c_str(), sysregs);
    start_addr = addr_ret.start_addr;

    std::shared_ptr<SoftCoreConfig> config = std::make_shared<SoftCoreConfig>();
    config->overrideDefaultConfig(sim->cfgs);
    const uint64_t stackSizeOff = 23;
    uint64_t stackSize = ((config->meta_processor_threads_num * config->meta_processor_num)) << stackSizeOff;
    sim->refCore.memory->CreateMemoryBank(addr_ret.sp_addr, stackSize);
    // Checkpoint needs to init instmemory
    if (addr_ret.sp_addr == static_cast<uint64_t>(-1)) {
        sim->ckpt_file = true;
        sim->refCore.ckpt_file = true;
        sim->refInfo.ckpt_file = true;
    }
    // Load ELF image into reference memory
    *(sim->refCore.memory) = sim->memory;

    sim->core->setSysreg(SystemReg::SYS_CYCLE, 0);

    if (args.blockNum != static_cast<uint64_t>(-1)) {
        if (args.blockNum > sim->memory.execBlockCnt) {
            cout << "Warning! The number of specified blocks is greater than the number of blocks in the elf." << endl;
        }
    }
    sim->maxBCount = std::min(sim->memory.execBlockCnt, args.blockNum);

    sim->core->Reset();
    for (uint32_t i = 0; i < sim->core->configs.scalar_smt_thread; ++i) {
        sim->refCore.ResetPC(start_addr, i);
        if (sim->core->configs.bp_mode == 0) {
            sim->core->setBPC(start_addr, i);
        }
}

    // Set warmup block number
    sim->core->setWarmup(args.blockWarmup, args.minstWarmup);

    InitRegisters(sim, regs, sysregs, addr_ret.sp_addr, sim->core->configs);

    ret = sim->cfgs->UnuseCheck();
    ASSERT(!ret && "Invalid configs input");
    ASSERT(!sim->fakeJcore && "gfsim should not be fake jcore");
}

static void RunSimulation(const shared_ptr<SimSys>& sim, CommandLineArgs& args)
{
    bool terminate = false;

    auto logTraceEnable = [sim, &args](LogStatus &logStatus) {
        if (logStatus == LogStatus::LOGS_NORMAL) {
            return;
        }
        if (logStatus == LogStatus::LOGS_WAIT_START && sim->getCycles() >= args.debugLogStartCycle) {
            EnableLog(sim, args);
            if (args.NeedEndTrace()) {
                logStatus = LogStatus::LOGS_WAIT_END;
                return;
            }
        }
        if (logStatus == LogStatus::LOGS_WAIT_END && sim->getCycles() >= args.debugLogEndCycle) {
            DisEnableLog(sim, args);
            logStatus = LogStatus::LOGS_NORMAL;
        }
    };
    LogStatus logStatus = args.GetLogStatus();

    while (!terminate) {
        logTraceEnable(logStatus);
        LoggerManager::GetManager().SetCycles(sim->getCycles());

        // Check and run reference core
        sim->RunReference(args.stopPC);
        // Progress simulator with one tick
        sim->step();
        // Redirect externa-internal queues
        redirectQueue(sim.get());
        // Perform legal check with reference
        sim->ResVerify();
        sim->PrintPipeView();
        // Enable trace at given PC
        sim->enableTrace(args.debugLogStartPC);
        // Terminate the main loop
        terminate = sim->needTerminate();

        if (sim->getCycles() >= args.stopCycles) {
            break;
        }
    }
}

//-----------------------------------------------------------------
// main
//-----------------------------------------------------------------
int main(int argc, char *argv[])
{
    InstallFatalSignalHandlers();

    struct CommandLineArgs args;

    int errCode = ParseCommandLine(argc, argv, args);
    if (errCode != 0) {
        return errCode;
    }
    // validate blockNum and blockWarmup
    if (args.blockWarmup != -1ULL && args.blockNum != -1ULL) {
        ASSERT(args.blockNum > args.blockWarmup && "total block number must be larger than warmup block number");
    }

    ProcessConfigs(args.configFiles, args.cfgs);

    if (args.filename.empty()) {
        cerr << "No ELF provided" << endl;
        return EX_USAGE;
    }

    ProcessSeedMode(args.seedMode);

    // Build simulator system
    auto sim = std::make_shared<SimSys>();
    InitConfigurations(sim, args);
    InitSimulatorSystem(sim, args);

    RunSimulation(sim, args);
    sim->ReportVectorCore();
    sim->ReportStat();
    sim->DumpSwimLaneToJson();
    if (sim->testFinisherSeen) {
        const uint16_t status = static_cast<uint16_t>(sim->testFinisherValue & 0xFFFFU);
        cout << "linx_test_finisher write addr=0x10009000 val=0x"
             << hex << setw(4) << setfill('0') << status << dec
             << (sim->testFinisherFailed ? " fail" : " pass") << endl;
        return sim->testFinisherFailed ? EX_SOFTWARE : 0;
    }
    return 0;
}

static void memFixDelaySocQueue(SimSys *sim)
{
    for (uint32_t i = 0; i < sim->core->mem_delay.size(); ++i) {
        while (!sim->core->mem_delay[i].empty()) {
            GFUMemReq req = sim->core->mem_delay[i].read();
            RandomModelSoC::CheckReqSize(req);
            RandomModelSoC::SimMemOper(sim, req);
            req.socReturnCycle = sim->getCycles();
            if ((!req.is_store) || (req.lsuTypeId == LSUType::BRIDGE_TABLE)) {
                sim->core->memRetQ[i].Write(req);
            }
        }
    }
}

static void redirectQueue(SimSys *sim)
{
    auto processMemReqQ = [](SimSys* sim) {
        for (uint32_t i = 0; i < sim->core->memReqQ.size(); ++i) {
            while (!sim->core->memReqQ[i].Empty()) {
                GFUMemReq req = sim->core->memReqQ[i].Read();
                if (req.lsuTypeId == LSUType::BRIDGE_TABLE) {
                    constexpr uint64_t bridgeCacheLineSize = 128;
                    ASSERT((req.addr & ~(bridgeCacheLineSize - 1)) == req.addr);
                    ASSERT((req.size & ~(bridgeCacheLineSize - 1)) == req.size);
                }
                req.socAaccelptCycle = sim->getCycles();
                sim->core->mem_delay[i].write(req);
            }
        }
    };

    // Check block header request queue and perform external callback
    while (!sim->core->bFetchReqQ.Empty()) {
        GFUMemReq req = sim->core->bFetchReqQ.Read();
        sim->core->bFetchRetQ.Write(req);
    }

    // Check ifetch request queue and perform external callback
    while (!sim->core->iFetchReqQ.Empty()) {
        GFUMemReq req = sim->core->iFetchReqQ.Read();
        sim->core->iFetchRetQ.Write(req);
    }

    // Check memory request queue and perform external callback
    if (!sim->core->configs.soc_enable) {
        if (!sim->core->configs.soc_random) {
            processMemReqQ(sim);
            memFixDelaySocQueue(sim);
        } else {
            sim->core->socRandomModel.Work();
            sim->core->socRandomModel.Xfer();
        }
    }

    for (uint32_t i = 0; i < sim->core->mem_delay.size(); ++i) {
        sim->core->mem_delay[i].Work();
        sim->core->mem_delay[i].Xfer();
    }
}
