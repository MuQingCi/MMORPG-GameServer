#include "service/aoi/map.h"

#include "base/yaml.h"

#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace
{
std::string Trim(const std::string& text)
{
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

bool ParseTuple(const std::string& text, size_t count, std::vector<uint32_t>& values)
{
    values.clear();
    size_t begin = 0;
    for (;;)
    {
        const size_t comma = text.find(',', begin);
        const std::string part = Trim(text.substr(begin, comma == std::string::npos ? comma : comma - begin));
        uint32_t value = 0;
        const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || result.ec != std::errc{} || result.ptr != part.data() + part.size()) return false;
        values.push_back(value);
        if (values.size() > count) return false;
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return values.size() == count;
}
} // namespace

bool Map::Load(const std::string& path, uint32_t cellSize, Map& out, std::string& err)
{
    err.clear();
    auto fail = [&](const std::string& reason) { err = path + ": " + reason; return false; };
    if (cellSize == 0 || cellSize > uint32_t(std::numeric_limits<int32_t>::max()))
        return fail("cellSize must be in [1, INT32_MAX]");
    std::ifstream in(path);
    if (!in) return fail("cannot open map file");
    std::stringstream stream;
    stream << in.rdbuf();
    YamlLite yaml;
    if (!yaml.Parse(stream.str(), err)) { err = path + ": " + err; return false; }
    Map map;
    std::vector<uint32_t> values;
    if (!ParseTuple(yaml.GetStr("MapConfig.map_size"), 2, values))
        return fail("MapConfig.map_size must be length,width");
    if (values[0] == 0 || values[1] == 0 ||
        values[0] > uint32_t(std::numeric_limits<int32_t>::max()) ||
        values[1] > uint32_t(std::numeric_limits<int32_t>::max()))
        return fail("map dimensions must be in [1, INT32_MAX]");
    map.length_ = values[0];
    map.width_ = values[1];
    map.cellSize_ = cellSize;
    if (uint64_t(map.getCols()) * map.getRows() > std::numeric_limits<uint32_t>::max())
        return fail("grid cell count exceeds uint32_t cell index range");

    // YamlLite 将当前 '- block_1: x,y,length,width' 保存为列表字符串。
    if (yaml.Has("MapConfig.block_area") && !yaml.GetStr("MapConfig.block_area").empty())
        return fail("MapConfig.block_area must be a list");
    std::set<std::string> names;
    for (const std::string& item : yaml.GetList("MapConfig.block_area"))
    {
        const size_t colon = item.find(':');
        std::string tuple = item;
        if (colon != std::string::npos)
        {
            const std::string name = Trim(item.substr(0, colon));
            if (name.empty() || !names.insert(name).second) return fail("empty or duplicate block name");
            tuple = item.substr(colon + 1);
        }
        if (!ParseTuple(tuple, 4, values)) return fail("block must be centerX,centerY,length,width: " + item);
        const int64_t x2 = int64_t(values[0]) * 2, y2 = int64_t(values[1]) * 2;
        if (values[2] == 0 || values[3] == 0 ||
            x2 - values[2] < 0 || y2 - values[3] < 0 ||
            x2 + values[2] >= int64_t(map.length_) * 2 ||
            y2 + values[3] >= int64_t(map.width_) * 2)
            return fail("block dimensions must be positive and its closed rectangle must lie inside map: " + item);
        map.blocks_.push_back({values[0], values[1], values[2], values[3]});
    }
    const auto unused = yaml.UnusedKeys();
    if (!unused.empty()) return fail("unknown/unused map config key: " + unused.front());
    out = std::move(map);
    return true;
}

bool Map::contains(int32_t x, int32_t y) const
{
    return x >= 0 && y >= 0 && uint32_t(x) < length_ && uint32_t(y) < width_;
}

bool Map::DstIsBlock(int32_t x, int32_t y) const
{
    // 未加载或越界位置也不可通行。
    if (!contains(x, y)) return true;
    for (const auto& block : blocks_)
    {
        const int64_t dx = int64_t(x) * 2 - int64_t(block.pos_x) * 2;
        const int64_t dy = int64_t(y) * 2 - int64_t(block.pos_y) * 2;
        // 二倍坐标避免奇数边长的半像素边界被整数除法截断。
        if (dx >= -int64_t(block.length) && dx <= int64_t(block.length) &&
            dy >= -int64_t(block.width) && dy <= int64_t(block.width)) return true;
    }
    return false;
}