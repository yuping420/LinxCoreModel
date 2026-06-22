#include "SoftCore.h"

namespace JCore {
void SoftCore::GenCodeTemplate(BlockFuncPtr block)
{
    // 调用接口，执行code gen
    (void)block;
    switch (block->opcode) {
        case Opcode::OP_MCOPY:
            GenCodeMCOPY(block);
            break;
        case Opcode::OP_MSET:
            GenCodeMSET(block);
            break;
        case Opcode::OP_FENTRY:
            GenCodeFENTRY(block);
            break;
        case Opcode::OP_FEXIT:
            GenCodeFEXIT(block);
            break;
        case Opcode::OP_FRET_RA:
            GenCodeFRETRA(block);
            break;
        case Opcode::OP_FRET_STK:
            GenCodeFRETSTK(block);
            break;
        default:
            ASSERT(false && "Unsupport ct opcode");
    }
}

MInstFuncPtr SoftCore::InitCTInst(BlockFuncPtr block, Opcode op, EncodeLen len)
{
    MInstFuncPtr inst = std::make_shared<MInstFunc>();
    inst->opcode = op;
    inst->codeLen = len;
    inst->threadId = block->threadId;
    inst->pc = block->ctLocalPC;
    inst->ctGen = true;
    // 和 CA 对齐，都+4
    block->ctLocalPC += 4;
    inst->execSeq = threadStatus[block->threadId].minstId;
    inst->check = true;
    inst->localExecSeq = block->commitInstNum;
    InstGroup grp = OpcodeManager::Inst().GetOpcodeGroup(op);
    switch (grp) {
        case InstGroup::LOAD:
        case InstGroup::STORE:
        case InstGroup::CACHE_MAINTAIN:
            inst->accMemInfo = std::make_shared<AaccelssMemInfo>();
            break;
        case InstGroup::SETC:
            inst->setcInfo = std::make_shared<SetcInfo>();
            break;
        default:
            if (op == Opcode::OP_SETC_TGT) {
                inst->setcInfo = std::make_shared<SetcInfo>();
            }
            break;
    }
    return inst;
}

void SoftCore::ExecuteCTInst(MInstFuncPtr inst, BlockFuncPtr &block)
{
    ExecuteSTDMinst(inst, block);
}

void SoftCore::GenCodeMCOPY(BlockFuncPtr block)
{
    block->Dump();
    MInstFuncPtr inst = nullptr;
    /* \brief addi RegSrc2, 0, ->u 状态机的内部缓存ldCnt，用来记录内存拷贝字节数 */
    inst = InitCTInst(block, Opcode::OP_ADDI);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, block->srcATag[SRC2_IDX]));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    ExecuteCTInst(inst, block);
    /* \brief addi RegSrc0, 0, ->u 状态机的内部缓存dstAddr，用来存储目的内存地址 */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_ADDI);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, block->srcATag[SRC0_IDX]));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    ExecuteCTInst(inst, block);
    /* \brief addi RegSrc1, 0, ->u 状态机的内部缓存srcAddr，用来存储源内存地址 */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_ADDI);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, block->srcATag[SRC1_IDX]));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    ExecuteCTInst(inst, block);
    block->ctStatus = std::make_shared<BlockFunc::CTStatus>();
    block->ctStatus->ldCnt = block->srcData[SRC2_IDX];
    block->ctStatus->dstAddr = block->srcData[SRC0_IDX];
    block->ctStatus->srcAddr = block->srcData[SRC1_IDX];

    GenCodeMCOPYStage1(block);
    GenCodeMCOPYStage2(block);
    GenCodeMCOPYStage3(block);
}

