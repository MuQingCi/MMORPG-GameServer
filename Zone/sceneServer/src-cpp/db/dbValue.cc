#include "dbValue.h"
#include "common/msg.h"

namespace dbv {

namespace {
constexpr uint16_t kWireMagic = 0xDB01;  // 帧魔数(防错位/串包)
constexpr uint8_t kWireVersion = 1;      // 线格式版本
constexpr int kMaxDepth = 64;            // 值树深度上限
constexpr uint32_t kMaxErrMsg = 64 * 1024;
constexpr uint32_t kMaxItems = 1u << 24;  // 数组元素/参数量上限
}  // namespace

static void EncodeValD(std::string& s, const Val& v, int depth) {
  s.push_back(char(v.kind));
  if (depth > kMaxDepth) return;
  switch (v.kind) {
    case kInt:
      PutI64(s, v.i);
      break;
    case kStr: {
      PutU32(s, (uint32_t)v.s.size());
      s.append(v.s);
      break;
    }
    case kArr: {
      PutU32(s, (uint32_t)v.a.size());
      for (const auto& e : v.a) EncodeValD(s, e, depth + 1);
      break;
    }
    default:
      break;
  }
}

void EncodeVal(std::string& s, const Val& v) { EncodeValD(s, v, 0); }

static bool DecodeValD(const char*& p, const char* end, Val& v, int depth) {
  if (depth > kMaxDepth) return false;
  if (p >= end) return false;
  v.kind = Kind(uint8_t(*p++));
  switch (v.kind) {
    case kInt: {
      if (end - p < 8) return false;
      v.i = GetI64(p);
      p += 8;
      return true;
    }
    case kStr: {
      if (end - p < 4) return false;
      uint32_t n = GetU32(p);
      p += 4;
      if ((size_t)(end - p) < n) return false;
      v.s.assign(p, n);
      p += n;
      return true;
    }
    case kArr: {
      if (end - p < 4) return false;
      uint32_t n = GetU32(p);
      p += 4;
      // 合理性校验: 元素数不能超过剩余字节数(每元素至少 1 字节 tag)
      if (n > kMaxItems || (size_t)(end - p) < (size_t)n) return false;
      v.a.clear();
      v.a.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        Val e;
        if (!DecodeValD(p, end, e, depth + 1)) return false;
        v.a.push_back(std::move(e));
      }
      return true;
    }
    default:
      return v.kind == kNil;
  }
}

bool DecodeVal(const char*& p, const char* end, Val& v) {
  return DecodeValD(p, end, v, 0);
}

std::string DbTask::Encode() const {
  std::string s;
  PutU16(s, kWireMagic);
  s.push_back(char(kWireVersion));
  s.push_back(char(kind));
  PutU32(s, (uint32_t)args.size());
  for (auto& a : args) {
    PutU32(s, (uint32_t)a.size());
    s.append(a);
  }
  return s;
}

bool DbTask::Decode(const std::string& body, DbTask& out) {
  if (body.size() < 8) return false;
  const char* p = body.data();
  const char* end = p + body.size();
  if (GetU16(p) != kWireMagic) return false;
  p += 2;
  if (uint8_t(*p++) != kWireVersion) return false;
  out.kind = uint8_t(*p++);
  uint32_t n = GetU32(p);
  p += 4;
  if (n > kMaxItems) return false;
  out.args.clear();
  out.args.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    if (end - p < 4) return false;
    uint32_t len = GetU32(p);
    p += 4;
    if ((size_t)(end - p) < len) return false;
    out.args.emplace_back(p, len);
    p += len;
  }
  return p == end;  // 尾部不得有残留
}

std::string DbResult::Encode() const {
  std::string s;
  PutU16(s, kWireMagic);
  s.push_back(char(kWireVersion));
  PutU32(s, (uint32_t)errcode);  // errcode 改 u32, 避免负码/大码截断
  PutU32(s, (uint32_t)errmsg.size());
  s.append(errmsg);
  EncodeVal(s, data);
  return s;
}

bool DbResult::Decode(const std::string& body, DbResult& out) {
  if (body.size() < 11) return false;
  const char* p = body.data();
  const char* end = p + body.size();
  if (GetU16(p) != kWireMagic) return false;
  p += 2;
  if (uint8_t(*p++) != kWireVersion) return false;
  out.errcode = (int32_t)GetU32(p);
  p += 4;
  uint32_t elen = GetU32(p);
  p += 4;
  if (elen > kMaxErrMsg || (size_t)(end - p) < elen) return false;
  out.errmsg.assign(p, elen);
  p += elen;
  if (!DecodeVal(p, end, out.data)) return false;
  return p == end;  // 尾部不得有残留
}

}  // namespace dbv
