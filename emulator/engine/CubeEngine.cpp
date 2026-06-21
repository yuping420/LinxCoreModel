#include <cstdint>
#include <iomanip>
#include "../../isa/ISACommon/DataType.h"
#include "ISA.h"
#include "SoftCore.h"
#include "softfloat-types.h"
#include "softfloat.h"
#include "../../isa/calculate/CubeCalculate.h"
#include "../../isa/calculate/FloatPointUtils.h"

namespace JCore {


// 分型中, 小矩阵的行或列为16
const int FIXED_ROW_COLUMN = 16;
// 分型中, 小矩阵的总大小为512B
const int FIXED_MATRIX_SIZE = 512;
// 分型中, 缩放矩阵的小矩阵的总大小为32B
const int FIXED_MATRIX_ZOOM_SIZE = 32;
// 分型中, 缩放矩阵的小矩阵的总大小为16B(特殊情况, 在HiF4格式下)
const int FIXED_MATRIX_ZOOM_HIF4_SIZE = 32;

[[maybe_unused]] const int COMPRESSION_BYTE = 8;

std::vector<uint64_t> SoftCore::LoadFromAcc(DataType dataType, size_t m, size_t n, uint64_t tileBaseAddr,
                                            uint64_t thread)
{
    std::vector<uint64_t> matrix(m * n, 0);
    uint64_t addr = 0;
    uint64_t data = 0;
    uint64_t eleSize = CubeCalculate::EleSize(dataType);
    for (size_t i = 0; i < m; ++i) {
        for (size_t j = 0; j < n; ++j) {
            addr = CubeCalculate::EleOffset(tileBaseAddr, i * n + j, dataType);
            data = CubeCalculate::EleDataExtract(threadStatus[thread].archStatus.tileReg.Load(addr, 0, eleSize), i * n + j, dataType);
            matrix[i * n + j] = data;
        }
    }
    return matrix;
}

std::vector<uint64_t> SoftCore::LoadFromTileRegisterRD(DataType dataType, size_t m, size_t n, uint64_t tileBaseAddr,
                                                       uint64_t thread)
{
    std::vector<uint64_t> matrix(m * n, 0);
    uint64_t addr = 0;
    uint64_t data = 0;
    uint64_t eleSize = CubeCalculate::EleSize(dataType);
    for (size_t i = 0; i < m; ++i) {
        for (size_t j = 0; j < n; ++j) {
            addr = CubeCalculate::EleOffset(tileBaseAddr, i * n + j, dataType);
            data = CubeCalculate::EleDataExtract(threadStatus[thread].archStatus.tileReg.Load(addr, 0, eleSize), i * n + j, dataType);
            matrix[i * n + j] = data;
        }
    }
    return matrix;
}

std::vector<uint64_t> SoftCore::LoadFromTileRegisterCD(DataType dataType, size_t m, size_t n, uint64_t tileBaseAddr,
                                                       uint64_t thread)
{
    std::vector<uint64_t> matrix(m * n, 0);
    uint64_t addr = 0;
    uint64_t data = 0;
    uint64_t eleSize = CubeCalculate::EleSize(dataType);
    for (size_t i = 0; i < m; ++i) {
        for (size_t j = 0; j < n; ++j) {
            addr = CubeCalculate::EleOffset(tileBaseAddr, j * n + i, dataType);
            data = CubeCalculate::EleDataExtract(threadStatus[thread].archStatus.tileReg.Load(addr, 0, eleSize), j * n + i, dataType);
            matrix[i * n + j] = data;
        }
    }
    return matrix;
}

std::vector<uint64_t> SoftCore::LoadFromTileRegisterZz(DataType dataType, std::pair<size_t, size_t> entireMatrix,
                                                       std::pair<size_t, size_t> blockMatrix, uint64_t tileBaseAddr,
                                                       uint64_t thread)
{
    std::vector<uint64_t> matrix(entireMatrix.first * entireMatrix.second, 0);

    size_t blockn = entireMatrix.second / blockMatrix.second;

    auto calOffset = [blockn](size_t i, size_t j, size_t blockMatrixRow, size_t blockMatrixColumn) -> size_t {
        size_t blockI = i / blockMatrixRow;
        size_t blockJ = j / blockMatrixColumn;
        size_t innerX = i % blockMatrixRow;
        size_t innerY = j % blockMatrixColumn;

        size_t blockOffset = blockI * blockn + blockJ;
        blockOffset *= (blockMatrixRow * blockMatrixColumn);
        size_t innerOffset = innerX * blockMatrixColumn + innerY;
        return blockOffset + innerOffset;
    };

    uint64_t data = 0;
    uint64_t addr = 0;
    uint64_t eleSize = CubeCalculate::EleSize(dataType);
    for (size_t i = 0; i < entireMatrix.first; ++i) {
        for (size_t j = 0; j < entireMatrix.second; ++j) {
            size_t offset = i * entireMatrix.second + j;
            size_t oriOffset = calOffset(i, j, blockMatrix.first, blockMatrix.second);
            addr = CubeCalculate::EleOffset(tileBaseAddr, oriOffset, dataType);
            data = threadStatus[thread].archStatus.tileReg.Load(addr, 0, eleSize);
            matrix[offset] = CubeCalculate::EleDataExtract(data, oriOffset, dataType);
        }
    }

    return matrix;
}

std::vector<uint64_t> SoftCore::LoadFromTileRegisterZn(DataType dataType, std::pair<size_t, size_t> entireMatrix,
                                                       std::pair<size_t, size_t> blockMatrix, uint64_t tileBaseAddr,
                                                       uint64_t thread)
{
    size_t blockm = (entireMatrix.first + blockMatrix.first - 1) / blockMatrix.first;
    size_t blockn = (entireMatrix.second + blockMatrix.second - 1) / blockMatrix.second;
    size_t afterPaddingM = blockm * blockMatrix.first;
    size_t afterPaddingN = blockn * blockMatrix.second;
    std::vector<uint64_t> matrix(afterPaddingM * afterPaddingN, 0);

    auto calOffset = [blockn](size_t i, size_t j, size_t blockMatrixRow, size_t blockMatrixColumn) -> size_t {
        size_t blockI = i / blockMatrixRow;
        size_t blockJ = j / blockMatrixColumn;
        size_t innerX = i % blockMatrixRow;
        size_t innerY = j % blockMatrixColumn;

        size_t blockOffset = blockI * blockn + blockJ;
        blockOffset *= (blockMatrixRow * blockMatrixColumn);
        size_t innerOffset = innerY * blockMatrixRow + innerX;
        return blockOffset + innerOffset;
    };

    uint64_t data = 0;
    uint64_t addr = 0;
    uint64_t eleSize = CubeCalculate::EleSize(dataType);
    for (size_t i = 0; i < afterPaddingM; ++i) {
        for (size_t j = 0; j < afterPaddingN; ++j) {
            size_t offset = i * afterPaddingN + j;
            if (i < entireMatrix.first && j < entireMatrix.second) {
                size_t oriOffset = calOffset(i, j, blockMatrix.first, blockMatrix.second);
                addr = CubeCalculate::EleOffset(tileBaseAddr, oriOffset, dataType);
                data = threadStatus[thread].archStatus.tileReg.Load(addr, 0, eleSize);
                matrix[offset] = CubeCalculate::EleDataExtract(data, oriOffset, dataType);
            } else {
                matrix[offset] = 0;
            }
        }
    }

    return matrix;
}

std::vector<uint64_t> SoftCore::LoadFromTileRegisterNn(DataType dataType, std::pair<size_t, size_t> entireMatrix,
                                                       std::pair<size_t, size_t> blockMatrix, uint64_t tileBaseAddr,
                                                       uint64_t thread)
{
    std::vector<uint64_t> matrix(entireMatrix.first * entireMatrix.second, 0);

    size_t blockm = entireMatrix.first / blockMatrix.first;

    auto calOffset = [blockm](size_t i, size_t j, size_t blockMatrixRow, size_t blockMatrixColumn) -> size_t {
        size_t blockI = i / blockMatrixRow;
        size_t blockJ = j / blockMatrixColumn;
        size_t innerX = i % blockMatrixRow;
        size_t innerY = j % blockMatrixColumn;

        size_t blockOffset = blockJ * blockm + blockI;
        blockOffset *= (blockMatrixRow * blockMatrixColumn);
        size_t innerOffset = innerY * blockMatrixRow + innerX;
        return blockOffset + innerOffset;
    };

    uint64_t data = 0;
    uint64_t addr = 0;
    uint64_t eleSize = CubeCalculate::EleSize(dataType);
    for (size_t i = 0; i < entireMatrix.first; ++i) {
        for (size_t j = 0; j < entireMatrix.second; ++j) {
            size_t offset = i * entireMatrix.second + j;
            size_t oriOffset = calOffset(i, j, blockMatrix.first, blockMatrix.second);
            addr = CubeCalculate::EleOffset(tileBaseAddr, oriOffset, dataType);
            data = threadStatus[thread].archStatus.tileReg.Load(addr, 0, eleSize);
            matrix[offset] = CubeCalculate::EleDataExtract(data, oriOffset, dataType);
        }
    }

    return matrix;
}

std::vector<uint64_t> SoftCore::LoadFromTileRegisterNz(DataType dataType, std::pair<size_t, size_t> entireMatrix,
                                                       std::pair<size_t, size_t> blockMatrix, uint64_t tileBaseAddr,
                                                       uint64_t thread)
{
    size_t blockm = (entireMatrix.first + blockMatrix.first - 1) / blockMatrix.first;
    size_t blockn = (entireMatrix.second + blockMatrix.second - 1) / blockMatrix.second;
    size_t afterPaddingM = blockm * blockMatrix.first;
    size_t afterPaddingN = blockn * blockMatrix.second;
    std::vector<uint64_t> matrix(afterPaddingM * afterPaddingN, 0);

    auto calOffset = [blockm](size_t i, size_t j, size_t blockMatrixRow, size_t blockMatrixColumn) -> size_t {
        size_t blockI = i / blockMatrixRow;
        size_t blockJ = j / blockMatrixColumn;
        size_t innerX = i % blockMatrixRow;
        size_t innerY = j % blockMatrixColumn;

        size_t blockOffset = blockJ * blockm + blockI;
        blockOffset *= (blockMatrixRow * blockMatrixColumn);
        size_t innerOffset = innerX * blockMatrixColumn + innerY;
        return blockOffset + innerOffset;
    };

    uint64_t data = 0;
    uint64_t addr = 0;
    uint64_t eleSize = CubeCalculate::EleSize(dataType);
    for (size_t i = 0; i < afterPaddingM; ++i) {
        for (size_t j = 0; j < afterPaddingN; ++j) {
            size_t offset = i * afterPaddingN + j;
            if (i < entireMatrix.first && j < entireMatrix.second) {
                size_t oriOffset = calOffset(i, j, blockMatrix.first, blockMatrix.second);
                addr = CubeCalculate::EleOffset(tileBaseAddr, oriOffset, dataType);
                data = threadStatus[thread].archStatus.tileReg.Load(addr, 0, eleSize);
                matrix[offset] = CubeCalculate::EleDataExtract(data, oriOffset, dataType);
            } else {
                matrix[offset] = 0;
            }
        }
    }

    return matrix;
}

JCore::DataType SoftCore::ConvertTo4ByteType(JCore::DataType dstType)
{
    JCore::DataType retType = dstType;
    switch (dstType) {
        case JCore::DataType::FP64:
        case JCore::DataType::FP32:
        case JCore::DataType::TF32:
        case JCore::DataType::HF32:
        case JCore::DataType::FP16:
        case JCore::DataType::BF16:
        case JCore::DataType::HIF8:
        case JCore::DataType::FP8:
        case JCore::DataType::FP8_1:
        case JCore::DataType::FP6:
        case JCore::DataType::FP6_1:
        case JCore::DataType::FP4:
        case JCore::DataType::FP4_1:
        case JCore::DataType::SF8:
        case JCore::DataType::HIF4:
            retType = JCore::DataType::FP32;
            break;
        case JCore::DataType::INT64:
        case JCore::DataType::INT32:
        case JCore::DataType::INT16:
        case JCore::DataType::INT8:
        case JCore::DataType::INT4:
            retType = JCore::DataType::INT32;
            break;
        case JCore::DataType::UINT64:
        case JCore::DataType::UINT32:
        case JCore::DataType::UINT16:
        case JCore::DataType::UINT8:
        case JCore::DataType::UINT4:
            retType = JCore::DataType::UINT32;
            break;
        default:
            break;
    }
        return retType;
}

void SoftCore::ScaleAcc(std::vector<uint64_t>& dst, JCore::DataType srcType, bool needScale, uint64_t scaleVal)
{
    if (!needScale) {
        return;
    }

    for (size_t i = 0; i < dst.size(); i++) {
        dst[i] = CubeCalculate::EleMul(dst[i], scaleVal, srcType, this->status);
    }
}

void SoftCore::DataFormatCvt(std::vector<uint64_t>& dst, JCore::DataType srcType, JCore::DataType dstType,
                             uint64_t thread)
{
    auto getFRM = [this, thread]() {
        uint32_t FRM = 0;
        uint64_t CSTATE = threadStatus[thread].archStatus.sysreg[SystemReg::SYS_CSTATE];
        if (GetBits(CSTATE, 39, 39)) {
            // RV使能字段非0, FRM 字段有效，使用指定的舍入模式
            FRM = GetBits(CSTATE, FRM_BIT_END, FRM_BIT_BEGIN);
        } else {
            FRM = RNE_MODE;
        }
        return FRM;
    };

    auto OpCvtType = [](DataType dataType) -> OPConvertType {
        switch (dataType) {
            case DataType::FP64:
                return OPConvertType::OPCVT_FP64;
            case DataType::FP32:
                return OPConvertType::OPCVT_FP32;
            case DataType::FP16:
                return OPConvertType::OPCVT_FP16;
            case DataType::BF16:
                return OPConvertType::OPCVT_BF16;
            case DataType::FP8:
                return OPConvertType::OPCVT_FP8;
            case DataType::FP8_1:
                return OPConvertType::OPCVT_FP8_1;
            case DataType::INT64:
                return OPConvertType::OPCVT_S64;
            case DataType::INT32:
                return OPConvertType::OPCVT_S32;
            case DataType::INT16:
                return OPConvertType::OPCVT_S16;
            case DataType::INT8:
                return OPConvertType::OPCVT_S8;
            case DataType::UINT64:
                return OPConvertType::OPCVT_U64;
            case DataType::UINT32:
                return OPConvertType::OPCVT_U32;
            case DataType::UINT16:
                return OPConvertType::OPCVT_U16;
            case DataType::UINT8:
                return OPConvertType::OPCVT_U8;
            case DataType::FP4:
                return OPConvertType::OPCVT_FP4;
            case DataType::FP4_1:
                return OPConvertType::OPCVT_FP4_1;
            case DataType::TF32:
            case DataType::HF32:
            case DataType::HIF8:
            case DataType::FP6:
            case DataType::FP6_1:
            case DataType::SF8:
            case DataType::INT4:
            case DataType::UINT4:
                assert(0 && "Not support such type convert yet");
            default:
                assert(0 && "No corresponding data conversion type");
        }
    };

    if (srcType == DataType::HIF4) {
        return;
    }

    for (uint64_t i = 0; i < dst.size(); i++) {
        dst[i] = JCore::Calculate::ConvertAggre(dst[i], OpCvtType(srcType), OpCvtType(dstType), getFRM());
    }
}

static std::vector<uint64_t> DoMatrixPadding(const std::vector<uint64_t>& matrix,
                                             uint64_t row, uint64_t col, uint64_t externRow, uint64_t externCol)
{
    std::vector<uint64_t> externMatrix(externRow * externCol, 0);
    for (size_t i = 0; i < externRow; ++i) {
        for (size_t j = 0; j < externCol; ++j) {
            size_t offset = i * externCol + j;
            if (i >= row || j >= col) {
                externMatrix[offset] = 0;
            } else {
                size_t moffset = i * col + j;
                externMatrix[offset] = matrix[moffset];
            }
        }
    }

    return externMatrix;
}

static DataType GetAccType(DataType srcLType, DataType srcRType)
{
    const static std::unordered_set<DataType> FP_TYPES = {
        DataType::FP64,
        DataType::FP32,
        DataType::TF32,
        DataType::HF32,
        DataType::FP16,
        DataType::BF16,
        DataType::HIF8,
        DataType::FP8,
        DataType::FP8_1,
        DataType::FP6,
        DataType::FP6_1,
        DataType::FP4,
        DataType::FP4_1,
        DataType::SF8,
        DataType::HIF4,
    };
    const static std::unordered_set<DataType> SIGNED_TYPES = {
        DataType::INT64,
        DataType::INT32,
        DataType::INT16,
        DataType::INT8,
        DataType::INT4 ,
    };

    bool lF = (FP_TYPES.count(srcLType) != 0);
    bool rF = (FP_TYPES.count(srcRType) != 0);

    bool lS = (SIGNED_TYPES.count(srcLType) != 0);
    bool rS = (SIGNED_TYPES.count(srcRType) != 0);

    if (lF || rF) {
        return DataType::FP32;
    }
    if (lS || rS) {
        return DataType::INT32;
    }
    return DataType::UINT32;
}

void SoftCore::ExecuteCUBE(BlockFuncPtr block)
{
    uint64_t m = block->lb0;
    uint64_t n = block->lb1;
    uint64_t k = block->lb2;
    uint64_t externM = m;
    uint64_t externN = n;
    uint64_t externK = k;
    DataType accSrcType = DataType::INVALID;

    auto CalExtendShape = [&externM, &externN, &externK](uint64_t m, uint64_t n, uint64_t k, double eleSize) {
        size_t row = FIXED_ROW_COLUMN;
        size_t column = FIXED_MATRIX_SIZE / static_cast<uint64_t>(static_cast<double>(row) * eleSize);
        externM = ((m + row - 1) / row) * row;
        externK = ((k + column - 1) / column) * column;

        column = FIXED_ROW_COLUMN;
        /*
         row = FIXED_MATRIX_SIZE / (column * eleSize);
         externK = ((k + row - 1) / row) * row;
         */
        externN = ((n + column - 1) / column) * column;
    };

    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        LOG_INFO << "Cube template shape: M=" << std::dec << m << ", N=" << n << ", K=" << k;
    }
    constexpr uint64_t lIdx = 0;
    constexpr uint64_t rIdx = 1;
    constexpr uint64_t cIdx = 2;