void SoftCore::GenCodeMCOPYStage1(BlockFuncPtr block)
{
    auto &curStatus = block->ctStatus;
    MInstFuncPtr inst = nullptr;
    /* 源和目的起始地址字节对齐相同的场景，分为：1.都是8字节对齐的；2.不是8字节对齐的 */
    if (GetBits(curStatus->srcAddr, SRC2_IDX, 0) == GetBits(curStatus->dstAddr, SRC2_IDX, 0)) {
        if (GetBits(curStatus->srcAddr, SRC2_IDX, 0) != 0) {
            /* \brief generate(andi u#2, 0xff8, ->u); 将目的地址低三位置为0 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ANDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0xff8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(ldi.u [u#1, 0], ->t) 从目的地址前8字节对齐处加载8字节数据 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_LDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(andi u#2, 0xff8, ->u) 将源地址低三位置为0 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ANDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0xff8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(ldi.u [u#1, 0], ->t) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_LDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* 更新:ct_ldOt+=8; ct_stCnt+=min(8-srcAr[2:0], ct_ldCnt); ct_ldCnt-=ct_stCnt */
            curStatus->ldOffset += NUM8;
            curStatus->stCnt += std::min((NUM8 - GetBits(curStatus->srcAddr, SRC2_IDX, 0)),
                                                   curStatus->ldCnt);
            curStatus->ldCnt -= curStatus->stCnt;
            /* \brief generate(srli t#1, ct_srcAr[2:0]*8, ->t) 将从源地址加载的数据移至寄存器低位 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SRLI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                           (GetBits(curStatus->srcAddr, SRC2_IDX, 0) * NUM8)));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(bfi t#3, t#1, ct_dstAr[2:0], ct_stCnt, ->t) 将源有效数据拼接在目的有效数据高位 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_BFI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, SRC2_IDX));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                           GetBits(curStatus->dstAddr, 2, 0) * NUM8));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stCnt * NUM8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(sdi.u t#1, [u#2, ct_stOt]) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
            ExecuteCTInst(inst, block);
            /* 将拼接后的数据写到目的地址，更新:ct_stOt+=8, ct_stCnt=0 */
            curStatus->stCnt = 0;
            curStatus->stOffset += 8;
        } else {
            /* 如果源和目的地址是8字节对齐的，则第一阶段不需要产生指令 */
        }
    } else if (GetBits(curStatus->srcAddr, SRC2_IDX, 0) > GetBits(curStatus->dstAddr, SRC2_IDX, 0)) {
        /* 源地址低三位大于目的地址低三位的场景，分为：1.目的起始地址是8字节对齐的；2.目的起始地址不是8字节对齐的 */
        /* 如果目的起始地址不是8字节对齐的，则需要从目的地址加载一次数据，拼接后写到目的地址 */
        if (GetBits(curStatus->dstAddr, SRC2_IDX, 0) != 0) {
            /* \brief generate(andi u#2, 0xff8, ->u) 将目的地址低三位置为0 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ANDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0xff8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(andi u#2, 0xff8, ->u) 将源地址低三位置为0 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ANDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0xff8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(ldi.u [u#1, 0], ->t) 从源地址前8字节对齐处加载8字节数据 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_LDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* 更新:ct_ldOt+=8; ct_stCnt+=min(8-srcAr[2:0], ct_ldCnt); ct_ldCnt-=ct_stCnt */
            curStatus->ldOffset += NUM8;
            curStatus->stCnt += std::min(NUM8 - GetBits(curStatus->srcAddr, SRC2_IDX, 0), curStatus->ldCnt);
            curStatus->ldCnt -= curStatus->stCnt;
            /* \brief generate(ldi.u [u#2, 0], ->t) 从目的地址前8字节对齐处加载8字节数据 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_LDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            if (curStatus->ldCnt > 0) {
                /* \brief generate(ldi.u [u#1, ct.ldOt], ->t) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_LDI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->ldOffset));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* 更新:ct_ldOt+=8; ct_stCnt+=min(8, ct_ldCnt); ct_ldCnt-=min(8, ct_ldCnt) */
                curStatus->ldOffset += NUM8;
                curStatus->stCnt += std::min(NUM8, curStatus->ldCnt);
                curStatus->ldCnt -= std::min(NUM8, curStatus->ldCnt);
                /* \brief generate(concat t#1, t#3, shamt) -> t 拼接连续两次加载的数据并移位：shamt=64-srcAr[2:0]*8 */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_CCAT);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, SRC2_IDX));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM,
                                                               NUM64 -
                                                               GetBits(curStatus->srcAddr, SRC2_IDX, 0) * NUM8));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
                ExecuteCTInst(inst, block);
                /* \brief generate(addi t#2, 0, ->t) 拷贝一次源内存加载的数据，用于后序ccat指令索引 */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_ADDI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* \brief generate(bfi t#4, t#2, ct_dstAr[2:0], N, ->t) */
                /* 将源有效数据拼接在目的有效数据高位：N=min(8-ct_dstAr[2:0], ct_stCnt) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_BFI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 3));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                               GetBits(curStatus->dstAddr, 2, 0) * NUM8));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                               std::min(NUM8 - GetBits(curStatus->dstAddr, SRC2_IDX, 0),
                                                                        curStatus->stCnt) * NUM8));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* \brief generate(sdi.u t#1, [u#2, ct_stOt]) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SDI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新:ct_stOt+=8; ct_stCnt-=min(8-ct_dstAr[2:0], ct_stCnt); ct_vld=1 */
                curStatus->stOffset += 8;
                curStatus->stCnt -= std::min(NUM8 - GetBits(curStatus->dstAddr, SRC2_IDX, 0), curStatus->stCnt);
                curStatus->vld = true;
            } else {
                /* \brief generate(srli t#2, ct_srcAr[2:0]*8, ->t) 将从源地址加载的数据移至寄存器低位 */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SRLI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                               GetBits(curStatus->srcAddr, SRC2_IDX, 0) * NUM8));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* \brief generate(bfi t#2, t#1, ct_dstAr[2:0], N, ->t) */
                /* 将源有效数据拼接在目的有效数据高位：N=min(8-ct_dstAr[2:0], ct_stCnt) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_BFI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                               GetBits(curStatus->dstAddr, 2, 0) * NUM8));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                               std::min(NUM8 - GetBits(curStatus->dstAddr, SRC2_IDX, 0),
                                                                        curStatus->stCnt) * NUM8));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* \brief generate(sdi.u t#1, [u#2, ct_stOt]) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SDI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                curStatus->stOffset += NUM8;
                curStatus->stCnt -= std::min(NUM8 - GetBits(curStatus->dstAddr, SRC2_IDX, 0), curStatus->stCnt);
            }
        } else {    /* 如果目的起始地址是8字节对齐的，则直接向目的地址搬移数据 */
            /* \brief generate(addi u#2, 0, ->u) 拷贝一次目的地址，保证后面索引为U#1 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ADDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(andi u#2, 0xff8, ->u) 将源地址低三位置为0 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ANDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0xff8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(ldi.u [u#1, 0], ->t) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_LDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* 更新:ct_ldOt+=8; ct_stCnt+=min(8-srcAr[2:0], ct_ldCnt); ct_ldCnt-=ct_stCnt */
            curStatus->ldOffset += 8;
            curStatus->stCnt += std::min(NUM8 - GetBits(curStatus->srcAddr, SRC2_IDX, 0), curStatus->ldCnt);
            curStatus->ldCnt -= curStatus->stCnt;
            if (curStatus->ldCnt <= 0) {
                /* \brief generate(srli t#1, ct_srcAr[2:0]*8, ->t) 将从源地址加载的数据移至寄存器低位 */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SRLI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                               GetBits(curStatus->srcAddr, SRC2_IDX, 0) * NUM8));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
            } else {
                /* \brief generate(addi zero, 0, ->t) 将从源地址加载的数据移至寄存器低位 */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_ADDI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
            }
        }
    } else {
        /* 源地址低三位小于目的地址低三位的场景，分为：1.源起始地址是8字节对齐的；2.源起始地址不是8字节对齐的 */
        if (GetBits(curStatus->srcAddr, SRC2_IDX, 0) != 0) {
            /* \brief generate(andi u#2, 0xff8, ->u) 将目的地址低三位置为0 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ANDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0xff8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(ldi.u [u#1, 0], ->t) 从目的地址前8字节对齐处加载8字节数据 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_LDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(andi u#2, 0xff8, ->u) 将源地址低三位置为0 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ANDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0xff8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(ldi.u [u#1, 0], ->t) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_LDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* 更新:ct_ldOt+=8; ct_stCnt+=min(8-ct_srcAr[2:0], ct_ldCnt); ct_ldCnt-=ct_stCnt */
            curStatus->ldOffset += 8;
            curStatus->stCnt += std::min(NUM8 - GetBits(curStatus->srcAddr, SRC2_IDX, 0), curStatus->ldCnt);
            curStatus->ldCnt -= curStatus->stCnt;
            /* generate(srli t#1, ct_srcAr[2:0]*8, ->t) 将从源地址加载的有效数据右移到低位 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SRLI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                           GetBits(curStatus->srcAddr, SRC2_IDX, 0) * NUM8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(addi t#2, 0, ->t) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ADDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* generate(bfi t#4, t#2, ct_dstAr[2:0], N, ->t) N=min(8-ct_dstAr[2:0], ct_ldCnt)*8 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_BFI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 3));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                           GetBits(curStatus->dstAddr, 2, 0) * NUM8));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, std::min(
                NUM8 - GetBits(curStatus->dstAddr, SRC2_IDX, 0), curStatus->stCnt) * NUM8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(sdi.u t#1, [u#2, ct_stOt]) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
            ExecuteCTInst(inst, block);
            /* 更新:ct_stOt+=8; ct_stCnt-=min(8-ct_dstAr[2:0], ct_stCnt); vld=1 */
            curStatus->stOffset += 8;
            curStatus->stCnt -= std::min(NUM8 - GetBits(curStatus->dstAddr, SRC2_IDX, 0), curStatus->stCnt);
            curStatus->vld = true;
        } else {
            /* generate(andi u#2, 0xff8, ->u) 将目的地址低三位置为0 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ANDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0xff8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* generate(ldi.u [u#1, 0], ->t) 从目的地址前8字节对齐处加载8字节数据 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_LDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* generate(addi u#2, 0, ->u) 拷贝一次源地址，保证后面索引为u#2 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ADDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(ldi.u [u#1, 0], ->t) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_LDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* 更新:ct_ldOt+=8; ct_stCnt+=min(8-ct_srcAr[2:0], ct_ldCnt); ct_ldCnt-=ct_stCnt */
            curStatus->ldOffset += 8;
            curStatus->stCnt += std::min(NUM8 - GetBits(curStatus->srcAddr, SRC2_IDX, 0), curStatus->ldCnt);
            curStatus->ldCnt -= curStatus->stCnt;
            /* generate(bfi t#2, t#1, ct_dstAr[2:0], N, ->t) N=min(8-ct_dstAr[2:0], ct_ldCnt) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_BFI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM,
                                                           GetBits(curStatus->dstAddr, 2, 0) * NUM8));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, std::min(
                NUM8 - GetBits(curStatus->dstAddr, SRC2_IDX, 0), curStatus->stCnt) * NUM8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* \brief generate(sdi.u t#1, [u#2, ct_stOt]) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
            ExecuteCTInst(inst, block);
            /* 更新:ct_stOt+=8; ct_stCnt-=min(8-ct_dstAr[2:0], ct_stCnt); vld=1 */
            curStatus->stOffset += 8;
            curStatus->stCnt -= std::min(NUM8 - GetBits(curStatus->dstAddr, SRC2_IDX, 0), curStatus->stCnt);
            curStatus->vld = true;
        }
    }
}

