#include "common/yaml.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace
{
// 去掉行尾注释（# 之后的内容），并裁掉首尾空白
std::string StripCommentAndTrim(const std::string& line)
{
    std::string s;
    bool inSingle = false;
    bool inDouble = false;
    size_t cut = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];
        if (c == '\'' && !inDouble)
            inSingle = !inSingle;
        else if (c == '"' && !inSingle)
            inDouble = !inDouble;
        else if (c == '#' && !inSingle && !inDouble)
        {
            cut = i;
            break;
        }
    }

    s = (cut == std::string::npos) ? line : line.substr(0, cut);

    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 去掉值两端可能存在的成对引号
std::string Unquote(const std::string& v)
{
    if (v.size() >= 2 &&
        ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
        return v.substr(1, v.size() - 2);
    return v;
}

struct Frame
{
    int indent = -1;
    std::string path;
};
}  // namespace

bool YamlLite::Parse(const std::string& text, std::string& err)
{
    values_.clear();
    lists_.clear();

    std::vector<Frame> stack;
    stack.push_back({-1, ""});

    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size())
    {
        const size_t nl = text.find('\n', pos);
        const std::string raw = (nl == std::string::npos) ? text.substr(pos)
                                                         : text.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        ++lineNo;

        // 换行符统一处理
        std::string line = raw;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.find('\t') != std::string::npos)
        {
            err = "line " + std::to_string(lineNo) + ": tab is not allowed for indentation";
            return false;
        }

        const std::string content = StripCommentAndTrim(line);
        if (content.empty())
            continue;

        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;

        // 弹掉所有"不比本行更外层"的层级：同缩进视为同级
        while (stack.size() > 1 && indent <= static_cast<size_t>(stack.back().indent))
            stack.pop_back();

        const std::string& parent = stack.back().path;

        // ---- 数组项 ----
        if (content.size() >= 1 && content[0] == '-')
        {
            std::string item = StripCommentAndTrim(content.substr(1));
            if (item.empty())
            {
                err = "line " + std::to_string(lineNo) + ": empty list item";
                return false;
            }
            lists_[parent].push_back(Unquote(item));
            continue;
        }

        // ---- key: value ----
        const size_t colon = content.find(':');
        if (colon == std::string::npos)
        {
            err = "line " + std::to_string(lineNo) + ": missing ':' (" + content + ")";
            return false;
        }

        const std::string key = StripCommentAndTrim(content.substr(0, colon));
        if (key.empty())
        {
            err = "line " + std::to_string(lineNo) + ": empty key";
            return false;
        }

        const std::string fullKey = parent.empty() ? key : parent + "." + key;
        const std::string value = StripCommentAndTrim(content.substr(colon + 1));

        if (value.empty())
        {
            // 空值 => 打开一个新层级（可能是 map，也可能是下面紧跟的数组）
            stack.push_back({static_cast<int>(indent), fullKey});
            continue;
        }

        values_[fullKey] = Unquote(value);
    }

    return true;
}

bool YamlLite::Has(const std::string& key) const
{
    Mark(key);
    // 标量键与数组键都算"存在"：
    // 只查 values_ 会漏掉 gateways 这类数组键，导致"搬家检查"漏判
    return values_.find(key) != values_.end() || lists_.find(key) != lists_.end();
}

bool YamlLite::HasPrefix(const std::string& prefix) const
{
    auto startsWith = [&prefix](const std::string& k) {
        return k.size() >= prefix.size() && k.compare(0, prefix.size(), prefix) == 0;
    };

    for (const auto& kv : values_)
    {
        if (startsWith(kv.first))
        {
            Mark(kv.first);   // 视为已消费：它属于"已迁移/已识别"的段，不该再报 unknown
            return true;
        }
    }
    for (const auto& kv : lists_)
    {
        if (startsWith(kv.first))
        {
            Mark(kv.first);
            return true;
        }
    }
    return false;
}

void YamlLite::Mark(const std::string& key) const
{
    consumed_.insert(key);
}

std::vector<std::string> YamlLite::UnusedKeys() const
{
    std::vector<std::string> out;
    for (const auto& kv : values_)
    {
        if (consumed_.find(kv.first) == consumed_.end())
            out.push_back(kv.first);
    }
    return out;
}

std::string YamlLite::GetStr(const std::string& key, const std::string& def) const
{
    Mark(key);
    auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

int64_t YamlLite::GetInt(const std::string& key, int64_t def) const
{
    Mark(key);
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty())
        return def;
    return static_cast<int64_t>(std::strtoll(it->second.c_str(), nullptr, 10));
}

uint64_t YamlLite::GetUInt(const std::string& key, uint64_t def) const
{
    const int64_t v = GetInt(key, static_cast<int64_t>(def));
    return v < 0 ? def : static_cast<uint64_t>(v);
}

bool YamlLite::GetPort(const std::string& key, uint16_t def, uint16_t& out, std::string& err) const
{
    const uint64_t v = GetUInt(key, def);
    if (v == 0 || v > 65535)
    {
        err = key + " must be in (0, 65535], got " + std::to_string(v);
        return false;
    }
    out = static_cast<uint16_t>(v);
    return true;
}

double YamlLite::GetDouble(const std::string& key, double def) const
{
    Mark(key);
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty())
        return def;
    return std::strtod(it->second.c_str(), nullptr);
}

bool YamlLite::GetBool(const std::string& key, bool def) const
{
    Mark(key);
    auto it = values_.find(key);
    if (it == values_.end())
        return def;

    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (v == "true" || v == "yes" || v == "on" || v == "1")
        return true;
    if (v == "false" || v == "no" || v == "off" || v == "0")
        return false;
    return def;
}

std::vector<std::string> YamlLite::GetList(const std::string& key) const
{
    Mark(key);
    auto it = lists_.find(key);
    return it == lists_.end() ? std::vector<std::string>{} : it->second;
}
