#include <cmath>
#include <map>
#include <set>
#include <functional>

#include "CubeCalculate.h"

#include "ISACommon/DecodeUtiles.h"

namespace JCore {
// Transformation Part
bool CubeCalculate::SwitchRD2ZZ(std::vector<uint64_t>& matrix,
                                size_t blockRow, size_t blockCol,
                                size_t totalRow, size_t totalCol)
{
    std::vector<uint64_t> finalMatrix;
    finalMatrix.reserve(matrix.size());

    int numBlockRows = (totalRow + blockRow - 1) / blockRow;
    int numBlockCols = (totalCol + blockCol - 1) / blockCol;

    for (int tr = 0; tr < numBlockRows; ++tr) {
        for (int tc = 0; tc < numBlockCols; ++tc) {
            int startRow = tr * blockRow;
            int startCol = tc * blockCol;
            int totalElements = blockRow * blockCol;
            for (int k = 0; k < totalElements; ++k) {
                int i = k / blockRow;
                int j = k % blockRow;
                int r = startRow + i;
                int c = startCol + j;
                finalMatrix.push_back(matrix[r * totalCol + c]);
            }
        }
    }
    matrix = finalMatrix;

    return true;
}

bool CubeCalculate::SwitchRD2NN(std::vector<uint64_t>& matrix,
                                size_t blockRow, size_t blockCol,
                                size_t totalRow, size_t totalCol)
{
    std::vector<uint64_t> finalMatrix;
    finalMatrix.reserve(matrix.size());

    int numBlockRows = (totalRow + blockRow - 1) / blockRow;
    int numBlockCols = (totalCol + blockCol - 1) / blockCol;

    for (int tc = 0; tc < numBlockCols; ++tc) {
        for (int tr = 0; tr < numBlockRows; ++tr) {
            int startRow = tr * blockRow;
            int startCol = tc * blockCol;
            int totalElements = blockRow * blockCol;
            for (int k = 0; k < totalElements; ++k) {
                int j = k / blockRow;
                int i = k % blockRow;
                int r = startRow + i;
                int c = startCol + j;
                finalMatrix.push_back(matrix[r * totalCol + c]);
            }
        }
    }
    matrix = finalMatrix;

    return true;
}

bool CubeCalculate::SwitchRD2ZN(std::vector<uint64_t>& matrix,
                                size_t blockRow, size_t blockCol,
                                size_t totalRow, size_t totalCol)
{
    std::vector<uint64_t> finalMatrix;
    finalMatrix.reserve(matrix.size());

    int numBlockRows = (totalRow + blockRow - 1) / blockRow;
    int numBlockCols = (totalCol + blockCol - 1) / blockCol;

    for (int tr = 0; tr < numBlockRows; ++tr) {
        for (int tc = 0; tc < numBlockCols; ++tc) {
            int startRow = tr * blockRow;
            int startCol = tc * blockCol;
            int totalElements = blockRow * blockCol;
            for (int k = 0; k < totalElements; ++k) {
                int i = k / blockCol;
                int j = k % blockCol;
                int r = startRow + j;
                int c = startCol + i;
                finalMatrix.push_back(matrix[r * totalCol + c]);
            }
        }
    }
    matrix = finalMatrix;

    return true;
}

bool CubeCalculate::SwitchRD2NZ(std::vector<uint64_t>& matrix,
                                size_t blockRow, size_t blockCol,
                                size_t totalRow, size_t totalCol)
{
    std::vector<uint64_t> finalMatrix;
    finalMatrix.reserve(matrix.size());

    int numBlockRows = (totalRow + blockRow - 1) / blockRow;
    int numBlockCols = (totalCol + blockCol - 1) / blockCol;

    for (int tc = 0; tc < numBlockCols; ++tc) {
        for (int tr = 0; tr < numBlockRows; ++tr) {
            for (size_t k = 0; k < blockRow * blockCol; ++k) {
                int i = k / blockCol;
                int j = k % blockCol;
                size_t srcRow = tr * blockRow + i;
                size_t srcCol = tc * blockCol + j;
                size_t srcIdx = srcRow * totalCol + srcCol;
                finalMatrix.emplace_back((srcRow < totalRow && srcCol < totalCol) ? matrix[srcIdx] : 0ull);
            }
        }
    }
    matrix = finalMatrix;

    return true;
}

bool CubeCalculate::SwitchCD2NZ(std::vector<uint64_t>& matrix,
                                size_t blockRow, size_t blockCol,
                                size_t totalRow, size_t totalCol)
{
    std::vector<uint64_t> finalMatrix;
    finalMatrix.reserve(matrix.size());
    size_t resultIndex = 0;

    size_t blockRowCount = (totalRow + blockRow - 1) / blockRow;
    size_t blockColCount = (totalCol + blockCol - 1) / blockCol;

    auto processBlock = [&finalMatrix, &matrix, &totalCol, &totalRow, &blockCol, &blockRow, &resultIndex]
                        (int blockRowIdx, int blockColIdx) {
        int startRow = blockRowIdx * blockRow;
        int startCol = blockColIdx * blockCol;

        for (size_t innerRow = 0; innerRow < blockRow; ++innerRow) {
            for (size_t innerCol = 0; innerCol < blockCol; ++innerCol) {
                size_t actualRow = startRow + innerRow;
                size_t actualCol = startCol + innerCol;

                if (actualRow < totalRow && actualCol < totalCol) {
                    size_t srcIndex = actualRow + actualCol * totalRow;
                    finalMatrix[resultIndex++] = matrix[srcIndex];
                }
            }
        }
    };

    for (size_t blockColIdx = 0; blockColIdx < blockColCount; ++blockColIdx) {
        for (size_t blockRowIdx = 0; blockRowIdx < blockRowCount; ++blockRowIdx) {
            processBlock(blockRowIdx, blockColIdx);
        }
    }
    matrix = finalMatrix;
    return true;
}


bool CubeCalculate::SwitchCD2ZN(std::vector<uint64_t>& matrix,
                                size_t blockRow, size_t blockCol,
                                size_t totalRow, size_t totalCol)
{
    std::vector<uint64_t> finalMatrix;
    finalMatrix.reserve(matrix.size());
    int resultIndex = 0;

    size_t blockRowCount = totalRow / blockRow;
    size_t blockColCount = totalCol / blockCol;

    for (size_t blockI = 0; blockI < blockRowCount; ++blockI) {
        for (size_t blockJ = 0; blockJ < blockColCount; ++blockJ) {
            size_t startRow = blockI * blockRow;
            size_t startCol = blockJ * blockCol;

            auto processColumn = [&finalMatrix, &matrix, &startRow, &startCol, &totalRow, &blockRow,
                                &resultIndex] (int col) {
                for (size_t row = 0; row < blockRow; ++row) {
                    size_t actualRow = startRow + row;
                    size_t actualCol = startCol + col;
                    finalMatrix[resultIndex++] = matrix[actualCol * totalRow + actualRow];
                }
            };

            for (size_t col = 0; col < blockCol; ++col) {
                processColumn(col);
            }
        }
    }
    matrix = finalMatrix;
    return true;
}

bool CubeCalculate::SwitchRD2CD(std::vector<uint64_t>& matrix, size_t totalRow, size_t totalCol)
{
    std::vector<uint64_t> finalMatrix;
    finalMatrix.reserve(matrix.size());

    for (size_t j = 0; j < totalCol; ++j) {
        for (size_t i = 0; i < totalRow; ++i) {
            finalMatrix.emplace_back(matrix[i * totalCol + j]);
        }
    }

    matrix = finalMatrix;
    return true;
}


// Calculation Part
// left * right
uint64_t CubeCalculate::EleMul(uint64_t left, uint64_t right, DataType dataType, float_status status)
{
    uint64_t result = 0;
    const static std::map<DataType, std::function<uint64_t(uint64_t, uint64_t, float_status*)>> CALC_MAP = {
        {DataType::FP64, [](uint64_t a, uint64_t b, float_status* status) {
                return float64_mul(static_cast<float64>(a), static_cast<float64>(b), status);
            }
        },
        {DataType::FP32,  [](uint64_t a, uint64_t b, float_status* status) {
                return float32_mul(static_cast<float32>(a), static_cast<float32>(b), status);
            }
        },
        {DataType::FP16,  [](uint64_t a, uint64_t b, float_status* status) {
                return float16_mul(static_cast<float16>(a), static_cast<float16>(b), status);
            }
        },
        {DataType::BF16,  [](uint64_t a, uint64_t b, float_status* status) {
                return bfloat16_mul(static_cast<bfloat16>(a), static_cast<bfloat16>(b), status);
            }
        },
        {DataType::FP8,   [](uint64_t a, uint64_t b, float_status* status) {
                return float8_mul(static_cast<float8>(a), static_cast<float8>(b), status);
            }
        },
        {DataType::FP8_1, [](uint64_t a, uint64_t b, float_status* status) {
                return float8_1_mul(static_cast<float8_1>(a), static_cast<float8_1>(b), status);
            }
        },
        {DataType::FP4,   [](uint64_t a, uint64_t b, float_status* status) {
                return float4_mul(static_cast<float4>(a), static_cast<float4>(b), status);
            }
        },
        {DataType::FP4_1, [](uint64_t a, uint64_t b, float_status* status) {
                return float4_1_mul(static_cast<float4_1>(a), static_cast<float4_1>(b), status);
            }
        },
        {DataType::INT64, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int64_t>(a) * static_cast<int64_t>(b);
            }
        },
        {DataType::INT32, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int32_t>(a) * static_cast<int32_t>(b);
            }
        },
        {DataType::INT16, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int16_t>(a) * static_cast<int16_t>(b);
            }
        },
        {DataType::INT8, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int8_t>(a) * static_cast<int8_t>(b);
            }
        },
        {DataType::UINT64, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
            }
        },
        {DataType::UINT32, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint32_t>(a) * static_cast<uint32_t>(b);
            }
        },
        {DataType::UINT16, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint16_t>(a) * static_cast<uint16_t>(b);
            }
        },
        {DataType::UINT8, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint8_t>(a) * static_cast<uint8_t>(b);
            }
        },
    };
    auto it = CALC_MAP.find(dataType);
    if (it == CALC_MAP.end()) {
        std::cerr << "Unsupported dataType: " << BriefDataType2String(dataType) << std::endl;
        assert(0);
    }
    result = it->second(left, right, &status);
    return result;
}

