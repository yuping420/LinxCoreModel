#include <cstdint>
#include <iomanip>
#include <vector>
#include "SoftCore.h"
#include "../../isa/ISACommon/DataType.h"
#include "../../isa/calculate/TileOpCommonCalc.h"
#include "ISA.h"

namespace JCore {

// 固定矩阵行或列为16
const int FIXED_ROW_COLUMN = 16;
// 固定矩阵总大小为512B
const int FIXED_MATRIX_SIZE = 512;

void PrintVecData(std::vector<uint64_t> src, size_t col, size_t row)
{
    for (size_t i = 0; i < row; ++i) {
        for (size_t j = 0; j < col; ++j) {
            std::cout<<"0x"<<std::hex<<std::uppercase<<std::setw(16)<<src[i * col + j]<<" ";
        }
        std::cout << std::endl;
    }
}

static size_t AlignTo(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void SoftCore::ExecuteTMOV(BlockFuncPtr block, TcopyShape& tCopy)
{
    std::vector<uint64_t> src;
    uint64_t tileBaseAddr = block->srcTile[0]->baseAddr;
    uint64_t tileDstAddr = block->dstTile[0]->baseAddr;
    uint32_t offset = 0;
    uint32_t eleSize = CubeCalculate::EleSize(tCopy.dataType);
    size_t blockRow = 0;
    size_t blockCol = 0;
    src = LoadFromTileRegister(tCopy.dataType, tCopy.validRow, tCopy.validCol,
                               tileBaseAddr, FractalType::RD, block->threadId);
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Src(before switch): " << std::endl;
        CubeCalculate::PrintData(src, tCopy.validRow, tCopy.validCol);
    }
    switch (tCopy.tileConvertType) {
        case LayOut::NORM:
            break;
        case LayOut::NZ2ND:
        case LayOut::ND2NZ:
            blockRow = FIXED_ROW_COLUMN;
            blockCol = FIXED_MATRIX_SIZE / static_cast<uint64_t>((static_cast<double>(blockRow) * eleSize));
            CubeCalculate::SwitchRD2NZ(src, blockRow, blockCol, tCopy.validRow, tCopy.validCol);
            break;
        case LayOut::NZ2DN:
            blockRow = FIXED_ROW_COLUMN;
            blockCol = FIXED_MATRIX_SIZE / static_cast<uint64_t>((static_cast<double>(blockRow) * eleSize));
            CubeCalculate::SwitchRD2NZ(src, blockRow, blockCol, tCopy.validRow, tCopy.validCol);
            CubeCalculate::SwitchRD2CD(src, tCopy.validRow, tCopy.validCol);
            break;
        case LayOut::DN2NZ:
            blockRow = FIXED_ROW_COLUMN;
            blockCol = FIXED_MATRIX_SIZE / static_cast<uint64_t>((static_cast<double>(blockRow) * eleSize));
            CubeCalculate::SwitchRD2CD(src, tCopy.validRow, tCopy.validCol);
            CubeCalculate::SwitchCD2NZ(src, blockRow, blockCol, tCopy.validRow, tCopy.validCol);
            break;
        case LayOut::DN2ZN:
            blockCol = FIXED_ROW_COLUMN;
            blockRow = FIXED_MATRIX_SIZE / static_cast<uint64_t>((static_cast<double>(blockCol) * eleSize));
            CubeCalculate::SwitchRD2CD(src, tCopy.validRow, tCopy.validCol);
            CubeCalculate::SwitchCD2ZN(src, blockRow, blockCol, tCopy.validRow, tCopy.validCol);
            break;
        default:
            assert(0 && "Not supported");
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "blockRow: " << std::dec << blockRow << std::endl;
        std::cout << "blockCol: " << std::dec << blockCol << std::endl;
        std::cout << "Src(after switch): " << std::endl;
        CubeCalculate::PrintData(src, tCopy.validRow, tCopy.validCol);
    }
    PadValue paddingValue = block->blockAttr->padValue;
    uint64_t paddingData = TileOpCommonCalc::FillEle(tCopy.dataType, paddingValue);
    std::vector<uint64_t> dst(tCopy.totalRow * tCopy.totalCol, paddingData);
    for (size_t i = 0; i < tCopy.validRow; ++i) {
        for (size_t j = 0; j < tCopy.validCol; ++j) {
            dst[i * tCopy.totalCol + j] = src[i * tCopy.validCol + j];
        }
    }
    if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
        std::cout << "Src(after padding): " << std::endl;
        CubeCalculate::PrintData(src, tCopy.validRow, tCopy.validCol);
    }
    for (size_t k = 0; k < (tCopy.totalRow * tCopy.totalCol); ++k) {
        threadStatus[block->threadId].archStatus.tileReg.Store(tileDstAddr, offset, eleSize, dst[k]);
        offset += eleSize;
    }
}

