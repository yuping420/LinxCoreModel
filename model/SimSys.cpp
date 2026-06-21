#include <cassert>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <unistd.h>

#include "../emulator/SoftCore.h"
#include "core/Core.h"
#include "SimSys.h"

using namespace std;
namespace JCore {

namespace {
constexpr uint64_t kLinxTestFinisherAddr = 0x10009000;
constexpr uint16_t kLinxTestFinisherPass = 0x5555;
}

void SimSys::step() {
    if (!systemStatus.EcallRunning()) {
        cycles++;
    }

    core->setSysreg(SystemReg::SYS_CYCLE,0);
    // parallel Work - update openmp in the future
    for (auto module : modules)
        module->Work();

    // transfer next state back to current
    for (auto module : modules)
        module->Xfer();
}

int64_t SimSys::SetRefInfo(int64_t recordCnt)
{
    for (int64_t i = 0; i < recordCnt; ++i) {
        if (refInfo.refTrace.checkFull()) {
            return i;
        }

        if (perfectSimON) {
            refInfo.refTrace.incWPtr();
        }
        refInfo.bptr = (refInfo.bptr + 1) % core->configs.block_rob_depth;
    }

    return recordCnt;
}

void SimSys::RunReference(uint64_t stopPC)
{
    for (uint64_t thread = 0; thread < core->configs.scalar_smt_thread; thread++) {
        for (uint64_t i = 0; i < core->bctrl->configs.bctrl_bandwidth; i++) {
            if (refCore.GetPC(thread) == stopPC) {
                break;
            }
            if (refCore.coreSimEnd || refCore.GetTotalBlockNum() >= maxBCount) {
                break;
            }
            if (resVerifyManager[thread] && resVerifyManager[thread]->RefBlockInfoListFull()) {
                break;
            }
            refCore.EmulatorBlock(thread);
            RecordTrace(thread);
            RecordPerectBPInfo(thread);
        }
    }
}

void SimSys::RecordTrace(uint64_t threadId)
{
    if (resVerifyManager[threadId] == nullptr) {
        return;
    }
    auto CvtData = [](const TileDataPtr& a, size_t size) -> std::shared_ptr<TileRegVerifyData> {
        std::shared_ptr<TileRegVerifyData> ptr = std::make_shared<TileRegVerifyData>();

        const size_t val = 4;    // data_size in model is 32bit, 32 / uint8_t = 4
        const size_t shfitSize = 8;   // TileDataPtr->datas contains uint8_t
        ptr->data.reserve(size / val + 1);
        for (size_t i = 0; i < size && i < a->datas.size(); i += val) {
            uint64_t packed = 0;
            for (size_t j = 0; j < val && (i + j) < size  && (i + j) < a->datas.size(); j++) {
                packed = (packed << shfitSize) | a->datas[i + j];
            }
            ptr->data.push_back(packed);
        }

        return ptr;
    };
    BlockFuncPtr refBlock = refCore.threadStatus[threadId].currentBlock;
    if (refBlock == nullptr) {
        return;
    }
    BlockVerifyPtr verifyBlk = std::make_shared<BlockVerifyInfo>();
    verifyBlk->isReferenc = true;
    verifyBlk->bpc = refBlock->barg.bpc;
    if (IsBlockTypeParallel(refBlock->blockType)) {
        verifyBlk->isParBlock = true;
    }
    if (verifyBlk->isParBlock && resVerifyManager[threadId]->config.verifyParBlockTileReg) {
        for (auto &dstTile : refBlock->dstTile) {
            TileDataPtr data = refCore.threadStatus[threadId].archStatus.tileReg.GetTileDataPtr(dstTile->baseAddr);
            auto cvtData = CvtData(data, dstTile->size);
            cvtData->shapeM = refBlock->lb0;
            cvtData->shapeN = refBlock->lb1;
            verifyBlk->tileRegData.push_back(cvtData);
        }
    }

    if (verifyBlk->isParBlock && !resVerifyManager[threadId]->config.verifyParBlockMinst) {
        resVerifyManager[threadId]->RecordRefBlockInfo(verifyBlk);
        return;
    }
    for (uint32_t i = 0; i < refCore.instLogs[0].size(); i++) {
        MInst& minst = refCore.instLogs[0][i];
        InstVerifyInfo verifyInst = InstVerifyInfo();
        verifyInst.isReferenc = true;
        verifyInst.isSIMTMinst = minst.codeLen == EncodeLen::ENL_V;
        verifyInst.tpc = minst.pc;
        verifyInst.data = minst.GetResult();
        verifyInst.opcode = minst.opcode;
        verifyInst.check = minst.check;
        verifyBlk->instVerifyInfoList.push_back(verifyInst);
    }
    resVerifyManager[threadId]->RecordRefBlockInfo(verifyBlk);
}

void SimSys::RecordPerectBPInfo(uint64_t threadId)
{
    if (resVerifyManager[threadId] == nullptr) {
        return;
    }
    if (core->configs.bp_mode == static_cast<uint64_t>(BP_Mode::FULL_BP)) {
        return;
    }
    BlockFuncPtr refBlock = refCore.threadStatus[0].currentBlock;
    BlockPerfectPtr perfect = std::make_shared<BlockPerfectInfo>(true);
    BlockPerfectPtr ifuPerfect = std::make_shared<BlockPerfectInfo>(false);
    perfect->bpc = refBlock->barg.bpc;
    bool notBtext = true;
    for (uint32_t i = 0; i < refCore.instLogs[0].size(); i++) {
        MInst& minst = refCore.instLogs[0][i];
        if (minst.ctGen) {
            // 模板块的 PC 无意义，不需要记录
            continue;
        }
        if (notBtext) {
            // BCC IFU
            perfect->instPCList.push_back(minst.pc);
        } else {
            // Vector IFU
            ifuPerfect->instPCList.push_back(minst.pc);
        }
        if (minst.opcode == Opcode::OP_B_TEXT) {
            notBtext = false;
        }
    }
    resVerifyManager[threadId]->RecordPerfectBlockInfo(perfect, ifuPerfect);
}

// 后续不会继续使用，但是perfect LD-ST 还未适配，代码暂时保留。
// void SimSys::runReference(uint64_t stop_pc) {
//     (void)stop_pc;
    // /* For perfect bp */
    // if (refInfo.headerTrace.size() >= core->configs.block_rob_depth) {
    //     return;
    // }
    // if ((refCore.get_pc() == stop_pc) && (stop_pc != static_cast<uint64_t>(-1))) {
    //     return;
    // }
    // if (refCore.sim_end || refCore.block_cnt >= refCore.maxBlockCnt) {
    //     return;
    // }
    // // For perfect load/store
    // if (perfectSimON && refInfo.refTrace.checkFull()) {
    //     return;
    // }
    // // For refcore
    // if (resVerifyManager && resVerifyManager->RefBlockInfoListFull()) {
    //     return;
    // }