void SoftCore::GenCodeMCOPYStage2(BlockFuncPtr block)
{
    auto &curStatus = block->ctStatus;
    MInstFuncPtr inst = nullptr;

    while (curStatus->ldCnt > 0) {
        curStatus->vld = false;

        /* \brief generate(ldi.u [u#1, ct_ldOt], ->t) 加载一次8字节 */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_LDI_U);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, curStatus->ldOffset));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* 更新:ct_ldOt+=8; ct_stCnt+=min(8, ct_ldCnt); ct_ldCnt-=min(8, ct_ldCnt) */
        curStatus->ldOffset += 8;
        curStatus->stCnt += std::min(NUM8, curStatus->ldCnt);
        curStatus->ldCnt -= std::min(NUM8, curStatus->ldCnt);
        if (GetBits(curStatus->srcAddr, SRC2_IDX, 0) > GetBits(curStatus->dstAddr, SRC2_IDX, 0)) {
            /* generate(concat t#1, t#3, shamt) shamt = 64 - (ct_srcAr[2:0] - ct_dstAr[2:0]) * 8 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_CCAT);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 2));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM,
                                                           NUM64 -
                                                           (GetBits(curStatus->srcAddr, SRC2_IDX, 0) -
                                                           GetBits(curStatus->dstAddr, SRC2_IDX, 0)) * NUM8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
            ExecuteCTInst(inst, block);
        } else if (GetBits(curStatus->srcAddr, SRC2_IDX, 0) < GetBits(curStatus->dstAddr, SRC2_IDX, 0)) {
            /* generate(concat t#1, t#3, shamt) shamt = (ct_dstAr[2:0] - ct_srcAr[2:0]) * 8 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_CCAT);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 2));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM,
                                                           (GetBits(curStatus->dstAddr, SRC2_IDX, 0) -
                                                           GetBits(curStatus->srcAddr, SRC2_IDX, 0)) * NUM8));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
            ExecuteCTInst(inst, block);
        }
        // 如果前面加载的数据或拼接移位后待写出的数据够8个字节，则写到目的地址
        if (curStatus->stCnt >= 8) {
            /* \brief generate(sdi.u t#1, [u#2, ct_stOt]) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_INVALID, 0));
            ExecuteCTInst(inst, block);
            /* 更新:ct_stOt+=8; ct_stCnt-=8; ct_vld = 1 */
            curStatus->stOffset += 8;
            curStatus->stCnt -= 8;
            curStatus->vld = true;
        }
        // 更新寻址偏移，防止offset超限
        if (curStatus->ldOffset >= 2032) {
            /* generate(addi u#2, ct_stOt, ->u) 更新内存写Offset */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ADDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);
            /* generate(addi u#2, ct_ldOt, ->u) 更新内存读Offset */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ADDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->ldOffset));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            ExecuteCTInst(inst, block);

            curStatus->stOffset = 0;
            curStatus->ldOffset = 0;
        }
    }
}

