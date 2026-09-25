#include "routeTable.h"

#include "common/msg.h"

#include <google/protobuf/descriptor.h>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace RouteTable
{

//TODO g_routes可以读取yaml配置文件来将玩法路由表配置化
// ---------------------------------------------------------------------------
// 玩法路由表：Module/Method 对应关系(与 proto/gameProto.proto 及客户端保持一致)
//   lua_fn 非空 => 由 Lua 脚本实现; 为空 => 由 C++ 模块处理
// ---------------------------------------------------------------------------
static const std::vector<Route> g_routes = {

    // ---------------- PLAYER(3) 移动 ----------------
    {Module::PLAYER, Method::PLAYER_MOVE, "gs.WalkReq", "gs.WalkAck", "move.c2s_walk"},
    {Module::PLAYER, Method::PLAYER_PATH, "gs.PathReq", "gs.PathAck", "move.c2s_path"},

    // ---------------- 仅出站的推送路由（无请求体）----------------
    // req_type 为空 => 只作为出站名字解析源，不参与入站分发
    {Module::PLAYER, Method::PLAYER_PUSH_STATE, "", "gs.SCPushState", ""},
    {Module::SYS, Method::SYS_RET_TIP, "", "gs.RetTip", ""},
};

static const std::vector<Route>& Routes() { return g_routes; }

// 根据(Module,Method)找对应路由
static const std::map<std::pair<uint16_t, uint16_t>, const Route*>& C2SMap()
{
    static const std::map<std::pair<uint16_t, uint16_t>, const Route*> m = [] {
        std::map<std::pair<uint16_t, uint16_t>, const Route*> t;
        for (auto& r : g_routes)
        {
            if (r.req_type != nullptr && r.req_type[0] != '\0')
                t[{r.Module, r.Method}] = &r;
        }
        return t;
    }();
    return m;
}

static const std::map<std::string, const Route*>& NameMap()
{
    static const std::map<std::string, const Route*> m = [] {
        std::map<std::string, const Route*> t;
        for (auto& r : g_routes)
        {
            if (r.ack_type != nullptr && r.ack_type[0] != '\0')
            {
                t[r.ack_type] = &r;   // 完整名 "gs.WalkAck"
                const std::string full = r.ack_type;
                const size_t dot = full.find('.');
                if (dot != std::string::npos)
                    t[full.substr(dot + 1)] = &r;   // 短名 "WalkAck"
            }
        }
        return t;
    }();
    return m;
}

const std::vector<Route>& All() { return Routes(); }

const Route* FindC2S(uint16_t Module, uint16_t Method)
{
    const auto& m = C2SMap();
    auto it = m.find({Module, Method});
    return it == m.end() ? nullptr : it->second;
}

const Route* FindByTypeName(const std::string& msg_name)
{
    const auto& m = NameMap();
    auto it = m.find(msg_name);
    return it == m.end() ? nullptr : it->second;
}

std::vector<std::string> Validate()
{
    std::vector<std::string> errs;
    std::set<std::pair<uint16_t, uint16_t>> seen;
    std::map<std::string, const Route*> ackSeen;

    for (auto& r : g_routes)
    {
        if (r.req_type != nullptr && r.req_type[0] != '\0' &&
            !seen.insert({r.Module, r.Method}).second)
        {
            errs.push_back("duplicate (Module,Method)=" + std::to_string(r.Module) + "," +
                           std::to_string(r.Method) + " type=" + r.req_type);
        }

        auto check = [&](const char* name, const char* tag) {
            if (name == nullptr || name[0] == '\0')
                return;
            if (google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(name) == nullptr)
            {
                errs.push_back(std::string("unknown ") + tag + " message: " + name +
                               " (Module=" + std::to_string(r.Module) + ")");
            }
        };
        check(r.req_type, "req_type");
        check(r.ack_type, "ack_type");

        if (r.ack_type != nullptr && r.ack_type[0] != '\0')
        {
            const std::string full = r.ack_type;
            const size_t dot = full.find('.');
            const std::string shortName = dot == std::string::npos ? full : full.substr(dot + 1);
            for (const std::string& key : {full, shortName})
            {
                auto it = ackSeen.find(key);
                if (it != ackSeen.end() && it->second != &r)
                {
                    errs.push_back("ack name collision: " + key + " used by (" +
                                   std::to_string(it->second->Module) + "," +
                                   std::to_string(it->second->Method) + ") and (" +
                                   std::to_string(r.Module) + "," +
                                   std::to_string(r.Method) + ")");
                }
                else
                {
                    ackSeen[key] = &r;
                }
            }
        }
    }
    return errs;
}

}  // namespace RouteTable