    // // Handle for remaing ref info.
    // if (remainingRefVld && remainingRefCnt > 0) {
    //     int64_t recordCnt = SetRefInfo(remainingRefCnt);
    //     ASSERT(remainingRefCnt >= recordCnt);
    //     remainingRefCnt -= recordCnt;
    //     if (remainingRefCnt > 0) {
    //         return;
    //     }
    //     ASSERT(remainingRefCnt == 0);
    //     refCore.block_cnt++;
    //     // simt block is the last block
    //     if (refCore.block_cnt >= refCore.maxBlockCnt) {
    //         return;
    //     }
    // }

    // int error = 0;
    // //Fetch block instruction
    // BInst binst = refCore.FetchAndDecode();
    // remainingRefVld = false;
    // refSpGlobal = true;

    // // Initialize block execution
    // refCore.InitBlock(binst);

    // // Execute block instruction based on type
    // switch (binst.header.blockBstartType) {
    //     case BlockCodeType::C_BSTART_0:
    //     case BlockCodeType::BSTART_1:
    //     case BlockCodeType::L_BSTART_3:
    //     case BlockCodeType::C_BSTART_BRANCH_0:
    //     case BlockCodeType::BSTART_BRANCH_1:
    //         if (binst.header.type == BlockType::BLK_TYPE_PAR) {
    //             refCore.ExecuteFvecBlock(binst, error);
    //             refCore.startCyc = refCore.accumulateCycle;
    //             break;
    //         }
    //         refCore.ExecuteStandardBlock(binst, error);
    //         break;
    //     case BlockCodeType::MCOPY_1:
    //     case BlockCodeType::MSET_1:
    //     case BlockCodeType::FENTRY_1:
    //     case BlockCodeType::FEXIT_1:
    //     case BlockCodeType::FRET_RA_1:
    //     case BlockCodeType::FRET_STK_1:
    //     case BlockCodeType::BLBAR_1:
    //         refCore.ExecuteMemBlock(binst, error);
    //         break;
    //     case BlockCodeType::LTEMP:
    //         refCore.ExecuteLocalMemBlock(binst, error);
    //         break;
    //     case BlockCodeType::C_LOOP_0:
    //     case BlockCodeType::LOOP_1:
    //         refCore.ExecuteMultiEngine(binst, 0);
    //         refCore.prologStage();
    //         break;
    //     case BlockCodeType::PMC_BOUNDARY:
    //         break;
    //     case BlockCodeType::ACRC:
    //         if (binst.header.type == BlockType::BLK_TYPE_PAR) {
    //             refCore.ExecuteFvecBlock(binst, error);
    //             refCore.startCyc = refCore.accumulateCycle;
    //             break;
    //         } else if (binst.header.delayInst == OP_ACRC
    //                     && binst.header.acrcReqType == ACRC_REQUEST_TYPE::SCT_SYS) {
    //             refCore.ExecuteEcallBlock(binst, error);
    //         } else if (binst.header.delayInst == OP_ACRE) {
    //         }
    //         break;
    //     default:
    //         cout << "binst.header.blockBstartType is " << static_cast<int>(binst.header.blockBstartType) << endl;
    //         ASSERT(0 && "Error: Block blockBstartType not yet supported\n");
    //         break;
    // }

    // refCore.CommitBlock(binst, error);
    // refCore.block_cnt++;
    // RecordTrace(binst, false);
    // RecordRefInfo(binst, false);
    // if (!core->sRenameUnit->checkBlockRecorded(binst.header.bpc) && perfectLoadStore && perfectStackRename) {
    //     core->sRenameUnit->setHistoryTableVld(binst.header.bpc);
    // }