void SoftCore::GenCodeMCOPYStage3(BlockFuncPtr block)
{
    auto &curStatus = block->ctStatus;
    MInstFuncPtr inst = nullptr;

    if (curStatus->stCnt > 0) {
        // 如果前一阶段的最后一步，有完整8字节的数据写出，那么剩余未写出的数据目前在t#2寄存器中且不在低位，所以需要将寄存器中有效数据右移至低位；
        // 如果前一阶段的最后一步，没有完整8字节的数据写出，那么剩余未写出的数据目前在t#1寄存器中且在低位，所以不需要移位。
        if (curStatus->vld) {
            uint64_t imm = 0;
            if (GetBits(curStatus->srcAddr, SRC2_IDX, 0) < GetBits(curStatus->dstAddr, SRC2_IDX, 0)) {
                imm = NUM64 - (GetBits(curStatus->dstAddr, SRC2_IDX, 0) -
                               GetBits(curStatus->srcAddr, SRC2_IDX, 0)) * NUM8;
            } else if (GetBits(curStatus->srcAddr, SRC2_IDX, 0) > GetBits(curStatus->dstAddr, SRC2_IDX, 0)) {
                imm = (GetBits(curStatus->srcAddr, SRC2_IDX, 0) - GetBits(curStatus->dstAddr, SRC2_IDX, 0)) * NUM8;
            }
            /* \brief generate(srli t#2, imm, ->t) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SRLI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 1));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, imm));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
        }
        switch (curStatus->stCnt) {
            case 1:
                /* \brief generate(sbi.u t#1, [u#2, ct_stOt]) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SBI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 1 */
                curStatus->stOffset += 1;
                break;
            case 2:
                /* \brief generate(shi.u t#1, [u#2, ct_stOt]) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SHI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* ct_stOt += 2 */
                curStatus->stOffset += 2;
                break;
            case 3:
                /* \brief generate(shi.u t#1, [u#2, ct_stOt]) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SHI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 2 */
                curStatus->stOffset += 2;
                /* \brief generate(srli t#1, 16, ->t) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SRLI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 16));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* \brief generate(sbi.u t#1, [u#2, ct_stOt]) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SBI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 1 */
                curStatus->stOffset += 1;
                break;
            case 4:
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SWI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 4 */
                curStatus->stOffset += 4;
                break;
            case 5:
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SWI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 4 */
                curStatus->stOffset += 4;
                /* \brief generate(srli t#1, 32, ->t) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SRLI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 32));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* \brief generate(sbi.u t#1, [u#2, ct_stOt]) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SBI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 1 */
                curStatus->stOffset += 1;
                break;
            case 6:
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SWI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 4 */
                curStatus->stOffset += 4;
                /* \brief generate(srli t#1, 32, ->t) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SRLI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 32));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* \brief generate(shi.u t#1, [u#2, ct_stOt]) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SHI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 2 */
                curStatus->stOffset += 2;
                break;
            case 7:
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SWI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 4 */
                curStatus->stOffset += 4;
                /* \brief generate(srli t#1, 32, ->t) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SRLI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 32));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* \brief generate(shi.u t#1, [u#2, ct_stOt]) */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SHI_U);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 2 */
                curStatus->stOffset += 2;
                /* \brief srli t#1, 16, ->t */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SRLI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 16));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                /* \brief sbi.u t#1, [u#2, ct_stOt] */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_SBI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 1));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->stOffset));
                ExecuteCTInst(inst, block);
                /* 更新ct_stOt += 1 */
                curStatus->stOffset += 1;
                break;
            default:
                break;
        }
    }
}


