#ifndef CLEARMOON_PROTO_PB_LUA_H
#define CLEARMOON_PROTO_PB_LUA_H

#include <string>

namespace google
{
namespace protobuf
{
class Message;
}
}  // namespace google

struct lua_State;

/**
 * @brief protobuf <-> Lua 表 的桥
 *
 * 为什么必须有它：
 *   逻辑线程收到 Msg::body 是 protobuf 字节流，而 Lua 脚本要的是 table；
 *   Lua 侧 net.send 又要把 table 转回字节流。没有这层，"收包 -> 反序列化 ->
 *   业务 -> 回包"这条竖线在 Lua 上是断的。
 *
 * 线程安全：
 *   - Descriptor / DescriptorPool / MessageFactory 是不可变数据，多线程只读安全；
 *   - Message 实例必须 per-call 或 per-thread，本模块一律在调用内创建/销毁，
 *     不缓存任何 Message*。
 *
 * 约定：
 *   - enum 用**字符串名**表示（lua 侧可读、向前兼容更好）；也接受数字；
 *   - repeated 字段用 Lua 数组（1 起）；
 *   - map 字段用 Lua 表（key -> value）；
 *   - 未设置的字段不写进 table，读时返回 nil。
 */
namespace pbl
{

// 按全名（"gs.RetTip"）创建消息实例；失败返回 nullptr（调用方负责 delete）
google::protobuf::Message* NewByName(const std::string& fullName);

// 校验名字是否存在（启动自检用，不产生实例）
bool TypeExists(const std::string& fullName);

// Lua 表（栈上 idx）-> Message。失败返回 false，并保持 Lua 栈平衡
bool TableToMessage(lua_State* L, int idx, google::protobuf::Message* msg);

// Message -> Lua 表（压栈，成功时栈顶是表，返回 true）
bool MessageToTable(lua_State* L, const google::protobuf::Message* msg);

// 便捷：body 字节流 -> Lua 表（压栈）
bool BodyToTable(lua_State* L, const std::string& fullName, const std::string& body);

// 便捷：Lua 表 -> body 字节流
bool TableToBody(lua_State* L, int idx, const std::string& fullName, std::string& out);

}  // namespace pbl

#endif
