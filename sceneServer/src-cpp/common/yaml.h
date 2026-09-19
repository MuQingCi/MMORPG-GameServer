#ifndef CLEARMOON_COMMON_YAML_H
#define CLEARMOON_COMMON_YAML_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

/**
 * @brief 极简 YAML 子集解析器（**只服务于配置文件的读取**）
 *
 * 支持的语法（缩进表示层级，缩进必须用空格）：
 *   Root:
 *       name: value          # 行尾注释
 *       num: 123
 *       flag: true
 *       list:
 *           - a
 *           - b
 *
 * 解析结果被拍平成两点查询：
 *   - 标量：Get("Root.name") / GetInt / GetBool
 *   - 数组：GetList("Root.list")
 * 键大小写敏感，路径用 '.' 连接。
 *
 * 明确不支持（遇到即报错，不做"猜"的解析）：
 *   - Tab 缩进；- 多级数组；- 行内 map/数组
 * '#' 在单/双引号内不会被视为注释起始（"a # b" 能正确解析）。
 *
 * 【防线】所有 Get* 都会记录"被消费过的键"，Load 结束后调用 UnusedKeys() 即可列出yaml里写了但代码从没读过**的标量键。它的价值在于抓两类事故：
 *   1. 键名拼错（sceneID vs sceneId）—— 否则会静默回落到默认值，完全看不出错；
 *   2. 配置项搬了家但旧文件没删（例如 db 段已迁到 zoneConfig，sceneConfig 里还留着）。
 */
class YamlLite
{
public:
    bool Parse(const std::string& text, std::string& err);

    bool Has(const std::string& key) const;
    // 是否存在以 prefix 开头的键（含数组键）。
    // 用途：检测"某个配置段是否还在这个文件里"（容器键本身没有值，Has() 看不到它）
    bool HasPrefix(const std::string& prefix) const;

    std::string GetStr(const std::string& key, const std::string& def = "") const;
    int64_t GetInt(const std::string& key, int64_t def = 0) const;
    uint64_t GetUInt(const std::string& key, uint64_t def = 0) const;
    double GetDouble(const std::string& key, double def = 0.0) const;
    bool GetBool(const std::string& key, bool def = false) const;
    std::vector<std::string> GetList(const std::string& key) const;

    // yaml 里存在、但没有任何 Get*/Has* 读过的标量键（升序）
    std::vector<std::string> UnusedKeys() const;

private:
    void Mark(const std::string& key) const;

    std::map<std::string, std::string> values_;
    std::map<std::string, std::vector<std::string>> lists_;
    // 被消费过的键（mutable：查询接口本身是 const 语义的"只读"）
    mutable std::set<std::string> consumed_;
};

#endif