    // remainingRefCnt = 1;
    // if (core->configs.bp_mode != 0) {
    //     remainingRefCnt = 1;
    // }
    // uint32_t recordCnt = SetRefInfo(remainingRefCnt);
    // ASSERT(remainingRefCnt >= recordCnt);
    // remainingRefCnt -= recordCnt;
    // // It is full for queue, ref info is remaing.
    // if (remainingRefCnt > 0) {
    //     remainingRefVld = true;
    //     refCore.block_cnt--;
    // }
    // ASSERT(remainingRefCnt >= 0);
// }

// void SimSys::RecordMInst(const BInst &binst, const MInst &minst, EcallDat &dat, bool &recorded, uint32_t idx)
// {
//     //Init perfect reference info
//     RefTraceEntry &refEntry = refInfo.refTrace.back();
//     refEntry.bpc = binst.header.bpc;

//     if (binst.header.branch == BranchType::ECALL && (minst.opcode == OP_ADDI || minst.opcode == OP_ACRC)) {
//         dat.tpc.push_back(minst.tpc);
//         dat.src.push_back(minst.src2.data);
//         dat.check = false;
//     }

//     if (perfectLoadStore) {
//         LoadStoreInfo       lsInfo;
//         lsInfo.depend_id.clear();
//         if (OpcodeIsLoad(minst.opcode)) {
//             lsInfo.ref_vld = true;
//             lsInfo.addr = minst.addr;
//             lsInfo.is_load = true;
//             lsInfo.data = minst.dst0.data;
//             lsInfo.size = opcode_get_op_width(minst.opcode);
//             lsInfo.bid = refInfo.refTrace.wptr;
//             lookupRefStq(lsInfo);
//             refEntry.lsInfoQ.push_back(lsInfo);
//         } else if (OpcodeIsStore(minst.opcode)) {
//             lsInfo.ref_vld = true;
//             lsInfo.is_load = false;
//             lsInfo.id_vld = true;
//             lsInfo.addr = minst.addr;
//             lsInfo.data = minst.src0.data;
//             lsInfo.id = refInfo.refStoreCount;
//             lsInfo.size = opcode_get_op_width(minst.opcode);
//             lsInfo.bid = refInfo.refTrace.wptr;
//             updateRefStq(lsInfo);
//             refInfo.refStoreCount++;
//             refEntry.lsInfoQ.push_back(lsInfo);
//         }
//         if (!recorded) {
//             recorded = checkStackRename(refCore.traceMInsts[0], idx, binst.header.bpc);
//         }
//     }
//     if (perfectGetSet) {
//         GetSetInfo          gsInfo;
//         gsInfo.ref_vld = true;
//         gsInfo.bpc = refEntry.bpc;
//         gsInfo.tpc = minst.tpc;

//         if ((minst.src0.type == OPD_LREG || minst.src0.type == OPD_GREG)) {
//             gsInfo.src0_vld = true;
//             gsInfo.src0_data = minst.src0.data;
//         }
//         if (minst.src1.type == OPD_LREG || minst.src1.type == OPD_GREG) {
//             gsInfo.src1_vld = true;
//             gsInfo.src1_data = minst.src1.data;
//         }
//         if (minst.src2.type == OPD_LREG || minst.src2.type == OPD_GREG) {
//             gsInfo.src2_vld = true;
//             gsInfo.src2_data = minst.src2.data;
//         }
//         if (minst.src3.type == OPD_LREG || minst.src3.type == OPD_GREG) {
//             gsInfo.src3_vld = true;
//             gsInfo.src3_data = minst.src3.data;
//         }
//         if (minst.dst1.type == OPD_LREG || minst.dst1.type == OPD_GREG) {
//             gsInfo.dst1_vld = true;
//             gsInfo.dst1_data = minst.dst1.data;
//         }
//         refEntry.gsInfoQ.push_back(gsInfo);
//     }
//     return;
// }

// void SimSys::RecordTrace(BInst &binst, bool partly) {
//     // Init hyper block trace.
//     bool recorded = core->sRenameUnit->checkBlockRecorded(binst.header.bpc) && !perfectStackRename;
//     if (partly) {
//         recorded = true;
//     }
//     EcallDat dat;
//     if (binst.header.branch == BranchType::ECALL) {
//         dat.bpc = binst.header.bpc;
//         dat.bid = refInfo.bptr;
//     }
//     // Push micro-instruction results into queue
//     vector<uint32_t> instIds(refCore.traceMInsts.size(), 0);
//     for (uint32_t i = 0; i < refCore.traceMInsts[0].size(); i++) {
//         uint32_t lane_size = 1;
//         MInst& minst = refCore.traceMInsts[0][i];
//         if (minst.isSimtOp) {
//             lane_size = refCore.bstate.size();
//         }
//         for (uint32_t lane = 0; lane < lane_size; ++lane) {
//             uint32_t instId = instIds[lane];
//             ASSERT(lane < refCore.traceMInsts.size());
//             ASSERT(instId < refCore.traceMInsts[lane].size());
//             auto &minst = refCore.traceMInsts[lane][instId];
//             RecordMInst(binst, minst, dat, recorded, instId);
//             ++instIds[lane];
//         }
//     }
//     if (binst.header.branch == BranchType::ECALL) {
//         refInfo.ecall_data.push_back(dat);
//     }

//     // provide information about fetching bin to the CA model in perfect bp mode
//     if (core->configs.bp_mode != static_cast<uint64_t>(BP_Mode::FULL_BP)) {
//         // headerTrace for perfect bp
//         FetchInfo fetchInfo = FetchInfo();
//         fetchInfo.pc = binst.header.bpc;
//         fetchInfo.size = binst.header.hsize;
//         fetchInfo.bnextSize = binst.header.bnextExtendSize;
//         fetchInfo.isHeader = true;
//         fetchInfo.branchTaken = binst.header.branchTaken;
//         fetchInfo.bin = binst.header.bin;
//         uint64_t offset = 0;
//         if (binst.header.startBin) {
//             fetchInfo.bin = binst.header.startBin;
//             fetchInfo.size = ((binst.header.startBin & 0x1) == 1) ? WIDTH_32: WIDTH_16;
//             offset = fetchInfo.size;
//             fetchInfo.battrCnt = binst.header.battr.size();
//         }
//         refInfo.headerTrace.push(fetchInfo);
//         for (uint32_t i = 0; i < binst.header.battr.size(); i++) {
//             fetchInfo.bin = binst.header.battr[i];
//             fetchInfo.size = ((binst.header.battr[i] & 0x1) == 1) ? WIDTH_32: WIDTH_16;
//             fetchInfo.pc += offset;
//             refInfo.headerTrace.push(fetchInfo);
//             offset = fetchInfo.size;
//         }

//         // minst tarce for perfect bp
//         for (uint32_t pos = 0; pos < binst.minsts.size(); ++pos) {
//             auto &minst = binst.minsts[pos];
//             if (!minst.needInPerfBp) {  // only the required minsts send request
//                 continue;
//             }
//             fetchInfo = FetchInfo();
//             fetchInfo.pc = minst.tpc;
//             fetchInfo.size = minst.msize;
//             fetchInfo.isMinst = true;
//             fetchInfo.bin = minst.encoding;
//             // The last non-bstop instruction is the last instruction of this Block
//             if ((pos == binst.minsts.size() - 1 && minst.opcode != OP_B_STOP) ||
//                 (pos == binst.minsts.size() - 2 && binst.minsts[pos + 1].opcode == OP_B_STOP)) {
//                 fetchInfo.isLastInst = true;
//             }
//             if (binst.header.type != BlockType::BLK_TYPE_PAR) {
//                 refInfo.headerTrace.push(fetchInfo);
//             }
//         }

//         // at the end of the execution, an extra block header needs to be sent
//         if (((refCore.block_cnt >= refCore.maxBlockCnt) || refCore.sim_end)) {
//             fetchInfo = FetchInfo();
//             fetchInfo.pc = binst.header.bpc;
//             fetchInfo.size = binst.header.hsize;
//             fetchInfo.bnextSize = binst.header.bnextExtendSize;
//             fetchInfo.isHeader = true;
//             fetchInfo.bin = binst.header.bin;
//             refInfo.headerTrace.push(fetchInfo);
//         }
//     }
// }


// void SimSys::RecordRefInfo(BInst &binst, bool partly)
// {
//     (void)partly;
//     if (resVerifyManager == nullptr) {
//         return;
//     }
//     BlockVerifyPtr verifyBlk = std::make_shared<BlockVerifyInfo>();
//     verifyBlk->isReferenc = true;
//     verifyBlk->bpc = binst.header.bpc;
//     if (binst.header.type == BlockType::BLK_TYPE_PAR) {
//         verifyBlk->isParBlock = true;
//     }
//     if (verifyBlk->isParBlock && resVerifyManager->config.verifyParBlockTileReg) {
//         verifyBlk->bpc = binst.header.bstartBpc; // Adapt after delete origin DFX
//         for (auto &it : refCore.tileRegDataList) {
//             verifyBlk->tileRegData.push_back(it);
//         }
//     }

//     if (verifyBlk->isParBlock && !resVerifyManager->config.verifyParBlockMinst) {
//         resVerifyManager->RecordRefBlockInfo(verifyBlk);
//         return;
//     }
//     std::vector<uint32_t> instIds(refCore.traceMInsts.size(), 0);
//     for (uint32_t i = 0; i < refCore.traceMInsts[0].size(); i++) {
//         MInst& minst = refCore.traceMInsts[0][i];
//         if (minst.cubeData) {
//             verifyBlk->bpc = binst.header.bstartBpc; // Adapt after delete origin DFX
//             continue;
//         }
//         uint32_t lane_size = minst.isSimtOp ? refCore.lane_num : 1;
//         for (uint32_t lane = 0; lane < lane_size; ++lane) {
//             uint32_t instId = instIds[lane];
//             ASSERT(lane < refCore.traceMInsts.size());
//             ASSERT(instId < refCore.traceMInsts[lane].size());
//             auto &minst = refCore.traceMInsts[lane][instId];
//             InstVerifyInfo verifyInst = InstVerifyInfo();
//             verifyInst.isReferenc = true;
//             verifyInst.isSIMTMinst = minst.isSimtOp;
//             verifyInst.tpc = minst.tpc;
//             verifyInst.data = minst.dst0.data;
//             verifyInst.opcode = minst.opcode;
//             verifyInst.check = minst.check;
//             verifyBlk->instVerifyInfoList.push_back(verifyInst);
//             ++instIds[lane];
//         }
//     }
//     resVerifyManager->RecordRefBlockInfo(verifyBlk);
// }

void SimSys::Reset()
{

}

void SimSys::enableTrace(uint64_t trace_pc) {
    /* if (refCore.get_pc() == trace_pc) {
        setVerbose(StageID::BCC_ALL);
        setVerbose(StageID::OPE_ALL);
    } */
}

void SimSys::ReleaseRefCoreInfo(bool isLastHeader)
{
    if (perfectSimON) {
        if ((core->configs.bp_mode == 0) || (core->configs.bp_mode != 0 && isLastHeader)) {
            dequeRefStq(refInfo.refTrace.rptr);
            refInfo.refTrace.pop_front();
        }
    }
}

void SimSys::printInst(uint64_t bpc, uint64_t tpc, ostream &out) {
    (void)bpc,
    (void)tpc,
    (void)out;
}

// ECALL template 需要，暂时未适配。
void SimSys::setData(MInst &inst, BlockType type)
{
    (void)inst;
    (void)type;
    // inst.src0.data = inst.src0.type == OPD_INVALID ? 0 : refCore.getOpData(inst.src0);
    // inst.src1.data = inst.src1.type == OPD_INVALID ? 0 : refCore.getOpData(inst.src1);
    // inst.src2.data = inst.src2.type == OPD_INVALID ? 0 : refCore.getOpData(inst.src2);
    // inst.src3.data = inst.src3.type == OPD_INVALID ? 0 : refCore.getOpData(inst.src3);
}

void SimSys::printMInst(uint64_t tpc, ostream &out, BlockType type) {
    (void)tpc;
    (void)out;
    (void)type;
}

void SimSys::ReportStat() {
    if (!cycles) return;
    if (!correctBCount) return;
    core->ReportStat();
}

void SimSys::ReportVectorCore() {
    if (!cycles) return;
    if (!correctBCount) return;

    auto t = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now() - simStartTime);
    cout << "Simulation time: " << dec << t.count() << "s " << endl;
}

