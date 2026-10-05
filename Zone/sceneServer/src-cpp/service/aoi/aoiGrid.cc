#include "service/aoi/aoiGrid.h"
#include <cstdint>
#include <optional>
#include <vector>
#include <algorithm>

AoiGrid::AoiGrid(uint32_t minX,
                 uint32_t minY,
                 uint32_t cols,
                 uint32_t rows,
                 uint32_t cellSize,
                 uint32_t viewRadiusCells)
                : minX_(minX), 
                  minY_(minY), 
                  cols_(cols), 
                  rows_(rows), 
                  cellSize_(cellSize), 
                  r_(viewRadiusCells)
{
    // 分配所有格子
    if (cols_ > 0 && rows_ > 0)
        cells_.resize(static_cast<size_t>(cols_) * static_cast<size_t>(rows_));
}

std::optional<uint32_t> AoiGrid::TryCellOf(Position x, Position y) const
{
    if (cellSize_ == 0 || cols_ == 0 || rows_ == 0)
        return std::nullopt;

    // 用 int64 计算边界，避免溢出
    const int64_t x64    = static_cast<int64_t>(x);
    const int64_t y64    = static_cast<int64_t>(y);
    const int64_t minX64 = static_cast<int64_t>(minX_);
    const int64_t minY64 = static_cast<int64_t>(minY_);
    const int64_t spanX  = static_cast<int64_t>(cols_) * static_cast<int64_t>(cellSize_);
    const int64_t spanY  = static_cast<int64_t>(rows_) * static_cast<int64_t>(cellSize_);

    // 区间是 [min, min + span)，所以上界用 >=
    if (x64 < minX64 || x64 >= minX64 + spanX ||
        y64 < minY64 || y64 >= minY64 + spanY)
    {
        return std::nullopt;
    }

    const uint32_t col = static_cast<uint32_t>((x64 - minX64) / static_cast<int64_t>(cellSize_));
    const uint32_t row = static_cast<uint32_t>((y64 - minY64) / static_cast<int64_t>(cellSize_));
    return row * cols_ + col;
}

    
const AoiCell& AoiGrid::getCell(uint32_t cellIdx) const
{
    // 调用方负责合法性检查
    return cells_[cellIdx];
}


std::vector<uint32_t> AoiGrid::getWatchCellIdx(Position x, Position y) const
{
    std::vector<uint32_t> result;

    //根据位置算出所在格子索引
    const auto optIdx = TryCellOf(x, y);
    if (!optIdx.has_value())
        return result;
    
    const uint32_t centerCol = *optIdx % cols_;
    const uint32_t centerRow = *optIdx / cols_;

    const int32_t r      = static_cast<int32_t>(r_);
    const int32_t colsI  = static_cast<int32_t>(cols_);
    const int32_t rowsI  = static_cast<int32_t>(rows_);

    int32_t col0 = static_cast<int32_t>(centerCol) - r; //左边界
    int32_t col1 = static_cast<int32_t>(centerCol) + r; //右边界
    int32_t row0 = static_cast<int32_t>(centerRow) - r; //上边界
    int32_t row1 = static_cast<int32_t>(centerRow) + r; //下边界

    // 裁剪到网格范围
    if (col0 < 0)       col0 = 0;
    if (row0 < 0)       row0 = 0;
    if (col1 >= colsI)  col1 = colsI - 1;
    if (row1 >= rowsI)  row1 = rowsI - 1;

    // 预留空间，避免多次扩容
    const size_t count = static_cast<size_t>(col1 - col0 + 1) *
                         static_cast<size_t>(row1 - row0 + 1);
    result.reserve(count);

    for (int32_t j = row0; j <= row1; ++j)
    {
        for (int32_t i = col0; i <= col1; ++i)
        {
            result.push_back(static_cast<uint32_t>(j) * cols_ + static_cast<uint32_t>(i));
        }
    }
    return result;
}   


bool AoiGrid::visible(uint32_t cellA, uint32_t cellB) const
{
    if (cellA >= cells_.size() || cellB >= cells_.size())
        return false;

    const int32_t colA = static_cast<int32_t>(cellA % cols_);
    const int32_t rowA = static_cast<int32_t>(cellA / cols_);
    const int32_t colB = static_cast<int32_t>(cellB % cols_);
    const int32_t rowB = static_cast<int32_t>(cellB / cols_);

    const int32_t dx = colA > colB ? colA - colB : colB - colA;
    const int32_t dy = rowA > rowB ? rowA - rowB : rowB - rowA;
    const int32_t r  = static_cast<int32_t>(r_);

    // 切比雪夫距离：两个格子都在对方的方形视野内
    return dx <= r && dy <= r;
}


void AoiGrid::addEntity(EntityId entityId, uint32_t cell) 
{
    if (cell >= cells_.size())
        return;
    cells_[cell].entys_.push_back(entityId);
}

void AoiGrid::removeEntity(EntityId entityId, uint32_t cell)
{
    if (cell >= cells_.size())
        return;

    auto& ents = cells_[cell].entys_;
    // remove-erase 惯用法：删除所有等于 entityId 的元素
    ents.erase(std::remove(ents.begin(), ents.end(), entityId), ents.end());
}


void AoiGrid::addWatcher(EntityId observerId, uint32_t cell)
{
    if (cell >= cells_.size())
        return;
    cells_[cell].watchers_.push_back(observerId);
}

void AoiGrid::removeWatcher(EntityId observerId, uint32_t cell)
{
    if (cell >= cells_.size())
        return;

    auto& ws = cells_[cell].watchers_;
    ws.erase(std::remove(ws.begin(), ws.end(), observerId), ws.end());
}


const std::vector<EntityId>& AoiGrid::entities(uint32_t cell) const
{
    // 越界时返回一个静态空 vector，避免 UB
    static const std::vector<EntityId> kEmpty;
    if (cell >= cells_.size())
        return kEmpty;
    return cells_[cell].entys_;
}

const std::vector<EntityId>& AoiGrid::watchers(uint32_t cell) const
{
    static const std::vector<EntityId> kEmpty;
    if (cell >= cells_.size())
        return kEmpty;
    return cells_[cell].watchers_;
}