static uint32_t F32ToBits(float x)
{
    uint32_t u;
    std::memcpy(&u, &x, sizeof(u));
    return u;
}

uint64_t CubeCalculate::EleMulScale(uint64_t baseValue, uint64_t scaleValue,
                                    DataType dataType, float_status status)
{
    uint64_t result = 0;
    // E8M0
    uint8_t scaleU8 = static_cast<uint8_t>(scaleValue);
    int k = static_cast<int>(scaleU8) - 127;
    float scaleF = std::ldexp(1.0F, k);
    // softfloat representation
    float32 scaleVal = F32ToBits(scaleF);
    switch (dataType) {
        case DataType::FP8: {
            float32 base = float8_to_float32(static_cast<float8>(baseValue), &status);
            result = float32_mul(base, scaleVal, &status);
            return result;
        }
        case DataType::FP4: {
            float32 base = float4_to_float32(static_cast<float4>(baseValue), &status);
            result = float32_mul(base, scaleVal, &status);
            return result;
        }
        case DataType::FP4_1: {
            float32 base = float4_1_to_float32(static_cast<float4_1>(baseValue), &status);
            result = float32_mul(base, scaleVal, &status);
            return result;
        }
        default:
            std::cerr << "Unsupported data type" << std::endl;
            assert(0);
    }
    return result;
}

