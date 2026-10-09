#ifndef CLEARMOON_CONFIG_AOICONFIG_H
#define CLEARMOON_CONFIG_AOICONFIG_H

#include "base/yaml.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>

// 首版仅支持九宫格；内联读取实现使现有 SceneConfig 构建无需额外登记源文件。
class AoiConfig
{
public:
    uint32_t tickMs = 50;
    std::string aoiKind = "nineGrid";
    uint32_t cellSize = 50;
    uint32_t viewRange = 50;  // 地图坐标单位，不是格数

    uint32_t viewRadiusCells() const
    {
        return cellSize == 0 ? 0 : viewRange / cellSize + (viewRange % cellSize != 0);
    }

    bool Validate(std::string& err) const
    {
        if (aoiKind != "nineGrid")
            err = "SceneServer.aoi.aoiKind only supports nineGrid";
        else if (tickMs == 0)
            err = "SceneServer.aoi.tickMs must be > 0";
        else if (cellSize == 0 || cellSize > uint32_t(std::numeric_limits<int32_t>::max()))
            err = "SceneServer.aoi.nineGrid.cellSize must be in [1, INT32_MAX]";
        else if (viewRange > uint32_t(std::numeric_limits<int32_t>::max()))
            err = "SceneServer.aoi.nineGrid.viewRange must be <= INT32_MAX";
        else
        {
            err.clear();
            return true;
        }
        return false;
    }

    static bool Load(const YamlLite& yaml, AoiConfig& out, std::string& err)
    {
        AoiConfig cfg = out;
        auto readUInt = [&](const std::string& key, uint32_t& value) {
            if (!yaml.Has(key)) return true;
            const std::string text = yaml.GetStr(key);
            uint32_t parsed = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
            {
                err = key + " must be an unsigned 32-bit integer, got '" + text + "'";
                return false;
            }
            value = parsed;
            return true;
        };
        cfg.aoiKind = yaml.GetStr("SceneServer.aoi.aoiKind", cfg.aoiKind);
        uint32_t cellPerLogicThread = 0;
        if (!readUInt("SceneServer.aoi.tickMs", cfg.tickMs) ||
            !readUInt("SceneServer.aoi.nineGrid.cellSize", cfg.cellSize) ||
            !readUInt("SceneServer.aoi.nineGrid.viewRange", cfg.viewRange) ||
            !readUInt("SceneServer.aoi.nineGrid.cellPerLogicThread", cellPerLogicThread))
            return false;
        if (cellPerLogicThread != 0)
        {
            err = "SceneServer.aoi.nineGrid.cellPerLogicThread must be 0 (full-scene replica per worker)";
            return false;
        }
        // 当前 yaml 中的空段不产生键；非空参数不允许被静默忽略。
        if (yaml.Has("SceneServer.aoi.crossLinkedList") ||
            yaml.HasPrefix("SceneServer.aoi.crossLinkedList."))
        {
            err = "SceneServer.aoi.crossLinkedList is not supported";
            return false;
        }
        if (!cfg.Validate(err)) return false;
        out = cfg;
        return true;
    }
};

#endif