void SoftCore::ExecuteTLOAD(BlockFuncPtr block, TcopyShape& tCopy)
{
    std::vector<uint64_t> tileBaseAddr(config.tileRegMaxOutputNum);
    for (size_t i = 0; i < tCopy.tileRegDstNum; ++i) {
        tileBaseAddr[i] = block->dstTile[i]->baseAddr;
    }

    uint64_t eleSize = CubeCalculate::EleSize(tCopy.dataType);
    uint64_t stride = tCopy.stride;
    PadValue paddingValue = block->blockAttr->padValue;
    uint64_t paddingData = TileOpCommonCalc::FillEle(tCopy.dataType, paddingValue);

    uint64_t dstFracSize = FIXED_MATRIX_SIZE;
    uint64_t dstFracRow = 0;
    uint64_t dstFracCol = 0;

    uint64_t srcAddrStart = tCopy.memBaseAddr;
    uint64_t dstAddrStart = tileBaseAddr[0];
    uint64_t dstRowWidth = tCopy.totalCol * eleSize;
    std::map<size_t, uint64_t> writtenData;
    uint64_t srcAddrRow = 0;
    uint64_t dstAddrRow = 0;
    uint64_t loadData = 0;
    int dstFracI = 0;
    int dstFracJ = 0;
    size_t i = 0;
    size_t j = 0;
    size_t rowOfOneTile = 0;
    size_t colOfOneTile = 0;

    auto doMemoryLoad = [&i, &j, &tCopy, &loadData, this, &paddingData, &eleSize](uint64_t memBase, uint64_t idx) {
        if (i < tCopy.validRow && j < tCopy.validCol) {
            uint64_t addr = memBase + eleSize * idx;
            loadData = this->memory->Load(addr, eleSize, true);
        } else {
            loadData = paddingData;
        }
        if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
            std::cout << "Load From Memory(Store to Tile) data: 0x" << std::hex << loadData << std::endl;
        }
    };

    auto doTileStore = [&block, this, &eleSize](uint64_t tileBase, uint64_t lineOffset,
                                                              uint64_t idx, uint64_t data) {
        uint64_t addr = tileBase + lineOffset + eleSize * idx;
        if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
            std::cout << "Store to Tile data: 0x" << std::hex << data << std::endl;
        }
        this->threadStatus[block->threadId].archStatus.tileReg.Store(addr, 0, eleSize, data);
    };

    switch (tCopy.tileConvertType) { // TODO: There is a problem with the multi-output logic of TLOAD
        case LayOut::NORM:
            stride = (stride == 0) ? (tCopy.validCol * eleSize) : stride;
            tCopy.totalRow = tCopy.tileTotalSize / (tCopy.totalCol * eleSize);
            assert(tCopy.totalRow >= tCopy.validRow && "Row need >= RowValid, maybe tileSize is too small");
            rowOfOneTile = tCopy.totalRow / tCopy.tileRegDstNum;
            dstRowWidth = tCopy.totalCol * eleSize;
            for (i = 0; i < tCopy.totalRow; i++) {
                srcAddrRow = srcAddrStart + i * stride;
                dstAddrStart = tileBaseAddr[i / rowOfOneTile];
                dstAddrRow = (i % rowOfOneTile) * dstRowWidth;
                for (j = 0; j < tCopy.totalCol; j++) {
                    doMemoryLoad(srcAddrRow, j);
                    doTileStore(dstAddrStart, dstAddrRow, j, loadData);
                }
            }
            break;
        case LayOut::ND2NZ:
            stride = (stride == 0) ? (tCopy.validCol * eleSize) : stride;
            dstFracRow = FIXED_ROW_COLUMN;
            dstFracCol = dstFracSize / (dstFracRow * eleSize);
            tCopy.totalCol = AlignTo(tCopy.totalCol, dstFracCol);
            tCopy.totalRow = tCopy.tileTotalSize / (tCopy.totalCol * eleSize);
            assert(tCopy.totalRow >= tCopy.validRow && "Row need >= RowValid, maybe tileSize is too small");
            colOfOneTile = tCopy.totalCol / tCopy.tileRegDstNum;
            for (i = 0; i < tCopy.totalRow; i++) {
                srcAddrRow = srcAddrStart + i * stride;
                for (j = 0; j < tCopy.totalCol; j++) {
                    doMemoryLoad(srcAddrRow, j);
                    dstAddrStart = tileBaseAddr[j / colOfOneTile];
                    dstFracJ = (j % colOfOneTile) / dstFracCol;
                    doTileStore(dstAddrStart, 0, (dstFracJ * tCopy.totalRow *
                                                  dstFracCol + i * dstFracCol + j % dstFracCol), loadData);
                }
            }
            break;
        case LayOut::ND2ZN:
            stride = (stride == 0) ? (tCopy.validCol * eleSize) : stride;
            dstFracCol = FIXED_ROW_COLUMN;
            dstFracRow = dstFracSize / (dstFracCol * eleSize);
            tCopy.totalCol = AlignTo(tCopy.totalCol, dstFracCol);
            tCopy.totalRow = tCopy.tileTotalSize / (tCopy.totalCol * eleSize);
            assert(tCopy.totalRow >= tCopy.validRow && "Row need >= RowValid, maybe tileSize is too small");
            rowOfOneTile = tCopy.totalRow / tCopy.tileRegDstNum;
            for (i = 0; i < tCopy.totalRow; i++) {
                srcAddrRow = srcAddrStart + i * stride;
                dstAddrStart = tileBaseAddr[i / rowOfOneTile];
                dstFracI = (i % rowOfOneTile) / dstFracRow;
                for (j = 0; j < tCopy.totalCol; j++) {
                    doMemoryLoad(srcAddrRow, j);
                    doTileStore(dstAddrStart, 0, (dstFracI * tCopy.totalCol * dstFracRow +
                                                  j * dstFracRow + i % dstFracRow), loadData);
                }
            }
            break;
        case LayOut::DN2NZ:
            stride = (stride == 0) ? (tCopy.validRow * eleSize) : stride;
            dstFracRow = FIXED_ROW_COLUMN;
            dstFracCol = dstFracSize / (dstFracRow * eleSize);
            tCopy.totalCol = AlignTo(tCopy.totalCol, dstFracCol);
            tCopy.totalRow = tCopy.tileTotalSize / (tCopy.totalCol * eleSize);
            assert(tCopy.totalRow >= tCopy.validRow && "Row need >= RowValid, maybe tileSize is too small");
            colOfOneTile = tCopy.totalCol / tCopy.tileRegDstNum;
            for (i = 0; i < tCopy.totalRow; i++) {
                for (j = 0; j < tCopy.totalCol; j++) {
                    srcAddrRow = srcAddrStart + j * stride;
                    doMemoryLoad(srcAddrRow, i);
                    dstAddrStart = tileBaseAddr[j / colOfOneTile];
                    dstFracJ = (j % colOfOneTile) / dstFracCol;
                    doTileStore(dstAddrStart, 0, (dstFracJ * tCopy.totalRow *
                                                  dstFracCol + i * dstFracCol + j % dstFracCol), loadData);
                }
            }
            break;
        case LayOut::DN2ZN:
            stride = (stride == 0) ? (tCopy.validRow * eleSize) : stride;
            dstFracCol = FIXED_ROW_COLUMN;
            dstFracRow = dstFracSize / (dstFracCol * eleSize);
            tCopy.totalCol = AlignTo(tCopy.totalCol, dstFracCol);
            tCopy.totalRow = tCopy.tileTotalSize / (tCopy.totalCol * eleSize);
            assert(tCopy.totalRow >= tCopy.validRow && "Row need >= RowValid, maybe tileSize is too small");
            rowOfOneTile = tCopy.totalRow / tCopy.tileRegDstNum;
            for (i = 0; i < tCopy.totalRow; i++) {
                dstAddrStart = tileBaseAddr[i / rowOfOneTile];
                dstFracI = (i % rowOfOneTile) / dstFracRow;
                for (j = 0; j < tCopy.totalCol; j++) {
                    srcAddrRow = srcAddrStart + j * stride;
                    doMemoryLoad(srcAddrRow, i);
                    doTileStore(dstAddrStart, 0, dstFracI * tCopy.totalCol *
                                                 dstFracRow + j * dstFracRow + i % dstFracRow, loadData);
                }
            }
            break;
        default:
            // assert(0 && "TLOAD does not currently support other types of data transfer");
            break;
    }

    for (size_t counter = 0; counter < tCopy.tileRegDstNum; ++counter) {

    }
    return;
}