// left + right
uint64_t CubeCalculate::EleAdd(uint64_t left, uint64_t right, DataType dataType, float_status status)
{
    uint64_t result = 0;
    const static std::map<DataType, std::function<uint64_t(uint64_t, uint64_t, float_status*)>> CALC_MAP = {
        {DataType::FP64, [](uint64_t a, uint64_t b, float_status* status) {
                return float64_add(static_cast<float64>(a), static_cast<float64>(b), status);
            }
        },
        {DataType::FP32,  [](uint64_t a, uint64_t b, float_status* status) {
                return float32_add(static_cast<float32>(a), static_cast<float32>(b), status);
            }
        },
        {DataType::FP16,  [](uint64_t a, uint64_t b, float_status* status) {
                return float16_add(static_cast<float16>(a), static_cast<float16>(b), status);
            }
        },
        {DataType::BF16,  [](uint64_t a, uint64_t b, float_status* status) {
                return bfloat16_add(static_cast<bfloat16>(a), static_cast<bfloat16>(b), status);
            }
        },
        {DataType::FP8,   [](uint64_t a, uint64_t b, float_status* status) {
                return float8_add(static_cast<float8>(a), static_cast<float8>(b), status);
            }
        },
        {DataType::FP8_1, [](uint64_t a, uint64_t b, float_status* status) {
                return float8_1_add(static_cast<float8_1>(a), static_cast<float8_1>(b), status);
            }
        },
        {DataType::FP4,   [](uint64_t a, uint64_t b, float_status* status) {
                return float4_add(static_cast<float4>(a), static_cast<float4>(b), status);
            }
        },
        {DataType::FP4_1, [](uint64_t a, uint64_t b, float_status* status) {
                return float4_1_add(static_cast<float4_1>(a), static_cast<float4_1>(b), status);
            }
        },
        {DataType::INT64, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int64_t>(a) + static_cast<int64_t>(b);
            }
        },
        {DataType::INT32, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int32_t>(a) + static_cast<int32_t>(b);
            }
        },
        {DataType::INT16, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int16_t>(a) + static_cast<int16_t>(b);
            }
        },
        {DataType::INT8, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int8_t>(a) + static_cast<int8_t>(b);
            }
        },
        {DataType::UINT64, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
            }
        },
        {DataType::UINT32, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint32_t>(a) + static_cast<uint32_t>(b);
            }
        },
        {DataType::UINT16, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint16_t>(a) + static_cast<uint16_t>(b);
            }
        },
        {DataType::UINT8, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint8_t>(a) + static_cast<uint8_t>(b);
            }
        },
    };
    auto it = CALC_MAP.find(dataType);
    if (it == CALC_MAP.end()) {
        std::cerr << "Unsupported dataType: " << BriefDataType2String(dataType) << std::endl;
        assert(0);
    }
    result = it->second(left, right, &status);
    return result;
}

