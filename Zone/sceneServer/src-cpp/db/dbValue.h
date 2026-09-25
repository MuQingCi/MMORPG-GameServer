#ifndef CLEARMOON_SCENE_DB_DB_VALUE_H
#define CLEARMOON_SCENE_DB_DB_VALUE_H
// ============================================================================
// db_value.h —— DB 层消息体编解码
//   DB线程与逻辑线程之间通过 Msg.body 传递"任务/结果"。
//   结果是一棵值树 Val(支持 NULL/整数/字符串/数组递归),便于统一表达
//   MySQL 行集与 Redis 回复(简化:字符串一律当作字符串,不区分 bulk/simple)。
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

namespace dbv {

enum Kind : uint8_t { kNil = 0, kInt = 1, kStr = 2, kArr = 3 };

struct Val {
  Kind kind = kNil;
  int64_t i = 0;
  std::string s;
  std::vector<Val> a;

  static Val Nil() { return Val{}; }
  static Val Int(int64_t v) {
    Val x;
    x.kind = kInt;
    x.i = v;
    return x;
  }
  static Val Str(std::string v) {
    Val x;
    x.kind = kStr;
    x.s = std::move(v);
    return x;
  }
  static Val Arr(std::vector<Val> v) {
    Val x;
    x.kind = kArr;
    x.a = std::move(v);
    return x;
  }
  bool IsNil() const { return kind == kNil; }
  bool IsInt() const { return kind == kInt; }
  bool IsStr() const { return kind == kStr; }
  bool IsArr() const { return kind == kArr; }
};

// DB任务
struct DbTask {
  // 1 = MySQL select(返回行), 2 = MySQL exec(返回[影响行,自增id]), 3 = Redis 命令
  uint8_t kind = 1;
  std::vector<std::string> args;  // select/exec: args[0]=sql,args[1..]=占位参数; redis:整条命令数组
  std::string Encode() const;
  static bool Decode(const std::string& body, DbTask& out);
};

// DB结果
struct DbResult {
  int32_t errcode = 0;      // 0 成功
  std::string errmsg;       // 失败原因
  Val data;                 // 结果树
  std::string Encode() const;
  static bool Decode(const std::string& body, DbResult& out);
};

// 把 Val 序列化进 string(内部用, Encode 已用)
void EncodeVal(std::string& s, const Val& v);
bool DecodeVal(const char*& p, const char* end, Val& v);

}  // namespace dbv

#endif