void SimSys::resetStats() {
    core->resetStats();
}

bool SimSys::needTerminate() {
    bool term = true;
    for (size_t i = 0; i < terminate.size(); ++i) {
        term = (term && terminate[i]);
    }
    if (term) {
        return true;
    }
    if (testFinisherSeen) return true;
    if (correctBCount == maxBCount) return true;
    return false;
}

void SimSys::observeTestFinisher(uint64_t address, uint64_t data, int width)
{
    for (int i = 0; i < width; i++) {
        uint64_t byteAddr = address + i;
        uint64_t byteData = (data >> (i * 8)) & 0xFF;
        if (byteAddr < kLinxTestFinisherAddr || byteAddr >= kLinxTestFinisherAddr + sizeof(uint32_t)) {
            continue;
        }
        uint32_t finisherByte = static_cast<uint32_t>(byteAddr - kLinxTestFinisherAddr);
        uint64_t shift = finisherByte * 8;
        testFinisherValue = (testFinisherValue & ~(0xFFULL << shift)) | (byteData << shift);
        testFinisherByteMask |= (1U << finisherByte);
        if ((testFinisherByteMask & 0x3U) != 0x3U) {
            continue;
        }
        uint16_t status = static_cast<uint16_t>(testFinisherValue & 0xFFFFU);
        testFinisherSeen = true;
        testFinisherFailed = (status != kLinxTestFinisherPass);
    }
}