void SoftCore::ExecuteTSTORE(BlockFuncPtr block, TcopyShape& tCopy)
{
    std::vector<uint64_t> tileBaseAddr(config.tileRegMaxInputNum);
    for (size_t i = 0; i < tCopy.tileRegSrcNum; ++i) {
        tileBaseAddr[i] = block->srcTile[i]->baseAddr;
    }
    uint64_t eleSize = CubeCalculate::EleSize(tCopy.dataType);
    double eleRSize = CubeCalculate::EleRealSize(tCopy.dataType);
    uint64_t stride = tCopy.stride;

    uint64_t srcFracSize = FIXED_MATRIX_SIZE;
    uint64_t srcFracRow = 0;
    uint64_t srcFracCol = 0;

    uint64_t srcAddrStart = tileBaseAddr[0];
    uint64_t dstAddrStart = tCopy.memBaseAddr;
    uint64_t srcRowWidth = tCopy.totalCol * eleSize;
    uint64_t srcAddrRow = 0;
    uint64_t dstAddrRow = 0;
    uint64_t loadData = 0;
    int srcFracI = 0;
    int srcFracJ = 0;
    size_t rowOfOneTile = 0;
    size_t colOfOneTile = 0;
    std::map<size_t, uint64_t> writtenData;

    auto loadFromTile = [this, &block, &tCopy, &eleSize](uint64_t baseAddr, uint64_t idx) {
        uint64_t addr = CubeCalculate::EleOffset(baseAddr, idx, tCopy.dataType);
        uint64_t data = this->threadStatus[block->threadId].archStatus.tileReg.Load(addr, 0, eleSize);
        return CubeCalculate::EleDataExtract(data, idx, tCopy.dataType);
    };

    auto storeToMemory = [this, &tCopy, &eleSize, &writtenData](uint64_t memBase, uint64_t idx, uint64_t data) {
        uint64_t addr = CubeCalculate::EleOffset(memBase, idx, tCopy.dataType);
        data = CubeCalculate::EleDataMerge(data, idx, tCopy.dataType);
        if (writtenData.count(addr) != 0) {
            data |= writtenData[addr];
        } else {
            writtenData.insert({addr, data});
        }
        this->memory->Store(addr, data, eleSize);
        if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
            std::cout << "Load From Tile(Store to Mem) data: 0x" << std::hex << data << std::endl;
        }
    };

    switch (tCopy.tileConvertType) { // TODO: TSTORE has a problem with its multi-input logic
        case LayOut::NORM:
            stride = (stride == 0) ? (tCopy.validCol * eleRSize) : stride;
            srcRowWidth = tCopy.totalCol * eleSize;
            rowOfOneTile = tCopy.totalRow / tCopy.tileRegSrcNum;
            for (size_t i = 0; i < tCopy.validRow; i++) {
                srcAddrStart = tileBaseAddr[i / rowOfOneTile];
                srcAddrRow = (i % rowOfOneTile) * srcRowWidth;
                dstAddrRow = dstAddrStart + i * stride;
                for (size_t j = 0; j < tCopy.validCol; j++) {
                    loadData = loadFromTile(srcAddrStart + srcAddrRow, j);
                    storeToMemory(dstAddrRow, j, loadData);
                }
            }
            break;
        case LayOut::NZ2ND:
            stride = (stride == 0) ? (tCopy.validCol * eleRSize) : stride;
            srcFracRow = FIXED_ROW_COLUMN;
            srcFracCol = srcFracSize / (srcFracRow * eleSize);
            tCopy.totalCol = AlignTo(tCopy.totalCol, srcFracCol);
            tCopy.totalRow = tCopy.tileTotalSize / (tCopy.totalCol * eleSize);
            assert(tCopy.totalRow >= tCopy.validRow && "Row need >= RowValid, maybe tileSize is too small");
            colOfOneTile = tCopy.totalCol / tCopy.tileRegSrcNum;
            for (size_t i = 0; i < tCopy.validRow; i++) {
                dstAddrRow = dstAddrStart + i * stride;
                for (size_t j = 0; j < tCopy.validCol; j++) {
                    srcAddrStart = tileBaseAddr[j / colOfOneTile];
                    srcFracJ = (j % colOfOneTile) / srcFracCol;
                    loadData = loadFromTile(srcAddrStart, (srcFracJ * tCopy.totalRow * srcFracCol +
                                                           i * srcFracCol + j % srcFracCol));
                    storeToMemory(dstAddrRow, j, loadData);
                }
            }
            break;
        case LayOut::ZN2ND:
            stride = (stride == 0) ? (tCopy.validCol * eleRSize) : stride;
            srcFracCol = FIXED_ROW_COLUMN;
            srcFracRow = srcFracSize / (srcFracCol * eleSize);
            tCopy.totalCol = AlignTo(tCopy.totalCol, srcFracCol);
            tCopy.totalRow = tCopy.tileTotalSize / (tCopy.totalCol * eleSize);
            assert(tCopy.totalRow >= tCopy.validRow && "Row need >= RowValid, maybe tileSize is too small");
            rowOfOneTile = tCopy.totalRow / tCopy.tileRegSrcNum;
            for (size_t i = 0; i < tCopy.validRow; i++) {
                dstAddrRow = dstAddrStart + i * stride;
                srcAddrStart = tileBaseAddr[i / rowOfOneTile];
                srcFracI = (i % rowOfOneTile) / srcFracRow;
                for (size_t j = 0; j < tCopy.validCol; j++) {
                    loadData = loadFromTile(srcAddrStart, srcFracI * tCopy.totalCol * srcFracRow +
                                            j * srcFracRow + i % srcFracRow);
                    storeToMemory(dstAddrRow, j, loadData);
                }
            }
            break;
        default:
            assert(0 && "TSTORE does not currently support other types of data transfer");
    }

    for (size_t counter = 0; counter < tCopy.tileRegSrcNum; ++counter) {
    }
    return;
}