// left - right
uint64_t CubeCalculate::EleSub(uint64_t left, uint64_t right, DataType dataType, float_status status)
{
    uint64_t result = 0;
    const static std::map<DataType, std::function<uint64_t(uint64_t, uint64_t, float_status*)>> CALC_MAP = {
        {DataType::FP64, [](uint64_t a, uint64_t b, float_status* status) {
                return float64_sub(static_cast<float64>(a), static_cast<float64>(b), status);
            }
        },
        {DataType::FP32,  [](uint64_t a, uint64_t b, float_status* status) {
                return float32_sub(static_cast<float32>(a), static_cast<float32>(b), status);
            }
        },
        {DataType::FP16,  [](uint64_t a, uint64_t b, float_status* status) {
                return float16_sub(static_cast<float16>(a), static_cast<float16>(b), status);
            }
        },
        {DataType::BF16,  [](uint64_t a, uint64_t b, float_status* status) {
                return bfloat16_sub(static_cast<bfloat16>(a), static_cast<bfloat16>(b), status);
            }
        },
        {DataType::FP8,   [](uint64_t a, uint64_t b, float_status* status) {
                return float8_sub(static_cast<float8>(a), static_cast<float8>(b), status);
            }
        },
        {DataType::FP8_1, [](uint64_t a, uint64_t b, float_status* status) {
                return float8_1_sub(static_cast<float8_1>(a), static_cast<float8_1>(b), status);
            }
        },
        {DataType::FP4,   [](uint64_t a, uint64_t b, float_status* status) {
                return float4_sub(static_cast<float4>(a), static_cast<float4>(b), status);
            }
        },
        {DataType::FP4_1, [](uint64_t a, uint64_t b, float_status* status) {
                return float4_1_sub(static_cast<float4_1>(a), static_cast<float4_1>(b), status);
            }
        },
        {DataType::INT64, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int64_t>(a) - static_cast<int64_t>(b);
            }
        },
        {DataType::INT32, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int32_t>(a) - static_cast<int32_t>(b);
            }
        },
        {DataType::INT16, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int16_t>(a) - static_cast<int16_t>(b);
            }
        },
        {DataType::INT8, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int8_t>(a) - static_cast<int8_t>(b);
            }
        },
        {DataType::UINT64, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint64_t>(a) - static_cast<uint64_t>(b);
            }
        },
        {DataType::UINT32, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint32_t>(a) - static_cast<uint32_t>(b);
            }
        },
        {DataType::UINT16, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint16_t>(a) - static_cast<uint16_t>(b);
            }
        },
        {DataType::UINT8, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint8_t>(a) - static_cast<uint8_t>(b);
            }
        },
    };
    auto it = CALC_MAP.find(dataType);
    if (it == CALC_MAP.end()) {
        std::cerr << "Unsupported dataType: " << BriefDataType2String(dataType) << std::endl;
        assert(0);
    }
    result = it->second(left, right, &status);
    return result;
}