bool SimSys::buildSystem() {
    core = std::make_shared<Core>();
    addModule(core);
    core->buildCore(this);
    terminate.resize(core->configs.scalar_smt_thread, false);
    refCore.config.multiThreadNum = core->configs.scalar_smt_thread;
    refCore.config.laneNum = core->configs.simtLane;
    refCore.Init();
    refCore.recordMInst = true;

    refInfo.refTrace = CycArray<RefTraceEntry> (core->configs.block_rob_depth);
    perfectGetSet = core->configs.perfect_get_set;
    perfectLoadStore = core->configs.perfect_load_store;
    perfectSimON = perfectGetSet || perfectLoadStore;

    return true;
}

Core* SimSys::GetCore()
{
    return core->GetThis();
}

void SimSys::addModule(std::shared_ptr<SimObj> m) {
    modules.push_back(m);
}

void SimSys::setGPR(uint32_t id, uint64_t data, uint32_t stid) {
    core->setGPR(id, data, stid);
}

void SimSys::setSysreg(uint32_t id, uint64_t data) {
    core->setSysreg(static_cast<SystemReg>(id), data);
}

FRMMode SimSys::GetFRM()
{
    uint64_t cState = core->getSysreg(SystemReg::SYS_CSTATE);
    uint64_t bits = GetBits(cState, FRM_BIT_BEGIN, FRM_BIT_END);
    if (bits >= static_cast<uint64_t>(FRMMode::FRM_RNA)) {
        return FRMMode::FRM_RNE;
    }
    return static_cast<FRMMode>(bits);
}

uint64_t SimSys::getGPR(uint32_t id, uint32_t stid) {
    return core->getGPR(id, stid);
}

uint64_t SimSys::fetchData(uint64_t address, int width) {
    uint64_t data;
    if (ckpt_file) {
        // change to byte aaccelss
        data = 0;
        for (int i = 0; i < width; i++) {
            uint64_t byte = memory.Load(address+i, 1, false);
            data = data | (byte << (8*i));
        }
    } else {
        // change to byte aaccelss
        data = loadData(address, width, false);
    }

    return data;
}

