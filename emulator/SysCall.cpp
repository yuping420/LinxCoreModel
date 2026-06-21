#include <cstdint>

#include "SysCall.h"
#include "SoftCore.h"

namespace JCore {

bool SoftCore::ExecuteSysCall(BlockFuncPtr &currentBlock)
{
    uint64_t xbCid = 0xffffffffffffffff;
    auto &archStatus = threadStatus[currentBlock->threadId].archStatus;
    /* handle ecall in elf */
    if (!ckpt_file) {
        EcallAgent<uint64_t, SoftMemory> eca(archStatus.gpr.data(), xbCid, *memory);
        archStatus.gpr[2] = eca();

        MInst minst;
        [[maybe_unused]] uint64_t tpc = 0;
        [[maybe_unused]] uint32_t id = 0;

        std::unordered_map<int, Opcode> get_st_op = {
            {1,    Opcode::OP_SBI},
            {2,    Opcode::OP_SHI},
            {4,    Opcode::OP_SWI},
            {8,    Opcode::OP_SDI},
        };
        for (int i = 0; i < static_cast<int>(GPR::GPR_COUNT); i++) {
            minst = MInst();
            minst.check = false;
            tpc += 4;

            minst.opcode = Opcode::OP_ADDI;
            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, archStatus.gpr[i], archStatus.gpr[i]));
            minst.dsts.emplace_back(std::make_shared<Operand>(OperandType::OPD_GREG, i, archStatus.gpr[i]));
            // TODO: gfsim adapt

            if (verbose) {
                LOG_INFO << minst.Dump();
            }
            ++id;
        }
        size_t ecaAccStoreInfoVecSize = eca.aaccelssor.storeInfoVector.size();
        for (size_t i = 0; i < ecaAccStoreInfoVecSize; i++) {
            uint64_t adr = eca.aaccelssor.storeInfoVector[i].addr;
            uint64_t dat = eca.aaccelssor.storeInfoVector[i].data;
            int len = eca.aaccelssor.storeInfoVector[i].len;

            // addi zero, data
            minst = MInst();
            minst.check = false;
            tpc += 4;

            minst.opcode = Opcode::OP_ADDI;
            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0, 0));
            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, dat, dat));
            minst.dsts.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0, dat));

            if (verbose) {
                LOG_INFO << minst.Dump();
            }
            ++id;

            minst = MInst();
            minst.check = false;
            tpc += 4;

            // addi zero, addr
            minst.opcode = Opcode::OP_ADDI;

            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0, 0));
            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, adr, adr));
            minst.dsts.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, adr, adr));

            if (verbose) {
                LOG_INFO << minst.Dump();
            }
            ++id;

            minst = MInst();
            minst.check = false;
            tpc += 4;

            // sdi t#2 [t#1, 0]
            memory->Store(adr, dat, len);
            minst.opcode = get_st_op[len];

            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1, dat));
            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0, adr));
            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0, 0));

            if (verbose) {
                LOG_INFO << minst.Dump();
            }
            ++id;
        }

        /* generate acrc at end */
        minst = MInst();
        minst.check = false;
        tpc += 4;
        minst.opcode = Opcode::OP_ACRC;

        /* exit_group is sim_end */
        // ACRC标志为结束的系统调用
        if (archStatus.gpr[xX1] == 0x5e) {
            threadStatus[currentBlock->threadId].simEnd = true;
            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0,
                                                              static_cast<uint64_t>(ACRC_REQUEST_TYPE::SCT_SYS)));
        } else {
            minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0,
                                                              static_cast<uint64_t>(ACRC_REQUEST_TYPE::SCT_INVAL)));
        }
        if (verbose) {
            LOG_INFO << minst.Dump();
        }
        ++id;
        return true;
    }

    /* handle ecall in slice */
    static uint64_t ecall_idx = 0;
    uint64_t e_ret;
    TGPR tmp = memory->InitEcall(ecall_idx);
    for (int i = 0; i < static_cast<int>(GPR::GPR_COUNT); i++) {
        archStatus.gpr[i] = tmp.gpr[i];
    }
    e_ret = memory->GetEcallRet(ecall_idx);
    if (e_ret != 512 && e_ret != (uint64_t)-513) {
        archStatus.gpr[2] = e_ret;
    }
    tmp.gpr[2] = archStatus.gpr[2];

    MInst minst;
    [[maybe_unused]] uint64_t tpc = 0;
    [[maybe_unused]] uint32_t id = 0;
    for (int i = 0; i < static_cast<int>(GPR::GPR_COUNT); i++) {
        minst = MInst();
        minst.check = false;
        tpc += 4;
        minst.opcode = Opcode::OP_ADDI;

        minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0, 0));
        minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, tmp.gpr[i], tmp.gpr[i]));

        minst.dsts.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0, tmp.gpr[i]));
        minst.dsts.emplace_back(std::make_shared<Operand>(OperandType::OPD_GREG, i, tmp.gpr[i]));

        if (verbose) {
            LOG_INFO << minst.Dump();
        }
        ++id;
    }
    size_t memLdataSize = memory->ldata[ecall_idx].size();
    constexpr uint32_t shortInstOffset = 4;
    for (size_t i = 0; i < memLdataSize; i++) {
        uint64_t adr = memory->ldata[ecall_idx][i].addr;
        uint64_t dat = memory->ldata[ecall_idx][i].data;
        if (memory->Load(adr, 8, false) == dat) {
            continue;
        }
        memory->Store(adr, dat, 8);
        minst = MInst();
        minst.check = false;
        tpc += 4;
        minst.opcode = Opcode::OP_ADDI;

        minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0, 0));
        minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, dat, dat));

        minst.dsts.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0, dat));

        if (verbose) {
            LOG_INFO << minst.Dump();
        }
        ++id;

        minst = MInst();
        minst.check = false;
        tpc += 4;

        minst.opcode = Opcode::OP_ADDI;

        minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0, 0));
        minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, adr, adr));

        minst.dsts.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0, adr));

        if (verbose) {
            LOG_INFO << minst.Dump();
        }
        ++id;

        minst = MInst();
        minst.check = false;
        tpc += shortInstOffset;

        minst.opcode = Opcode::OP_SDI;

        minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1, dat));
        minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0, adr));
        minst.srcs.emplace_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));

        minst.dsts.emplace_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0, adr));

        if (verbose) {
            LOG_INFO << minst.Dump();
        }
        ++id;
    }
    ecall_idx++;
    return true;
}

} // namespace JCore