    double eleSize = 0;
    uint64_t baseAddrL = 0;
    uint64_t baseAddrR = 0;
    uint64_t baseAddrC = 0;
    DataType srcLType = block->dataType;
    DataType srcRType = (block->blockAttr != nullptr && block->blockAttr->dataType != DataType::INVALID) ?
                        block->blockAttr->dataType : block->dataType;
    DataType dstType = GetAccType(srcLType, srcRType);
    DataType scaleLType = DataType::FP8;
    DataType scaleRType = DataType::FP8;

    CubeMatShape shape(m, n, k, block->dataType, srcRType, dstType);
    uint64_t tileAddr = 0;
    LayOut tileConvertType = LayOut::NORM;
    std::vector<uint64_t> srcL;
    std::vector<uint64_t> srcR;
    std::vector<uint64_t> srcD;

    std::vector<uint64_t> srcLscale;
    std::vector<uint64_t> srcLscaleE16;
    std::vector<uint64_t> srcRscale;
    std::vector<uint64_t> srcRscaleE16;

    std::vector<uint64_t> acc;
    auto &tileReg = threadStatus[block->threadId].archStatus.tileReg;
    std::vector<uint64_t> dst(m * n, CubeCalculate::EleZero(shape.dstType));
    switch (block->tileOp) {
        case TileOp::TMATMUL:
            eleSize = CubeCalculate::EleRealSize(shape.dstType);
            baseAddrL = block->srcTile[lIdx]->baseAddr;
            baseAddrR = block->srcTile[rIdx]->baseAddr;
            srcL = LoadFromTileRegister(shape.srcLType, m, k, baseAddrL, FractalType::NZ, block->threadId);
            srcR = LoadFromTileRegister(shape.srcRType, k, n, baseAddrR, FractalType::ZN, block->threadId);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(RD), dataType: " << shape.srcLType << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcR(RD), dataType: " << shape.srcRType << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
            }
            CalExtendShape(m, n, k, eleSize);
            accSrcType = ConvertTo4ByteType(shape.dstType);
            DataFormatCvt(srcL, shape.srcLType, accSrcType, block->threadId);
            DataFormatCvt(srcR, shape.srcRType, accSrcType, block->threadId);
            shape.InitShape(m, n, k, accSrcType);
            shape.InitExtern(externM, externN, externK);
            dst.resize(externM * externN, CubeCalculate::EleZero(shape.dstType));
            srcL = DoMatrixPadding(srcL, m, k, externM, externK);
            srcR = DoMatrixPadding(srcR, k, n, externK, externN);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "dst, dataType: " << shape.dstType << std::endl;
            }
            MatrixMul(dst, srcL, srcR, shape);
            break;
        case TileOp::TMATMUL_BIAS:
            eleSize = CubeCalculate::EleRealSize(shape.dstType);
            baseAddrL = block->srcTile[lIdx]->baseAddr;
            baseAddrR = block->srcTile[rIdx]->baseAddr;
            baseAddrC = block->srcTile[cIdx]->baseAddr;
            srcL = LoadFromTileRegister(shape.srcLType, m, k, baseAddrL, FractalType::NZ, block->threadId);
            srcR = LoadFromTileRegister(shape.srcRType, k, n, baseAddrR, FractalType::ZN, block->threadId);
            srcD = LoadFromTileRegister(DataType::FP32, m, n, baseAddrC, FractalType::NZ, block->threadId);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(RD), dataType: " << shape.srcLType << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcR(RD), dataType: " << shape.srcRType << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
                std::cout << "SrcD(RD): " << std::endl;
                CubeCalculate::PrintData(srcL, m, n);
            }
            CalExtendShape(m, n, k, eleSize);
            accSrcType = ConvertTo4ByteType(shape.dstType);
            DataFormatCvt(srcL, shape.srcLType, accSrcType, block->threadId);
            DataFormatCvt(srcR, shape.srcRType, accSrcType, block->threadId);
            DataFormatCvt(srcD, shape.dstType, accSrcType, block->threadId);
            shape.InitShape(m, n, k, accSrcType);
            shape.InitExtern(externM, externN, externK);
            dst.resize(externM * externN, CubeCalculate::EleZero(shape.dstType));
            srcL = DoMatrixPadding(srcL, m, k, externM, externK);
            srcR = DoMatrixPadding(srcR, k, n, externK, externN);
            srcD = DoMatrixPadding(srcD, m, n, externM, externN);
            MatrixMul(dst, srcL, srcR, shape);
            MatrixAdd(dst, dst, srcD, shape);
            break;
        case TileOp::TMATMUL_ACC:
            eleSize = CubeCalculate::EleRealSize(shape.dstType);
            baseAddrL = block->srcTile[lIdx]->baseAddr;
            baseAddrR = block->srcTile[rIdx]->baseAddr;
            srcL = LoadFromTileRegister(shape.srcLType, m, k, baseAddrL, FractalType::NZ, block->threadId);
            srcR = LoadFromTileRegister(shape.srcRType, k, n, baseAddrR, FractalType::ZN, block->threadId);
            acc = LoadFromAcc(DataType::FP32, m, n, tileReg.tileReg[OperandType::OPD_TILE_ACC].startAddr_,
                              block->threadId);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(RD), dataType: " << shape.srcLType << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcR(RD), dataType: " << shape.srcRType << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
                std::cout << "ACC(RD): " << std::endl;
                CubeCalculate::PrintData(acc, m, n);
            }
            CalExtendShape(m, n, k, eleSize);
            accSrcType = ConvertTo4ByteType(shape.dstType);
            DataFormatCvt(srcL, shape.srcLType, accSrcType, block->threadId);
            DataFormatCvt(srcR, shape.srcRType, accSrcType, block->threadId);
            shape.InitShape(m, n, k, accSrcType);
            shape.InitExtern(externM, externN, externK);
            dst.resize(externM * externN, CubeCalculate::EleZero(shape.dstType));
            srcL = DoMatrixPadding(srcL, m, k, externM, externK);
            srcR = DoMatrixPadding(srcR, k, n, externK, externN);
            acc = DoMatrixPadding(acc, m, n, externM, externN);
            MatrixMul(dst, srcL, srcR, shape);
            MatrixAdd(dst, dst, acc, shape);
            break;
        // 对于带 Scaled 的矩阵乘，施加 scale 的过程已经做了类型转换（FPX->FP32），所以内部不再需要做 DataFormatCvt
        // TODO: 对于 HiF4，目前实现看起来存在问题（理应是 FP32，但是实际采用了 double）
        case TileOp::TMATMULMX: {
            DataType dataTypeA = block->dataType;
            DataType dataTypeB = (block->blockAttr != nullptr && block->blockAttr->dataType != DataType::INVALID) ?
                                 block->blockAttr->dataType : dataTypeA;
            double eleSizeA = CubeCalculate::EleRealSize(dataTypeA);
            if (dataTypeA == DataType::HIF4) {
                scaleLType = DataType::FP16;
            }
            if (dataTypeB == DataType::HIF4) {
                scaleRType = DataType::FP16;
            }

            // scaleA的总列数
            uint32_t kscaleA = (dataTypeA == DataType::HIF4) ? (k / FIXED_MATRIX_ZOOM_HIF4_SIZE) :
                               (k / FIXED_MATRIX_ZOOM_SIZE);
            // scaleB的总行数
            uint32_t kscaleB = (dataTypeB == DataType::HIF4) ? (k / FIXED_MATRIX_ZOOM_HIF4_SIZE) :
                               (k / FIXED_MATRIX_ZOOM_SIZE);

            baseAddrL = block->srcTile[0]->baseAddr;
            uint64_t baseAddrLScale = block->srcTile[1]->baseAddr;
            baseAddrR = block->srcTile[2]->baseAddr;
            uint64_t baseAddrRScale = block->srcTile[3]->baseAddr;
            srcL = LoadFromTileRegister(dataTypeA, m, k, baseAddrL, FractalType::NZ, block->threadId);
            srcR = LoadFromTileRegister(dataTypeB, k, n, baseAddrR, FractalType::ZN, block->threadId);
            srcLscale = LoadFromTileRegister(scaleLType, m, kscaleA, baseAddrLScale, FractalType::ZZ,
                block->threadId);
            srcRscale = LoadFromTileRegister(scaleRType, kscaleB, n, baseAddrRScale, FractalType::NN,
                block->threadId);

            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(RD): " << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcLscale(RD): " << std::endl;
                CubeCalculate::PrintData(srcLscale, m, kscaleA);
                std::cout << "SrcR(RD): " << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
                std::cout << "SrcRscale(RD): " << std::endl;
                CubeCalculate::PrintData(srcRscale, kscaleB, n);
            }
            // 非HiF4格式下, 每32个连续元素共享一个缩放因子
            if (dataTypeA == DataType::HIF4) {
                // HiF4格式下, srcL固定为行16、列64; srcLscale固定为行16、列8
                MatrixScaleLHiF4(srcL, srcLscale, {m, k});
            } else {
                MatrixScale(srcL, srcLscale, {m, k}, FIXED_MATRIX_ZOOM_SIZE, dataTypeA);
            }
            if (dataTypeB == DataType::HIF4) {
                // HiF4格式下, srcR固定为行64、列16; srcLscale固定为行16、列8
                MatrixScaleRHiF4(srcR, srcRscale, {k, n});
            } else {
                MatrixScale(srcR, srcRscale, {k, n}, FIXED_MATRIX_ZOOM_SIZE, dataTypeB);
            }
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(RD)——After Scale: " << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcR(RD)——After Scale: " << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
            }
            // 以srcL的eleSize为基准
            CalExtendShape(m, n, k, eleSizeA);
            accSrcType = ConvertTo4ByteType(dataTypeA);
            shape.InitShape(m, n, k, accSrcType);
            shape.InitExtern(externM, externN, externK);
            dst.resize(externM * externN, CubeCalculate::EleZero(accSrcType));
            srcL = DoMatrixPadding(srcL, m, k, externM, externK);
            srcR = DoMatrixPadding(srcR, k, n, externK, externN);
            MatrixMul(dst, srcL, srcR, shape);
            break;
        }
        case TileOp::TMATMULMX_BIAS: {
            DataType dataTypeA = block->dataType;   // dataTypeC == dataTypeA
            DataType dataTypeB = (block->blockAttr != nullptr && block->blockAttr->dataType != DataType::INVALID) ?
                                 block->blockAttr->dataType : dataTypeA;
            if (dataTypeA == DataType::HIF4) {
                scaleLType = DataType::FP16;
            }
            if (dataTypeB == DataType::HIF4) {
                scaleRType = DataType::FP16;
            }
            double eleSizeA = CubeCalculate::EleRealSize(dataTypeA);

            // scaleA的总列数
            uint32_t kscaleA = (dataTypeA == DataType::HIF4) ? (k / FIXED_MATRIX_ZOOM_HIF4_SIZE) :
                               (k / FIXED_MATRIX_ZOOM_SIZE);
            // scaleB的总行数
            uint32_t kscaleB = (dataTypeB == DataType::HIF4) ? (k / FIXED_MATRIX_ZOOM_HIF4_SIZE) :
                               (k / FIXED_MATRIX_ZOOM_SIZE);

            baseAddrL = block->srcTile[0]->baseAddr;
            uint64_t baseAddrLScale = block->srcTile[1]->baseAddr;
            baseAddrR = block->srcTile[2]->baseAddr;
            uint64_t baseAddrRScale = block->srcTile[3]->baseAddr;
            baseAddrC = block->srcTile[4]->baseAddr;
            // 以RD方式取出时, src已为原分型格式(srcL-Nz、srcR-Zn), 无需再转型一次
            srcL = LoadFromTileRegister(dataTypeA, m, k, baseAddrL, FractalType::NZ, block->threadId);
            srcR = LoadFromTileRegister(dataTypeB, k, n, baseAddrR, FractalType::ZN, block->threadId);
            // 以RD方式取出时, srcscale已为原分型格式(srcLscale-Nn、srcRscale-Zz), 无需再转型一次
            srcLscale = LoadFromTileRegister(scaleLType, m, kscaleA, baseAddrLScale, FractalType::ZZ,
                block->threadId);
            srcRscale = LoadFromTileRegister(scaleRType, kscaleB, n, baseAddrRScale, FractalType::NN,
                block->threadId);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(Nz): " << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcLscale(Nn): " << std::endl;
                CubeCalculate::PrintData(srcLscale, m, kscaleA);
                std::cout << "SrcR(Zn): " << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
                std::cout << "SrcRscale(Zz): " << std::endl;
                CubeCalculate::PrintData(srcRscale, kscaleB, n);
            }
            // 非HiF4格式下, 每32个连续元素共享一个缩放因子
            if (dataTypeA == DataType::HIF4) {
                // HiF4格式下, srcL固定为行16、列64; srcLscale固定为行16、列8
                MatrixScaleLHiF4(srcL, srcLscale, {m, k});
            } else {
                MatrixScale(srcL, srcLscale, {m, k}, FIXED_MATRIX_ZOOM_SIZE, dataTypeA);
            }
            if (dataTypeB == DataType::HIF4) {
                // HiF4格式下, srcR固定为行64、列16; srcLscale固定为行16、列8
                MatrixScaleRHiF4(srcR, srcRscale, {k, n});
            } else {
                MatrixScale(srcR, srcRscale, {k, n}, FIXED_MATRIX_ZOOM_SIZE, dataTypeB);
            }
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(Nz)——After Scale: " << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcR(Zn)——After Scale: " << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
            }
            srcD = LoadFromTileRegister(DataType::FP32, m, n, baseAddrC, FractalType::NZ, block->threadId);
            // 以srcL的eleSize为基准
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(RD): " << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcR(RD): " << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
                std::cout << "SrcD(RD): " << std::endl;
                CubeCalculate::PrintData(srcD, m, n);
            }
            CalExtendShape(m, n, k, eleSizeA);
            accSrcType = ConvertTo4ByteType(dataTypeA);
            DataFormatCvt(srcD, DataType::FP32, accSrcType, block->threadId);
            shape.InitShape(m, n, k, accSrcType);
            shape.InitExtern(externM, externN, externK);
            dst.resize(externM * externN, CubeCalculate::EleZero(accSrcType));
            srcL = DoMatrixPadding(srcL, m, k, externM, externK);
            srcR = DoMatrixPadding(srcR, k, n, externK, externN);
            srcD = DoMatrixPadding(srcD, m, n, externM, externN);
            MatrixMul(dst, srcL, srcR, shape);
            MatrixAdd(dst, dst, srcD, shape);
            break;
        }
        case TileOp::TMATMULMX_ACC: {
            DataType dataTypeA = block->dataType;
            DataType dataTypeB = (block->blockAttr != nullptr && block->blockAttr->dataType != DataType::INVALID) ?
                                 block->blockAttr->dataType : dataTypeA;
            double eleSizeA = CubeCalculate::EleRealSize(dataTypeA);
            if (dataTypeA == DataType::HIF4) {
                scaleLType = DataType::FP16;
            }
            if (dataTypeB == DataType::HIF4) {
                scaleRType = DataType::FP16;
            }
            // scaleA的总列数
            uint32_t kscaleA = (dataTypeA == DataType::HIF4) ? (k / FIXED_MATRIX_ZOOM_HIF4_SIZE) :
                               (k / FIXED_MATRIX_ZOOM_SIZE);
            // scaleB的总行数
            uint32_t kscaleB = (dataTypeB == DataType::HIF4) ? (k / FIXED_MATRIX_ZOOM_HIF4_SIZE) :
                               (k / FIXED_MATRIX_ZOOM_SIZE);

            baseAddrL = block->srcTile[0]->baseAddr;
            uint64_t baseAddrLScale = block->srcTile[1]->baseAddr;
            baseAddrR = block->srcTile[2]->baseAddr;
            uint64_t baseAddrRScale = block->srcTile[3]->baseAddr;
            // 以RD方式取出时, src已为原分型格式(srcL-Nz、srcR-Zn), 无需再转型一次
            srcL = LoadFromTileRegister(dataTypeA, m, k, baseAddrL, FractalType::NZ, block->threadId);
            srcR = LoadFromTileRegister(dataTypeB, k, n, baseAddrR, FractalType::ZN, block->threadId);
            // 以RD方式取出时, srcscale已为原分型格式(srcLscale-Nn、srcRscale-Zz), 无需再转型一次
            srcLscale = LoadFromTileRegister(scaleLType, m, kscaleA, baseAddrLScale, FractalType::ZZ,
                block->threadId);
            srcRscale = LoadFromTileRegister(scaleRType, kscaleB, n, baseAddrRScale, FractalType::NN,
                block->threadId);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(Nz): " << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcLscale(Nn): " << std::endl;
                CubeCalculate::PrintData(srcLscale, m, kscaleA);
                std::cout << "SrcR(Zn): " << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
                std::cout << "SrcRscale(Zz): " << std::endl;
                CubeCalculate::PrintData(srcRscale, kscaleB, n);
            }
            // 非HiF4格式下, 每32个连续元素共享一个缩放因子
            if (dataTypeA == DataType::HIF4) {
                // HiF4格式下, srcL固定为行16、列64; srcLscale固定为行16、列8
                MatrixScaleLHiF4(srcL, srcLscale, {m, k});
            } else {
                MatrixScale(srcL, srcLscale, {m, k}, FIXED_MATRIX_ZOOM_SIZE, dataTypeA);
            }
            if (dataTypeB == DataType::HIF4) {
                // HiF4格式下, srcR固定为行64、列16; srcLscale固定为行16、列8
                MatrixScaleRHiF4(srcR, srcRscale, {k, n});
            } else {
                MatrixScale(srcR, srcRscale, {k, n}, FIXED_MATRIX_ZOOM_SIZE, dataTypeB);
            }
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(Nz)——After Scale: " << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcR(Zn)——After Scale: " << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
            }
            // 以srcL的eleSize为基准
            acc = LoadFromAcc(DataType::FP32, m, n, tileReg.tileReg[OperandType::OPD_TILE_ACC].startAddr_,
                block->threadId);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "SrcL(RD)——After Scale: " << std::endl;
                CubeCalculate::PrintData(srcL, m, k);
                std::cout << "SrcR(RD)——After Scale: " << std::endl;
                CubeCalculate::PrintData(srcR, k, n);
                std::cout << "ACC(RD): " << std::endl;
                CubeCalculate::PrintData(srcD, m, n);
            }
            CalExtendShape(m, n, k, eleSizeA);
            accSrcType = ConvertTo4ByteType(shape.dstType);
            shape.InitShape(m, n, k, accSrcType);
            shape.InitExtern(externM, externN, externK);
            dst.resize(externM * externN, CubeCalculate::EleZero(accSrcType));
            srcL = DoMatrixPadding(srcL, m, k, externM, externK);
            srcR = DoMatrixPadding(srcR, k, n, externK, externN);
            acc = DoMatrixPadding(acc, m, n, externM, externN);
            MatrixMul(dst, srcL, srcR, shape);
            MatrixAdd(dst, dst, acc, shape);
            break;
        }
        case TileOp::ACCCVT:
            {
                bool needScale = false;
                needScale = block->srcData.size() > 0;
                uint64_t scaleVal = 0;
                acc = LoadFromAcc(shape.dstType, m, n, tileReg.tileReg[OperandType::OPD_TILE_ACC].startAddr_,
                                  block->threadId);
                if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                    std::cout << "ACC: " << std::endl;
                    CubeCalculate::PrintData(acc, m, n);
                }
                if (!block->srcData.empty()) {
                    scaleVal = block->srcData[0];
                }
                eleSize = CubeCalculate::EleRealSize(shape.dstType);
                tileAddr = block->dstTile[0]->baseAddr;
                bool needrowMax = false;
                if (block->dstTile.size() > 1) {
                    needrowMax = true;
                }
                tileConvertType = block->blockAttr->layout;
                dst = acc;
                ScaleAcc(dst, DataType::FP32, needScale, scaleVal);
                // DataFormatCvt(dst, shape.srcType, shape.dstType, block->threadId);
                AccToTileRegister(dst, {m, n}, shape.dstType, tileAddr,
                                  tileConvertType, block->blockAttr->canon, block->threadId);
                if (needrowMax) {
                    // do row max
                    uint64_t rowMaxBase = block->dstTile[1]->baseAddr;
                    RowMax(dst, rowMaxBase, shape.dstType, {m, n}, block->threadId);
                }
                break;
            }
        default:
            std::cerr << "Unsupport cube template func: " << static_cast<uint64_t>(block->tileOp) << std::endl;
            assert(0);
    }
    if (TileOp::ACCCVT != block->tileOp) {
        if (block->dstTile[0]->type == OperandType::OPD_TILE_ACC) {
            WriteToAcc(dst, shape, block->threadId);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "Dst: " << std::endl;
                CubeCalculate::PrintData(dst, m, n);
            }
        }
        CubeCalculate::ResultMatrix(dst, {m, n}, CubeCalculate::EleSize(shape.dstType), tileConvertType);
    }
}