void SoftCore::GenCodeMSET(BlockFuncPtr block)
{
    // (void)block;
    // block->Dump();
    // ASSERT(false && "ct undefine");
    MInstFuncPtr inst = nullptr;
    /* \brief addi RegSrc0, 0, ->t 状态机的内部缓存dstAddr，用来存储目的内存地址 */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_ADDI);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, block->srcATag[SRC0_IDX]));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
    ExecuteCTInst(inst, block);
    /* \brief addi RegSrc2, 0, ->u 状态机的内部缓存，用来存储写入内存字节数 */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_ADDI);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, block->srcATag[SRC2_IDX]));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    ExecuteCTInst(inst, block);
    /* \brief addi RegSrc1, 0, ->u 状态机的内部缓存，用来存储目的地址 */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_ADDI);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, block->srcATag[SRC1_IDX]));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    ExecuteCTInst(inst, block);
    block->ctStatus = std::make_shared<BlockFunc::CTStatus>();
    block->ctStatus->addr = block->srcData[SRC0_IDX];
    block->ctStatus->data = block->srcData[SRC1_IDX];
    block->ctStatus->count = block->srcData[SRC2_IDX];
    block->ctStatus->offset = 0;
    if (block->ctStatus->count <= 0) {
        return;
    }

    uint64_t byteData = block->ctStatus->data & GetMaskSafe(NUM8, 0);
    if (byteData == 0) {
        GenCodeMSETZero(block);
    } else {
        GenCodeMSETNoneZero(block);
    }
}