uint64_t SimSys::loadData(uint64_t address, int width, bool signedLoad) {
    // change to byte aaccelss to avoid cross section
    uint64_t result = 0;
    uint64_t pivot = 1;
    pivot = (pivot<<((width*8)-1));
    for (int i = 0; i <width; i++) {
        uint64_t b = memory.Load(address+i, 1, false);
        result += ((b&0xFF)<<(i*8));
    }
    if (signedLoad && (result & pivot)) {
        uint64_t comp = 0;
        uint64_t byte = 0xFF;
        for (int i=7; i>=width; i--) {
            comp += (byte<<(i*8));
        }
        result += comp;
    }
    return result;
}

void SimSys::storeData(uint64_t address, uint64_t data, int width) {
    observeTestFinisher(address, data, width);
    // change to byte aaccelss to avoid cross section
    for (int i = 0; i <width; i++) {
        memory.Store(address+i, (data>>(i*8))&0xFF, 1);
    }
}

void SimSys::setVerbose(StageID stageID) {
    verboseSwitch.insert(stageID);
}

void SimSys::setVerbose2(StageID stageID) {
    verboseSwitch2.insert(stageID);
}

void SimSys::unsetVerbose(StageID stageID) {
    verboseSwitch.erase(stageID);
    if (!verboseSwitch.size())
        verboseON = false;
}

void SimSys::unsetVerbose2(StageID stageID) {
    verboseSwitch2.erase(stageID);
    if (!verboseSwitch2.size())
        verboseON2 = false;
}

// Stack Rename 需要，后续适配，代码暂时保留。
// bool operandIsSP(Operand reg) {
//     return ((reg.type == OPD_LREG || reg.type == OPD_GREG) && regName[reg.value] == "sp");
// }

// bool operandIsLocalSP(Operand reg) {
//     return (reg.type == OPD_LREG && regName[reg.value] == "sp");
// }

// bool isLocalSetSP(MInst minst) {
//     return (operandIsLocalSP(minst.dst0) || operandIsLocalSP(minst.dst1));
// }

// bool isGlobalSP(Operand reg) {
//     return (reg.type == OPD_GREG && regName[reg.value] == "sp");
// }

// bool isStackLS(std::vector<MInst> minsts, uint32_t index) {
//     auto &minst = minsts[index];
//     if (!OpcodeIsLoadImm(minst.opcode) && !OpcodeIsStore(minst.opcode)) {
//         return false;
//     }
//     if (OpcodeIsStoreReg(minst.opcode)) {
//         return false;
//     }
//     if (OpcodeIsLoad(minst.opcode) && (!operandIsSP(minst.src0) || minst.src1.type != OPD_INVALID)) {
//         return false;
//     }
//     return true;
// }

// bool isRegType(Operand reg) {
//     return (reg.type == OPD_LREG || reg.type == OPD_GREG);
// }

// bool isSpOffset(MInst minst) {
//     if (minst.opcode != OP_ADDI && minst.opcode != OP_SUBI) {
//         return false;
//     }
//     return (operandIsLocalSP(minst.src0) && isGlobalSP(minst.dst1));
// }

// bool SimSys::checkStackRename(std::vector<MInst> minsts, uint32_t index, uint64_t bpc) {
//     if (!perfectStackRename) {
//         return true;
//     }
//     auto &minst = minsts[index];
//     bool spGlobal = refSpGlobal;
//     if (operandIsLocalSP(minst.dst0) || operandIsLocalSP(minst.dst1)) {
//         refSpGlobal = false;
//     }
//     // The block stop record for stack rename
//     if (OpcodeIsInnerJump(minst.opcode)) {
//         return true;
//     }
//     bool sp_set_local = isLocalSetSP(minst);
//     // The block continue recording
//     if (!isStackLS(minsts, index) && !(isSpOffset(minst) && spGlobal)) {
//         return sp_set_local;
//     }
//     // Store rename conditions :
//     // Condition 1 : const/lconst, add t#1, sp, store Tx, [t#1, imm]
//     // Condition 2 : store Tx, [sp, imm]
//     int64_t offImm = 0;
//     bool stack = true;
//     bool indirect = false;
//     if (OpcodeIsStore(minst.opcode)) {
//         if (minst.src1.type == OPD_LINK) {
//             if (index < minst.src1.value) {
//                 return sp_set_local;
//             }
//             auto &srcInst = minsts[index-minst.src1.value];
//             if ((srcInst.opcode != OP_ADDI && srcInst.opcode != OP_SUBI) || !operandIsSP(srcInst.src0)) {
//                 stack = false;
//             } else {
//                 // TODO
//                 stack = false;
//                 offImm = getCvtVal(srcInst.src2.cvt, srcInst.src2.shamt, srcInst.src2.value);
//                 indirect = true;
//                 if (srcInst.opcode == OP_SUBI)
//                     offImm = -offImm;
//             }
//         } else if (!operandIsSP(minst.src1)) {
//             stack = false;
//         }
//     }
//     if (!stack) {
//         return sp_set_local;
//     }
//     int64_t offset = getCvtVal(minst.src2.cvt, minst.src2.shamt, minst.src2.value);
//     offset += offImm;
//     if (minst.opcode == OP_ADDI) {
//         offset = -offset;
//     }
//     StackRenameBus req;
//     req.vld = true;
//     if (OpcodeIsLoad(minst.opcode)) {
//         req.type = StackInstType::STACK_GET;
//     } else if (OpcodeIsStore(minst.opcode)) {
//         req.type = StackInstType::STACK_SET;
//     } else {
//         req.type = StackInstType::SET_SP;
//     }
//     req.calculated = indirect;
//     req.imm = offset;
//     req.tpc = minst.tpc;
//     // cout<<"record BPC:0x"<<hex<<bpc<<" offset:"<<dec<<offset<<" "<<offImm<<" minst: "<<minst<<endl;
//     core->sRenameUnit->setHistoryTable(req, bpc);
//     // Local SP is been modified, the block stop record for stack rename
//     return sp_set_local;
// }