// dst:  [M, N] -> [externM, externN]
// srcL: [M, K] -> [externM, externK]
// srcR: [K, N] -> [externK, externN]
void SoftCore::MatrixMul(std::vector<uint64_t>& dst, const std::vector<uint64_t>& srcL,
                         const std::vector<uint64_t>& srcR, const CubeMatShape& shape)
{
    for (size_t i = 0; i < shape.externM; ++i) {
        for (size_t j = 0; j < shape.externN; ++j) {
            for (size_t k = 0; k < shape.externK; ++k) {
                dst[i * shape.externN + j] = CubeCalculate::EleAdd(dst[i * shape.externN + j],
                                                             CubeCalculate::EleMul(srcL[i * shape.externK + k],
                                                                                   srcR[k * shape.externN + j],
                                                                                   shape.dstType,
                                                                                   this->status),
                                                            shape.dstType,
                                                            this->status);
            }
        }
    }
}

void SoftCore::MatrixAdd(std::vector<uint64_t>& dst, const std::vector<uint64_t>& srcL,
                         const std::vector<uint64_t>& srcR, const CubeMatShape& shape)
{
    for (size_t i = 0; i < shape.externM; ++i) {
        for (size_t j = 0; j < shape.externN; ++j) {
            dst[i * shape.externN + j] = CubeCalculate::EleAdd(srcL[i * shape.externN + j],
            srcR[i * shape.externN + j], shape.dstType, this->status);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "srcL: " << std::hex << srcL[i * shape.externN + j] << std::endl;
                std::cout << "srcR: " << std::hex << srcR[i * shape.externN + j] << std::endl;
            }
        }
    }
}