void SoftCore::ExecuteMGATHER(BlockFuncPtr block)
{
    size_t totalRow = block->lb1;
    size_t totalCol = block->lb0;
    DataType dataType = block->dataType;
    uint64_t eleSize = CubeCalculate::EleSize(dataType);
    uint64_t eleSizeOffset = sizeof(uint32_t);
    uint64_t memBaseAddr = block->srcData[0];
    uint64_t tileBaseAddr = block->srcTile[0]->baseAddr;
    uint64_t tileDstAddr = block->dstTile[0]->baseAddr;
    uint64_t offset = 0;
    uint64_t data = 0;

    for (size_t i = 0; i < totalRow; ++i) {
        for (size_t j = 0; j < totalCol; ++j) {
            offset = threadStatus[block->threadId].archStatus.tileReg.Load(tileBaseAddr,
                                                                           (i * totalCol + j) * eleSizeOffset,
                                                                           eleSizeOffset);
            data = memory->Load(memBaseAddr + offset, eleSize, true);
            threadStatus[block->threadId].archStatus.tileReg.Store(tileDstAddr, (i * totalCol + j) * eleSize,
                                                                   eleSize, data);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "Load From Memory(Store to Tile) data: 0x" << std::hex << data
                          << " read addr:0x" << memBaseAddr + offset << std::endl;
            }
        }
    }
}

