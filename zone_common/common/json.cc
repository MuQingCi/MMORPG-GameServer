#include "common/json.h"
#include <cstdlib>
#include <cstring>

namespace {
struct Parser {
  const char* p;
  const char* end;
  const char* base;
  std::string err;

  void SkipWs() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      ++p;
  }
  bool Eof() {
    SkipWs();
    return p >= end;
  }
  bool Eat(char c) {
    SkipWs();
    if (p < end && *p == c) {
      ++p;
      return true;
    }
    return false;
  }
  bool Parse(JsonNode& out) {
    SkipWs();
    if (p >= end) return Fail("empty input");
    char c = *p;
    if (c == '{') return ParseObj(out);
    if (c == '[') return ParseArr(out);
    if (c == '"') return ParseStr(out);
    if (c == 't' || c == 'f') return ParseBool(out);
    if (c == 'n') return ParseNull(out);
    return ParseNum(out);
  }
  bool Fail(const char* msg) {
    if (err.empty()) {
      err = msg;
      err += " at offset ";
      err += std::to_string(p - base);
    }
    return false;
  }
  bool ParseObj(JsonNode& out) {
    out.type = JsonNode::kObj;
    ++p;  // {
    if (Eat('}')) return true;
    for (;;) {
      JsonNode key;
      if (!ParseStr(key)) return false;
      if (!Eat(':')) return Fail("expect ':'");
      JsonNode val;
      if (!Parse(val)) return false;
      out.obj.emplace_back(key.str, std::move(val));
      if (Eat('}')) return true;
      if (!Eat(',')) return Fail("expect ',' or '}'");
    }
  }
  bool ParseArr(JsonNode& out) {
    out.type = JsonNode::kArr;
    ++p;  // [
    if (Eat(']')) return true;
    for (;;) {
      JsonNode val;
      if (!Parse(val)) return false;
      out.arr.push_back(std::move(val));
      if (Eat(']')) return true;
      if (!Eat(',')) return Fail("expect ',' or ']'");
    }
  }
  bool ParseStr(JsonNode& out) {
    out.type = JsonNode::kStr;
    ++p;  // "
    std::string s;
    while (p < end) {
      char c = *p++;
      if (c == '"') {
        out.str = std::move(s);
        return true;
      }
      if (c == '\\') {
        if (p >= end) break;
        char e = *p++;
        switch (e) {
          case 'n': s.push_back('\n'); break;
          case 't': s.push_back('\t'); break;
          case 'r': s.push_back('\r'); break;
          case 'b': s.push_back('\b'); break;
          case 'f': s.push_back('\f'); break;
          case '\\': s.push_back('\\'); break;
          case '"': s.push_back('"'); break;
          case '/': s.push_back('/'); break;
          case 'u': {
            // 简易处理: 只支持 \u00xx ASCII 段
            if (end - p >= 4) {
              char hex[5] = {p[0], p[1], p[2], p[3], 0};
              p += 4;
              s.push_back((char)strtol(hex, nullptr, 16));
            }
            break;
          }
          default: s.push_back(e);
        }
      } else {
        s.push_back(c);
      }
    }
    return Fail("unterminated string");
  }
  bool ParseBool(JsonNode& out) {
    if (end - p >= 4 && strncmp(p, "true", 4) == 0) {
      out.type = JsonNode::kBool;
      out.b = true;
      p += 4;
      return true;
    }
    if (end - p >= 5 && strncmp(p, "false", 5) == 0) {
      out.type = JsonNode::kBool;
      out.b = false;
      p += 5;
      return true;
    }
    return Fail("bad literal");
  }
  bool ParseNull(JsonNode& out) {
    if (end - p >= 4 && strncmp(p, "null", 4) == 0) {
      out.type = JsonNode::kNull;
      p += 4;
      return true;
    }
    return Fail("bad literal");
  }
  bool ParseNum(JsonNode& out) {
    const char* start = p;
    while (p < end && (isdigit((unsigned char)*p) || *p == '-' || *p == '+' ||
                       *p == '.' || *p == 'e' || *p == 'E'))
      ++p;
    if (p == start) return Fail("bad number");
    out.type = JsonNode::kNum;
    out.num = strtod(std::string(start, p - start).c_str(), nullptr);
    return true;
  }
};
}  // namespace

bool JsonNode::Parse(const std::string& text, JsonNode& out, std::string& err) {
  Parser ps{text.data(), text.data() + text.size(), text.data(), ""};
  bool ok = ps.Parse(out);
  err = ps.err;
  if (ok) {
    ps.SkipWs();
    ok = ps.p >= ps.end;
    if (!ok) err = "trailing data";
  }
  return ok;
}