void SoftCore::MatrixScale(std::vector<uint64_t>& src,
                            const std::vector<uint64_t>& srcScale,
                            std::pair<size_t, size_t> matrix,
                            size_t elementsPerScale,
                            DataType dataType)
{
    const size_t totalElements = matrix.first * matrix.second;

    // 按行优先访问顺序计数：每elementsPerScale个src元素共享1个srcScale元素
    for (size_t elementIndex = 0; elementIndex < totalElements; ++elementIndex) {
        const size_t scaleIndex = elementIndex / elementsPerScale;
        const uint64_t scaleFactor = srcScale[scaleIndex];

        src[elementIndex] = CubeCalculate::EleMulScale(src[elementIndex],
                                                       scaleFactor,
                                                       dataType,
                                                       this->status);
    }
}

static void ScaleLExtract(std::vector<uint64_t> &srcScale, std::vector<uint64_t> &e6m2,
                          std::vector<uint64_t> &e8, std::vector<uint64_t> &e16)
{
    for (size_t i = 0; i < srcScale.size() / 2; ++i) {
        size_t e8Idx = i * 2;
        size_t e16Idx = i * 2 + 1;
        assert(e16Idx < srcScale.size());
        assert(e8Idx < srcScale.size());
        uint32_t scaleVal = srcScale[e8Idx];
        uint32_t scale16Val = srcScale[e16Idx];
        e16.push_back(scale16Val);
        e6m2.push_back(scaleVal & 0xffU);
        e8.push_back(((scaleVal >> 8) & 0xffU));
    }
}

