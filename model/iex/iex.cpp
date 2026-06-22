#include "iex/iex.h"
#include <algorithm>
#include <functional>
#include "mtccore/lsu/MtcLoadStoreUnit.h"
#include "core/Core.h"
#include "vectorcore/VectorCore.h"
#include "DFX/InstTracer.h"

namespace JCore {


using namespace std;

void IEXState::Reset() {
    nonSpecMemStall = false;
    specMemStall = false;
}

void IEXState::Build(IEXConfig const &configs, uint32_t peCount) {

}

void IEX::Build() {
    configs.overrideDefaultConfig(GetSim()->getCfgs());
    name = " IEX ";
    cmdIQStallCycle = 0;
    aluIQStallCycle = 0;
    aguIQStallCycle = 0;
    staIQStallCycle = 0;
    stdIQStallCycle = 0;
    bruIQStallCycle = 0;

    peCount = core->configs.stdPeCount + core->configs.simtPeCount + core->configs.memPeCount;

    iexCmdPickCount = (id == SCALAR_IEX) ? configs.iexCmdPicker : 0;
    iexAluPickCount = (core->IsVectorIex(machineType)) ?
        (configs.simt_iex_alu_iq_picker + configs.simt_iex_alu_iq_heter_picker) : configs.iexAluPicker;
    iexAluHeterPickCount = (core->IsVectorIex(machineType)) ? configs.simt_iex_alu_iq_heter_picker : 0;
    iexAguPickCount = (core->IsVectorIex(machineType)) ? configs.simt_iex_agu_iq_picker : 0;
    iexLdaPickCount = (core->IsVectorIex(machineType)) ? 0 : configs.iexLdaPicker;
    iexStaPickCount = (core->IsVectorIex(machineType)) ? 0 : configs.iexStaPicker;
    iexStdPickCount = (core->IsVectorIex(machineType)) ? configs.simt_iex_std_iq_picker : configs.iexStaPicker;
    iexBruPickCount = (core->IsVectorIex(machineType)) ? 0 : configs.iexBruPicker;
    iexScaPickCount = (core->IsVectorIex(machineType)) ? configs.simt_iex_sca_iq_picker : 0;

    iexCmdIqCount = (id == SCALAR_IEX) ? configs.iexCmdIqCount : 0;
    iexAluIqCount = (id == SCALAR_IEX) ? configs.iexAluIqCount : configs.simt_iex_alu_iq_count;
    iexAguIqCount = (id == SCALAR_IEX) ? 0 : configs.simt_iex_agu_iq_count;
    iexLdaIqCount = (id == SCALAR_IEX) ? configs.iexLdaIqCount : 0;
    iexStaIqCount = (id == SCALAR_IEX) ? configs.iexStaIqCount : 0;
    iexStdIqCount = (id == SCALAR_IEX) ? configs.iexStdIqCount : configs.simt_iex_std_iq_count;
    iexBruIqCount = (id == SCALAR_IEX) ? configs.iexBruIqCount : 0;
    iexScaIqCount = (id == SCALAR_IEX) ? 0 : configs.simt_iex_sca_iq_count;

    iexCmdIqDepth = (id == SCALAR_IEX) ? configs.iexCmdIqDepth : 0;
    iexAluIqDepth = (id == SCALAR_IEX) ? configs.iexAluIqDepth : configs.simt_iex_alu_iq_depth;
    iexAguIqDepth = (id == SCALAR_IEX) ? 0 : configs.simt_iex_agu_iq_depth;
    iexLdaIqDepth = (id == SCALAR_IEX) ? configs.iexLdaIqDepth : 0;
    iexStaIqDepth = (id == SCALAR_IEX) ? configs.iexStaIqDepth : 0;
    iexStdIqDepth = (id == SCALAR_IEX) ? configs.iexStdIqDepth : configs.simt_iex_std_iq_depth;
    iexBruIqDepth = (id == SCALAR_IEX) ? configs.iexBruIqDepth : 0;
    iexScaIqDepth = (id == SCALAR_IEX) ? 0 : configs.simt_iex_sca_iq_depth;

    cmdIQWport = (id == SCALAR_IEX) ? configs.iexCmdIqWport : 0;
    aluIQWport = (id == SCALAR_IEX) ? configs.iexAluIqWport : configs.simt_iex_alu_iq_wport;
    aguIQWport = (id == SCALAR_IEX) ? 0 : configs.simt_iex_agu_iq_wport;
    ldaIQWport = (id == SCALAR_IEX) ? configs.iexLdaIqWport : 0;
    staIQWport = (id == SCALAR_IEX) ? configs.iexStaIqWport : 0;
    stdIQWport = (id == SCALAR_IEX) ? configs.iexStdIqWport : configs.simt_iex_std_iq_wport;
    bruIQWport = (id == SCALAR_IEX) ? configs.iexBruIqWport : 0;
    scaIQWport = (id == SCALAR_IEX) ? 0 : configs.simt_iex_sca_iq_wport;

    if (id == MEM_IEX) {
        iexCmdIqCount = 0;
        iexAluIqCount = configs.mtc_iex_alu_iq_count;
        iexLdaIqCount = configs.mtc_iex_lda_iq_count;
        iexStaIqCount = configs.mtc_iex_sta_iq_count;
        iexStdIqCount = configs.mtc_iex_std_iq_count;
        iexBruIqCount = configs.mtc_iex_bru_iq_count;

        iexCmdIqDepth = 0;
        iexAluIqDepth = configs.mtc_iex_alu_iq_depth;
        iexLdaIqDepth = configs.mtc_iex_lda_iq_depth;
        iexStaIqDepth = configs.mtc_iex_sta_iq_depth;
        iexStdIqDepth = configs.mtc_iex_std_iq_depth;
        iexBruIqDepth = configs.mtc_iex_bru_iq_depth;

        cmdIQWport = 0;
        aluIQWport = configs.mtc_iex_alu_iq_wport;
        ldaIQWport = configs.mtc_iex_lda_iq_wport;
        staIQWport = configs.mtc_iex_sta_iq_wport;
        stdIQWport = configs.mtc_iex_std_iq_wport;
        bruIQWport = configs.mtc_iex_bru_iq_wport;

        iexCmdPickCount = 0;
        iexAluPickCount = configs.mtc_iex_alu_iq_picker;
        iexLdaPickCount = configs.mtc_iex_lda_iq_picker;
        iexStaPickCount = configs.mtc_iex_sta_iq_picker;
        iexStdPickCount = configs.mtc_iex_std_iq_picker;
        iexBruPickCount = configs.mtc_iex_bru_iq_picker;
    }

    currentROBs.resize(peCount);
    nextROBs.resize(peCount);

    for (uint32_t peId = 0; peId < core->configs.stdPeCount; ++peId) {
        auto pe = dynamic_pointer_cast<SPE>(core->peArray[peId]);
        if (!pe) {
            continue;
        }
        for (uint32_t tid = 0; tid < pe->prob.size(); ++tid) {
            currentROBs[peId].emplace_back(&pe->prob[tid]->getCurrent());
            nextROBs[peId].emplace_back(&pe->prob[tid]->getNext());
        }
    }

    current.Build(configs, peCount);
    next.Build(configs, peCount);
    iex_rt_wr_q = new SimQueue<RFReqBus>();

    // Build pipes
    for (uint32_t i = 0; i < iexCmdIqCount * iexCmdPickCount; i++) {
        cmdPipe.emplace_back(CMDPipe());
        cmdPipe[i].sim = GetSim();
        cmdPipe[i].top = this;
        cmdPipe[i].Build(i);
        cmdPipe[i].iex_rt_wr_q = iex_rt_wr_q;
    }
    for (uint32_t i = 0; i < iexAluIqCount * iexAluPickCount; i++) {
        aluPipe.emplace_back(ALUPipe());
        aluPipe[i].sim = GetSim();
        aluPipe[i].top = this;
        aluPipe[i].Build(i);
        aluPipe[i].iex_rt_wr_q = iex_rt_wr_q;
    }
    for (uint32_t i = 0; i < iexAguIqCount * iexAguPickCount; ++i) {
        aguPipe.emplace_back(AGUPipe());
        aguPipe[i].sim = GetSim();
        aguPipe[i].top = this;
        aguPipe[i].lda_req_q.resize(GetSim()->core->configs.scalar_smt_thread);
        aguPipe[i].lda_ret_q.resize(GetSim()->core->configs.scalar_smt_thread);
        aguPipe[i].sta_req_q.resize(GetSim()->core->configs.scalar_smt_thread);
        aguPipe[i].Build(i);
        aguPipe[i].iex_rt_wr_q = iex_rt_wr_q;
    }
    for (uint32_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {
        ldaPipe.emplace_back(LDAPipe());
        ldaPipe[i].sim = GetSim();
        ldaPipe[i].top = this;
        ldaPipe[i].lda_req_q.resize(GetSim()->core->configs.scalar_smt_thread);
        ldaPipe[i].lda_ret_q.resize(GetSim()->core->configs.scalar_smt_thread);
        ldaPipe[i].Build(i);
        ldaPipe[i].iex_rt_wr_q = iex_rt_wr_q;
        ASSERT(ldaPipe[i].lda_req_q.size() > 0);
    }

    for (uint32_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {
        ldascPipe.emplace_back(LDAPipe());
        ldascPipe[i].sim = GetSim();
        ldascPipe[i].top = this;
        ldascPipe[i].lda_req_q.resize(GetSim()->core->configs.scalar_smt_thread);
        ldascPipe[i].lda_ret_q.resize(GetSim()->core->configs.scalar_smt_thread);
        ldascPipe[i].Build(i + iexLdaIqCount * iexLdaPickCount);
        ldascPipe[i].iex_rt_wr_q = iex_rt_wr_q;
    }

    for (uint32_t i = 0; i < iexStaIqCount * iexStaPickCount; i++) {
        staPipe.emplace_back(STAPipe());
        staPipe[i].sim = GetSim();
        staPipe[i].top = this;
        staPipe[i].sta_req_q.resize(GetSim()->core->configs.scalar_smt_thread);
        staPipe[i].Build(i);
        staPipe[i].iex_rt_wr_q = iex_rt_wr_q;
    }
    for (uint32_t i = 0; i < iexStdIqCount * iexStdPickCount; i++) {
        stdPipe.emplace_back(STDPipe());
        stdPipe[i].sim = GetSim();
        stdPipe[i].top = this;
        stdPipe[i].std_req_q.resize(GetSim()->core->configs.scalar_smt_thread);
        stdPipe[i].Build(i);
    }
    for (uint32_t i = 0; i < iexBruIqCount * iexBruPickCount; i++) {
        bruPipe.emplace_back(BRUPipe());
        bruPipe[i].sim = GetSim();
        bruPipe[i].top = this;
        bruPipe[i].Build(i);
        bruPipe[i].iex_rt_wr_q = iex_rt_wr_q;
    }
    for (uint32_t i = 0; i < iexScaIqCount * iexScaPickCount; i++) {
        scaPipe.emplace_back(VecScalarPipe());
        scaPipe[i].sim = GetSim();
        scaPipe[i].top = this;
        scaPipe[i].Build(i);
        scaPipe[i].iex_rt_wr_q = iex_rt_wr_q;
    }

    if (core->IsVectorIex(machineType)) {
        std::shared_ptr<VecPE> pe = core->vectorTop->GetPE(coreId);
        ASSERT(pe);

        core->SetConfigs("VALU ISQ Cnt", configs.GetCondfigValue("simt_iex_alu_iq_count"));
        core->SetConfigs("VALU ISQ Entry Num", configs.GetCondfigValue("simt_iex_alu_iq_depth"));
        core->SetConfigs("VALU ISQ Picker", configs.GetCondfigValue("simt_iex_alu_iq_picker"));
        core->SetConfigs("VALU ISQ WPort", configs.GetCondfigValue("simt_iex_alu_iq_wport"));
        core->SetConfigs("VSTD ISQ Cnt", configs.GetCondfigValue("simt_iex_std_iq_count"));
        core->SetConfigs("VSTD ISQ Entry Num", configs.GetCondfigValue("simt_iex_std_iq_depth"));
        core->SetConfigs("VSTD ISQ Picker", configs.GetCondfigValue("simt_iex_std_iq_picker"));
        core->SetConfigs("VSTD ISQ WPort", configs.GetCondfigValue("simt_iex_std_iq_wport"));
        core->SetConfigs("Scalar ISQ Cnt", configs.GetCondfigValue("simt_iex_sca_iq_count"));
        core->SetConfigs("Scalar ISQ Entry Num", configs.GetCondfigValue("simt_iex_sca_iq_depth"));
        core->SetConfigs("Scalar ISQ Picker", configs.GetCondfigValue("simt_iex_sca_iq_picker"));
        core->SetConfigs("Scalar ISQ Wport", configs.GetCondfigValue("simt_iex_sca_iq_wport"));
        core->SetConfigs("AGU ISQ Cnt", configs.GetCondfigValue("simt_iex_agu_iq_count"));
        core->SetConfigs("AGU ISQ Entry Num", configs.GetCondfigValue("simt_iex_agu_iq_depth"));
        core->SetConfigs("AGU ISQ Picker", configs.GetCondfigValue("simt_iex_agu_iq_picker"));
        core->SetConfigs("AGU ISQ WPort", configs.GetCondfigValue("simt_iex_agu_iq_wport"));
    }

    if (id == MEM_IEX) {
        core->SetMtcConfigs("ALU ISQ Cnt", configs.GetCondfigValue("mtc_iex_alu_iq_count"));
        core->SetMtcConfigs("ALU ISQ Entry Num", configs.GetCondfigValue("mtc_iex_alu_iq_depth"));
        core->SetMtcConfigs("BRU ISQ Cnt", configs.GetCondfigValue("mtc_iex_bru_iq_count"));
        core->SetMtcConfigs("BRU ISQ Entry Num", configs.GetCondfigValue("mtc_iex_bru_iq_depth"));
        core->SetMtcConfigs("LDA ISQ Cnt", configs.GetCondfigValue("mtc_iex_lda_iq_count"));
        core->SetMtcConfigs("LDA ISQ Entry Num", configs.GetCondfigValue("mtc_iex_lda_iq_depth"));
        core->SetMtcConfigs("ST ISQ Cnt", configs.GetCondfigValue("mtc_iex_sta_iq_count"));
        core->SetMtcConfigs("ST ISQ Entry Num", configs.GetCondfigValue("mtc_iex_sta_iq_depth"));
    }

    uint32_t threadCount = id == SCALAR_IEX ? core->configs.scalar_smt_thread : core->configs.threadCount;
    iex_pe_rslv_array.resize(peCount);
    for (uint32_t i = 0; i < peCount; i++) {
        iex_pe_rslv_array[i].resize(threadCount, nullptr);
        rf_ct_q.emplace_back(new SimQueue<SimInst>());
    }

    for (uint32_t i = 0; i < iexCmdIqCount * iexCmdPickCount; i++) {
        cmdPipe[i].rslv_array.resize(peCount);
        for (uint32_t j = 0; j < peCount; j++) {
            cmdPipe[i].rslv_array[j] = core->coreInterface.iex_pe_rslv_array[id][j];
            cmdPipe[i].rf_ct_q = rf_ct_q;
        }
    }
    for (uint32_t i = 0; i < iexAluIqCount * iexAluPickCount; i++) {
        aluPipe[i].rslv_array.resize(peCount);
        for (uint32_t j = 0; j < peCount; j++) {
            aluPipe[i].rslv_array[j] = core->coreInterface.iex_pe_rslv_array[id][j];
            aluPipe[i].rf_ct_q = rf_ct_q;
        }
    }
    // vector iex, agu pipe, for both sta and lda use
    iex_lsu_lda_array.resize(core->configs.scalar_smt_thread);
    lsu_iex_lret_array.resize(core->configs.scalar_smt_thread);
    iex_lsu_sta_array.resize(core->configs.scalar_smt_thread);
    iex_lsu_std_array.resize(core->configs.scalar_smt_thread);
    for (uint32_t stid = 0; stid < core->configs.scalar_smt_thread; ++stid) {
        for (uint64_t i = 0; i < iexAguIqCount * iexAguPickCount; i++) {
            iex_lsu_lda_array[stid].push_back(new SimQueue<MemReqBus>());
            lsu_iex_lret_array[stid].push_back(new SimQueue<MemReqBus>());
            iex_lsu_sta_array[stid].push_back(new SimQueue<MemReqBus>());
            aguPipe[i].lda_req_q[stid] = iex_lsu_lda_array[stid][i];
            aguPipe[i].lda_ret_q[stid] = lsu_iex_lret_array[stid][i];
            aguPipe[i].sta_req_q[stid] = iex_lsu_sta_array[stid][i];
            aguPipe[i].sta_req_q[stid]->InitMaxSize(MAX_ST_Q_SIZE);
            aguPipe[i].rslv_array.resize(peCount);
            for (uint32_t j = 0; j < peCount; j++) {
                aguPipe[i].rslv_array[j] = core->coreInterface.iex_pe_rslv_array[id][j];
            }
        }
    }

    for (uint32_t stid = 0; stid < core->configs.scalar_smt_thread; ++stid) {
        for (uint64_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {
            iex_lsu_lda_array[stid].push_back(new SimQueue<MemReqBus>());
            lsu_iex_lret_array[stid].push_back(new SimQueue<MemReqBus>());
            ASSERT(ldaPipe[i].lda_req_q.size() > 0);
            ldaPipe[i].lda_req_q[stid] = iex_lsu_lda_array[stid][i];
            ldaPipe[i].lda_ret_q[stid] = lsu_iex_lret_array[stid][i];
            ldaPipe[i].rslv_array.resize(peCount);
            for (uint32_t j = 0; j < peCount; j++) {
                ldaPipe[i].rslv_array[j] = core->coreInterface.iex_pe_rslv_array[id][j];
            }
        }
    }

    for (uint32_t stid = 0; stid < core->configs.scalar_smt_thread; ++stid) {
        for (uint64_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {
            ldascPipe[i].lda_req_q[stid] = iex_lsu_lda_array[stid][i];
        }
    }

    for (uint32_t stid = 0; stid < core->configs.scalar_smt_thread; ++stid) {
        for (uint64_t i = 0; i < iexStaIqCount * iexStaPickCount; i++) {
            iex_lsu_sta_array[stid].push_back(new SimQueue<MemReqBus>());
            staPipe[i].sta_req_q[stid] = iex_lsu_sta_array[stid][i];
            staPipe[i].sta_req_q[stid]->InitMaxSize(MAX_ST_Q_SIZE);
            staPipe[i].rslv_array.resize(peCount);
            for (uint32_t j = 0; j < peCount; j++) {
                staPipe[i].rslv_array[j] = core->coreInterface.iex_pe_rslv_array[id][j];
            }
        }
    }

    for (uint32_t stid = 0; stid < core->configs.scalar_smt_thread; ++stid) {
        for (uint64_t i = 0; i < iexStdIqCount * iexStdPickCount; i++) {
            iex_lsu_std_array[stid].push_back(new SimQueue<MemReqBus>());
            stdPipe[i].std_req_q[stid] = iex_lsu_std_array[stid][i];
            stdPipe[i].std_req_q[stid]->InitMaxSize(MAX_ST_Q_SIZE);
            stdPipe[i].rslv_array.resize(peCount);
            for (uint32_t j = 0; j < peCount; j++) {
                stdPipe[i].rslv_array[j] = core->coreInterface.iex_pe_rslv_array[id][j];
            }
        }
    }

    for (uint64_t i = 0; i < iexBruIqCount * iexBruPickCount; i++) {
        bruPipe[i].rslv_array.resize(peCount);
        for (uint32_t j = 0; j < peCount; j++) {
            bruPipe[i].rslv_array[j] = core->coreInterface.iex_pe_rslv_array[id][j];
        }
    }

    for (uint32_t i = 0; i < iexScaIqCount * iexScaPickCount; i++) {
        scaPipe[i].rslv_array.resize(peCount);
        for (uint32_t j = 0; j < peCount; j++) {
            scaPipe[i].rslv_array[j] = core->coreInterface.iex_pe_rslv_array[id][j];
            scaPipe[i].rf_ct_q = rf_ct_q;
        }
    }

    // Build dispatch unit
    dispatchUnit.top = this;
    dispatchUnit.sim = this->GetSim();
    dispatchUnit.Build(peCount);
    for (uint32_t i = 0; i < peCount; i++) {
        dispatchUnit.pe_iex_cmd_array.push_back(nullptr);
        dispatchUnit.pe_iex_alu_array.push_back(nullptr);
        dispatchUnit.pe_iex_lda_array.push_back(nullptr);
        dispatchUnit.pe_iex_sc_lda_array.push_back(nullptr);
        dispatchUnit.pe_iex_sta_array.push_back(nullptr);
        dispatchUnit.pe_iex_std_array.push_back(nullptr);
        dispatchUnit.pe_iex_bru_array.push_back(nullptr);
        dispatchUnit.pe_iex_vec_agu_array.push_back(nullptr);
        dispatchUnit.pe_iex_vec_scalar_array.push_back(nullptr);
    }

    // Reduce Unit
    rd.top = this;
    rd.Build();

    // Build register file
    rf.top = this;
    rf.Build(core->configs.ggpr_count, peCount);
    for (uint32_t i = 0; i < iexCmdIqCount; i++) {
        rf.rf_rd_req.emplace_back(&(cmdPipe[i].rf_rd_req));
        rf.rf_data_ret.emplace_back(&(cmdPipe[i].rf_data_ret));
    }
    for (uint32_t i = 0; i < iexAluIqCount * iexAluPickCount; i++) {
        rf.rf_rd_req.emplace_back(&(aluPipe[i].rf_rd_req));
        for (auto& req : aluPipe[i].rf_wr_req) {
            rf.rf_wr_req.emplace_back(&req);
        }
        rf.rf_data_ret.emplace_back(&(aluPipe[i].rf_data_ret));
    }
    for (uint32_t i = 0; i < iexAguIqCount * iexAguPickCount; i++) {
        rf.rf_rd_req.emplace_back(&(aguPipe[i].rf_rd_req));
        rf.rf_wr_req.emplace_back(&(aguPipe[i].rf_wr_req));
        rf.rf_ld_wr_req.emplace_back(&aguPipe[i].rf_ld_wr_req);
        rf.rf_data_ret.emplace_back(&(aguPipe[i].rf_data_ret));
    }
    for (uint32_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {
        rf.rf_rd_req.emplace_back(&(ldaPipe[i].rf_rd_req));
        rf.rf_wr_req.emplace_back(&(ldaPipe[i].rf_wr_req));
        rf.rf_ld_wr_req.emplace_back(&ldaPipe[i].rf_ld_wr_req);
        rf.rf_data_ret.emplace_back(&(ldaPipe[i].rf_data_ret));
    }
    for (uint32_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {
        rf.rf_rd_req.emplace_back(&(ldascPipe[i].rf_rd_req));
        rf.rf_wr_req.emplace_back(&(ldascPipe[i].rf_wr_req));
        rf.rf_ld_wr_req.emplace_back(&ldascPipe[i].rf_ld_wr_req);
        rf.rf_data_ret.emplace_back(&(ldascPipe[i].rf_data_ret));
    }
    for (uint32_t i = 0; i < iexStaIqCount * iexStaPickCount; i++) {
        rf.rf_rd_req.emplace_back(&(staPipe[i].rf_rd_req));
        rf.rf_wr_req.emplace_back(&(staPipe[i].rf_wr_req));
        rf.rf_data_ret.emplace_back(&(staPipe[i].rf_data_ret));
    }
    for (uint32_t i = 0; i < iexStdIqCount * iexStdPickCount; i++) {
        rf.rf_rd_req.emplace_back(&(stdPipe[i].rf_rd_req));
        rf.rf_wr_req.emplace_back(&(stdPipe[i].rf_wr_req));
        rf.rf_data_ret.emplace_back(&(stdPipe[i].rf_data_ret));
    }
    for (uint32_t i = 0; i < iexBruIqCount * iexBruPickCount; i++) {
        rf.rf_rd_req.emplace_back(&(bruPipe[i].rf_rd_req));
        rf.rf_wr_req.emplace_back(&(bruPipe[i].rf_wr_req));
        rf.rf_data_ret.emplace_back(&(bruPipe[i].rf_data_ret));
    }
    for (uint32_t i = 0; i < iexScaIqCount * iexScaPickCount; i++) {
        rf.rf_rd_req.emplace_back(&(scaPipe[i].rf_rd_req));
        for (auto& req : scaPipe[i].rf_wr_req) {
            rf.rf_wr_req.emplace_back(&req);
        }
        rf.rf_data_ret.emplace_back(&(scaPipe[i].rf_data_ret));
    }
    rf.rf_wr_req.emplace_back(&(rd.rf_wr_req));
    // Build system register interface
    for (uint32_t i = 0; i < aluPipe.size(); i++) {
        core->coreInterface.sys_rd_req.emplace_back(&aluPipe[i].sys_rd_req);
        core->coreInterface.sys_wr_req.emplace_back(&aluPipe[i].sys_wr_req);
        core->coreInterface.sys_data_ret.emplace_back(&aluPipe[i].sys_data_ret);
    }
    for (uint32_t i = 0; i < stdPipe.size(); i++) {
        core->coreInterface.sys_rd_req.emplace_back(&stdPipe[i].sys_rd_req);
        core->coreInterface.sys_data_ret.emplace_back(&stdPipe[i].sys_data_ret);
    }
    for (uint32_t i = 0; i < aguPipe.size(); i++) {
        core->coreInterface.sys_rd_req.emplace_back(&aguPipe[i].sys_rd_req);
        core->coreInterface.sys_data_ret.emplace_back(&aguPipe[i].sys_data_ret);
    }
    for (uint32_t i = 0; i < ldaPipe.size(); i++) {
        core->coreInterface.sys_rd_req.emplace_back(&ldaPipe[i].sys_rd_req);
        core->coreInterface.sys_data_ret.emplace_back(&ldaPipe[i].sys_data_ret);
    }
    for (uint32_t i = 0; i < staPipe.size(); i++) {
        core->coreInterface.sys_rd_req.emplace_back(&staPipe[i].sys_rd_req);
        core->coreInterface.sys_data_ret.emplace_back(&staPipe[i].sys_data_ret);
    }
    for (uint32_t i = 0; i < bruPipe.size(); i++) {
        core->coreInterface.sys_rd_req.emplace_back(&bruPipe[i].sys_rd_req);
        core->coreInterface.sys_data_ret.emplace_back(&bruPipe[i].sys_data_ret);
    }
    for (uint32_t i = 0; i < scaPipe.size(); i++) {
        core->coreInterface.sys_rd_req.emplace_back(&scaPipe[i].sys_rd_req);
        core->coreInterface.sys_wr_req.emplace_back(&scaPipe[i].sys_wr_req);
        core->coreInterface.sys_data_ret.emplace_back(&scaPipe[i].sys_data_ret);
    }

    // Build ready table
    rtable.top = this;
    rtable.sim = this->GetSim();
    rtable.Build(peCount);
    rtable.rf_wr_q = &rf.rf_wr_q;
    rtable.isq_wake_q = &iq.isq_wake_q;
    rtable.iex_rt_wr_q = iex_rt_wr_q;
    rtable.rslv_array.resize(peCount);
    for (uint32_t i = 0; i < peCount; i++) {
        rtable.rslv_array[i] = core->coreInterface.iex_pe_rslv_array[id][i];
        rtable.rf_ct_q[i] = rf_ct_q[i];
    }

    // Build issue queue
    iq.top = this;
    iq.Build();
    for (uint32_t i = 0; i < iexCmdIqCount * iexCmdPickCount; i++) {
        cmdPipe[i].p1_inst = &iq.selectCmdInst[i];
    }
    for (uint32_t i = 0; i < iexAluIqCount * iexAluPickCount; i++) {
        aluPipe[i].p1_inst = &iq.selectAluInst[i];
        if (core->IsVectorIex(machineType)) {
            iq.aluPipeStallFn[i] = bind(&IEX::checkSimtAluStall, this, placeholders::_1, i);
        }
    }

    for (uint32_t i = 0; i < iexAguIqCount * iexAguPickCount; i++) {
        aguPipe[i].p1_inst = &iq.selectAguInst[i];
    }
    for (uint32_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {
        ldaPipe[i].p1_inst = &iq.selectLdaInst[i];
    }
    for (uint32_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {
        ldascPipe[i].p1_inst = &iq.selectSCLdaInst[i];
    }
    for (uint32_t i = 0; i < iexStaIqCount * iexStaPickCount; i++) {
        staPipe[i].p1_inst = &iq.selectStaInst[i];
    }
    for (uint32_t i = 0; i < iexStdIqCount * iexStdPickCount; i++) {
        stdPipe[i].p1_inst = &iq.selectStdInst[i];
    }
    for (uint32_t i = 0; i < iexBruIqCount * iexBruPickCount; i++) {
        bruPipe[i].p1_inst = &iq.selectBruInst[i];
    }
    for (uint32_t i = 0; i < iexScaIqCount * iexScaPickCount; i++) {
        scaPipe[i].p1_inst = &iq.selectScaInst[i];
        if (core->IsVectorIex(machineType)) {
            iq.scaPipeStallFn[i] = bind(&IEX::checkSimtScaStall, this, placeholders::_1, i);
        }
    }

    // link internal crossbar
    bn.top = this;
    bn.Build();
    for (uint32_t i = 0; i < iexCmdIqCount * iexCmdPickCount; i++) {
        bn.pipe_i2.emplace_back(&(cmdPipe[i].i2_inst));
        bn.pipe_e1.emplace_back(&(cmdPipe[i].e1_inst));
        bn.pipe_w1.emplace_back(&(cmdPipe[i].w1_inst));
    }
    for (uint32_t i = 0; i < iexAluIqCount * iexAluPickCount; i++) {
        bn.pipe_i2.emplace_back(&(aluPipe[i].i2_inst));
        // E1/E2/E3/E4/E5
        vector<reference_wrapper<vector<SimInst*>>> pipe_ex = {
            ref(bn.pipe_e1), ref(bn.pipe_e2), ref(bn.pipe_e3), ref(bn.pipe_e4), ref(bn.pipe_e5)
        };
        for (uint32_t idx = 0; idx < pipe_ex.size(); idx++) {
            if (idx < aluPipe[i].ex_inst.size()) {
                for (auto& inst : aluPipe[i].ex_inst[idx]) {
                    pipe_ex[idx].get().emplace_back(&inst);
                }
            }
        }
        // W1
        for (auto& inst : aluPipe[i].w1_inst) {
            bn.pipe_w1.emplace_back(&inst);
        }
        // W2
        for (auto& inst : aluPipe[i].w2_inst) {
            bn.pipe_w2.emplace_back(&inst);
        }
    }
    for (uint32_t i = 0; i < iexAguIqCount * iexAguPickCount; i++) {
        bn.pipe_i2.emplace_back(&(aguPipe[i].i2_inst));
        bn.pipe_e1.emplace_back(&(aguPipe[i].e1_inst));
        bn.pipe_e4.emplace_back(&(aguPipe[i].e4_inst));
        bn.pipe_w1.emplace_back(&(aguPipe[i].w1_inst));
        bn.pipe_w2.emplace_back(&(aguPipe[i].w2_inst));
    }
    for (uint32_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {
        bn.pipe_i2.emplace_back(&(ldaPipe[i].i2_inst));
        bn.pipe_e1.emplace_back(&(ldaPipe[i].e1_inst));
        bn.pipe_e4.emplace_back(&(ldaPipe[i].e4_inst));
        bn.pipe_w1.emplace_back(&(ldaPipe[i].w1_inst));
        bn.pipe_w2.emplace_back(&(ldaPipe[i].w2_inst));
    }

    for (uint32_t i = 0; i < iexLdaIqCount * iexLdaPickCount; i++) {  // x00600317
        bn.pipe_i2.emplace_back(&(ldascPipe[i].i2_inst));
        bn.pipe_e1.emplace_back(&(ldascPipe[i].e1_inst));
        bn.pipe_e4.emplace_back(&(ldascPipe[i].e4_inst));
        bn.pipe_w1.emplace_back(&(ldascPipe[i].w1_inst));
        bn.pipe_w2.emplace_back(&(ldascPipe[i].w2_inst));
    }

    for (uint32_t i = 0; i < iexStaIqCount * iexStaPickCount; i++) {
        bn.pipe_i2.emplace_back(&(staPipe[i].i2_inst));
        bn.pipe_e1.emplace_back(&(staPipe[i].e1_inst));
        bn.pipe_w1.emplace_back(&(staPipe[i].w1_inst));
        bn.pipe_w2.emplace_back(&(staPipe[i].w2_inst));
    }
    for (uint32_t i = 0; i < iexStdIqCount * iexStdPickCount; i++) {
            bn.pipe_i2.emplace_back(&(stdPipe[i].i2_inst));
            bn.pipe_e1.emplace_back(&(stdPipe[i].e1_inst));
        }
    for (uint32_t i = 0; i < iexBruIqCount * iexBruPickCount; i++) {
        bn.pipe_i2.emplace_back(&(bruPipe[i].i2_inst));
        bn.pipe_e1.emplace_back(&(bruPipe[i].e1_inst));
    }
    for (uint32_t i = 0; i < iexScaIqCount * iexScaPickCount; i++) {
        // on vector scalar pipe, i1 will get reg data, so we need to add i1 stage on bn's i2
        bn.pipe_i2.emplace_back(&(scaPipe[i].i1_inst));
        // E1/E2/E3/E4/E5
        vector<reference_wrapper<vector<SimInst*>>> pipe_ex = {
            ref(bn.pipe_e1), ref(bn.pipe_e2), ref(bn.pipe_e3), ref(bn.pipe_e4), ref(bn.pipe_e5)
        };
        for (uint32_t idx = 0; idx < pipe_ex.size(); idx++) {
            if (idx < scaPipe[i].ex_inst.size()) {
                for (auto& inst : scaPipe[i].ex_inst[idx]) {
                    pipe_ex[idx].get().emplace_back(&inst);
                }
            }
        }
        // W1
        for (auto& inst : scaPipe[i].w1_inst) {
            bn.pipe_w1.emplace_back(&inst);
        }
        // W2
        for (auto& inst : scaPipe[i].w2_inst) {
            bn.pipe_w2.emplace_back(&inst);
        }
    }

    stats = std::make_shared<IEXStats>(GetSim()->getRpt());
    stats->id = id;
    stats->Reset();
    stats->Build(configs);

    iexmdb.top = this;
    iexmdb.Build(configs.iex_mdb_confidence, configs.iex_mdb_enable);

    tileLdCredit.resize(4);
}

void IEX::BuildSupplement()
{
    std::shared_ptr<VecPE> pe = core->vectorTop->GetPE(coreId);
    ASSERT(pe);

    uint32_t peOffset = coreId + core->configs.stdPeCount;
    for (uint32_t tid = 0; tid < pe->prob.size(); ++tid) {
        currentROBs[peOffset].emplace_back(&pe->prob[tid]->getCurrent());
        nextROBs[peOffset].emplace_back(&pe->prob[tid]->getNext());
    }
}

void IEX::Reset() {

    dispatchUnit.Reset();
    rtable.Reset();

    for (uint32_t i=0; i<cmdPipe.size(); i++) {
        cmdPipe[i].Reset();
    }
    for (uint32_t i = 0; i<aluPipe.size(); i++) {
        aluPipe[i].Reset();
    }
    for (uint32_t i = 0; i < ldaPipe.size(); i++) {
        ldaPipe[i].Reset();
    }
    for (uint32_t i = 0; i < ldascPipe.size(); i++) {
        ldascPipe[i].Reset();
    }
    for (uint32_t i = 0; i < staPipe.size(); i++) {
        staPipe[i].Reset();
    }
    for (uint32_t i = 0; i < stdPipe.size(); i++) {
        stdPipe[i].Reset();
    }
    for (uint32_t i = 0; i < bruPipe.size(); i++) {
        bruPipe[i].Reset();
    }
    for (uint32_t i = 0; i < aguPipe.size(); i++) {
        aguPipe[i].Reset();
    }
    for (uint32_t i = 0; i < scaPipe.size(); i++) {
        scaPipe[i].Reset();
    }

    iq.Reset();
    rf.Reset();
    current.Reset();
    next.Reset();
    ssrsetOrderQ.Reset();
    setMemStall(true, true);
    iexmdb.Reset();
}

void IEX::resetStats() {
    stats->Reset();
    stats->Build(configs);
}

void IEX::Work() {
    // look up and dispatch
    dispatchUnit.Work();
    rtable.Work();
    // Stage 4: Three stages of issue queue
    iq.Work();
    if (configs.iex_dispatch_mode == 0) {
        updateIQStall();
        checkIQStall();
    }
    for (uint32_t i = 0; i < cmdPipe.size(); i++) {
        cmdPipe[i].Work();
        // if (cmdPipe[i].i1_inst) {
        //     SimInst inst = cmdPipe[i].i1_inst;
        //     PLpvInfo lpvInfo = inst->GetLpv();
        //     iq.WakeupDummy(inst->peID, inst->dTag, lpvInfo, inst->tid);
        // }
    }
    for (uint32_t i = 0; i < aluPipe.size(); i++) {
        aluPipe[i].Work();
        if (aluPipe[i].i1_inst && aluPipe[i].i1_inst->opcode == Opcode::OP_SSRGET) {
            SimInst inst = aluPipe[i].i1_inst;
            InstWakeupIQ(inst);
        }
        if (aluPipe[i].i2_inst && (aluPipe[i].i2_inst->opcode == Opcode::OP_SSRSET ||
            OpcodeIsBDIM(aluPipe[i].i2_inst->opcode))) {
            auto &inst = aluPipe[i].i2_inst;
            iq.WakeupIQTag(WakeupInfo(OperandType::OPD_SYS, inst->pdsts_[DST0_IDX]->value,
                inst->pdsts_[DST0_IDX]->recycled), inst->GetLpv(), inst->peID, inst->tid, inst->stid);
        }
    }
    for (uint32_t i = 0; i < ldaPipe.size(); i++) {
        ldaPipe[i].Work();
    }
    for (uint32_t i = 0; i < ldascPipe.size(); i++) {
        ldascPipe[i].Work();
    }
    for (uint32_t i = 0; i < staPipe.size(); i++) {
        staPipe[i].Work();
    }
    for (uint32_t i = 0; i < stdPipe.size(); i++) {
        stdPipe[i].Work();
    }
    for (uint32_t i = 0; i < bruPipe.size(); i++) {
        bruPipe[i].Work();
    }
    for (uint32_t i = 0; i < aguPipe.size(); i++) {
        aguPipe[i].Work();
    }
    for (uint32_t i = 0; i < scaPipe.size(); i++) {
        scaPipe[i].Work();
        if (scaPipe[i].i1_inst && scaPipe[i].i1_inst->opcode == Opcode::OP_SSRGET) {
            SimInst inst = scaPipe[i].i1_inst;
            InstWakeupIQ(inst);
        }
        if (scaPipe[i].i1_inst && (scaPipe[i].i1_inst->opcode == Opcode::OP_SSRSET ||
            OpcodeIsBDIM(scaPipe[i].i1_inst->opcode))) {
            auto &inst = scaPipe[i].i1_inst;
            iq.WakeupIQTag(WakeupInfo(OperandType::OPD_SYS, inst->pdsts_[DST0_IDX]->value,
                inst->pdsts_[DST0_IDX]->recycled), inst->GetLpv(), inst->peID, inst->tid, inst->stid);
        }
        for (auto& inst : scaPipe[i].ex_inst[0]) { // TO BE CONFIRMED if e1 only
            if (inst && OpcodeIsInnerJump(inst->opcode)) {
                brRlsvQ.push_back(inst);
                innerBranchResolve(inst);
            }
        }
    }

    receiveFromLSU();

    for (auto &pipe : bruPipe) {
        auto &inst = pipe.e1_inst;
        if (inst) {
            brRlsvQ.push_back(inst);
            innerBranchResolve(inst);
        }
    }
    for (auto it = brRlsvQ.begin(); it != brRlsvQ.end(); ) {
        if (branchResolve((*it))) {
            it = brRlsvQ.erase(it);
        } else {
            ++it;
        }
    }
    doStats();
    iexmdb.Work();
    if ((core->IsVectorIex(machineType)) || (core->IsMtcIex(machineType))) {
        rd.Work();
        HandleVcoreResp();
    }
    // Stage 5: read regfile
    rf.Work();
}

void IEX::InstWakeupIQ(SimInst &inst)
{
    PLpvInfo lpvInfo = inst->GetLpv();
    for (auto &pdst : inst->pdsts_) {
        iq.WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled), lpvInfo, inst->peID, inst->tid, inst->stid);
    }
}

void IEX::PreReleaseVRF()
{
    auto releaseInsts = [this]() {
        for (const auto& inst : m_pendingVrfReleaseInsts) {
            PEResolveBus peResolve;
            peResolve.peid = inst->peID;
            peResolve.bid = inst->bid;
            peResolve.rid = inst->rid;
            peResolve.tid = inst->GetTid();
            peResolve.isSrcRead = true;
            iex_pe_rslv_array[inst->peID][inst->GetTid()]->Write(peResolve);
        }
        m_pendingVrfReleaseInsts.clear();
    };
    if (core->configs.vrf_release_mode == 2U) {
        // Release previous cycle's
        releaseInsts();
    }

    for (uint32_t i = 0; i < aluPipe.size(); i++) {
        const SimInst& inst = aluPipe[i].i1_inst;
        if (!inst) {
            continue;
        }
        m_pendingVrfReleaseInsts.emplace_back(inst);
    }
    for (uint32_t i = 0; i < stdPipe.size(); i++) {
        const SimInst& inst = stdPipe[i].i1_inst;
        if (!inst) {
            continue;
        }
        m_pendingVrfReleaseInsts.emplace_back(inst);
    }
    for (uint32_t i = 0; i < aguPipe.size(); i++) {
        const SimInst& inst = aguPipe[i].i1_inst;
        if (!inst) {
            continue;
        }
        m_pendingVrfReleaseInsts.emplace_back(inst);
    }
    for (uint32_t i = 0; i < scaPipe.size(); i++) {
        const SimInst& inst = scaPipe[i].i1_inst;
        if (!inst) {
            continue;
        }
        m_pendingVrfReleaseInsts.emplace_back(inst);
    }

    if (core->configs.vrf_release_mode == 3U) {
        // Release immediately
        releaseInsts();
    }
}

void IEX::pipeMove() {
    for (uint32_t i = 0; i < cmdPipe.size(); i++) {
        cmdPipe[i].moveLpv();
        cmdPipe[i].move();
    }
    for (uint32_t i = 0; i < aluPipe.size(); i++) {
        aluPipe[i].moveLpv();
        aluPipe[i].move();
    }
    for (uint32_t i = 0; i < ldaPipe.size(); i++) {
        ldaPipe[i].moveLpv();
        ldaPipe[i].move();
    }

    for (uint32_t i = 0; i < ldascPipe.size(); i++) {
        ldascPipe[i].moveLpv();
        ldascPipe[i].move();
    }
    for (uint32_t i = 0; i < staPipe.size(); i++) {
        staPipe[i].moveLpv();
        staPipe[i].move();
    }
    for (uint32_t i = 0; i < stdPipe.size(); i++) {
        stdPipe[i].moveLpv();
        stdPipe[i].move();
    }
    for (uint32_t i = 0; i < bruPipe.size(); i++) {
        bruPipe[i].moveLpv();
        bruPipe[i].move();
    }
    for (uint32_t i = 0; i < aguPipe.size(); i++) {
        aguPipe[i].moveLpv();
        aguPipe[i].move();
    }
    for (uint32_t i = 0; i < scaPipe.size(); i++) {
        scaPipe[i].moveLpv();
        scaPipe[i].move();
    }
}

void IEX::Xfer() {
    bn.Xfer();
    releaseIQEntry();
    current = next;
    for (uint32_t i = 0; i < peCount; i++) {
        rf_ct_q[i]->Work();
    }
    ssrsetOrderQ.Work();
    dispatchUnit.Xfer();
    rtable.Xfer();
    iq.Xfer();
    rf.Xfer();
    if (core->IsVectorIex(machineType) && ((core->configs.vrf_release_mode == 2U)
        || (core->configs.vrf_release_mode == 3U))) {
        PreReleaseVRF();
    }
    pipeMove();
    stats->cycles++;
    iexmdb.Xfer();
    if (core->IsVectorIex(machineType)) {
        RegConflictStat();
    }
}

void IEX::RegConflictStat()
{
    set<uint32_t> src1RegBank;
    set<uint32_t> src2RegBank;
    set<uint32_t> src3RegBank;
    constexpr uint64_t rfBankNum = 4;
    auto recordRfBank = [&src1RegBank, &src2RegBank, &src3RegBank](SimInst &inst) {
        if (inst == nullptr) {
            return;
        }
        for (auto &psrc : inst->psrcs_) {
            if (OperandTypeIsVReg(psrc->type) && !psrc->dataFromBypass) {

            }
        }
        if (inst->psrcs_.size() < SRC2_IDX) {
            return;
        }
        auto &src1 = inst->psrcs_[SRC1_IDX];
        if (OperandTypeIsVReg(src1->type) && !src1->dataFromBypass) {
            src1RegBank.insert(src1->ptag % rfBankNum);
        }
        if (inst->psrcs_.size() < SRC3_IDX) {
            return;
        }
        auto &src2 = inst->psrcs_[SRC2_IDX];
        if (OperandTypeIsVReg(src2->type) && !src2->dataFromBypass) {
            src2RegBank.insert(src2->ptag % rfBankNum);
        }
        if (inst->psrcs_.size() < SRC4_IDX) {
            return;
        }
        auto &src3 = inst->psrcs_[SRC3_IDX];
        if (OperandTypeIsVReg(src3->type) && !src3->dataFromBypass) {
            src3RegBank.insert(src3->ptag % rfBankNum);
        }
    };
    auto checkConflict = [](SimInst &inst, const set<uint32_t>& aluRegBank) -> bool {
        if (inst == nullptr) {
            return false;
        }
        if (!OpcodeIsStore(inst->opcode)) {
            return false;
        }
        // TODO: change to std pipe and check srcD
        // if (OpcodeIsStoreReg(inst->opcode)) {
        //     if (inst->src2.vld && inst->src2.IsVRFVld() && !inst->src2.dataFromBypass) {
        //         return aluRegBank.count(inst->src2.vrfPtag % rfBankNum) != 0;
        //     }
        // } else {
        //     if (inst->src0.vld && inst->src0.IsVRFVld() && !inst->src0.dataFromBypass) {
        //         return aluRegBank.count(inst->src0.vrfPtag % rfBankNum) != 0;
        //     }
        // }
        // 目前解码已经讲全部store 类指令 data 放到srcD.
        if (inst->psrcs_.empty()) {
            return false;
        }
        auto &src0 = inst->psrcs_[SRC0_IDX];
        if (OperandTypeIsVReg(src0->type) && !src0->dataFromBypass) {
            return aluRegBank.count(src0->ptag % rfBankNum) != 0;
        }
        return false;
    };
    for (auto &pipe : aluPipe) {
        recordRfBank(pipe.i2_inst);
    }
    // TODO: After enable std in vector core, change the aguPipe to stdPipe
    bool src1Conflict = false;
    bool src2Conflict = false;
    bool src3Conflict = false;
    for (size_t i = 0; i < aguPipe.size(); ++i) {
        auto &pipe = aguPipe[i];
        auto &inst = pipe.i2_inst;
        // TODO: STD split
        if (checkConflict(inst, src1RegBank) && !src1Conflict) {
            src1Conflict = true;
            ++stats->src1Conflict;
        }
        if (checkConflict(inst, src2RegBank) && !src2Conflict) {
            src2Conflict = true;
            ++stats->src2Conflict;
        }
        if (checkConflict(inst, src3RegBank) && !src3Conflict) {
            src3Conflict = true;
            ++stats->src3Conflict;
        }
    }
}

SimSys *IEX::GetSim() {
    return core->GetSim();
}

void IEX::setMemStall(bool spec, bool status) {
    if (spec) {
        next.specMemStall = status;
    } else {
        next.nonSpecMemStall = status;
        current.nonSpecMemStall = status;
    }
}

void IEX::SetRegReadyTable(OperandType type, uint32_t ptag, bool ready,
                           PLpvInfo lpvInfo, uint32_t peID, uint32_t tid)
{
    LOG_INFO_M(machineType, Stage::NA) << "Set " << GetOperandType(type) << dec << ptag <<  " Ready Table to "
                                       << boolalpha << ready << noboolalpha;
    rtable.SetRegReadyTable(type, ptag, ready, lpvInfo, peID, tid);
}

void IEX::innerBranchResolve(SimInst &inst) {
    if (!inst) {
        return;
    }
    if (!OpcodeIsInnerJump(inst->opcode)) {
        return;
    }
    if (inst->brInfo) {
        ResolveFunc(inst, inst->brInfo->targetPC);
    }
}

void IEX::ResolveFunc(SimInst& inst, uint64_t dataOut)
{
    // 标量块不再有分类块，后需要均走vector 分支。
    // // Condition(1) Unexpected inner-jump instruction in "no-branch" block.
    // // Condition(2) Mispred in "branch" block.
    // PLpvInfo lpvInfo = inst->GetLpv();
    // if ((inst->blockNoBranch && dataOut != inst->fallTPC) ||
    //     (!inst->blockNoBranch && dataOut != inst->bp_dst)) {
    //     PEResolveBus peResolve;
    //     peResolve.peid = inst->peID;
    //     peResolve.bid = inst->bid;
    //     peResolve.rid = inst->rid;
    //     peResolve.tid = inst->tid;
    //     iex_pe_rslv_array[inst->peID][inst->tid]->Write(peResolve);

    //     core->flushUnit->flush_stats->IntraBlockMisprediction++;
    //     if (core->pTracer->IsEnabled()) {
    //         core->pTracer->PushInstrEvent(inst->bid, inst, InstrEvent::MISPRED);
    //     }
    // } else if (inst->blockNoBranch && dataOut == inst->fallTPC) {
    //     // Condition(3) Unexpected inner-jump instruction in "no-branch" block but dst==tpc+instSize
    //     // (no need to do flush)
    //     // Report branch but continue running
    //     wakeupSeq(inst->peID, inst->rid.val, lpvInfo, inst->tid);
    // } else if (!inst->blockNoBranch && dataOut == inst->bp_dst) {
    //     // Condition(4) Correct prediction in "branch" block
    //     // Report branch and continue running
    //     wakeupSeq(inst->peID, inst->rid.val, lpvInfo, inst->tid);
    // } else {
    //     ASSERT(false && "Something wrong at inner-jump instruction executing!");
    // }

    if (!core->IsVectorIex(machineType) && (id != MEM_IEX)) {
        return;
    }

    if (OpcodeIsCondInnerJump(inst->opcode)) {
        uint64_t fetchTPC = dataOut;
        if (core->IsVecPe(inst->peID)) {
            std::shared_ptr<VecPE> pe = core->vectorTop->GetPE(inst->coreID);
            pe->ifu.ContinueFetch(inst->tid, fetchTPC, inst->bid, true, inst->stid);
        } else {
            ASSERT(false && "Scalar PE not support Branch Inst");
        }
    }
}

bool IEX::branchResolve(SimInst &inst) {
    if (!inst || !OpcodeIsSetc(inst->opcode)) {
        return true;
    }
    auto peID = inst->peID;

    // retrieve predicted bpc
    BlockCommandPtr cmd = GetSim()->core->bctrl->blockROB.GetBlockCMDPtr(inst->bid, inst->stid);
    IDBus idBus;
    idBus.vld = true;
    idBus.bid = inst->bid;
    idBus.rid = inst->rid;
    idBus.stid = inst->stid;
    idBus.lsID = inst->lsID;
    idBus.iexTyp = inst->iexType;
    bool misPred = false;
    bool resolveTaken = false;
    uint64_t resolveTarget = 0;
    bool resolved = false;
    bool predicted = true;
    // check prediction against calculation result
    switch (cmd->branchType) {
        case BranchType::BLK_BR_COND:
            if (!(OpcodeIsSetc(inst->opcode))) {
                core->peArray[peID]->SetBlockException(inst->bid, inst->stid, "Conditional branch resolve");
                return true;
            } else if (predicted) {
                misPred = cmd->machineInst && inst->setcInfo->setcTaken != cmd->machineInst->bfuInfo->predict_taken;
                resolveTaken = inst->setcInfo->setcTaken;
                resolveTarget = cmd->barg.target;
                GetSim()->core->bctrl->blockROB.resolveBlock(idBus, misPred, resolveTaken, resolveTarget, idBus.stid);
                resolved = true;
            }
            break;
        case BranchType::BLK_BR_IND: // 必然跳转，通过setc.tgt设置跳转位置
        case BranchType::BLK_BR_ICALL: // 必然跳转，通过setc.tgt 设置跳转位置，setret 设置返回值
        case BranchType::BLK_BR_CALL:  // 必然跳转，跳转位置在BSTART，D1 处理，setret 设置返回值
        case BranchType::BLK_BR_DIRECT: // 必然跳转，跳转位置在BSTART，D1 处理。
        case BranchType::BLK_BR_RET: {  // 跳转到ra。setc.tgt 设置跳转位置
            if (!predicted) {
                return false;
            }
            // 此分支只处理setc_tgt
            misPred = cmd->machineInst && cmd->machineInst->bfuInfo->predict_taken != true;
            misPred |= inst->setcInfo->setcTarget != cmd->machineInst->bfuInfo->predict_tgt;
            resolveTaken = true;
            resolveTarget = inst->setcInfo->setcTarget;
            GetSim()->core->bctrl->blockROB.resolveBlock(idBus, misPred, resolveTaken, resolveTarget, idBus.stid);
            resolved = true;
            break;
        }
        default:
            LOG_INFO_M(machineType, Stage::WB) << "Report block exception setc in non-conditional block "
                <<inst<<" : BPC "<<hex<<inst->bpc<<" TPC "<<inst->pc<<" BID "<<dec<<inst->bid;
            core->peArray[peID]->SetBlockException(inst->bid, inst->stid, "Uncongnized branc resolve");
            resolved = true;
            break;
    }

    LOG_INFO_M(machineType, Stage::E1) << inst << "Predict miss " << misPred <<" target pc: 0x"<< hex << resolveTarget;
    return resolved;
}

void IEX::loadBranchResolve(SimInst &inst)
{
    (void)inst;
    // 本优化，暂不支持。
    // if (inst == nullptr || !inst->loadCmp.loadResolve) {
    //     return;
    // }
    // BHeader header = core->peArray[inst->peID]->getBlockHeader(inst->bid);
    // if (header->branchType != BranchType::BLK_BR_COND) {
    //     LOG_INFO_M(machineType, Stage::WB) << "Report block exception setc in non-conditional block "
    //         <<inst<<" : load BPC "<<hex<<inst->bpc<<" TPC "<<inst->pc<<" BID "<<dec<<inst->bid;
    //     core->peArray[inst->peID]->setBlockException(inst->bid, "load-branch resolve in non-conditional block");
    //     return;
    // }
    // IDBus idBus;
    // idBus.vld = true;
    // idBus.bid = inst->bid;
    // idBus.rid = inst->rid;
    // idBus.lsID = inst->lsID;
    // idBus.tid = inst->tid;
    // uint64_t data = inst->dst0.dataOut_vld ? inst->dst0.dataOut : inst->dst1.dataOut;
    // bool resolveTaken = inst->loadCmp.opcode == Opcode::OP_SETC_EQ ? (data == 0) : (data != 0);
    // bool misPred = header->machineInst && (resolveTaken != header->machineInst->bfuInfo->predict_taken);
    // uint64_t resolveTarget = resolveTaken ? header->divertBPC : header->fallBPC;
    // core->bctrl->blockROB.resolveBlock(idBus, misPred, resolveTaken, resolveTarget);

    // stats->loadResolveCnt++;
    // LOG_INFO_M(machineType, Stage::E4) << inst << "Predict miss " << misPred <<" resolve taken: " << resolveTaken;
}

void IEX::setPtagReady(uint32_t lptag, PLpvInfo &lpvInfo, bool ready) {
    rtable.SetRegReadyTable(OperandType::OPD_GREG, lptag, ready, lpvInfo);
}

uint64_t IEX::getGPR(uint32_t ptag) {
    return rf.getGPR(ptag);
}

void IEX::setGPR(uint32_t ptag, uint64_t data) {
    rf.setGPR(ptag, data);
    rtable.InitGGPRRtable(ptag, data);
}

void IEX::setMemWakeup(MemReqBus const &mem) {
    if (!mem.vld || mem.stack_vld) {
        return;
    }
    // Instruction Wakeup (External)
    uint32_t tid = 0;
    if (GetSim()->core->IsVectorIex(machineType)) {
        tid = mem.tid;
    } else {
        tid = mem.stid;
    }
    auto peID = mem.peID;
    auto &rob_next = *nextROBs[mem.peID][tid];

    SimInst &inst = rob_next[mem.rid.val].inst;
    PLpvInfo lpvInfo = std::make_shared<LpvInfo>();
    if (mem.specWakeup) {
        lpvInfo->LoadInit(mem.pipeID);
    }

    inst->wakeupCnt += mem.laneSet.size();
    if (inst->wakeupCnt >= mem.realReqCnt) {
        inst->loadWakeuped = true;
        for (auto &pdst : inst->pdsts_) {
            if (mem.specWakeup &&
                (pdst->type == OperandType::OPD_TLINK || pdst->type == OperandType::OPD_ULINK)) {
                continue;
            }
            iq.WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled), lpvInfo, inst->peID,
                inst->tid, inst->stid);
        }
    }

    if (core->IsVecPe(peID)) {
        std::shared_ptr<VecPE> pe = core->vectorTop->GetPE(inst->coreID);
        pe->SetMemWakeup(mem);
    } else {
        std::shared_ptr<SPE> pe = dynamic_pointer_cast<SPE>(core->peArray[inst->peID]);
        pe->SetMemWakeup(mem);
    }
}

void IEX::PrintTlsPipeViewLog(SimInst &inst, MemReqBus mem)
{
    InstPipeViewPtr instInfo = std::make_shared<InstPipeViewInfo>();
    instInfo->bid = inst->bid.val;
    instInfo->cycleInfo = std::make_shared<CycleInfo>();
    instInfo->cycleInfo->instStartCycle = inst->pipeCycle->instStartCycle;
    instInfo->cycleInfo->sendToScalperCycle = mem.sendToScalperCycle;
    instInfo->cycleInfo->sendToTileReqCycle = inst->pipeCycle->sendToTileReqCycle;
    instInfo->cycleInfo->genPrefetchCycle = mem.genPrefetchCycle;
    instInfo->cycleInfo->prefetchDataRetCycle = mem.prefetchDataRetCycle;
    instInfo->cycleInfo->genLoadReadReqCycle = mem.genLoadReadReqCycle;
    instInfo->cycleInfo->sendFromScalperCycle = mem.sendFromScalperCycle;
    instInfo->cycleInfo->sendMemoryReqCycle = mem.sendMemoryReqCycle;
    instInfo->cycleInfo->tileDataRetCycle = inst->pipeCycle->tileDataRetCycle;
    instInfo->cycleInfo->genStoreReqCycle = inst->pipeCycle->genStoreReqCycle;
    instInfo->cycleInfo->loadDataReturnCycle = inst->pipeCycle->loadDataReturnCycle;
    instInfo->cycleInfo->tlsCompleteCycle = GetSim()->getCycles();
    instInfo->cycleInfo->retireCycle = GetSim()->getCycles() + 1;

    std::stringstream oss;
    oss << "Tload ";
    oss << " Read GM 0x" << std::hex << mem.addr << " B" << std::dec <<mem.bid.val << ":G" <<
        std::dec << mem.gid.val << ":R" << std::dec << mem.rid.val <<":SUB" << std::dec <<
        mem.subrid.val;
    instInfo->label =  oss.str();
    GetSim()->GetViewManager(inst->stid)->RecordMinst(instInfo);
}

void IEX::setMemData(MemReqBus const &mem) {
    if (!mem.vld) {
        return;
    }
    uint32_t rid = mem.rid.val;
    auto peID = mem.peID;
    ASSERT(currentROBs[peID][mem.tid] != nullptr);
    ASSERT(nextROBs[peID][mem.tid] != nullptr);
    auto &rob_current = *currentROBs[peID][mem.tid];
    auto &rob_next = *nextROBs[peID][mem.tid];

    if (rob_current[rid].status == INST_NEEDFLUSH) {
        return;
    }

    uint32_t retLane = 0;
    for (auto lane: mem.laneSet) {
        retLane = rob_next.resolveData(mem, lane, mem.simtMask);
    }

    // Retrieve SimInst
    SimInst inst = std::make_shared<SimInstInfo>(*rob_next[mem.rid.val].inst);
    for (auto &pdst : inst->pdsts_) {
        pdst->data = mem.data;
    }

    if ((core->IsVectorIex(inst->iexType) || (inst->iexType == MEM_IEX)) && inst->lanes > 0) {
        if (retLane < mem.realReqCnt) {
            return;
        }
    }

    if ((inst->iexType == MEM_IEX) && (inst->opcode == Opcode::OP_TLD)) {
        inst->peID = peID;

        inst->pipeCycle->loadDataReturnCycle = core->GetSim()->cycles;

        rob_next[mem.rid.val].inst->subInstCnt = rob_current[mem.rid.val].inst->subInstCnt - 1;
        LOG_INFO_M(Unit::MIEX, Stage::NA) <<" inst complete "<< mem;

        if (rob_next[mem.rid.val].inst->subInstCnt == 0) {
            TloadSendSeqToTileScb(mem, inst, true);
        } else {
            TloadSendSeqToTileScb(mem, inst, false);
        }

        if (GetSim()->GetViewManager(0)->config.printPipeView) {
            PrintTlsPipeViewLog(inst, mem);
        }

        if (rob_next[mem.rid.val].inst->subInstCnt != 0) {
            return;
        }
    }
    inst->isLoadReturn = true;
    loadBranchResolve(inst);

    inst->pipeCycle->lsuRecvCycle = mem.lsuRecvCycle;
    inst->pipeCycle->ldqPickCycle = mem.ldqPickCycle;
    inst->pipeCycle->ldqIssueCycle = mem.ldqIssueCycle;
    inst->pipeCycle->l1MissCycle = mem.l1MissCycle;
    inst->pipeCycle->l2MissCycle = mem.l2MissCycle;
    inst->pipeCycle->memRntCycle = mem.memRntCycle;
    inst->pipeCycle->l2RntCycle = mem.l2RntCycle;
    inst->pipeCycle->l1RntCycle = mem.l1RntCycle;
    inst->iq_name = mem.iq_name;
    inst->pipeCycle->ldRntCycle = GetSim()->getCycles();
    ++stats->total_mem_load_req_cnt;
    stats->total_mem_load_latency += (GetSim()->getCycles() - mem.lsuRecvCycle);

    if (core->IsVectorIex(machineType)) {
        for (auto &pipe : aguPipe) {
            if (!pipe.e4_inst) {
                pipe.e4_inst = inst;
                return;
            }
        }
    } else {
        for (auto &pipe : ldaPipe) {
            if (!pipe.e4_inst) {
                pipe.e4_inst = inst;
                return;
            }
        }
    }
    ASSERT(false && "No pipe to handle response from load.");
}

void IEX::TloadSendSeqToTileScb(MemReqBus const &mem, SimInst &inst, bool islast) const
{
    LOG_INFO_M(Unit::MIEX, Stage::E4) <<" send data to tile SCB, mem "<< mem <<
        " real tload addr 0x" << hex << mem.addr << " islast " << islast;
    uint64_t baseGMAddr = inst->psrcs_[SRC0_IDX]->baseAddr;   // 整个memory的初始地址
    // 这个指令所包含的数据的初始地址
    uint64_t startAddr = mem.addr;
    ASSERT((startAddr % 256) == 0);
    // 这个指令所包含的数据的末尾地址
    uint64_t endAddr = mem.addr + inst->psrcs_[SRC0_IDX]->mtcShapeSize;
    ASSERT(inst->psrcs_[SRC0_IDX]->mtcShapeSize <= 256);
    uint64_t datatypeSize = inst->psrcs_[SRC0_IDX]->mtcElementWidth;
    // 要写入tile reg的 初始地址
    uint64_t baseTRAddr = inst->pdsts_[DST0_IDX]->mtcRealAddr;
    uint64_t addrDiff = (startAddr > baseGMAddr) ? (startAddr - baseGMAddr) : (baseGMAddr - startAddr);
    ASSERT(addrDiff % datatypeSize == 0);
    ASSERT((endAddr - baseGMAddr) % datatypeSize == 0);
    // 两行两个元素间的差距，byte
    uint64_t stride = inst->psrcs_[SRC0_IDX]->mtcStrideGM;
    uint32_t dstFracSize = 512;     // 小z分型大小
    // tileReg块的总行数
    uint32_t rowNum = inst->pdsts_[DST0_IDX]->mtcD2TR;
    // tileReg块的总列数
    uint32_t colNum = inst->pdsts_[DST0_IDX]->mtcD1TR;

    LOG_INFO_M(Unit::MTC, Stage::NA) << "baseGMAddr:" << hex << baseGMAddr <<
        " ,stride:" << hex << stride <<
        " ,datatypeSize:" << dec << datatypeSize <<
        ", d2GM:" << dec << inst->psrcs_[SRC0_IDX]->mtcD2GM <<
        ", d1GM:" << dec << inst->psrcs_[SRC0_IDX]->mtcD1GM <<
        ", d2TR:" << dec << inst->pdsts_[DST0_IDX]->mtcD2TR <<
        ", d1TR:" << dec << inst->pdsts_[DST0_IDX]->mtcD1TR <<
        ", baseTRAddr:" << hex << baseTRAddr <<
        ", layout:" << GetLayOutName(inst->psrcs_[SRC0_IDX]->mtcLayout);

    if ((inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::DN2NZ) || (inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::DN2ZN)) {
        uint32_t tmp = rowNum;
        rowNum = colNum;
        colNum = tmp;
    }
    for (uint64_t addr = startAddr; addr < endAddr; addr += datatypeSize) {
        ASSERT((addr + datatypeSize) <= endAddr);
        if (addr < baseGMAddr) { continue; }
        // 元素的byte是横着摆
        uint32_t row = ((addr - baseGMAddr) /  stride);
        ASSERT((((addr - baseGMAddr) %  stride) % datatypeSize) == 0);
        uint32_t col = (((addr - baseGMAddr) %  stride) / datatypeSize);
        if ((row >= inst->psrcs_[SRC0_IDX]->mtcD2GM) || (col >= inst->psrcs_[SRC0_IDX]->mtcD2GM)) {
            continue;
        }
        if ((inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::DN2NZ) ||
            (inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::DN2ZN)) {
            uint32_t tmp = row;
            row = col;
            col = tmp;
        }
        // 计算目的地址
        uint64_t dstTRAddr;
        if (inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::NORM) {
            dstTRAddr = baseTRAddr + row * datatypeSize * colNum + col * datatypeSize;
        } else if (inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::ND2NZ) {
            uint32_t dstFracRow = 16;
            uint32_t dstFracCol = dstFracSize / (dstFracRow * datatypeSize);  // z分型的列数（c0)
            ASSERT((rowNum % dstFracRow) == 0);
            ASSERT((colNum % dstFracCol) == 0);
            uint32_t dstFracJ = col / dstFracCol;
            dstTRAddr = baseTRAddr + (dstFracJ * rowNum * dstFracCol + row * dstFracCol + col % dstFracCol)
                * datatypeSize;
        } else if (inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::ND2ZN) {
            uint32_t dstFracCol = 16;
            uint32_t dstFracRow = dstFracSize / (dstFracCol * datatypeSize);  // z分型的列数（c0)
            ASSERT((rowNum % dstFracRow) == 0);
            ASSERT((colNum % dstFracCol) == 0);
            uint32_t dstFracI = row / dstFracRow;
            dstTRAddr = baseTRAddr + (dstFracI * colNum * dstFracRow + col * dstFracRow + row % dstFracRow) *
                datatypeSize;
        } else if (inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::DN2NZ) {
            uint32_t dstFracRow = 16;
            uint32_t dstFracCol = dstFracSize / (dstFracRow * datatypeSize);  // z分型的列数（c0)
            ASSERT((rowNum % dstFracRow) == 0);
            ASSERT((colNum % dstFracCol) == 0);
            uint32_t dstFracJ = col / dstFracCol;
            dstTRAddr = baseTRAddr + (dstFracJ * rowNum * dstFracCol + row * dstFracCol + col % dstFracCol) *
                datatypeSize;
        } else if (inst->psrcs_[SRC0_IDX]->mtcLayout == LayOut::DN2ZN) {
            uint32_t dstFracCol = 16;
            uint32_t dstFracRow = dstFracSize / (dstFracCol * datatypeSize);  // z分型的列数（c0）
            ASSERT((rowNum % dstFracRow) == 0);
            ASSERT((colNum % dstFracCol) == 0);
            uint32_t dstFracI = row / dstFracRow;
            dstTRAddr = baseTRAddr + (dstFracI * colNum * dstFracRow + col * dstFracRow + row % dstFracRow) *
                datatypeSize;
        } else {
            ASSERT(false);
            return;
        }

        LOG_INFO_M(Unit::MTC, Stage::NA) << " bid: " << dec <<inst->bid.GetVal() << " gid: "
            << dec << inst->gid.GetVal() << dec << " rid: " << dec << inst->rid.GetVal()
            << " GMaddr: 0x" << hex << addr
            << " dstTRAddr: 0x" << hex << dstTRAddr
            << " is from last inst: " << islast;

        VecTileRegStReq req;
        // 判断一个element内部是否跨tileSCB entry, 预期不跨
        bool isCross = (dstTRAddr & (~0xff)) != ((dstTRAddr + datatypeSize - 1) & (~0xff));
        ASSERT(!isCross);
        req.SetStid(mem.stid);
        // 设置 写入的SCB entry 地址
        req.SetDest(dstTRAddr & (~0xff));
        // 设置 写入的 data 大小
        req.SetSize(datatypeSize);
        // 设置 mask
        DataMask dataMask(1);
        dataMask.Reset();
        dataMask.Set((dstTRAddr & 0xff), (dstTRAddr & 0xff) + datatypeSize);
        req.SetDataMask(dataMask);
        // 设置 data
        uint8_t data[256] = {0};
        for (uint32_t j = 0; j < datatypeSize; j++) {
            data[j + (dstTRAddr & 0xff)] = mem.mtc_reqData.data.bits[(addr & 0xff) + j];
        }
        req.SetData(data, datatypeSize);
        req.SetReqId(99);
        req.SetBid(inst->bid);
        req.SetGid(inst->gid);
        req.SetRid(inst->rid);
        if (islast) {
            req.SetLast();
        }
        core->iex[MEM_IEX]->iexScbReqQ->Write(req);
    }
}

void IEX::SetTileLsuPipeView(const MemRequest& mem)
{
    uint32_t rid = mem.rid.val;
    auto peID = mem.peID;
    ASSERT(currentROBs[peID][mem.tid] != nullptr);
    ASSERT(nextROBs[peID][mem.tid] != nullptr);
    auto &rob_current = *currentROBs[peID][mem.tid];
    auto &rob_next = *nextROBs[peID][mem.tid];
    if (rob_current[rid].status == INST_NEEDFLUSH) {
        return;
    }
    SimInst& inst = rob_next[mem.rid.val].inst;
    inst->pipeCycle->l1MissCycle = mem.tileLdMissCyc;
    inst->pipeCycle->l1RntCycle = mem.tileLdRefillCyc;
    inst->pipeCycle->e2Cycle = mem.e2Cyc;
    inst->pipeCycle->e3Cycle = mem.e3Cyc;
    inst->pipeCycle->e4Cycle = GetSim()->getCycles();
}

void IEX::setFlush(FlushBus &flushReq) {
    for (auto stage: stages) {
        if (!(*stage)) {
            continue;
        }
        if ((*stage)->stid != flushReq.req.stid) {
            continue;
        }
        if (flushReq.baseOnPE && flushReq.req.peID != (*stage)->peID) {
            continue;
        }
        bool lessEq = flushReq.baseOnBid ? LessEqual(flushReq.req.bid, (*stage)->bid):
            LessEqual(flushReq.req.bid, flushReq.req.rid, (*stage)->bid, (*stage)->rid);
        if (lessEq) {
            (*stage) = shared_ptr<SimInstInfo>(nullptr);
        }
    }

    next.flush(flushReq);
    dispatchUnit.flush(flushReq);
    iq.flush(flushReq);
    rf.flush(flushReq);
    rtable.flush(flushReq);
    flushPipe(flushReq);
    flushIQPECount();

    auto memFMatch = [&flushReq](MemReqBus &req)->bool {
        if (flushReq.req.stid != req.stid) {
            return false;
        }
        if (flushReq.baseOnPE && flushReq.req.peID != req.peID) {
            return false;
        }
        return (flushReq.baseOnBid ? LessEqual(flushReq.req.bid, req.bid):
            LessEqual(flushReq.req.bid, flushReq.req.rid, req.bid, req.rid));
    };

    for (uint32_t stid = 0; stid < lsu_iex_lret_array.size(); ++stid) {
        for (auto simQ : lsu_iex_lret_array[stid]) {
            simQ->FlushIf(memFMatch);
        }
    }

    if (flushReq.baseOnPE && !ssrsetOrderQ.Empty()) {
        ASSERT(false && "simt mode flush but the there is system set.");
    }

    auto match = [&flushReq](SimInst &inst) -> bool {
        if (inst->stid != flushReq.req.stid) {
            return false;
        }
        if (flushReq.baseOnPE && flushReq.req.peID != inst->peID) {
            return false;
        }
        return (flushReq.baseOnBid ? LessEqual(flushReq.req.bid, inst->bid):
            LessEqual(flushReq.req.bid, flushReq.req.rid, inst->bid, inst->rid));
    };
    ssrsetOrderQ.FlushIf(match);
    for (auto it = brRlsvQ.begin(); it != brRlsvQ.end(); ) {
        if (match(*it)) {
            it = brRlsvQ.erase(it);
        } else {
            ++it;
        }
    }
}

void IEX::flushIQPECount() {
    auto update_count = [] (vector<IssueState> iq_array, DispatchInfo &info) {
        for (auto &c : info.count) {
            c = 0;
        }

        for (uint32_t i = 0; i < iq_array.size(); i++) {
            for (auto inst : iq_array[i].entry) {
                if (inst) {
                    info.count[inst->peID]++;
                }
            }
            if (iq_array[i].size < iq_array[info.dispatchID].size) {
                info.dispatchID = i;
            }
        }
    };
    update_count(iq.next.cmdIQ, dispatchUnit.cmdInfo);
    update_count(iq.next.aluIQ, dispatchUnit.aluInfo);
    update_count(iq.next.aguIQ, dispatchUnit.aguInfo);
    update_count(iq.next.ldaIQ, dispatchUnit.ldaInfo);
    update_count(iq.next.staIQ, dispatchUnit.staInfo);
    update_count(iq.next.stdIQ, dispatchUnit.stdInfo);
    update_count(iq.next.bruIQ, dispatchUnit.bruInfo);
    update_count(iq.next.scaIQ, dispatchUnit.scaInfo);
}

void IEXState::flush(FlushBus &flushReq) {
}

void IEX::flushPipe(FlushBus &flushReq) {
    for (auto &pipe : ldaPipe) {
        pipe.flush(flushReq);
    }
    for (auto &pipe : ldascPipe) {
        pipe.flush(flushReq);
    }
    for (auto &pipe : staPipe) {
        pipe.flush(flushReq);
    }
    for (auto &pipe : stdPipe) {
        pipe.flush(flushReq);
    }
    for (auto &pipe : cmdPipe) {
        pipe.flush(flushReq);
    }
    for (auto &pipe : aluPipe) {
        pipe.flush(flushReq);
    }
    for (auto &pipe : bruPipe) {
        pipe.flush(flushReq);
    }
    for (auto &pipe : aguPipe) {
        pipe.flush(flushReq);
    }
    for (auto &pipe : scaPipe) {
        pipe.flush(flushReq);
    }
}

bool waitingGgpr(SimInst inst) {
    if (inst->IsReady()) {
        return false;
    }
    for (auto &psrc : inst->psrcs_) {
        if (psrc->type == OperandType::OPD_GREG && !psrc->ready) {
            return true;
        }
    }
    return false;
}

bool IEX::waitingGet() {
    // bool any_waiting_get = false;
    auto checkIQ = [](const std::vector<IssueState>& iqArray)->bool {
        for (auto &iq : iqArray) {
            if (iq.size == 0) {
                continue;
            }

            for (auto &e : iq.entry) {
                if (e && waitingGgpr(e)) {
                    return true;
                }
            }
        }
        return false;
    };

    if (checkIQ(iq.current.cmdIQ) || checkIQ(iq.current.aluIQ) || checkIQ(iq.current.aguIQ)
        || checkIQ(iq.current.staIQ)|| checkIQ(iq.current.stdIQ) || checkIQ(iq.current.bruIQ)) {
        return true;
    }

    return false;
}

MemReqBus getMemReq(SimInst &inst) {
    MemReqBus memReq;
    memReq.peID = inst->peID;
    memReq.stid = inst->stid;

    // Service non-speculative load/store first
    if (inst) {
        memReq.addr = inst->accMemInfo->accMemAddr;

        memReq.vld = true;
        memReq.opcode = inst->opcode;
        memReq.is_load = OpcodeIsLoad(memReq.opcode);
        memReq.size = GetLoadStoreBytes(memReq.opcode);
        memReq.prefetch = false;
        memReq.bid = inst->bid;
        memReq.rid = inst->rid;
        memReq.instUid = inst->uid;
        memReq.tpc = inst->pc;
        memReq.lsID = inst->lsID;
        memReq.is_llsc = false;

        // if (inst->srcSP.vld) {
        //     memReq.stack_vld = true;
        // } else {
        //     memReq.stack_vld = false;
        // }

        if (!memReq.is_load) {
            memReq.data_vld = true;
            memReq.data = inst->srcs[SRC0_IDX]->data;
            memReq.type = inst->type;
        } else {
            memReq.data_vld = false;
        }
    }

    return memReq;
}

void IEX::requestLSU() {
    auto request = [this](SimInst &e1_inst, SimQueue<MemReqBus>* &iex_lsu_q) {
        if (!e1_inst) {
            return;
        }

        // Generate request.
        MemReqBus req = getMemReq(e1_inst);
        if (!req.vld) {
            return;
        }

        // Send to LSU.
        iex_lsu_q->Write(req);
    };

    for (uint32_t i = 0; i < aguPipe.size(); i++) {
        if (aguPipe[i].e1_inst && checkLdStall(aguPipe[i].e1_inst->stid)) {
            break;
        }
        if (aguPipe[i].e1_inst && OpcodeIsLoad(aguPipe[i].e1_inst->opcode)) {
            request(aguPipe[i].e1_inst, iex_lsu_lda_array[aguPipe[i].e1_inst->stid][i]);
        } else if (aguPipe[i].e1_inst && OpcodeIsStore(aguPipe[i].e1_inst->opcode)) {
            request(aguPipe[i].e1_inst, iex_lsu_sta_array[aguPipe[i].e1_inst->stid][i]);
        }
    }

    for (uint32_t i = 0; i < ldaPipe.size(); i++) {
        if (ldaPipe[i].e1_inst && checkLdStall(ldaPipe[i].e1_inst->stid)) {
            break;
        }
        request(ldaPipe[i].e1_inst, iex_lsu_lda_array[ldaPipe[i].e1_inst->stid][i]);
    }

    for (uint32_t i = 0; i < staPipe.size(); i++) {
        if (staPipe[i].e1_inst && GetLsucheckStoreStall(1, staPipe[i].e1_inst->stid)) {
            break;
        }
        request(staPipe[i].e1_inst, iex_lsu_sta_array[staPipe[i].e1_inst->stid][i]);
    }

    for (uint32_t i = 0; i < stdPipe.size(); i++) {
        if (stdPipe[i].e1_inst && !GetLsucheckStoreStall(1, stdPipe[i].e1_inst->stid)) {
            break;
        }
        request(stdPipe[i].e1_inst, iex_lsu_std_array[stdPipe[i].e1_inst->stid][i]);
    }
}

template<typename T>
static void CheckPipeValid(vector<T> &pipe, bool& pipeFull)
{
    for (auto &p : pipe) {
        if (!p.e4_inst) {
            pipeFull = false;
            break;
        }
    }
}

void IEX::BackToPipe(bool& pipeFull)
{
    if (core->IsVectorIex(machineType)) {
        CheckPipeValid(aguPipe, pipeFull);
    } else {
        CheckPipeValid(ldaPipe, pipeFull);
    }
}

void IEX::receiveFromLSU() {
    auto handleReq = [this](SimQueue<MemReqBus>& simQ, uint32_t& count) {
        while (!simQ.Empty()) {
            bool pipeFull = true;
            BackToPipe(pipeFull);
            if (pipeFull) {
                break;
            }
            count++;
            MemReqBus memResp = simQ.Read();
            setMemData(memResp);
        }
    };
    uint32_t count = 0;
    for (uint32_t stid = 0; stid < lsu_iex_lret_array.size(); ++stid) {
        for (auto simQ : lsu_iex_lret_array[stid]) {
            handleReq(*simQ, count);
        }
    }
}

void IEX::ResetRdyTable(OperandType type, uint32_t ptag, uint32_t peid, uint32_t tid) {
    ASSERT(tid != -1U);
    PLpvInfo lpvInfo;
    this->SetRegReadyTable(type, ptag, false, lpvInfo, peid, tid);
};

void IEX::setPipeCancel(uint32_t pipeID) {
    auto cancelInst = [pipeID, this](SimInst &inst) {
        if (inst && inst->CheckCancel(pipeID)) {
            PLpvInfo lpvInfo;
            PEResolveBus peResolve;
            peResolve.peid = inst->peID;
            peResolve.bid = inst->bid;
            peResolve.rid = inst->rid;
            peResolve.isLDCancel = true;
            this->iex_pe_rslv_array[inst->peID][inst->GetTid()]->Write(peResolve);
            if (inst->SrcTypeContain(OperandType::STACK_POINTER)) {
                this->rtable.setROBReadyTable(inst->peID, inst->rid.val, lpvInfo, false);
            }
            for (auto pdst : inst->pdsts_) {
                this->ResetRdyTable(pdst->type, pdst->ptag, inst->peID, inst->GetTid());
                if (pdst->type == OperandType::STACK_POINTER) {
                    this->core->sRenameUnit->setSpPtagReady(pdst->ptag, false, lpvInfo);
                }
            }

            inst = std::shared_ptr<SimInstInfo>(nullptr);
        }
    };

    for (auto &pipe : cmdPipe) {
        cancelInst(pipe.w1_inst);
        cancelInst(pipe.e1_inst);
        cancelInst(pipe.i2_inst);
        cancelInst(pipe.i1_inst);
        if (pipe.p1_inst && *pipe.p1_inst) {
            cancelInst(*pipe.p1_inst);
        }
    }

    for (auto &pipe : aluPipe) {
        for (auto& inst : pipe.w2_inst) {
            cancelInst(inst);
        }
        for (auto& inst : pipe.w1_inst) {
            cancelInst(inst);
        }
        for (auto& v_inst : pipe.ex_inst) {
            for (auto& inst : v_inst) {
                cancelInst(inst);
            }
        }
        for (auto& iter : pipe.e0_inst) {
            auto& inst = iter.second;
            cancelInst(inst);
        }
        cancelInst(pipe.i2_inst);
        cancelInst(pipe.i1_inst);
        if (pipe.p1_inst && *pipe.p1_inst) {
            cancelInst(*pipe.p1_inst);
        }
    }

    for (auto &pipe : bruPipe) {
        cancelInst(pipe.e1_inst);
        cancelInst(pipe.i2_inst);
        cancelInst(pipe.i1_inst);
        if (pipe.p1_inst && *pipe.p1_inst) {
            cancelInst(*pipe.p1_inst);
        }
    }

    for (auto &pipe : ldaPipe) {
        cancelInst(pipe.w2_inst);
        cancelInst(pipe.w1_inst);
        cancelInst(pipe.e1_inst);
        cancelInst(pipe.i2_inst);
        cancelInst(pipe.i1_inst);
        if (pipe.p1_inst && *pipe.p1_inst) {
            cancelInst(*pipe.p1_inst);
        }
    }

    for (auto &pipe : ldascPipe) {
        cancelInst(pipe.w2_inst);
        cancelInst(pipe.w1_inst);
        cancelInst(pipe.e1_inst);
        cancelInst(pipe.i2_inst);
        cancelInst(pipe.i1_inst);
        if (pipe.p1_inst && *pipe.p1_inst) {
            cancelInst(*pipe.p1_inst);
        }
    }

    for (auto &pipe : staPipe) {
        cancelInst(pipe.w2_inst);
        cancelInst(pipe.w1_inst);
        cancelInst(pipe.e1_inst);
        cancelInst(pipe.i2_inst);
        cancelInst(pipe.i1_inst);
        if (pipe.p1_inst && *pipe.p1_inst) {
            cancelInst(*pipe.p1_inst);
        }
    }

    for (auto &pipe : stdPipe) {
        cancelInst(pipe.e1_inst);
        cancelInst(pipe.i2_inst);
        cancelInst(pipe.i1_inst);
        if (pipe.p1_inst && *pipe.p1_inst) {
            cancelInst(*pipe.p1_inst);
        }
    }

    for (auto &pipe : aguPipe) {
        cancelInst(pipe.w2_inst);
        cancelInst(pipe.w1_inst);
        cancelInst(pipe.e1_inst);
        cancelInst(pipe.i2_inst);
        cancelInst(pipe.i1_inst);
        if (pipe.p1_inst && *pipe.p1_inst) {
            cancelInst(*pipe.p1_inst);
        }
    }

    for (auto &pipe : scaPipe) {
        for (auto& inst : pipe.w2_inst) {
            cancelInst(inst);
        }
        for (auto& inst : pipe.w1_inst) {
            cancelInst(inst);
        }
        for (auto& v_inst : pipe.ex_inst) {
            for (auto& inst : v_inst) {
                cancelInst(inst);
            }
        }
        cancelInst(pipe.i1_inst);
        if (pipe.p1_inst && *pipe.p1_inst) {
            cancelInst(*pipe.p1_inst);
        }
    }
}

void IEX::reportCancel(MemReqBus &memReqBus) {
    if (!memReqBus.vld) {
        return;
    }

    auto &rob = *nextROBs[memReqBus.peID][memReqBus.tid];
    SimInst inst = rob[memReqBus.rid.val].inst;
    bool cancelEffect = (inst->wakeupCnt >= memReqBus.realReqCnt);
    ASSERT(inst->wakeupCnt > 0);
    inst->wakeupCnt -= memReqBus.laneSet.size();

    // All the cancel requests generate at the same time, but effect only onece.
    if (!cancelEffect) {
        return;
    }
    for (auto pdst : inst->pdsts_) {
        if (pdst->type == OperandType::STACK_POINTER) {
            PLpvInfo lpvInfo;
            core->sRenameUnit->setSpPtagReady(pdst->ptag, false, lpvInfo);
        } else {
            ResetRdyTable(pdst->type, pdst->ptag, inst->peID, memReqBus.tid);
        }
    }

    auto iexCancel = [this, &memReqBus](ExecEngineTyp iexType) {
        std::shared_ptr<IEX> iex;
        if (core->IsVectorIex(iexType)) {
            iex = GetSim()->core->vectorTop->GetIex(
                static_cast<uint32_t>(iexType) - static_cast<uint32_t>(ExecEngineTyp::IEX_NUM));
        } else {
            iex = GetSim()->core->iex[iexType];
        }
        ASSERT(iex != nullptr);
        iex->setPipeCancel(memReqBus.pipeID);
        iex->iq.setIQCancel(memReqBus.pipeID);
        iex->dispatchUnit.setCancel(memReqBus.pipeID);
    };

    iexCancel(SCALAR_IEX);
    if (core->configs.mtc_separate_enable) {
        iexCancel(MEM_IEX);
    }
    uint32_t iexNum = static_cast<uint32_t>(ExecEngineTyp::IEX_NUM) + core->configs.simtPeCount;
    for (uint32_t i = static_cast<uint32_t>(ExecEngineTyp::IEX_NUM); i < iexNum; ++i) {
        iexCancel(static_cast<ExecEngineTyp>(i));
    }
}

void IEX::SubReleaseIQEntryI2()
{
    for (auto &pipe : cmdPipe) {
        if (pipe.i2_inst) {
            iq.releaseCMDEntry(pipe.i2_inst->bid, pipe.i2_inst->rid, pipe.i2_inst->stid);
        }
    }

    for (auto &pipe : aluPipe) {
        for (auto& inst : pipe.release_iq_inst) {
            if (inst) {
                iq.releaseALUEntry(inst->bid, inst->rid, inst->stid);
                LOG_DEBUG_M(machineType, Stage::NA) << "ReleaseEntry C:" << dec << GetSim()->getCycles() << " " << inst->Dump();
            }
        }
        pipe.release_iq_inst.clear();
    }

    for (auto &pipe : bruPipe) {
        if (pipe.i2_inst) {
            iq.releaseBRUEntry(pipe.i2_inst->bid, pipe.i2_inst->rid, pipe.i2_inst->stid);
        }
    }

    for (auto &pipe : aguPipe) {
        if (pipe.i2_inst) {
            iq.releaseAGUEntry(pipe.i2_inst->bid, pipe.i2_inst->rid, pipe.i2_inst->stid);
        }
    }

    for (auto &pipe : ldaPipe) {
        if (pipe.i2_inst) {
            iq.ReleaseLDAEntry(pipe.i2_inst->bid, pipe.i2_inst->rid, pipe.i2_inst->stid);
        }
    }

    for (auto &pipe : ldascPipe) {
        if (pipe.i2_inst) {
            iq.ReleaseLDASCEntry(pipe.i2_inst->bid, pipe.i2_inst->rid, pipe.i2_inst->stid);
        }
    }

    for (auto &pipe : staPipe) {
        if (pipe.i2_inst) {
            iq.releaseSTAEntry(pipe.i2_inst->bid, pipe.i2_inst->rid, pipe.i2_inst->stid);
        }
    }

    for (auto &pipe : stdPipe) {
        if (pipe.i2_inst) {
            iq.releaseSTDEntry(pipe.i2_inst->bid, pipe.i2_inst->rid, pipe.i2_inst->stid);
        }
    }

    // Keep VecScalarPipe release at E1
    for (auto &pipe : scaPipe) {
        ASSERT(pipe.ex_inst.size() > 0);
        for (auto& inst : pipe.ex_inst[0]) {
            if (inst) {
                iq.ReleaseVecScaEntry(inst->bid, inst->rid, inst->stid);
            }
        }
    }
}

void IEX::SubReleaseIQEntryAfterIssue()
{
    for (auto& inst : iq.selectCmdInst) {
        if (inst) {
            iq.releaseCMDEntry(inst->bid, inst->rid, inst->stid);
        }
    }
    for (auto& inst : iq.selectAluInst) {
        if (inst) {
            iq.releaseALUEntry(inst->bid, inst->rid, inst->stid);
        }
    }
    for (auto& inst : iq.selectBruInst) {
        if (inst) {
            iq.releaseBRUEntry(inst->bid, inst->rid, inst->stid);
        }
    }
    for (auto& inst : iq.selectAguInst) {
        if (inst) {
            iq.releaseAGUEntry(inst->bid, inst->rid, inst->stid);
        }
    }
    for (auto& inst : iq.selectLdaInst) {
        if (inst) {
            iq.ReleaseLDAEntry(inst->bid, inst->rid, inst->stid);
        }
    }
    for (auto& inst : iq.selectSCLdaInst) {
        if (inst) {
            iq.ReleaseLDASCEntry(inst->bid, inst->rid, inst->stid);
        }
    }
    for (auto& inst : iq.selectStaInst) {
        if (inst) {
            iq.releaseSTAEntry(inst->bid, inst->rid, inst->stid);
        }
    }
    for (auto& inst : iq.selectStdInst) {
        if (inst) {
            iq.releaseSTDEntry(inst->bid, inst->rid, inst->stid);
        }
    }
    for (auto& inst : iq.selectScaInst) {
        if (inst) {
            iq.ReleaseVecScaEntry(inst->bid, inst->rid, inst->stid);
        }
    }
}

void IEX::releaseIQEntry() {
    if (core->IsVectorIex(machineType) || (id == MEM_IEX)) {
        if (configs.simt_iex_iq_dealloc_option == 1U) {
            // release at I2 stage
            SubReleaseIQEntryI2();
            return;
        } else if (configs.simt_iex_iq_dealloc_option == 2U) {
            // release after issue
            SubReleaseIQEntryAfterIssue();
            return;
        }
    }

    for (auto &pipe : cmdPipe) {
        if (pipe.e1_inst) {
            iq.releaseCMDEntry(pipe.e1_inst->bid, pipe.e1_inst->rid, pipe.e1_inst->stid);
        }
    }
    for (auto &pipe : aluPipe) {
        for (auto& inst : pipe.release_iq_inst) {
            if (inst) {
                iq.releaseALUEntry(inst->bid, inst->rid, inst->stid);
            }
        }
        pipe.release_iq_inst.clear();
    }
    for (auto &pipe : bruPipe) {
        if (pipe.e1_inst) {
            iq.releaseBRUEntry(pipe.e1_inst->bid, pipe.e1_inst->rid, pipe.e1_inst->stid);
        }
    }
    for (auto &pipe : aguPipe) {
        if (pipe.e1_inst) {
            iq.releaseAGUEntry(pipe.e1_inst->bid, pipe.e1_inst->rid, pipe.e1_inst->stid);
        }
    }
    for (auto &pipe : ldaPipe) {
        if (pipe.e1_inst) {
            iq.ReleaseLDAEntry(pipe.e1_inst->bid, pipe.e1_inst->rid, pipe.e1_inst->stid);
        }
    }
    for (auto &pipe : ldascPipe) {
        if (pipe.e1_inst) {
            iq.ReleaseLDASCEntry(pipe.e1_inst->bid, pipe.e1_inst->rid, pipe.e1_inst->stid);
        }
    }
    for (auto &pipe : staPipe) {
        if (pipe.e1_inst) {
            iq.releaseSTAEntry(pipe.e1_inst->bid, pipe.e1_inst->rid, pipe.e1_inst->stid);
        }
    }
    for (auto &pipe : stdPipe) {
        if (pipe.e1_inst) {
            iq.releaseSTDEntry(pipe.e1_inst->bid, pipe.e1_inst->rid, pipe.e1_inst->stid);
        }
    }
    for (auto &pipe : scaPipe) {
        ASSERT(pipe.ex_inst.size() > 0);
        for (auto& inst : pipe.ex_inst[0]) {
            if (inst) {
                iq.ReleaseVecScaEntry(inst->bid, inst->rid, inst->stid);
            }
        }
    }
}

void IEX::setMemResolve(MemReqBus const &mem) {
}

void IEX::checkIQStall() {
    auto checkStall_func = [this](uint64_t &iqStall, uint64_t &iqStallCnt) {
        if (iqStall < configs.max_iq_stall) {
            return;
        }

        for (uint32_t i = 0; i < GetSim()->core->configs.scalar_smt_thread; ++i) {
            if (core->bctrl->blockROB.IsOldestBlkComplete(i)) {
                continue;
            }
            iqStallCnt++;
            ROBID bid = core->bctrl->blockROB.getOldestBlockID(i);
            FlushReq req = FlushReq();
            req.vld = true;
            uint32_t brobCap = core->bctrl->blockROB.getBROBCapacity(i);
            IncROBID(bid, brobCap);
            req.bid = bid;
            req.type = FlushType::FAST_REPLAY;
            req.stid = i;
            iqStall = 0;
            core->flushUnit->flush_stats->IssueQueueStall++;
            iex_flush_rpt_q->Write(req);
        }
    };

    checkStall_func(cmdIQStallCycle, cmdIQStallCnt);
    checkStall_func(aluIQStallCycle, aluIQStallCnt);
    checkStall_func(aguIQStallCycle, aguIQStallCnt);
    checkStall_func(ldaIQStallCycle, ldaIQStallCnt);
    checkStall_func(staIQStallCycle, staIQStallCnt);
    checkStall_func(stdIQStallCycle, stdIQStallCnt);
    checkStall_func(bruIQStallCycle, bruIQStallCnt);
    checkStall_func(scaIQStallCycle, scaIQStallCnt);
}

void IEX::updateIQStall() {
    auto updateStall= [](uint64_t &stallCycle, vector<IssueState> &iq, bool stall) {
        bool stallFlag = false;
        for (uint32_t i = 0; i < iq.size(); i++) {
            if (stall && iq[i].full()) {
                stallFlag = true;
            } else {
                stallFlag = false;
                stallCycle = 0;
                return;
            }
        }

        if (stallFlag) {
            stallCycle++;
        }
    };

    updateStall(cmdIQStallCycle, iq.current.cmdIQ, iq.cmdIQStall);
    updateStall(aluIQStallCycle, iq.current.aluIQ, iq.aluIQStall);
    updateStall(aguIQStallCycle, iq.current.aguIQ, iq.aguIQStall);
    updateStall(ldaIQStallCycle, iq.current.ldaIQ, iq.ldaIQStall);
    updateStall(staIQStallCycle, iq.current.staIQ, iq.staIQStall);
    updateStall(stdIQStallCycle, iq.current.stdIQ, iq.stdIQStall);
    updateStall(bruIQStallCycle, iq.current.bruIQ, iq.bruIQStall);
    updateStall(scaIQStallCycle, iq.current.scaIQ, iq.scaIQStall);
}

bool IEX::GetLsucheckLoadCltStall(uint64_t addr, uint32_t width, uint32_t stid)
{
    if (id == SCALAR_IEX) {
        return core->memIntf[static_cast<int>(LSUType::SCALAR_LSU)]->CheckLoadCltStall(addr, width, stid);
    }
    if (core->IsVectorIex(machineType)) {
        return core->memIntf[static_cast<int>(LSUType::VECTOR_LSU)]->CheckLoadCltStall(addr, width, stid);
    }
    if (id == MEM_IEX) {
        return core->MtcmemIntf[0]->CheckLoadCltStall(addr, width);
    }
    ASSERT(false && "not support");
    return false;
}

bool IEX::GetLsucheckStoreStall(uint32_t size, uint32_t stid)
{
    if (id == SCALAR_IEX) {
        return core->memIntf[static_cast<int>(LSUType::SCALAR_LSU)]->CheckStoreStall(size, stid);
    }
    if (core->IsVectorIex(machineType)) {
        return core->memIntf[static_cast<int>(LSUType::VECTOR_LSU)]->CheckStoreStall(size, stid);
    }
    if (id == MEM_IEX) {
        return core->MtcmemIntf[0]->CheckStoreStall(size);
    }
    ASSERT(false && "not support");
    return false;
}

void IEX::GetLsustatsMembound(bool& anyload, bool& l1miss, bool& l2miss, bool& stqfull, uint32_t stid)
{
    if (id == SCALAR_IEX) {
        core->memIntf[static_cast<int>(LSUType::SCALAR_LSU)]->StatsMemBound(anyload, l1miss, l2miss, stqfull, stid);
        return;
    }
    if (core->IsVectorIex(machineType)) {
        core->memIntf[static_cast<int>(LSUType::VECTOR_LSU)]->StatsMemBound(anyload, l1miss, l2miss, stqfull, stid);
        return;
    }
    if (id == MEM_IEX) {
        core->MtcmemIntf[0]->StatsMemBound(anyload, l1miss, l2miss, stqfull);
        return;
    }
    ASSERT(false && "not support");
    return;
}

bool IEX::GetLsuload_to_use_enable()
{
    if (id == SCALAR_IEX) {
        return core->memIntf[static_cast<int>(LSUType::SCALAR_LSU)]->configs.load_to_use_enable;
    }
    if (core->IsVectorIex(machineType)) {
        return core->memIntf[static_cast<int>(LSUType::VECTOR_LSU)]->configs.load_to_use_enable;
    }
    if (id == MEM_IEX) {
        return core->MtcmemIntf[0]->configs.load_to_use_enable;
    }
    ASSERT(false && "not support");
    return false;
}

void IEX::doStats() {
    uint64_t slots = core->GetScalarPEDecWidth() * core->configs.stdPeCount;
    slots += core->GetVecPEDecWidth() * core->configs.simtPeCount;
    stats->slots += slots;
    if (iq.current.isEmpty()) {
        stats->slots_idle += slots;
        return;
    }

    auto& issued_cnt = stats->issued_cnt; // updated at issue, cleared at the end of doStats()
    if (issued_cnt <= 1) {
        ++stats->exec_stall;
        if (issued_cnt == 0) { // only stats memstall if no uop is issued in this cycle
            bool anyload = false, l1miss = false, l2miss = false, stqfull = false;
            for (uint32_t stid = 0; stid < GetSim()->core->configs.scalar_smt_thread; ++stid) {
                GetLsustatsMembound(anyload, l1miss, l2miss, stqfull, stid);
            }
            stats->memstall_anyload += uint32_t(anyload);
            stats->memstall_l1miss += uint32_t(l1miss);
            stats->memstall_l2miss += uint32_t(l2miss);
        }
    }

    issued_cnt = 0;
}

bool IEX::checkSimtAluStall(const SimInst& inst, uint32_t pipeIdx)
{
    // Stall by Scoreboard
    uint32_t noBusCfltCycles = configs.simt_iex_max_ex_no_rslv_cflt;
    if (configs.simt_iex_vec_rslv_cflt && aluPipe[pipeIdx].isPipeStallByScb(inst, noBusCfltCycles)) {
        return true;
    }
    // Stall by Iter Cycles
    if (configs.simt_iex_stall_per_pipe) {
        if (aluPipe[pipeIdx].isPipeStallByIterCycles(inst)) {
            return true;
        }
    }
    for (uint32_t i = 0; i < iexAluIqCount * iexAluPickCount; i++) {
        if (aluPipe[i].isPipeStallByIterCycles(inst)) {
            return true;
        }
    }
    // No stall
    return false;
}

bool IEX::checkSimtScaStall(const SimInst& inst, uint32_t pipeIdx)
{
    // Stall by Scoreboard
    uint32_t noBusCfltCycles = configs.simt_iex_max_ex_no_rslv_cflt;
    if (configs.simt_iex_sca_rslv_cflt && scaPipe[pipeIdx].isPipeStallByScb(inst, noBusCfltCycles)) {
        return true;
    }
    // No stall
    return false;
}

bool IEX::checkLdStall(uint32_t stid) {
    if (id == SCALAR_IEX) {
        return core->memIntf[static_cast<int>(LSUType::SCALAR_LSU)]->CheckLoadStall(stid);
    }
    if (core->IsVectorIex(machineType)) {
        return core->memIntf[static_cast<int>(LSUType::VECTOR_LSU)]->CheckLoadStall(stid);
    }
    if (id == MEM_IEX) {
        return core->MtcmemIntf[0]->CheckLoadStall();
    }
    ASSERT(false && "not support");
    return false;
}

template <class T>
bool IEX::CheckForPipeSpace(vector<T> &pipe)
{
    for (auto &p : pipe) {
        if (!p.e4_inst) {
            return true;
        }
    }

    return false;
}

template <typename T>
static bool CheckForVecPipeLd(vector<T> &pipe, bool separate)
{
    size_t size = separate ? pipe.size() - 1 : pipe.size();
    // ignore the separate st pipe, the last pipe is only for store
    for (size_t i = 0; i < size; ++i) {
        if (!pipe[i].e4_inst) {
            return true;
        }
    }
    return false;
}

void IEX::HandleVcoreResp()
{
    if (!core->IsVectorIex(machineType) && (id != MEM_IEX)) {
        return;
    }

    if (core->IsVectorIex(machineType)) {
        HandleVecResp();
    } else if (core->IsMtcIex(machineType)) {
        HandleMtcResp();
    }
}

void IEX::HandleVecResp()
{
    while (!vcoreIexLdResQ->Empty()) {
        if ((core->IsVectorIex(machineType) && !CheckForPipeSpace(aguPipe))) {
            break;
        }
        auto req = vcoreIexLdResQ->Read();
        ASSERT(req.isLoad);
        HandleLd(req);
        SetTileLsuPipeView(req);
        LOG_INFO_M(machineType, Stage::E4) << "Tile Load write back, B" << req.bid << "-G" << req.gid << "-T" <<
            req.tid << "-R" << req.rid;
    }

    while (!vcoreIexStResQ->Empty()) {
        auto req = vcoreIexStResQ->Read();
        ASSERT(!req.isLoad);
        LOG_INFO_M(machineType, Stage::E4) << "Tile Store write back, B" << req.bid << "-G" << req.gid << "-T" <<
            req.tid << "-R" << req.rid;
        HandleUbStResp(req);
    }
}

void IEX::HandleMtcResp()
{
    while (!vcoreIexResQ->Empty()) {
        MemRequest req = vcoreIexResQ->Front();
        if (req.isLoad) {
            if ((core->IsVectorIex(machineType) && !CheckForVecPipeLd(aguPipe, configs.simt_separate_ld_st_pipe)) ||
                (id == MEM_IEX && !CheckForPipeSpace(ldaPipe))) {
                break;
            }
            vcoreIexResQ->Read();
            HandleLd(req);
            SetTileLsuPipeView(req);
            LOG_INFO_M(machineType, Stage::E4) << "Tile Load write back, B" << req.bid << "-G" << req.gid << "-T" <<
                req.tid << "-R" << req.rid;
        } else {
            vcoreIexResQ->Read();
            HandleUbStResp(req);
            LOG_INFO_M(machineType, Stage::E2) << "Tile Store resolve, B" << req.bid << "-G" << req.gid << "-T" <<
                req.tid << "-R" << req.rid;
        }
    }
}

void IEX::HandleLd(MemRequest &req)
{
    if (configs.VAB_EN) {
        if (iq.vab->isGather(req)&& !req.toMemory) {
            iq.vab->resp2vab(req);
        } else {
            HandleUbLdResp(req);
        }
    } else {
        HandleUbLdResp(req);
    }
}

void IEX::ldapip_resolve(SimInst inst)
{
    for (auto &pipe : ldaPipe) {
        if (!pipe.e4_inst) {
            pipe.e4_inst = inst;
            break;
        }
    }
}

void IEX::agupip_resolve(SimInst inst)
{
    for (auto &pipe : aguPipe) {
        if (!pipe.e4_inst) {
            pipe.e4_inst = inst;
            break;
        }
    }
}

void IEX::VecLoadWakeUp(MemRequest &req)
{
    uint32_t peID = req.thread;
    auto &robNext = *nextROBs[peID][req.tid];
    SimInst &inst = robNext[req.rid.val].inst;
    if (robNext.entry[inst->rid.val].status == INST_RETIRED) {
        LOG_INFO_M(machineType, Stage::WB) << "error wakeup, " << req;
    }
    ASSERT(robNext.entry[inst->rid.val].status != INST_RETIRED);

    PLpvInfo lpvInfo = std::make_shared<LpvInfo>();
    for (auto pdst : inst->pdsts_) {
        iq.WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled), lpvInfo, inst->peID, inst->tid, inst->stid);
    };
}

void IEX::HandleUbLdResp(MemRequest &bus)
{
    uint32_t peID = bus.thread;
    auto &rob_next = *nextROBs[peID][bus.tid];
    SimInst &inst = rob_next[bus.rid.val].inst;
    if (rob_next.entry[inst->rid.val].status == INST_RETIRED) {
        LOG_INFO_M(machineType, Stage::WB) << "error resolve, " << bus;
    }
    ASSERT(rob_next.entry[inst->rid.val].status != INST_RETIRED);
    VecData retData;

    // TODO: code check
    retData.Init(bus.width * 8, bus.lanes);
    for (uint32_t lane = 0; lane < bus.lanes; ++lane) {
        uint64_t data = bus.data.Get(lane);
        retData.Set(data, lane);
    }

    inst->pdsts_[DST0_IDX]->vecData.data = retData.data;
    inst->pdsts_[DST0_IDX]->vecData.width = retData.width;
    inst->pdsts_[DST0_IDX]->vecData.size = retData.size;
    inst->pdsts_[DST0_IDX]->dataVld = true;
    inst->pdsts_[DST0_IDX]->vecDataVld = true;
    if (IsScalarInst(inst->codeLen)) {
        inst->pdsts_[DST0_IDX]->data = retData.Get(0);
    }
    inst->loadWakeuped = true;

    PLpvInfo lpvInfo = std::make_shared<LpvInfo>();
    for (auto pdst : inst->pdsts_) {
        iq.WakeupIQTag(WakeupInfo(pdst->type, pdst->ptag, pdst->recycled), lpvInfo, peID, inst->tid, inst->stid);
    }
    if (core->IsVectorIex(machineType)) {
        for (auto &pipe : aguPipe) {
            if (!pipe.e4_inst) {
                pipe.e4_inst = inst;
                break;
            }
        }
    } else if (id == MEM_IEX) {
        for (auto &pipe : ldaPipe) {
            if (!pipe.e4_inst) {
                pipe.e4_inst = inst;
                break;
            }
        }
    } else {
        ASSERT(0 && "Should not be here");
    }
}

void IEX::HandleUbStResp(MemRequest &bus)
{
    uint32_t peID = bus.thread;
    auto &rob_next = *nextROBs[peID][bus.tid];
    SimInst &inst = rob_next[bus.rid.val].inst;
    ASSERT(rob_next.entry[inst->rid.val].status != INST_RETIRED);
    rob_next[bus.rid.val].status = INST_COMPLETED;
    ASSERT(OpcodeIsStore(inst->opcode));
}

IEX::~IEX()
{
    DeletePtr(iex_rt_wr_q);
    ReleaseVecPtr(rf_ct_q);
    for (uint32_t stid = 0; stid < iex_lsu_lda_array.size(); ++stid) {
        ReleaseVecPtr(iex_lsu_lda_array[stid]);
        ReleaseVecPtr(lsu_iex_lret_array[stid]);
        ReleaseVecPtr(iex_lsu_sta_array[stid]);
        ReleaseVecPtr(iex_lsu_std_array[stid]);
    }
}

uint32_t IEX::GetPipeActiveLoad()
{
    uint32_t activeCnt = 0;
    if (core->IsVectorIex(machineType) || id == MEM_IEX) {
        for (auto &pipe : aguPipe) {
            if (pipe.p1_inst && *pipe.p1_inst && OpcodeIsLoad((*pipe.p1_inst)->opcode)) {
                ++activeCnt;
            }
            if (pipe.i1_inst && OpcodeIsLoad(pipe.i1_inst->opcode)) {
                ++activeCnt;
            }
            if (pipe.i2_inst && OpcodeIsLoad(pipe.i2_inst->opcode)) {
                ++activeCnt;
            }
            if (pipe.e1_inst && OpcodeIsLoad(pipe.e1_inst->opcode)) {
                ++activeCnt;
            }
            // 1 request is in simQ.
            if (pipe.e2_inst && OpcodeIsLoad(pipe.e2_inst->opcode)) {
                ++activeCnt;
            }
        }
    } else {
        for (auto &pipe : ldaPipe) {
            if (pipe.p1_inst && *pipe.p1_inst) {
                ++activeCnt;
            }
            if (pipe.i1_inst) {
                ++activeCnt;
            }
            if (pipe.i2_inst) {
                ++activeCnt;
            }
            if (pipe.e1_inst) {
                ++activeCnt;
            }
            if (pipe.e2_inst) {
                ++activeCnt;
            }
        }
    }

    return activeCnt;
}

uint32_t IEX::GetPipeActiveTLoad()
{
    uint32_t activeCnt = 0;
    if (id == SIMT_IEX || id == MEM_IEX) {
        for (auto &pipe : aguPipe) {
            if (pipe.p1_inst && *pipe.p1_inst && (*pipe.p1_inst)->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
            if (pipe.i1_inst && pipe.i1_inst->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
            if (pipe.i2_inst && pipe.i2_inst->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
            if (pipe.e1_inst && pipe.e1_inst->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
            // 1 request is in simQ.
            if (pipe.e2_inst && pipe.e2_inst->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
        }
    } else {
        for (auto &pipe : ldaPipe) {
            if (pipe.p1_inst && *pipe.p1_inst) {
                ++activeCnt;
            }
            if (pipe.i1_inst) {
                ++activeCnt;
            }
            if (pipe.i2_inst) {
                ++activeCnt;
            }
            if (pipe.e1_inst) {
                ++activeCnt;
            }
            if (pipe.e2_inst) {
                ++activeCnt;
            }
        }
    }
    return activeCnt;
}

uint32_t IEX::GetPipeMtcActiveLoad()
{
    uint32_t activeCnt = 0;
    if (id == SIMT_IEX || id == MEM_IEX) {
        for (auto &pipe : ldaPipe) {
            if (pipe.p1_inst && *pipe.p1_inst && OpcodeIsLoad((*pipe.p1_inst)->opcode)) {
                ++activeCnt;
            }
            if (pipe.i1_inst && OpcodeIsLoad(pipe.i1_inst->opcode)) {
                ++activeCnt;
            }
            if (pipe.i2_inst && OpcodeIsLoad(pipe.i2_inst->opcode)) {
                ++activeCnt;
            }
            if (pipe.e1_inst && OpcodeIsLoad(pipe.e1_inst->opcode)) {
                ++activeCnt;
            }
            // 1 request is in simQ.
            if (pipe.e2_inst && OpcodeIsLoad(pipe.e2_inst->opcode)) {
                ++activeCnt;
            }
        }
    } else {
        for (auto &pipe : ldaPipe) {
            if (pipe.p1_inst && *pipe.p1_inst) {
                ++activeCnt;
            }
            if (pipe.i1_inst) {
                ++activeCnt;
            }
            if (pipe.i2_inst) {
                ++activeCnt;
            }
            if (pipe.e1_inst) {
                ++activeCnt;
            }
            if (pipe.e2_inst) {
                ++activeCnt;
            }
        }
    }
    return activeCnt;
}

uint32_t IEX::GetPipeMtcActiveTLoad()
{
    uint32_t activeCnt = 0;
    if (id == SIMT_IEX || id == MEM_IEX) {
        for (auto &pipe : ldaPipe) {
            if (pipe.p1_inst && *pipe.p1_inst && (*pipe.p1_inst)->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
            if (pipe.i1_inst && pipe.i1_inst->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
            if (pipe.i2_inst && pipe.i2_inst->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
            if (pipe.e1_inst && pipe.e1_inst->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
            // 1 request is in simQ.
            if (pipe.e2_inst && pipe.e2_inst->opcode == Opcode::OP_TLD) {
                ++activeCnt;
            }
        }
    } else {
        for (auto &pipe : ldaPipe) {
            if (pipe.p1_inst && *pipe.p1_inst) {
                ++activeCnt;
            }
            if (pipe.i1_inst) {
                ++activeCnt;
            }
            if (pipe.i2_inst) {
                ++activeCnt;
            }
            if (pipe.e1_inst) {
                ++activeCnt;
            }
            if (pipe.e2_inst) {
                ++activeCnt;
            }
        }
    }
    return activeCnt;
}

uint32_t IEX::GetPipeActiveStore()
{
    uint32_t activeCnt = 0;
    if (core->IsVectorIex(machineType)) {
        for (auto &pipe : aguPipe) {
            if (pipe.p1_inst && *pipe.p1_inst && OpcodeIsStore((*pipe.p1_inst)->opcode)) {
                ++activeCnt;
            }
            if (pipe.i1_inst && OpcodeIsStore(pipe.i1_inst->opcode)) {
                ++activeCnt;
            }
            if (pipe.i2_inst && OpcodeIsStore(pipe.i2_inst->opcode)) {
                ++activeCnt;
            }
            if (pipe.e1_inst && OpcodeIsStore(pipe.e1_inst->opcode)) {
                ++activeCnt;
            }
        }
    } else {
        for (auto &pipe : staPipe) {
            if (pipe.p1_inst && *pipe.p1_inst) {
                ++activeCnt;
            }
            if (pipe.i1_inst) {
                ++activeCnt;
            }
            if (pipe.i2_inst) {
                ++activeCnt;
            }
            if (pipe.e1_inst) {
                ++activeCnt;
            }
        }
    }

    return activeCnt;
}

void IEX::ReportStat()
{
    std::string str = "IEX";
    if (core->IsVectorIex(machineType)) {
        stringstream ss;
        ss << "Vector Core " << coreId << " Detail Statistics";
        str = ss.str();
    } else if (id == MEM_IEX) {
        str = "MTC Core Detail Statistics";
    }
    stats->reportVecMtc(str);
}

bool IEX::IsLastLoadStore(const SimInst &inst, const IDBus &oldestBus)
{
    if (!oldestBus.vld) {
        return false;
    }

    if (inst->bid != oldestBus.bid || inst->gid != oldestBus.gid) {
        return false;
    }

    if (OpcodeIsLoad(inst->opcode) && ((inst->load_id == oldestBus.lid + 1) || inst->rid == oldestBus.rid)) {
        return true;
    }

    if ((OpcodeIsStore(inst->opcode) && ((inst->sid == oldestBus.sid + 1) || inst->rid == oldestBus.rid))) {
        return true;
    }

    return false;
}

} // namespace JCore