uint64_t CubeCalculate::EleMax(uint64_t left, uint64_t right, DataType dataType, float_status status)
{
    uint64_t result = 0;
    const static std::map<DataType, std::function<uint64_t(uint64_t, uint64_t, float_status*)>> CALC_MAP = {
        {DataType::FP64, [](uint64_t a, uint64_t b, float_status* status) {
                return float64_max(static_cast<float64>(a), static_cast<float64>(b), status);
            }
        },
        {DataType::FP32,  [](uint64_t a, uint64_t b, float_status* status) {
                return float32_max(static_cast<float32>(a), static_cast<float32>(b), status);
            }
        },
        {DataType::FP16,  [](uint64_t a, uint64_t b, float_status* status) {
                return float16_max(static_cast<float16>(a), static_cast<float16>(b), status);
            }
        },
        {DataType::BF16,  [](uint64_t a, uint64_t b, float_status* status) {
                return bfloat16_max(static_cast<bfloat16>(a), static_cast<bfloat16>(b), status);
            }
        },
        {DataType::FP8,   [](uint64_t a, uint64_t b, float_status* status) {
                return float8_max(static_cast<float8>(a), static_cast<float8>(b), status);
            }
        },
        {DataType::FP8_1, [](uint64_t a, uint64_t b, float_status* status) {
                return float8_1_max(static_cast<float8_1>(a), static_cast<float8_1>(b), status);
            }
        },
        {DataType::FP4,   [](uint64_t a, uint64_t b, float_status* status) {
                return float4_max(static_cast<float4>(a), static_cast<float4>(b), status);
            }
        },
        {DataType::FP4_1, [](uint64_t a, uint64_t b, float_status* status) {
                return float4_1_max(static_cast<float4_1>(a), static_cast<float4_1>(b), status);
            }
        },
        {DataType::INT64, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int64_t>(a) > static_cast<int64_t>(b) ? a : b;
            }
        },
        {DataType::INT32, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int32_t>(a) > static_cast<int32_t>(b) ? a : b;
            }
        },
        {DataType::INT16, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int16_t>(a) > static_cast<int16_t>(b) ? a : b;
            }
        },
        {DataType::INT8, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<int8_t>(a) > static_cast<int8_t>(b) ? a : b;
            }
        },
        {DataType::UINT64, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint64_t>(a) > static_cast<uint64_t>(b) ? a : b;
            }
        },
        {DataType::UINT32, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint32_t>(a) > static_cast<uint32_t>(b) ? a : b;
            }
        },
        {DataType::UINT16, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint16_t>(a) > static_cast<uint16_t>(b) ? a : b;
            }
        },
        {DataType::UINT8, [](uint64_t a, uint64_t b, float_status*) {
                return static_cast<uint8_t>(a) > static_cast<uint8_t>(b) ? a : b;
            }
        },
    };
    auto it = CALC_MAP.find(dataType);
    if (it == CALC_MAP.end()) {
        std::cerr << "Unsupported dataType: " << BriefDataType2String(dataType) << std::endl;
        assert(0);
    }
    result = it->second(left, right, &status);
    return result;
}

uint64_t CubeCalculate::EleZero(DataType dataType)
{
    uint64_t zero = 0;
    switch (dataType) {
        case DataType::FP64:
            zero = float64_zero;
            break;
        case DataType::FP32:
            zero = float32_zero;
            break;
        case DataType::FP16:
            zero = float16_zero;
            break;
        case DataType::BF16:
            zero = bfloat16_zero;
            break;
        case DataType::INT64:
        case DataType::INT32:
        case DataType::INT16:
        case DataType::INT8:
        case DataType::UINT64:
        case DataType::UINT32:
        case DataType::UINT16:
        case DataType::UINT8:
        case DataType::FP8:
        case DataType::FP8_1:
        case DataType::FP4:
        case DataType::FP4_1:
        case DataType::FP6:
        case DataType::FP6_1:
        case DataType::HIF4:
            break;
        default:
            std::cerr << "Unsupported dataType: " << BriefDataType2String(dataType) << std::endl;
            assert(0);
    }
    return zero;
}

const char* CubeCalculate::EleType(DataType dataType)
{
    switch (dataType) {
        case DataType::FP64:
            return "FP64(e11m52)";
        case DataType::FP32:
            return "FP32(e8m23)";
        case DataType::TF32:
            return "TF32(e8m10)";
        case DataType::HF32:
            return "HF32(e8m11)";
        case DataType::FP16:
            return "FP16(e5m10)";
        case DataType::BF16:
            return "BF16(e8m7)";
        case DataType::HIF8:
            return "HIF8";
        case DataType::FP8:
            return "FP8(e4m3)";
        case DataType::FP8_1:
            return "FP8_1(e5m2)";
        case DataType::FP6:
            return "FP6(e3m2)";
        case DataType::FP6_1:
            return "FP6_1(e2m3)";
        case DataType::FP4:
            return "FP4(e2m1)";
        case DataType::FP4_1:
            return "FP4_1(e1m2)";
        case DataType::SF8:
            return "SF8(e8m0)";
        case DataType::HIF4:
            return "HIF4";
        case DataType::INT64:
             return "INT64";
        case DataType::INT32:
            return "INT32";
        case DataType::INT16:
            return "INT16";
        case DataType::INT8:
            return "INT8";
        case DataType::INT4:
            return "INT4";
        case DataType::UINT64:
            return "UINT64";
        case DataType::UINT32:
            return "UINT32";
        case DataType::UINT16:
            return "UINT16";
        case DataType::UINT8:
            return "UINT8";
        case DataType::UINT4:
            return "UINT4";
        default:
            std::cerr << "Unsupported dataType: " << BriefDataType2String(dataType) << std::endl;
            assert(0);
    }
}