void SoftCore::ExecuteMSCATTER(BlockFuncPtr block)
{
    size_t totalRow = block->lb1;
    size_t totalCol = block->lb0;
    DataType dataType = block->dataType;
    uint64_t eleSize = CubeCalculate::EleSize(dataType);
    uint64_t eleSizeOffset = sizeof(uint32_t);
    uint64_t tileBaseAddr = block->srcTile[0]->baseAddr;
    uint64_t tileBaseAddrOffset = block->srcTile[1]->baseAddr;
    uint64_t memBaseAddr = block->srcData[0];
    uint64_t offset = 0;
    uint64_t data = 0;

    for (size_t i = 0; i < totalRow; ++i) {
        for (size_t j = 0; j < totalCol; ++j) {
            offset = threadStatus[block->threadId].archStatus.tileReg.Load(tileBaseAddrOffset,
                                                                           (i * totalCol + j) * eleSizeOffset,
                                                                           eleSizeOffset);
            data = threadStatus[block->threadId].archStatus.tileReg.Load(tileBaseAddr, offset,
                                                                         eleSize);
            memory->Store(memBaseAddr, data, eleSize);
            if (verbose && LoggerManager::GetManager().level <= LoggerLevel::DETAIL) {
                std::cout << "Load From Tile(Store to Mem) data: 0x" << std::hex << data << std::endl;
            }
        }
    }
}