static void ScaleRExtract(std::vector<uint64_t> &srcScale, std::vector<uint64_t> &e6m2,
                          std::vector<uint64_t> &e8, std::vector<uint64_t> &e16)
{
    bool fillE16 = false;
    for (size_t i = 0; i < srcScale.size(); ++i) {
        uint32_t scaleVal = srcScale[i];
        if (fillE16) {
            e16.push_back(scaleVal);
        } else {
            e6m2.push_back(scaleVal & 0xffU);
            e8.push_back(((scaleVal >> 8) & 0xffU));
        }

        if ((i + 1) % 16 == 0) {
            fillE16 = !fillE16;
        }
    }
}

void SoftCore::MatrixScaleLHiF4(std::vector<uint64_t>& src,
                                std::vector<uint64_t>& srcScale,
                                std::pair<size_t, size_t> matrix) const
{
    const size_t rows = matrix.first;
    const size_t cols = matrix.second;
    std::vector<uint64_t> e6m2;
    std::vector<uint64_t> e8;
    std::vector<uint64_t> e16;
    ScaleLExtract(srcScale, e6m2, e8, e16);

    std::vector<uint64_t> dst(src.size());
    float_status fpstatus;
    memset(&fpstatus, 0, sizeof(float_status));

    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            size_t i = r * cols + c;
            uint64_t ea = e6m2[i / 64];
            uint64_t eb = (e8[i / 64] >> (i % 8)) & 1;
            uint64_t ec = (e16[i / 64] >> (i % 16)) & 1;
            dst[i] = CubeCalculate::GetElementValue(src[i], ea, eb, ec, &fpstatus);
        }
    }
    src = dst;
}