void SoftCore::GenCodeMSETZero(BlockFuncPtr block)
{
    auto &curStatus = block->ctStatus;
    MInstFuncPtr inst = nullptr;
    while (curStatus->count > 0) {
        if (GetBits(curStatus->addr, NUM5, 0) == 0 && curStatus->count >= NUM64) {
            /* generate(addi t#1, 64, ->t) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_ADDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
            inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* generate(dc.zva t#1) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_DC_ZVA);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            ExecuteCTInst(inst, block);
            /* 状态机更新： */
            curStatus->count -= NUM64;
            curStatus->addr += NUM64;
            curStatus->offset = NUM64;
        } else if (GetBits(curStatus->addr, NUM2, 0) == 0 && curStatus->count >= NUM8) {
            /* generate(sdi.u zero, [t#1, ct.offset]) 状态机更新：ct.count -= 8; ct.offset += 8; ct.addr += 8 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
            ExecuteCTInst(inst, block);
            /* 状态机更新： */
            curStatus->count -= NUM8;
            curStatus->offset += NUM8;
            curStatus->addr += NUM8;
        } else if (GetBits(curStatus->addr, 1, 0) == 0 && curStatus->count >= NUM4) {
            /* generate(swi.u zero, [t#1, ct.offset]) 状态机更新：ct.count -= 4; ct.offset += 4; ct.addr += 4 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SWI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
            ExecuteCTInst(inst, block);
            /* 状态机更新： */
            curStatus->count -= NUM4;
            curStatus->offset += NUM4;
            curStatus->addr += NUM4;
        } else if (GetBits(curStatus->addr, 0, 0) == 0 && curStatus->count >= NUM2) {
            /* generate(shi.u zero, [t#1, ct.offset]) 状态机更新：ct.count -= 2; ct.offset += 2; ct.addr += 2 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SHI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
            ExecuteCTInst(inst, block);
            /* 状态机更新： */
            curStatus->count -= NUM2;
            curStatus->offset += NUM2;
            curStatus->addr += NUM2;
        } else {
            /* generate(sbi zero, [t#1, ct.offset]) 状态机更新：ct.count -= 1; ct.offset += 1; ct.addr += 1 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SBI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ZERO, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
            ExecuteCTInst(inst, block);
            /* 状态机更新： */
            curStatus->count -= 1;
            curStatus->offset += 1;
            curStatus->addr += 1;
        }
    }
}

void SoftCore::GenCodeMSETNoneZero(BlockFuncPtr block)
{
    auto &curStatus = block->ctStatus;
    MInstFuncPtr inst = nullptr;
    /* generate(hl.bfi RegSrc1, RegSrc1, 8, 8, ->u); 将源数据的低 8位复制到 8至15位, 写到U寄存器队列 */
    inst = InitCTInst(block, Opcode::OP_BFI, EncodeLen::ENL_H);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, block->srcATag[SRC1_IDX]));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, block->srcATag[SRC1_IDX]));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, NUM8));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, NUM8));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    ExecuteCTInst(inst, block);
    /* generate(hl.bfi u#1, u#1, 16, 16, ->u); 将源数据的低16位复制到16至31位, 写到U寄存器队列 */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_BFI, EncodeLen::ENL_H);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, NUM16));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, NUM16));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    ExecuteCTInst(inst, block);
    /* generate(hl.bfi u#1, u#1, 32, 32, ->u); 将源数据的低32位复制到32至63位, 写到U寄存器队列 */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_BFI, EncodeLen::ENL_H);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, NUM32));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, NUM32));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
    ExecuteCTInst(inst, block);
    while (curStatus->count > 0) {
        if (GetBits(curStatus->addr, NUM2, 0) == 0 && curStatus->count >= NUM8) {
            if (curStatus->offset >= (NUM2048 - NUM8)) {
                /* generate(addi t#1, ct.offset, ->t); offset超限，更新地址；重置ct.offset = 0 */
                inst = nullptr;
                inst = InitCTInst(block, Opcode::OP_ADDI);
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
                inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
                ExecuteCTInst(inst, block);
                curStatus->offset = 0;
            }
            /* generate(sdi.u u#1, [t#1, ct.offset]) 状态机更新：ct.count -= 8; ct.offset += 8; ct.addr += 8 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SDI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
            ExecuteCTInst(inst, block);
            /* 状态机更新： */
            curStatus->count -= NUM8;
            curStatus->offset += NUM8;
            curStatus->addr += NUM8;
        } else if (GetBits(curStatus->addr, NUM2, 0) == 0 && curStatus->count > NUM4) {
            /* generate(swi.u u#1, [t#1, ct.offset]) 状态机更新：ct.count -= 4; ct.offset += 4; ct.addr += 4 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SWI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
            ExecuteCTInst(inst, block);
            /* 状态机更新： */
            curStatus->count -= NUM4;
            curStatus->offset += NUM4;
            curStatus->addr += NUM4;
        } else if (GetBits(curStatus->addr, NUM2, 0) == 0 && curStatus->count > NUM2) {
            /* generate(shi.u u#1, [t#1, ct.offset]) 状态机更新：ct.count -= 2; ct.offset += 2; ct.addr += 2 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SHI_U);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
            ExecuteCTInst(inst, block);
            /* 状态机更新： */
            curStatus->count -= NUM2;
            curStatus->offset += NUM2;
            curStatus->addr += NUM2;
        } else {
            /* generate(sbi u#1, [t#1, ct.offset]) 状态机更新：ct.count -= 1; ct.offset += 1; ct.addr += 1 */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SBI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_ULINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, curStatus->offset));
            ExecuteCTInst(inst, block);
            /* 状态机更新： */
            curStatus->count -= 1;
            curStatus->offset += 1;
            curStatus->addr += 1;
        }
    }
}