uint64_t CubeCalculate::EleSize(DataType dataType)
{
    switch (dataType) {
        case DataType::FP64:
            return sizeof(float64);
        case DataType::FP32:
            return sizeof(float32);
        case DataType::FP16:
            return sizeof(float16);
        case DataType::BF16:
            return sizeof(bfloat16);
        case DataType::FP8:
            return sizeof(float8);
        case DataType::FP8_1:
            return sizeof(float8_1);
        case DataType::SF8:
            return sizeof(sf8);
        case DataType::INT64:
            return sizeof(int64_t);
        case DataType::INT32:
            return sizeof(int32_t);
        case DataType::INT16:
            return sizeof(int16_t);
        case DataType::INT8:
            return sizeof(int8_t);
        case DataType::UINT64:
            return sizeof(uint64_t);
        case DataType::UINT32:
            return sizeof(uint32_t);
        case DataType::UINT16:
            return sizeof(uint16_t);
        case DataType::UINT8:
            return sizeof(uint8_t);
        case DataType::FP4:
        case DataType::FP4_1:
        case DataType::FP6:
        case DataType::FP6_1:
        case DataType::HIF4:
            return sizeof(uint8_t);
            break;
        default:
            std::cerr << "Unsupported dataType: " << BriefDataType2String(dataType) << std::endl;
            assert(0);
    }
}

double CubeCalculate::EleRealSize(DataType dataType)
{
    static const std::map<DataType, float> SIZE_MAP = {
        {DataType::FP64,    sizeof(float64)},
        {DataType::FP32,    sizeof(float32)},
        {DataType::FP16,    sizeof(float16)},
        {DataType::BF16,    sizeof(bfloat16)},
        {DataType::FP8,     sizeof(float8)},
        {DataType::FP8_1,   sizeof(float8_1)},
        {DataType::SF8,     sizeof(sf8)},
        {DataType::INT64,   sizeof(int64_t)},
        {DataType::INT32,   sizeof(int32_t)},
        {DataType::INT16,   sizeof(int16_t)},
        {DataType::INT8,    sizeof(int8_t)},
        {DataType::UINT64,  sizeof(uint64_t)},
        {DataType::UINT32,  sizeof(uint32_t)},
        {DataType::UINT16,  sizeof(uint16_t)},
        {DataType::UINT8,   sizeof(uint8_t)},
        // TODO: FP6 is not fully support yet
        {DataType::FP6,     0.75},
        {DataType::FP6_1,   0.75},
        {DataType::FP4,     0.5},
        {DataType::FP4_1,   0.5},
        {DataType::INT4,    0.5},
        {DataType::UINT4,   0.5},
        {DataType::HIF4,    0.5},
    };
    auto it = SIZE_MAP.find(dataType);
    if (it == SIZE_MAP.end()) {
        std::cerr << "Unsupported dataType: " << BriefDataType2String(dataType) << std::endl;
        assert(0);
        return 0.;
    }
    return it->second;
}

// 针对4bit数据类型进行特化, 计算读/写时地址偏移量
uint64_t CubeCalculate::EleOffset(uint64_t base, uint64_t idx, DataType dataType)
{
    double eleSize = CubeCalculate::EleRealSize(dataType);
    uint64_t eleOffset = static_cast<uint64_t>(
                            std::floor(static_cast<double>(idx) * eleSize));
    uint64_t addr = base + eleOffset;
    return addr;
}

// 读时, 从对应1字节中抽取出对应4bit数据或从对应4字节中抽取出对应16bit数据
// 不适用FP6等需要跨字节的数据
uint64_t CubeCalculate::EleDataExtract(uint64_t originData, uint64_t idx, DataType dataType)
{
    const static std::set<DataType> NEED_MASK = {
        DataType::FP4,
        DataType::FP4_1,
        DataType::INT4,
        DataType::UINT4,
        DataType::HIF4,
    };
    auto maskValue = [](uint64_t data, double eleSize, uint64_t idx) -> uint64_t {
        constexpr static uint64_t bytes = 8;
        uint64_t needBits = static_cast<uint64_t>(bytes * eleSize);
        uint64_t bytesContains = static_cast<uint64_t>(1. / eleSize);
        uint64_t posInBytes = idx % bytesContains;
        uint64_t mask = ((1ULL << needBits) - 1);
        mask = mask << (needBits * posInBytes);
        return (data & mask) >> (needBits * posInBytes);
    };
    double eleSize = CubeCalculate::EleRealSize(dataType);
    if (NEED_MASK.count(dataType) != 0) {
        return maskValue(originData, eleSize, idx);
    }
    return originData;
}

