#ifndef CLEARMOON_SCENE_ROUTETABLE_H
#define CLEARMOON_SCENE_ROUTETABLE_H

// ============================================================================
// routeTable.h —— 协议路由注册表
//   (Module,Method) <-> protobuf 消息全名 <-> Lua 处理函数路径
//   新增玩法 = 在 proto 里加消息 + 在这里加一行即可被框架自动分发。
//
// 约束（与设计文档 3.2.3 一致）：
//   - 启动期构建、运行时‘只读’，可以被所有逻辑线程共享；
//   - 表项里只存名字字符串，绝不存 lua_State* 或任何指向具体状态的指针
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

namespace RouteTable
{

struct Route
{
    uint16_t Module;
    uint16_t Method;
    const char* req_type;        // C2S 请求消息全名(如 "gs.WalkReq")，入站分发用
    const char* ack_type;        // 出站应答/推送消息全名("gs.WalkAck")
    const char* lua_fn;          // Lua 处理函数路径("move.c2s_walk")；空 = C++ 处理
    uint32_t required_perm = 0;  // 处理该路由所需权限位(0=无需权限)
};

// 返回注册表(程序启动前静态初始化)
const std::vector<Route>& All();

// 启动自检: 返回错误明细(空 = 通过)。检查项:
//  1) (Module,Method) 重复; 2) ack 短名/全名映射冲突;
//  3) req_type/ack_type 能在 protobuf DescriptorPool 解析
std::vector<std::string> Validate();

// 入站路由: 根据网络帧头 (Module,Method) 找路由
const Route* FindC2S(uint16_t Module, uint16_t Method);
// 出站路由: 根据消息名找 Module/Method(供 C++ 侧构造 MSGTYPE_SEND 头)
const Route* FindByTypeName(const std::string& msg_name);

}  // namespace RouteTable

#endif