void SoftCore::MatrixScaleRHiF4(std::vector<uint64_t>& src,
                                std::vector<uint64_t>& srcScale,
                                std::pair<size_t, size_t> matrix) const
{
    const size_t rows = matrix.first;
    const size_t cols = matrix.second;
    std::vector<uint64_t> e6m2;
    std::vector<uint64_t> e8;
    std::vector<uint64_t> e16;
    ScaleRExtract(srcScale, e6m2, e8, e16);

    std::vector<uint64_t> dst(src.size());
    float_status fpstatus;
    memset(&fpstatus, 0, sizeof(float_status));

    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            size_t i = r * cols + c;
            uint64_t ea = e6m2[i / 64];
            uint64_t eb = (e8[i / 64] >> (i % 8)) & 1;
            uint64_t ec = (e16[i / 64] >> (i % 16)) & 1;
            dst[i] = CubeCalculate::GetElementValue(src[i], ea, eb, ec, &fpstatus);
        }
    }
    src = dst;
}

std::vector<uint64_t> SoftCore::LoadFromTileRegister(DataType dataType, size_t m, size_t n, uint64_t tileBaseAddr,
                                                     FractalType type, uint64_t threadId)
{
    // TODO: fix r0 or c0
    size_t row = 0;
    size_t column = 0;
    double eleSize = CubeCalculate::EleRealSize(dataType);
    switch (type) {
        case FractalType::RD:
            return LoadFromTileRegisterRD(dataType, m, n, tileBaseAddr, threadId);
        case FractalType::CD:
            return LoadFromTileRegisterCD(dataType, m, n, tileBaseAddr, threadId);
        case FractalType::NN:
            // only for scaled
            row = (dataType == DataType::FP8 ? 2 : 1);
            column = 16;
            return LoadFromTileRegisterNn(dataType, {m, n}, {row, column}, tileBaseAddr, threadId);
        case FractalType::NZ:
            row = FIXED_ROW_COLUMN;
            column = FIXED_MATRIX_SIZE / static_cast<size_t>((static_cast<double>(row) * eleSize));
            return LoadFromTileRegisterNz(dataType, {m, n}, {row, column}, tileBaseAddr, threadId);
        case FractalType::ZZ:
            // only for scaled
            row = 16;
            column = (dataType == DataType::FP8 ? 2 : 1);
            return LoadFromTileRegisterZz(dataType, {m, n}, {row, column}, tileBaseAddr, threadId);
        case FractalType::ZN:
            column = FIXED_ROW_COLUMN;
            row = FIXED_MATRIX_SIZE / static_cast<size_t>(static_cast<double>(column) * eleSize);
            return LoadFromTileRegisterZn(dataType, {m, n}, {row, column}, tileBaseAddr, threadId);
        default:
            std::cerr << "Unknown Tile Load type, abort\n";
            assert(0);
    }
    return {};
}