void SoftCore::ExecuteTMA(BlockFuncPtr block)
{
    TcopyShape tCopy(block->lb1, block->lb0);
    switch (block->tileOp) {
        case TileOp::TMOV:
            tCopy.dataType = block->dataType;
            if (block->dataType == DataType::INVALID) {
                // todo: Why need a uint8
                tCopy.dataType = DataType::UINT8;
            }
            // FIXME: This is a hack for a block without a blockAttr, may need to clarify this.
            if (!block->blockAttr) {
                block->blockAttr = std::make_shared<BlockAttribute>();
            }
            tCopy.tileConvertType = block->blockAttr->layout;
            // 缺省时默认
            tCopy.totalCol = (block->lb2 == 1) ? block->lb0 : block->lb2;
            tCopy.totalRow = block->srcTile[0]->size / (tCopy.totalCol * CubeCalculate::EleSize(tCopy.dataType));
            if (block->lb0 == 1 && block->lb1 == 1) {
                /* TCopy Impl Max Tile Size = 128 * 256 * 8 = 256KB */
                tCopy.validRow = tCopy.totalRow = 128;
                tCopy.validCol = tCopy.totalCol = 256;
                tCopy.dataType = DataType::UINT64;
            }
            ExecuteTMOV(block, tCopy);
            break;
        case TileOp::TLOAD:
            tCopy.dataType = block->dataType;
            tCopy.memBaseAddr = block->srcData[0];
            // 若为连续地址, 则缺省
            if (block->srcData.size() > 1) {
                tCopy.stride = block->srcData[1];
            }
            tCopy.tileRegDstNum = block->dstTile.size();
            tCopy.tileTotalSize = block->dstTile[0]->size * block->dstTile.size();  // 新版本有多输出机制, 当前的旧版本不合适
            // lb2缺省时默认
            if (block->lb2 == 1) {
                tCopy.totalCol = block->lb0;
            } else {
                tCopy.totalCol = block->lb2;
            }
            // TODO: The row here might be the reason for data validation misalignment
            // (which could not be derived from hardware)
            tCopy.totalRow = tCopy.validRow;
            tCopy.tileConvertType = block->blockAttr->layout;
            ExecuteTLOAD(block, tCopy);
            break;
        case TileOp::TSTORE:
            tCopy.dataType = block->dataType;
            tCopy.memBaseAddr = block->srcData[0];
            // 若为连续地址, 则缺省
            if (block->srcData.size() > 1) {
                tCopy.stride = block->srcData[1];
            }
            tCopy.tileRegSrcNum = block->srcTile.size();
            tCopy.tileTotalSize = block->srcTile[0]->size * block->srcTile.size();
            // lb2缺省时默认
            if (block->lb2 == 1) {
                tCopy.totalCol = block->lb0;
            } else {
                tCopy.totalCol = block->lb2;
            }
            // TODO: The row here might be the reason for data validation misalignment
            // (which could not be derived from hardware)
            tCopy.totalRow = tCopy.validRow;
            tCopy.tileConvertType = block->blockAttr->layout;
            ExecuteTSTORE(block, tCopy);
            break;
        case TileOp::MGATHER:
            ExecuteMGATHER(block);
            break;
        case TileOp::MSCATTER:
            ExecuteMSCATTER(block);
            break;
        default:
            break;
    }
}



} // namespace JCore
