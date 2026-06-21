#pragma once

#ifndef ARCH_STATUS
#define ARCH_STATUS

#include "ISA.h"
#include "TileReg.h"
#include "../common/DataStruct/RingQ.h"
namespace JCore {
struct ArchStatus {
    uint64_t                        bpc = 0;
    std::vector<uint64_t>           gpr;
    std::map<SystemReg, uint64_t>   sysreg;
    TileReg                         tileReg;

    ArchStatus()
    {
        gpr.clear();
        sysreg.clear();
        gpr.resize(static_cast<size_t>(GPR::GPR_COUNT), 0);
    }
};

struct PredictMaskEntry {
    std::vector<bool> mPred;

    PredictMaskEntry()
    {
        SetLen(1U);
    }
    ~PredictMaskEntry() = default;

    std::string Dump() const
    {
        std::stringstream oss;
        oss << std::noboolalpha;
        for (int i = mPred.size() - 1; i >= 0; --i) {
            oss << mPred.at(i);
        }
        return oss.str();
    }

    bool GetMask(uint32_t index = 0U) const
    {
        return mPred.at(index);
    }

    void SetLen(uint32_t len)
    {
        mPred.resize(len, true);
    }

    uint32_t GetLen() const
    {
        return mPred.size();
    }

    void SetMask(bool value, uint32_t index = 0U)
    {
        mPred.at(index) = value;
    }
};

struct Layer2ArchStatus {
    using GeneralRegs = std::unordered_map<OperandType, RingQ<uint64_t>>;
    GeneralRegs                 scalarGeneralReg;
    std::vector<GeneralRegs>    vectorGeneralReg;
    std::vector<PredictMaskEntry> predMask;

    uint64_t                    lc0 = 0; // completed laneNum
    uint64_t                    lc1 = 0;
    uint64_t                    lc2 = 0;

    Layer2ArchStatus() = default;

    void InitScalarGeneralReg(uint64_t scalarMaxIndex, uint64_t scalarMaxOutputNum)
    {
        scalarGeneralReg.clear();
        scalarGeneralReg[OperandType::OPD_TLINK] = RingQ<uint64_t>(scalarMaxIndex + scalarMaxOutputNum, scalarMaxIndex,
                                                                   "Scalar T Reg");
        scalarGeneralReg[OperandType::OPD_ULINK] = RingQ<uint64_t>(scalarMaxIndex + scalarMaxOutputNum, scalarMaxIndex,
                                                                   "Scalar U Reg");
    }

    void InitPredMask(uint64_t totalLane, uint64_t execLane)
    {
        predMask.resize(totalLane);
        for (uint64_t i = 0; i < totalLane; ++i) {
            predMask[i].SetMask(i < execLane); // mask = true if i < execLane else false
        }
    }

    std::vector<bool> GetLaneMask()
    {
        std::vector<bool> res;
        for (auto &it : predMask) {
            res.push_back(it.GetMask());
        }
        return res;
    }

    void InitVectorGeneralReg(uint64_t execLane, uint64_t vectorMaxIndex, uint64_t vectorMaxOutputNum,
                              VREGMode vregMode)
    {
        vectorGeneralReg.clear();
        for (uint64_t i = 0; i < execLane; i++) {
            std::unordered_map<OperandType, RingQ<uint64_t>> vecReg;
            std::string laneL = "lane" + std::to_string(i);
            switch (vregMode) {
                case VREGMode::VS8:
                    vecReg[OperandType::OPD_VTLINK] = RingQ<uint64_t>(vectorMaxIndex + vectorMaxOutputNum,
                                                                      vectorMaxIndex, laneL + "vector VT Reg");
                    vecReg[OperandType::OPD_VULINK] = RingQ<uint64_t>(vectorMaxIndex + vectorMaxOutputNum,
                                                                      vectorMaxIndex, laneL + "vector VU Reg");
                    break;
                case VREGMode::VS16:
                    vecReg[OperandType::OPD_VTLINK] = RingQ<uint64_t>(vectorMaxIndex + vectorMaxOutputNum,
                                                                      vectorMaxIndex, laneL + "vector VT Reg");
                    vecReg[OperandType::OPD_VULINK] = RingQ<uint64_t>(vectorMaxIndex + vectorMaxOutputNum,
                                                                      vectorMaxIndex, laneL + "vector VU Reg");
                    vecReg[OperandType::OPD_VMLINK] = RingQ<uint64_t>(vectorMaxIndex + vectorMaxOutputNum,
                                                                      vectorMaxIndex, laneL + "vector VM Reg");
                    vecReg[OperandType::OPD_VNLINK] = RingQ<uint64_t>(vectorMaxIndex + vectorMaxOutputNum,
                                                                      vectorMaxIndex, laneL + "vector VN Reg");
                    break;
                default:
                    ASSERT(false) << "Unsupport vregmodel" << GetVREGModeName(vregMode);
            }
            vectorGeneralReg.push_back(vecReg);
        }
    }

    void Reset()
    {
        for (auto &it : scalarGeneralReg) {
            it.second.Reset();
        }
        for (auto &laneStatus : vectorGeneralReg) {
            for (auto &it : laneStatus) {
                it.second.Reset();
            }
        }
    }
};
}
#endif