// 针对4bit数据类型进行特化, 计算写入时在同一字节中的偏移量
uint64_t CubeCalculate::EleDataMerge(uint64_t originData, uint64_t idx, DataType dataType)
{
    const static std::set<DataType> NEED_MASK = {
        DataType::FP4,
        DataType::FP4_1,
    };
    auto getNewData = [](uint64_t data, double eleSize, uint64_t idx) -> uint64_t {
        constexpr static uint64_t bytes = 8;
        uint64_t needBits = static_cast<uint64_t>(bytes * eleSize);
        // one bytes can contains how many element(TODO: FP6 may need other methods)
        uint64_t bytesContains = static_cast<uint64_t>(1. / eleSize);
        uint64_t posInBytes = idx % bytesContains;
        uint64_t mask = ((1ULL << needBits) - 1);
        return (data & mask) << (needBits * posInBytes);
    };
    double eleSize = CubeCalculate::EleRealSize(dataType);
    if (NEED_MASK.count(dataType) != 0) {
        return getNewData(originData, eleSize, idx);
    }
    return originData;
}

// Comparison Part
void CubeCalculate::ResultMatrix(const std::vector<uint64_t> &matrixData,
                                 std::pair<size_t, size_t> matrixShape,
                                 size_t eleSize,
                                 LayOut tileConvertType)
{
    std::vector<uint64_t> finalVec = matrixData;
    size_t tileRow = 0;
    size_t tileColumn = 0;
    switch (tileConvertType) {
        // 默认情况是由RD转为Nz
        case LayOut::NORM:
        case LayOut::ZZ2NZ:
        case LayOut::ZN2NZ:
        case LayOut::NN2NZ:
            tileRow = 16;
            tileColumn = 512 / (tileRow * eleSize);
            CubeCalculate::SwitchRD2NZ(finalVec, tileRow, tileColumn, matrixShape.first, matrixShape.second);
            break;
        case LayOut::ZZ2ZN:
        case LayOut::NZ2ZN:
        case LayOut::NN2ZN:
            tileRow = 16;
            tileColumn = 512 / (tileRow * eleSize);
            CubeCalculate::SwitchRD2ZN(finalVec, tileRow, tileColumn, matrixShape.first, matrixShape.second);
            break;
        case LayOut::ZZ2NN:
        case LayOut::ZN2NN:
        case LayOut::NZ2NN:
            CubeCalculate::SwitchRD2NN(finalVec, tileRow, tileColumn, matrixShape.first, matrixShape.second);
            break;
        case LayOut::ZN2ZZ:
        case LayOut::NZ2ZZ:
        case LayOut::NN2ZZ:
            CubeCalculate::SwitchRD2ZZ(finalVec, tileRow, tileColumn, matrixShape.first, matrixShape.second);
            break;
        default:
            std::cerr<<"ERROR Result Verificate: Unknown Tile store convert type\n";
            assert(0);
    }
}

static std::string FormatHex32(uint64_t v)
{
    std::ostringstream ss;
    ss << "0x" << std::hex << std::nouppercase
       << std::setfill('0') << std::setw(8)
       << static_cast<uint32_t>(v & 0xFFFFFFFFu);
    return ss.str(); // 长度固定为 10
}

