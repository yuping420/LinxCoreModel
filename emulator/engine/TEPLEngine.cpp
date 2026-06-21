#include <cassert>

#include "SoftCore.h"
#include "ISA.h"
#include "../../isa/calculate/TileOpCommonCalc.h"
#include "../../isa/calculate/CubeCalculate.h"
#include "../../isa/ISACommon/DataType.h"

namespace JCore {

void SoftCore::ExecuteTADD(BlockFuncPtr block,
                           std::pair<size_t, size_t> totalMatrix,
                           std::pair<size_t, size_t> validMatrix,
                           DataType dataType)
{
    std::vector<uint64_t> srcL;
    std::vector<uint64_t> srcR;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    srcL = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[0]->baseAddr,
                                FractalType::RD, block->threadId);
    srcR = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[1]->baseAddr,
                                FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = CubeCalculate::EleAdd(srcL[i * validMatrix.second + j],
                                                                    srcR[i * validMatrix.second + j],
                                                                    dataType, this->status);
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "SrcL: " << std::endl;
        CubeCalculate::PrintData(srcL, validMatrix.first, validMatrix.second);
        std::cout << "SrcR: " << std::endl;
        CubeCalculate::PrintData(srcR, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTSUB(BlockFuncPtr block,
                           std::pair<size_t, size_t> totalMatrix,
                           std::pair<size_t, size_t> validMatrix,
                           DataType dataType)
{
    std::vector<uint64_t> srcL;
    std::vector<uint64_t> srcR;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    srcL = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[0]->baseAddr,
                                FractalType::RD, block->threadId);
    srcR = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[1]->baseAddr,
                                FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = CubeCalculate::EleSub(srcL[i * validMatrix.second + j],
                                                                    srcR[i * validMatrix.second + j],
                                                                    dataType, this->status);
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "SrcL: " << std::endl;
        CubeCalculate::PrintData(srcL, validMatrix.first, validMatrix.second);
        std::cout << "SrcR: " << std::endl;
        CubeCalculate::PrintData(srcR, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTMUL(BlockFuncPtr block,
                           std::pair<size_t, size_t> totalMatrix,
                           std::pair<size_t, size_t> validMatrix,
                           DataType dataType)
{
    std::vector<uint64_t> srcL;
    std::vector<uint64_t> srcR;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    srcL = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[0]->baseAddr,
                                FractalType::RD, block->threadId);
    srcR = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[1]->baseAddr,
                                FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = CubeCalculate::EleMul(srcL[i * validMatrix.second + j],
                                                                    srcR[i * validMatrix.second + j],
                                                                    dataType, this->status);
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "SrcL: " << std::endl;
        CubeCalculate::PrintData(srcL, validMatrix.first, validMatrix.second);
        std::cout << "SrcR: " << std::endl;
        CubeCalculate::PrintData(srcR, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTMAX(BlockFuncPtr block,
                           std::pair<size_t, size_t> totalMatrix,
                           std::pair<size_t, size_t> validMatrix,
                           DataType dataType)
{
    std::vector<uint64_t> srcL;
    std::vector<uint64_t> srcR;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    srcL = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[0]->baseAddr,
                                FractalType::RD, block->threadId);
    srcR = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[1]->baseAddr,
                                FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = CubeCalculate::EleMax(srcL[i * validMatrix.second + j],
                                                                    srcR[i * validMatrix.second + j],
                                                                    dataType, this->status);
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "SrcL: " << std::endl;
        CubeCalculate::PrintData(srcL, validMatrix.first, validMatrix.second);
        std::cout << "SrcR: " << std::endl;
        CubeCalculate::PrintData(srcR, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTEXP(BlockFuncPtr block,
                           std::pair<size_t, size_t> totalMatrix,
                           std::pair<size_t, size_t> validMatrix,
                           DataType dataType)
{
    std::vector<uint64_t> src;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    src = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                               block->srcTile[0]->baseAddr,
                               FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = TileOpCommonCalc::EleExp(src[i * validMatrix.second + j],
                                                                       dataType, this->status);
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Src: " << std::endl;
        CubeCalculate::PrintData(src, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTRECIP(BlockFuncPtr block,
                             std::pair<size_t, size_t> totalMatrix,
                             std::pair<size_t, size_t> validMatrix,
                             DataType dataType)
{
    std::vector<uint64_t> src;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    src = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                               block->srcTile[0]->baseAddr,
                               FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = TileOpCommonCalc::EleRecip(src[i * validMatrix.second + j],
                                                                         dataType);
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Src: " << std::endl;
        CubeCalculate::PrintData(src, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTMULS(BlockFuncPtr block,
                            std::pair<size_t, size_t> totalMatrix,
                            std::pair<size_t, size_t> validMatrix,
                            DataType dataType, uint64_t scalar)
{
    std::vector<uint64_t> src;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    src = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                               block->srcTile[0]->baseAddr,
                               FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = CubeCalculate::EleMul(src[i * validMatrix.second + j],
                                                                    scalar, dataType, this->status);
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Scalar Data: 0x" << std::hex << scalar << std::endl;
        std::cout << "Src: " << std::endl;
        CubeCalculate::PrintData(src, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTEXPANDS(BlockFuncPtr block,
                               std::pair<size_t, size_t> totalMatrix,
                               std::pair<size_t, size_t> validMatrix,
                               DataType dataType, uint64_t scalar)
{
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = scalar;
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Scalar Data: 0x" << std::hex << scalar << std::endl;
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTROWSUM(BlockFuncPtr block,
                              std::pair<size_t, size_t> validMatrix,
                              DataType dataType, size_t totalRow)
{
    std::vector<uint64_t> src;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalRow, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    src = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                               block->srcTile[0]->baseAddr,
                               FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        uint64_t rowSum = 0;
        for (size_t j = 0; j < validMatrix.second; ++j) {
            rowSum = CubeCalculate::EleAdd(src[i * validMatrix.second + j],
                                           rowSum, dataType, this->status);
        }
        dst[i] = rowSum;
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Src: " << std::endl;
        CubeCalculate::PrintData(src, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalRow, 1);
    }
    for (size_t k = 0; k < totalRow; ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTROWMAX(BlockFuncPtr block,
                              std::pair<size_t, size_t> validMatrix,
                              DataType dataType, size_t totalRow)
{
    std::vector<uint64_t> src;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalRow, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    src = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                               block->srcTile[0]->baseAddr,
                               FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        uint64_t rowMax = 0;
        for (size_t j = 0; j < validMatrix.second; ++j) {
            rowMax = CubeCalculate::EleMax(src[i * validMatrix.second + j],
                                           rowMax, dataType, this->status);
        }
        dst[i] = rowMax;
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Src: " << std::endl;
        CubeCalculate::PrintData(src, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalRow, 1);
    }
    for (size_t k = 0; k < totalRow; ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTCOLSUM(BlockFuncPtr block,
                              std::pair<size_t, size_t> validMatrix,
                              DataType dataType, size_t totalCol)
{
    std::vector<uint64_t> src;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalCol, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    src = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                               block->srcTile[0]->baseAddr,
                               FractalType::RD, block->threadId);
    for (size_t j = 0; j < validMatrix.second; ++j) {
        uint64_t colSum = 0;
        for (size_t i = 0; i < validMatrix.first; ++i) {
            colSum = CubeCalculate::EleAdd(src[i * validMatrix.second + j],
                                           colSum, dataType, this->status);
        }
        dst[j] = colSum;
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Src: " << std::endl;
        CubeCalculate::PrintData(src, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, 1, totalCol);
    }
    for (size_t k = 0; k < totalCol; ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTCOLMAX(BlockFuncPtr block,
                              std::pair<size_t, size_t> validMatrix,
                              DataType dataType, size_t totalCol)
{
    std::vector<uint64_t> src;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalCol, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    src = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                               block->srcTile[0]->baseAddr,
                               FractalType::RD, block->threadId);
    for (size_t j = 0; j < validMatrix.second; ++j) {
        uint64_t colSum = 0;
        for (size_t i = 0; i < validMatrix.first; ++i) {
            colSum = CubeCalculate::EleMax(src[i * validMatrix.second + j],
                                           colSum, dataType, this->status);
        }
        dst[j] = colSum;
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Src: " << std::endl;
        CubeCalculate::PrintData(src, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, 1, totalCol);
    }
    for (size_t k = 0; k < totalCol; ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTROWEXPAND(BlockFuncPtr block,
                                 std::pair<size_t, size_t> totalMatrix,
                                 std::pair<size_t, size_t> validMatrix,
                                 DataType dataType)
{
    std::vector<uint64_t> src;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    src = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                               block->srcTile[0]->baseAddr,
                               FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        size_t firstData = src[i * validMatrix.second];
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = firstData;
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Src: " << std::endl;
        CubeCalculate::PrintData(src, validMatrix.first, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTCOLEXPANDSUB(BlockFuncPtr block,
                                    std::pair<size_t, size_t> totalMatrix,
                                    std::pair<size_t, size_t> validMatrix,
                                    DataType dataType)
{
    std::vector<uint64_t> srcL;
    std::vector<uint64_t> srcR;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    srcL = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[0]->baseAddr,
                                FractalType::RD, block->threadId);
    srcR = LoadFromTileRegister(dataType, 1, validMatrix.second,
                                block->srcTile[1]->baseAddr,
                                FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = CubeCalculate::EleSub(srcL[i * validMatrix.second + j],
                                                                    srcR[j],
                                                                    dataType, this->status);
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "SrcL: " << std::endl;
        CubeCalculate::PrintData(srcL, validMatrix.first, validMatrix.second);
        std::cout << "SrcR: " << std::endl;
        CubeCalculate::PrintData(srcR, 1, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTCOLEXPANDMUL(BlockFuncPtr block,
                                    std::pair<size_t, size_t> totalMatrix,
                                    std::pair<size_t, size_t> validMatrix,
                                    DataType dataType)
{
    std::vector<uint64_t> srcL;
    std::vector<uint64_t> srcR;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dataType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dataType);

    srcL = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                                block->srcTile[0]->baseAddr,
                                FractalType::RD, block->threadId);
    srcR = LoadFromTileRegister(dataType, 1, validMatrix.second,
                                block->srcTile[1]->baseAddr,
                                FractalType::RD, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = CubeCalculate::EleMul(srcL[i * validMatrix.second + j],
                                                                    srcR[j],
                                                                    dataType, this->status);
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "SrcL: " << std::endl;
        CubeCalculate::PrintData(srcL, validMatrix.first, validMatrix.second);
        std::cout << "SrcR: " << std::endl;
        CubeCalculate::PrintData(srcR, 1, validMatrix.second);
        std::cout << "Dst: " << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    for (size_t k = 0; k < (totalMatrix.first * totalMatrix.second); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTCVT(BlockFuncPtr block,
                           std::pair<size_t, size_t> totalMatrix,
                           std::pair<size_t, size_t> validMatrix,
                           DataType dataType,
                           DataType dstType)
{
    std::vector<uint64_t> src;
    uint64_t paddingValue = TileOpCommonCalc::FillEle(dstType, block->blockAttr->padValue);
    std::vector<uint64_t> dst(totalMatrix.first * totalMatrix.second, paddingValue);
    src = LoadFromTileRegister(dataType, validMatrix.first, validMatrix.second,
                               block->srcTile[0]->baseAddr,
                               FractalType::RD, block->threadId);
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "TCVT Before dataType conversion:" << std::endl;
        CubeCalculate::PrintData(src, validMatrix.first, validMatrix.second);
    }
    DataFormatCvt(src, dataType, dstType, block->threadId);
    for (size_t i = 0; i < validMatrix.first; ++i) {
        for (size_t j = 0; j < validMatrix.second; ++j) {
            dst[i * totalMatrix.second + j] = src[i * validMatrix.second + j];
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "TCVT After dataType conversion and Padding Value:" << std::endl;
        CubeCalculate::PrintData(dst, totalMatrix.first, totalMatrix.second);
    }
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(dstType);
    for (size_t k = 0; k < dst.size(); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(block->dstTile[0]->baseAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}


void SoftCore::ExecuteTEPL(BlockFuncPtr block)
{
    size_t validRow = 0;
    size_t validCol = 0;
    size_t totalRow = 0;
    size_t totalCol = 0;
    uint64_t scalar = 0;
    DataType dataType = block->dataType;
    DataType dstType = block->blockAttr->dataType;

    switch (block->tileOp) {
        case TileOp::TADD:          // 两个Tile的逐元素加法
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTADD(block, {totalRow, totalCol}, {validRow, validCol}, dataType);
            break;
        case TileOp::TSUB:          // 两个Tile的逐元素减法
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTSUB(block, {totalRow, totalCol}, {validRow, validCol}, dataType);
            break;
        case TileOp::TMUL:          // 两个Tile的逐元素乘法
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTMUL(block, {totalRow, totalCol}, {validRow, validCol}, dataType);
            break;
        case TileOp::TMAX:          // 两个tile的逐元素最大值
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTMAX(block, {totalRow, totalCol}, {validRow, validCol}, dataType);
            break;
        case TileOp::TEXP:          // 单个Tile的逐元素指数运算
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTEXP(block, {totalRow, totalCol}, {validRow, validCol}, dataType);
            break;
        case TileOp::TRECIP:        // 单个Tile的逐元素倒数
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTRECIP(block, {totalRow, totalCol}, {validRow, validCol}, dataType);
            break;
        case TileOp::TMULS:         // Tile与标量的主元素乘法
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            scalar = block->srcData[0];
            ExecuteTMULS(block, {totalRow, totalCol}, {validRow, validCol}, dataType, scalar);
            break;
        case TileOp::TEXPANDS:      // 将标量广播到目标Tile中
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->dstTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));     // dstTile
            scalar = block->srcData[0];
            ExecuteTEXPANDS(block, {totalRow, totalCol}, {validRow, validCol}, dataType, scalar);
            break;
        case TileOp::TROWSUM:       // 通过对列求和来归约每一行
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTROWSUM(block, {validRow, validCol}, dataType, totalRow);
            break;
        case TileOp::TROWMAX:       // 通过取列间最大值来归约每一行
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTROWMAX(block, {validRow, validCol}, dataType, totalRow);
            break;
        case TileOp::TROWEXPAND:    // 将每个源行的第一个元素广播到目标行中
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTROWEXPAND(block, {totalRow, totalCol}, {validRow, validCol}, dataType);
            break;
        case TileOp::TCOLSUM:       // 通过对行求和来归约每一列
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            ExecuteTCOLSUM(block, {validRow, validCol}, dataType, totalCol);
            break;
        case TileOp::TCOLMAX:       // 通过取行间最大值来归约每一列
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            ExecuteTCOLMAX(block, {validRow, validCol}, dataType, totalCol);
            break;
        case TileOp::TCOLEXPANDSUB: // 列广播减法：从每一列中减去一个每列标量向量
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTCOLEXPANDSUB(block, {totalRow, totalCol}, {validRow, validCol}, dataType);
            break;
        case TileOp::TCOLEXPANDMUL: // 列广播乘法：将每一列乘以一个每列标量向量
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTCOLEXPANDMUL(block, {totalRow, totalCol}, {validRow, validCol}, dataType);
            break;
        case TileOp::TCVT:
            validCol = block->lb0;
            validRow = block->lb1;
            totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            totalRow = block->srcTile[0]->size / (totalCol * CubeCalculate::EleSize(dataType));
            ExecuteTCVT(block, {totalRow, totalCol}, {validRow, validCol}, dataType, dstType);
            break;
        default:
            assert(false && "Such Tepl template is not currently supported");
    }
}
}