void SimSys::updateRefStq(LoadStoreInfo &lsInfo) {
    LoadStoreInfo e;
    e.id_vld = true;
    e.is_load = lsInfo.is_load;
    e.id = lsInfo.id;
    e.addr = lsInfo.addr;
    e.data = lsInfo.data;
    e.size = lsInfo.size;
    e.bid = lsInfo.bid;
    refInfo.refStq.emplace_back(e);
}

bool ls_overlap(uint64_t ld_addr, uint32_t ld_size, uint64_t st_addr, uint32_t st_size) {
    uint64_t ld_addr_end = ld_addr + ld_size - 1;
    uint64_t st_addr_end = st_addr + st_size - 1;
    return !(ld_addr_end < st_addr || ld_addr > st_addr_end);
}

void SimSys::lookupRefStq(LoadStoreInfo &ldInfo) {
    ldInfo.id_vld = false;
    for (auto it = refInfo.refStq.begin(); it != refInfo.refStq.end(); it++) {
        if (!it->id_vld || it->is_load)
            continue;
        // Check load store overlap
        if (ls_overlap(ldInfo.addr, ldInfo.size, it->addr, it->size)) {
            ldInfo.depend_id.push_back(it->id);
            ldInfo.id_vld = true;
        }
    }
    // Record depend store insts
    // std::unordered_map<uint64_t, bool> id_map;
    // for (uint64_t offset = 0; offset < ldInfo.size; offset++) {
    //     uint64_t addr = ldInfo.addr + offset;
    //     if (addr_map[addr].vld && !id_map[addr_map[addr].depend_id]) {
    //         ldInfo.depend_id.push_back(addr_map[addr].depend_id);
    //         id_map[addr_map[addr].depend_id] = true;
    //     }
    // }
}

bool SimSys::checkRefLoadReady(LoadStoreInfo &ldInfo) {
    for (auto it = ldInfo.depend_id.begin(); it != ldInfo.depend_id.end(); ) {
        if (lookupRefStqID(*it)) {
            it = ldInfo.depend_id.erase(it);
        } else {
            it++;
        }
    }
    return ldInfo.depend_id.empty();
}

bool SimSys::lookupRefStqID(uint64_t id) {
    if (refInfo.refStq.empty() || (!refInfo.refStq.empty() && refInfo.refStq.front().id > id)) {
        return true;
    }
    auto it = find_if(refInfo.refStq.begin(), refInfo.refStq.end(), [&](LoadStoreInfo& e){ return id==e.id; });
    return ( it == refInfo.refStq.end() || (it != refInfo.refStq.end() && !it->id_vld) );
}

void SimSys::dequeRefStq(uint64_t bid) {
    for (auto it = refInfo.refStq.begin(); it != refInfo.refStq.end(); ) {
        if (it->bid == bid) {
            it = refInfo.refStq.erase(it);
        } else {
            it++;
        }
    }
}

void SimSys::unsetRefStq(uint64_t id) {
    if (!refInfo.refStq.empty() && (refInfo.refStq.front().id > id || id > refInfo.refStq.back().id))
        return;
    for (auto it = refInfo.refStq.begin(); it != refInfo.refStq.end(); it++) {
        if (it->id == id) {
            it->id_vld = false;
            break;
        }
    }
}

void SimSys::recoverBlockRefStq(uint64_t bid) {
    for (auto it = refInfo.refStq.begin(); it != refInfo.refStq.end(); it++) {
        if (it->bid == bid) {
            it->id_vld = true;
        }
    }
}

void SimSys::recoverRefStq(uint64_t id) {
    for (auto it = refInfo.refStq.begin(); it != refInfo.refStq.end(); it++) {
        if (it->id == id) {
            it->id_vld = true;
            break;
        }
    }
}

// to avoid innerflush caused by stackrename
void SimSys::DeleteInfoQ(uint64_t sid) {
    auto flushId = [&sid] (std::deque<uint64_t> &depend_id) {
        for (auto it = depend_id.begin(); it != depend_id.end();) {
            if (*it == sid) {
                it = depend_id.erase(it);
            } else {
                it++;
            }
        }
    };
    for (uint32_t i = 0; i < core->configs.block_rob_depth; i++) {
        std::deque<LoadStoreInfo> &lsInfoQ = refInfo.refTrace.entry[i].lsInfoQ;
        for (auto it = lsInfoQ.begin(); it != lsInfoQ.end(); it++) {
            flushId(it->depend_id);
        }
    }
}

SimSys::~SimSys()
{
}

void SimSys::InitSwimLaneLogger(const std::string &fileName)
{
    swimLaneFile = fileName;
    swimLogger = std::make_shared<TraceLog::TraceLogger>();
    swimLogger->sim = this;
}