void SoftCore::AccToTileRegister(const std::vector<uint64_t> &matrixData, std::pair<size_t, size_t> matrixShape,
                                 DataType dataType, uint64_t tileBaseAddr, LayOut tileConvertType, bool canon,
                                 uint64_t thread)
{
    constexpr size_t accColForCanon = 8;
    std::vector<uint64_t> finalVec = matrixData;
    size_t tile_row = FIXED_ROW_COLUMN;
    size_t tile_column = canon ? accColForCanon : FIXED_ROW_COLUMN;
    switch (tileConvertType) {
        // 默认情况是由RD转为Nz
        case LayOut::NORM:
            CubeCalculate::SwitchRD2NZ(finalVec, tile_row, tile_column, matrixShape.first, matrixShape.second);
            break;
        case LayOut::NZ2ND:
            break;
        case LayOut::NZ2DN:
            CubeCalculate::SwitchRD2CD(finalVec, matrixShape.first, matrixShape.second);
            break;
        default:
            std::cerr<<"Unknown Tile store convert type\n";
            assert(0);
            break;
    }

    std::map<size_t, uint64_t> visited;
    size_t eleSize = CubeCalculate::EleSize(dataType);
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout<<"Store From Acc to Tile:" << std::endl;
    }
    for (size_t i = 0; i < matrixShape.first; ++i) {
        for (size_t j = 0; j < matrixShape.second; ++j) {
            size_t offset = i * matrixShape.second + j;
            uint64_t addr = CubeCalculate::EleOffset(tileBaseAddr, offset, dataType);
            uint64_t data = CubeCalculate::EleDataMerge(finalVec[offset], offset, dataType);
            if (visited.count(addr) != 0) {
                data |= visited[addr];
            } else {
                visited.insert({addr, data});
            }
            threadStatus[thread].archStatus.tileReg.Store(addr, 0, eleSize, data);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "0x" << std::right << std::setw(8) << std::setfill('0')
                          << std::hex << static_cast<uint32_t>(data & 0xffffffff) << " ";
            }
        }
        if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
            std::cout<<std::endl;
        }
    }
}