static void PrintDataBlock(const std::vector<uint64_t>& src,
                           uint64_t row,
                           uint64_t column,
                           uint64_t colBegin,
                           uint64_t colEnd)
{
    const uint64_t blockCols = colEnd - colBegin;
    // 行号列宽度
    size_t rowLabelWidth = std::max<size_t>(2, std::string("row").size());
    if (row > 0) {
        rowLabelWidth = std::max<size_t>(rowLabelWidth,
                                         std::string("r" + std::to_string(row - 1)).size());
    }
    // 每列最大宽度
    std::vector<size_t> colWidths(static_cast<size_t>(blockCols), 10);
    // 数据更新每列宽度
    for (uint64_t j = 0; j < blockCols; ++j) {
        uint64_t col = colBegin + j;
        for (uint64_t i = 0; i < row; ++i) {
            size_t idx = static_cast<size_t>(i * column + col);
            const auto s = FormatHex32(src[idx]);
            colWidths[static_cast<size_t>(j)] =
                std::max(colWidths[static_cast<size_t>(j)], s.length());
        }
    }
    // 列号表头纳入宽度计算
    for (uint64_t j = 0; j < blockCols; ++j) {
        uint64_t col = colBegin + j;
        const std::string h = "c" + std::to_string(col);
        colWidths[static_cast<size_t>(j)] =
            std::max(colWidths[static_cast<size_t>(j)], h.size());
    }
    auto printSepLine = [rowLabelWidth, blockCols, &colWidths]() {
        std::cout << "+";
        // 行号列
        std::cout << std::string(rowLabelWidth + 2, '-') << "+";
        // 数据列
        for (uint64_t j = 0; j < blockCols; ++j) {
            std::cout << std::string(colWidths[static_cast<size_t>(j)] + 2, '-') << "+";
        }
        std::cout << "\n";
    };
    std::cout << "[cols " << colBegin << ".." << (colEnd - 1) << "]\n";
    printSepLine();
    // 列号表头
    {
        std::cout << "|";
        std::cout << " " << std::left
                  << std::setw(static_cast<int>(rowLabelWidth))
                  << "row"
                  << std::right << " |";
        for (uint64_t j = 0; j < blockCols; ++j) {
            uint64_t col = colBegin + j;
            const std::string h = "c" + std::to_string(col);
            std::cout << " " << std::left
                      << std::setw(static_cast<int>(colWidths[static_cast<size_t>(j)]))
                      << h
                      << std::right << " |";
        }
        std::cout << "\n";
    }
    printSepLine();
    // 数据行
    for (uint64_t i = 0; i < row; ++i) {
        std::cout << "|";
        // 行号列
        const std::string rowLabel = "r" + std::to_string(i);
        std::cout << " " << std::left
                  << std::setw(static_cast<int>(rowLabelWidth))
                  << rowLabel
                  << std::right << " |";
        // 数据列
        for (uint64_t j = 0; j < blockCols; ++j) {
            uint64_t col = colBegin + j;
            size_t idx = static_cast<size_t>(i * column + col);
            const auto s = FormatHex32(src[idx]);
            std::cout << " " << std::left
                      << std::setw(static_cast<int>(colWidths[static_cast<size_t>(j)]))
                      << s
                      << std::right << " |";
        }
        std::cout << "\n";
        printSepLine();
    }
}

void CubeCalculate::PrintData(const std::vector<uint64_t>& src,
                              uint64_t row,
                              uint64_t column,
                              uint64_t maxColsPerBlock)
{
    if (row == 0 || column == 0 || maxColsPerBlock == 0) {
        return;
    }
    const uint64_t need = row * column;
    if (src.size() < static_cast<size_t>(need)) {
        std::cout << "[PrintData] src.size() < row*column, skip. ("
                  << src.size() << " < " << need << ")\n";
        return;
    }
    for (uint64_t colBegin = 0; colBegin < column; colBegin += maxColsPerBlock) {
        uint64_t colEnd = std::min<uint64_t>(column, colBegin + maxColsPerBlock);
        PrintDataBlock(src, row, column, colBegin, colEnd);
        if (colEnd < column) {
            std::cout << "\n";
        }
    }
}

static uint32_t E6m2ToF32Bits(uint8_t v)
{
    float res;
    uint32_t f32;
    res = powf(2.0f, ((v >> 2) & 0x3f) - 48);
    res *= 1.0 + ((v & 0b10) >> 1) * 0.5 + (v & 0b1) * 0.25;
    memcpy(&f32, &res,  4);
    return f32;
}

static uint32_t Hif4ToF32Bits(uint8_t v)
{
    float res;
    uint32_t f32;
    res = powf(2.0f, ((v >> 2) & 0x1) - 1);
    res *= (1.0 + ((v & 0b10) >> 1) * 0.5 + (v & 0b1) * 0.25);
    memcpy(&f32, &res,  4);
    return f32;
}

uint64_t CubeCalculate::GetElementValue(uint64_t origin, uint64_t ea, uint64_t eb, uint64_t ec, float_status* status)
{
    static const float BASE = 2.0;
    float32 baseVal = Hif4ToF32Bits(origin);
    float32 exponent = F32ToBits(powf(BASE, eb + ec));
    float32 e6M2 = E6m2ToF32Bits(ea);

    float32 result = float32_mul(e6M2, exponent, status);
    result = float32_mul(result, baseVal, status);
    return static_cast<uint64_t>(result);
}
} // namespace JCore