std::shared_ptr<TraceLog::TraceLogger> SimSys::GetSwimLogger()
{
    return swimLogger;
}

void SimSys::DumpSwimLaneToJson()
{
    if (swimLogger == nullptr) {
        return;
    }
    auto log = swimLogger->ToJson();
    std::ofstream os(swimLaneFile);
    os << log.dump(1) << std::endl;
    os.close();
}

void SimSys::ObjRegisterLogInfo(std::shared_ptr<SimObj> obj, size_t nameSeq, size_t globalSeq, string threadName)
{
    if (swimLogger == nullptr) {
        return;
    }

    std::string name = MachineName(obj->machineType) + "_" + std::to_string(nameSeq);
    std::string machineViewName = MachineName(obj->machineType) + "_Machine_View";
    swimLogger->SetThreadName(name + threadName, CORE_TOP_MACHINE_ID, obj->machineId);
    swimLogger->SetProcessName(name, obj->machineId, globalSeq);
    swimLogger->SetThreadName(machineViewName, obj->machineId, CORE_INTER_VIEW_TID);
}

void SimSys::RegisterMultiThread(std::shared_ptr<SimObj> obj, uint64_t num, size_t seq,
                                 unordered_map<uint64_t, string> threadNameMap)
{
    if (swimLogger == nullptr) {
        return;
    }
    std::string name = MachineName(obj->machineType) + "_" + std::to_string(seq);
    for (uint64_t i = 1; i < num; i++) {
        string threadName = name;
        if (threadNameMap.count(i) != 0) {
            threadName = name + threadNameMap.at(i);
        }
        swimLogger->SetThreadName(threadName, CORE_TOP_MACHINE_ID, obj->machineId + i);
    }
}

void SimSys::AddDependency(uint64_t blockId, int eventId, uint64_t scalarThreadId)
{
    if (GetSwimLogger() == nullptr) {
        return;
    }
    auto &vec = viewManager[scalarThreadId]->GetBlockSrcVec(blockId);
    for (size_t i = 0; i < vec.size(); i++) {
        GetSwimLogger()->AddFlow(vec[i], eventId);
    }
}

void SimSys::AddEventBegin(std::string name, Pid pid, Tid tid, std::string hint)
{
    if (GetSwimLogger() == nullptr) {
        return;
    }
    TimeStamp timestamp = getCycles();
    GetSwimLogger()->AddEventBegin(name, pid, tid, timestamp, hint);
}

void SimSys::AddEventEnd(Pid pid, Tid tid)
{
    if (GetSwimLogger() == nullptr) {
        return;
    }
    TimeStamp timestamp = getCycles();
    GetSwimLogger()->AddEventEnd(pid, tid, timestamp);
}

void SimSys::AddDuration(SwimLogData &logData)
{
    if (GetSwimLogger() == nullptr) {
        return;
    }
    GetSwimLogger()->AddDuration(logData);
}

int SimSys::GetEventId()
{
    if (GetSwimLogger() == nullptr) {
        return -1;
    }
    return GetSwimLogger()->GetEventId();
}

void SimSys::BuildViewManager(uint64_t depth, uint64_t threadNum)
{
    for (uint64_t thread = 0; thread < threadNum; thread++) {
        std::shared_ptr<ViewManager> view = std::make_shared<ViewManager>();
        view->sim = this;
        view->InitViewManager(depth);
        viewManager.emplace_back(view);
    }
}

std::shared_ptr<ViewManager> SimSys::GetViewManager(uint64_t threadId)
{
    return viewManager[threadId];
}

void SimSys::BuildVerifyManager(uint64_t depth, uint64_t threadNum)
{
    for (uint64_t thread = 0; thread < threadNum; thread++) {
        std::shared_ptr<ResVerifyManager> verify = std::make_shared<ResVerifyManager>();
        verify->sim = this;
        verify->Init(depth);
        resVerifyManager.emplace_back(verify);
    }
}

std::shared_ptr<ResVerifyManager> SimSys::GetVerifyManager(uint64_t threadId)
{
    return resVerifyManager[threadId];
}

void SimSys::ResetWaitCycle()
{
    for (auto &res : resVerifyManager) {
        res->ResetWaitCycle();
    }
}

void SimSys::ResVerify()
{
    bool ret = true;
    for (uint64_t thread = 0; thread < core->configs.scalar_smt_thread; thread++) {
        ret = resVerifyManager[thread]->Verify(core->bctrl->configs.bctrl_bandwidth);
        correctBCount = resVerifyManager[thread]->GetVerifiedBlockCount();
        correctICount = resVerifyManager[thread]->GetverifiedMinstCount();
        if (!ret) {
            // print rob information
            LOG_ERROR << "res verify failed";
            core->PrintCoreStatus(thread);
            LOG_ERROR << resVerifyManager[thread]->GetErrorInfo();
            ASSERT(false && "res verify failed");
        }
    }
}

void SimSys::PrintPipeView()
{
    for (uint64_t thread = 0; thread < core->configs.scalar_smt_thread; thread++) {
        viewManager[thread]->PrintPipeView(core->bctrl->configs.bctrl_bandwidth);
    }
}

bool SystemStatus::EcallRunning()
{
    return (ecallStatus == EcallStatus::EXECUTE || ecallStatus == EcallStatus::COMMITTED);
}
} // namespace JCore