void SoftCore::RowMax(std::vector<uint64_t>& dst, uint64_t rowMaxBase,
                      DataType dType, std::pair<size_t, size_t> matShape, uint64_t thread)
{
    std::vector<uint64_t> rowmax(matShape.first, 0);
    uint64_t maxVal = 0;
    for (size_t i = 0; i < matShape.first; i++) {
        maxVal = dst[i * matShape.second];
        for (size_t j = 1; j < matShape.second; j++) {
            maxVal = CubeCalculate::EleMax(maxVal, dst[i * matShape.second + j], dType, this->status);
        }
        rowmax[i] = maxVal;
    }

    std::map<size_t, uint64_t> visited;
    size_t eleSize = CubeCalculate::EleSize(dType);
    for (size_t i = 0; i < rowmax.size(); i++) {
        uint64_t addr = CubeCalculate::EleOffset(rowMaxBase, i, dType);
        uint64_t data = CubeCalculate::EleDataMerge(rowmax[i], i, dType);
        if (visited.count(addr) != 0) {
            data |= visited[addr];
        } else {
            visited.insert({addr, data});
        }
        threadStatus[thread].archStatus.tileReg.Store(addr, 0, eleSize, data);
    }
}

void SoftCore::WriteToTileRegister(const std::vector<uint64_t> &matrixData, std::pair<size_t, size_t> matrixShape,
                                   DataType dataType, uint64_t tileBaseAddr, FractalType storeType, uint64_t thread)
{
    std::vector<uint64_t> finalVec = matrixData;
    size_t tile_row = 0;
    size_t tile_column = 0;
    double eleRSize = CubeCalculate::EleRealSize(dataType);
    switch (storeType) {
        // 默认情况是由RD转为Nz
        case FractalType::NZ:
            tile_row = FIXED_ROW_COLUMN;
            tile_column = FIXED_MATRIX_SIZE / static_cast<uint64_t>(static_cast<double>(tile_row) * eleRSize);
            CubeCalculate::SwitchRD2NZ(finalVec, tile_row, tile_column, matrixShape.first, matrixShape.second);
            break;
        case FractalType::ZN:
            tile_column = FIXED_ROW_COLUMN;
            tile_row = FIXED_MATRIX_SIZE / static_cast<uint64_t>(static_cast<double>(tile_column) * eleRSize);
            CubeCalculate::SwitchRD2ZN(finalVec, tile_row, tile_column, matrixShape.first, matrixShape.second);
            break;
        case FractalType::NN:
            CubeCalculate::SwitchRD2NN(finalVec, tile_row, tile_column, matrixShape.first, matrixShape.second);
            break;
        case FractalType::ZZ:
            CubeCalculate::SwitchRD2ZZ(finalVec, tile_row, tile_column, matrixShape.first, matrixShape.second);
            break;
        case FractalType::RD:
            break;
        case FractalType::CD:
            CubeCalculate::SwitchRD2CD(finalVec, matrixShape.first, matrixShape.second);
            break;
        default:
            std::cerr<<"Unknown Tile store convert type\n";
            assert(0);
    }

    std::map<size_t, uint64_t> visited;
    // 存到tileReg里面
    size_t eleSize = CubeCalculate::EleSize(dataType);
    for (size_t i = 0; i < matrixShape.first; ++i) {
        for (size_t j = 0; j < matrixShape.second; ++j) {
            size_t offset = i * matrixShape.second + j;
            uint64_t addr = CubeCalculate::EleOffset(tileBaseAddr, offset, dataType);
            uint64_t data = CubeCalculate::EleDataMerge(finalVec[offset], offset, dataType);
            if (visited.count(addr) != 0) {
                data |= visited[addr];
            } else {
                visited.insert({addr, data});
            }
            threadStatus[thread].archStatus.tileReg.Store(addr, 0, eleSize, data);
        }
    }
}

void SoftCore::WriteToAcc(const std::vector<uint64_t>& matrixData, CubeMatShape shape, uint64_t thread)
{
    // 无需转型
    // 因ACC放入TileReg, 故需存到tileReg里面
    uint64_t tileBaseAddr = threadStatus[thread].archStatus.tileReg.tileReg[OperandType::OPD_TILE_ACC].startAddr_;
    DataType dataType = shape.dstType;
    size_t eleSize = CubeCalculate::EleSize(dataType);

    std::map<size_t, uint64_t> visited;
    for (size_t i = 0; i < shape.externM; i++) {
        for (size_t j = 0; j < shape.externN; ++j) {
            if (i >= shape.m || j >= shape.n) {
                continue;
            }
            uint64_t offset = i * shape.n + j;
            uint64_t addr = CubeCalculate::EleOffset(tileBaseAddr, offset, dataType);
            uint64_t data = CubeCalculate::EleDataMerge(matrixData[offset], offset, dataType);
            if (visited.count(addr) != 0) {
                data |= visited[addr];
            } else {
                visited.insert({addr, data});
            }
            // offset已在EleOffset中计算完毕
            threadStatus[thread].archStatus.tileReg.Store(addr, 0, eleSize, data);
        }
    }
    return;
}

} // namespace JCore