void SoftCore::GenCodeFENTRY(BlockFuncPtr block)
{
    uint64_t imm = block->srcData[SRC0_IDX];
    uint64_t uimm14To12 = GetBits(imm, 14, 12);
    uint64_t uimm11To0 = GetBits(imm, 11, 0);
    uint64_t m = block->srcData[SRC1_IDX];
    uint64_t n = block->srcData[SRC2_IDX];
    // R2-R23
    if (m < static_cast<uint64_t>(GPR::GPR_A0) || n > static_cast<uint64_t>(GPR::GPR_X3)) {
        ASSERT(false) << "Illegal FEntry input";
    }
    if (m > n) {
        n += static_cast<uint64_t>(GPR::GPR_COUNT);
    }

    uint64_t offset = -NUM8; // -8
    MInstFuncPtr inst = nullptr;
    if (uimm14To12 != 0) {
        /* \brief Generate(addi sp, 0, ->t) */
        inst = InitCTInst(block, Opcode::OP_ADDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, 0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* \brief Generate(lui uimm[14:12], ->t) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_LUI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, (uimm14To12 << 12)));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* \brief Generate(addi t#1, uimm[11:0], ->t) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, uimm11To0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* \brief Generate(sub sp, t#1, ->sp) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_SUB);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        ExecuteCTInst(inst, block);
        for (uint64_t i = m; i <= n; i++) {
            uint64_t idx = i % static_cast<uint64_t>(GPR::GPR_COUNT);
            if (idx < static_cast<uint64_t>(GPR::GPR_A0)) {
                continue;
            }
            /* \brief Generate(sdi R[idx], [t#3, offset]) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, idx));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 2));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, offset));
            offset -= NUM8;
            ExecuteCTInst(inst, block);
        }
    } else {
        /* \brief Generate(subi sp, uimm[11:0], ->sp) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_SUBI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, uimm11To0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        ExecuteCTInst(inst, block);
        for (uint64_t i = m; i <= n; i++) {
            uint64_t idx = i % static_cast<uint64_t>(GPR::GPR_COUNT);
            if (idx < static_cast<uint64_t>(GPR::GPR_A0)) {
                continue;
            }
            /* \brief Generate(sdi R[idx], [sp, uimm - offset]) */
            inst = nullptr;
            inst = InitCTInst(block, Opcode::OP_SDI);
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, idx));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
            inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, imm + offset));
            offset -= NUM8;
            ExecuteCTInst(inst, block);
        }
    }
}

