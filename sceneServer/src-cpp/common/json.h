// 极简 JSON 解析器(递归下降, 供 item_config.use_param 等使用)
#ifndef CLEARMOON_SCENE_COMMON_JSON_H
#define CLEARMOON_SCENE_COMMON_JSON_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct JsonNode {
  enum Type { kNull, kBool, kNum, kStr, kArr, kObj } type = kNull;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<JsonNode> arr;
  std::vector<std::pair<std::string, JsonNode>> obj;

  // 从文本解析; 成功返回 true
  static bool Parse(const std::string& text, JsonNode& out, std::string& err);

  const JsonNode* Get(const std::string& key) const {
    for (auto& kv : obj)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  int64_t GetInt(const std::string& key, int64_t def = 0) const {
    const JsonNode* n = Get(key);
    if (n && (n->type == kNum || n->type == kBool)) return (int64_t)n->num;
    if (n && n->type == kStr) return atoll(n->str.c_str());
    return def;
  }
};

#endif