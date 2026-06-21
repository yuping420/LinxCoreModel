#include "PC.h"

namespace JCore {
namespace Calculate {
namespace PC {

static bool CalcInstSetret(MInst &inst)
{
    if (inst.srcs.size() != SRC1_IDX) {
        return false;
    }
    inst.dsts[DST0_IDX]->data = inst.pc + inst.srcs[SRC0_IDX]->data;
    return true;
}

static bool CalcInstAddTPC(MInst &inst)
{
    if (inst.srcs.size() != SRC1_IDX) {
        return false;
    }
    constexpr uint64_t TPC_PAGE_MASK = ~0xfffULL;
    inst.dsts[DST0_IDX]->data = (inst.pc & TPC_PAGE_MASK) + inst.srcs[SRC0_IDX]->data;
    return true;
}

Handler Lookup(Opcode op)
{
    static const std::unordered_map<Opcode, Handler> PC_TABLE = std::unordered_map<Opcode, Handler> {
        {Opcode::OP_SETRET, &CalcInstSetret},
        {Opcode::OP_ADDTPC, &CalcInstAddTPC},
    };
    auto it = PC_TABLE.find(op);
    return (it == PC_TABLE.end()) ? 0 : it->second;
}

bool CalculatePC(MInst &inst)
{
    Handler fn = Lookup(inst.opcode);
    return fn ? fn(inst) : false;
}

} // namespace PC
} // namespace Calculate
} // namespace JCore