void SoftCore::GenCodeFEXIT(BlockFuncPtr block)
{
    uint64_t imm = block->srcData[SRC0_IDX];
    uint64_t uimm14To12 = GetBits(imm, 14, 12);
    uint64_t uimm11To0 = GetBits(imm, 11, 0);
    uint64_t m = block->srcData[SRC1_IDX];
    uint64_t n = block->srcData[SRC2_IDX];
    // R2-R23
    if (m < static_cast<uint64_t>(GPR::GPR_A0) || n > static_cast<uint64_t>(GPR::GPR_X3)) {
        ASSERT(false) << "Illegal FExit input";
    }
    if (m > n) {
        n += static_cast<uint64_t>(GPR::GPR_COUNT);
    }

    uint64_t offset = -NUM8; // -8
    MInstFuncPtr inst = nullptr;
    if (uimm14To12 != 0) {
        /* \brief Generate(lui uimm[14:12], ->t) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_LUI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, (uimm14To12 << 12)));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* \brief Generate(addi t#1, uimm[11:0], ->t) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, uimm11To0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* \brief Generate(add sp, t#1, ->sp) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADD);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        ExecuteCTInst(inst, block);
    } else {
        /* \brief Generate(addi sp, uimm[11:0], ->sp) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, uimm11To0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        ExecuteCTInst(inst, block);
    }
    for (uint64_t i = m; i <= n; i++) {
        uint64_t idx = i % static_cast<uint64_t>(GPR::GPR_COUNT);
        if (idx < static_cast<uint64_t>(GPR::GPR_A0)) {
            continue;
        }
        /* \brief Generate(ldi [sp, offset], -> R[idx]) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_LDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, offset));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, idx));
        ExecuteCTInst(inst, block);
        offset -= NUM8;
    }
}

void SoftCore::GenCodeFRETRA(BlockFuncPtr block)
{
    uint64_t imm = block->srcData[SRC0_IDX];
    uint64_t uimm14To12 = GetBits(imm, 14, 12);
    uint64_t uimm11To0 = GetBits(imm, 11, 0);
    uint64_t m = block->srcData[SRC1_IDX];
    uint64_t n = block->srcData[SRC2_IDX];
    // R2-R23
    if (m < static_cast<uint64_t>(GPR::GPR_A0) || n > static_cast<uint64_t>(GPR::GPR_X3)) {
        ASSERT(false) << "Illegal FExit input";
    }
    if (m > n) {
        n += static_cast<uint64_t>(GPR::GPR_COUNT);
    }

    uint64_t offset = -NUM8; // -8
    MInstFuncPtr inst = nullptr;
    /* \brief Generate(setc.tgt Ra) */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_SETC_TGT);

    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_RA)));
    ExecuteCTInst(inst, block);
    if (uimm14To12 != 0) {
        /* \brief Generate(lui uimm[14:12], ->t) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_LUI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, (uimm14To12 << 12)));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* \brief Generate(addi t#1, uimm[11:0], ->t) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, uimm11To0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* \brief Generate(add sp, t#1, ->sp) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADD);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        ExecuteCTInst(inst, block);
    } else {
        /* \brief Generate(addi sp, uimm[11:0], ->sp) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, uimm11To0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        ExecuteCTInst(inst, block);
    }
    for (uint64_t i = m; i <= n; i++) {
        uint64_t idx = i % static_cast<uint64_t>(GPR::GPR_COUNT);
        if (idx < static_cast<uint64_t>(GPR::GPR_A0)) {
            continue;
        }
        /* \brief Generate(ldi [sp, offset], -> R[idx]) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_LDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, offset));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, idx));
        ExecuteCTInst(inst, block);
        offset -= NUM8;
    }
}

void SoftCore::GenCodeFRETSTK(BlockFuncPtr block)
{
    uint64_t imm = block->srcData[SRC0_IDX];
    uint64_t uimm14To12 = GetBits(imm, 14, 12);
    uint64_t uimm11To0 = GetBits(imm, 11, 0);
    uint64_t m = block->srcData[SRC1_IDX];
    uint64_t n = block->srcData[SRC2_IDX];
    // R2-R23
    if (m < static_cast<uint64_t>(GPR::GPR_A0) || n > static_cast<uint64_t>(GPR::GPR_X3)) {
        ASSERT(false) << "Illegal FExit input";
    }
    if (m > n) {
        n += static_cast<uint64_t>(GPR::GPR_COUNT);
    }

    uint64_t offset = -NUM8; // -8
    MInstFuncPtr inst = nullptr;
    if (uimm14To12 != 0) {
        /* \brief Generate(lui uimm[14:12], ->t) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_LUI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, (uimm14To12 << 12)));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* \brief Generate(addi t#1, uimm[11:0], ->t) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, uimm11To0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        ExecuteCTInst(inst, block);
        /* \brief Generate(add sp, t#1, ->sp) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADD);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_TLINK, 0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        ExecuteCTInst(inst, block);
    } else {
        /* \brief Generate(addi sp, uimm[11:0], ->sp) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_ADDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_UIMM, uimm11To0));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        ExecuteCTInst(inst, block);
    }
    /* \brief Generate(ldi [sp, -8], ->R[M]) */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_LDI);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, offset));
    inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, m));
    ExecuteCTInst(inst, block);
    offset -= NUM8;
    /* \brief Generate(setc.tgt R[M]) */
    inst = nullptr;
    inst = InitCTInst(block, Opcode::OP_SETC_TGT);
    inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, m));
    ExecuteCTInst(inst, block);
    for (uint64_t i = m + 1; i <= n; i++) {
        uint64_t idx = i % static_cast<uint64_t>(GPR::GPR_COUNT);
        if (idx < static_cast<uint64_t>(GPR::GPR_A0)) {
            continue;
        }
        /* \brief Generate(ldi [sp, offset], -> R[idx]) */
        inst = nullptr;
        inst = InitCTInst(block, Opcode::OP_LDI);
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, static_cast<uint64_t>(GPR::GPR_SP)));
        inst->srcs.push_back(std::make_shared<Operand>(OperandType::OPD_SIMM, offset));
        inst->dsts.push_back(std::make_shared<Operand>(OperandType::OPD_GREG, idx));
        ExecuteCTInst(inst, block);
        offset -= NUM8;
    }
}

}
