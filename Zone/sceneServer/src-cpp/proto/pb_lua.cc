#include "proto/pb_lua.h"

#include "log/logger.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <lua.hpp>

#include <cstdint>
#include <string>

namespace pbl
{
namespace
{
using google::protobuf::Descriptor;
using google::protobuf::EnumDescriptor;
using google::protobuf::EnumValueDescriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::Reflection;

// protobuf v3x 起 name()/full_name() 返回 std::string_view：
// Lua C API 需要 C 串、日志流也没有 string_view 重载，这里统一转换
inline std::string NameStr(const FieldDescriptor* f) { return std::string(f->name()); }
inline std::string NameStr(const EnumValueDescriptor* v) { return std::string(v->name()); }
inline std::string FullNameStr(const FieldDescriptor* f) { return std::string(f->full_name()); }

// ---- 标量：Lua 值（栈上 idx）-> Message field ----
bool SetScalar(lua_State* L, int idx, const FieldDescriptor* f, Message* msg, const Reflection* refl)
{
    switch (f->cpp_type())
    {
        case FieldDescriptor::CPPTYPE_INT32:
            refl->SetInt32(msg, f, static_cast<int32_t>(luaL_checkinteger(L, idx)));
            return true;
        case FieldDescriptor::CPPTYPE_INT64:
            refl->SetInt64(msg, f, static_cast<int64_t>(luaL_checkinteger(L, idx)));
            return true;
        case FieldDescriptor::CPPTYPE_UINT32:
            refl->SetUInt32(msg, f, static_cast<uint32_t>(luaL_checkinteger(L, idx)));
            return true;
        case FieldDescriptor::CPPTYPE_UINT64:
            refl->SetUInt64(msg, f, static_cast<uint64_t>(luaL_checkinteger(L, idx)));
            return true;
        case FieldDescriptor::CPPTYPE_DOUBLE:
            refl->SetDouble(msg, f, static_cast<double>(luaL_checknumber(L, idx)));
            return true;
        case FieldDescriptor::CPPTYPE_FLOAT:
            refl->SetFloat(msg, f, static_cast<float>(luaL_checknumber(L, idx)));
            return true;
        case FieldDescriptor::CPPTYPE_BOOL:
            refl->SetBool(msg, f, lua_toboolean(L, idx) != 0);
            return true;
        case FieldDescriptor::CPPTYPE_STRING:
        {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            refl->SetString(msg, f, std::string(s ? s : "", len));
            return true;
        }
        case FieldDescriptor::CPPTYPE_ENUM:
        {
            const EnumDescriptor* ed = f->enum_type();
            const EnumValueDescriptor* ev = nullptr;
            // 优先按字符串名（可读性 + 向前兼容），也接受数字
            if (lua_type(L, idx) == LUA_TSTRING)
            {
                const char* name = lua_tostring(L, idx);
                ev = ed->FindValueByName(name ? name : "");
                if (ev == nullptr)
                {
                    LOG_WARNING << "pb_lua: unknown enum name " << (name ? name : "")
                                << " for field " << std::string(FullNameStr(f));
                    return false;
                }
            }
            else
            {
                ev = ed->FindValueByNumber(static_cast<int>(luaL_checkinteger(L, idx)));
                if (ev == nullptr)
                {
                    LOG_WARNING << "pb_lua: unknown enum number for field "
                                << std::string(FullNameStr(f));
                    return false;
                }
            }
            refl->SetEnum(msg, f, ev);
            return true;
        }
        case FieldDescriptor::CPPTYPE_MESSAGE:
            return TableToMessage(L, idx, refl->MutableMessage(msg, f));
        default:
            return false;
    }
}

// ---- 标量：Message field -> Lua 值（压栈）----
void PushScalar(lua_State* L, const FieldDescriptor* f, const Message& msg, const Reflection* refl)
{
    switch (f->cpp_type())
    {
        case FieldDescriptor::CPPTYPE_INT32:
            lua_pushinteger(L, refl->GetInt32(msg, f));
            break;
        case FieldDescriptor::CPPTYPE_INT64:
            lua_pushinteger(L, static_cast<lua_Integer>(refl->GetInt64(msg, f)));
            break;
        case FieldDescriptor::CPPTYPE_UINT32:
            lua_pushinteger(L, static_cast<lua_Integer>(refl->GetUInt32(msg, f)));
            break;
        case FieldDescriptor::CPPTYPE_UINT64:
            lua_pushinteger(L, static_cast<lua_Integer>(refl->GetUInt64(msg, f)));
            break;
        case FieldDescriptor::CPPTYPE_DOUBLE:
            lua_pushnumber(L, refl->GetDouble(msg, f));
            break;
        case FieldDescriptor::CPPTYPE_FLOAT:
            lua_pushnumber(L, refl->GetFloat(msg, f));
            break;
        case FieldDescriptor::CPPTYPE_BOOL:
            lua_pushboolean(L, refl->GetBool(msg, f) ? 1 : 0);
            break;
        case FieldDescriptor::CPPTYPE_STRING:
        {
            const std::string& s = refl->GetString(msg, f);
            lua_pushlstring(L, s.data(), s.size());
            break;
        }
        case FieldDescriptor::CPPTYPE_ENUM:
        {
            const EnumValueDescriptor* ev = refl->GetEnum(msg, f);
            lua_pushstring(L, NameStr(ev).c_str());
            break;
        }
        case FieldDescriptor::CPPTYPE_MESSAGE:
        {
            const Message& sub = refl->GetMessage(msg, f);
            if (!MessageToTable(L, &sub))
                lua_newtable(L);
            break;
        }
        default:
            lua_pushnil(L);
            break;
    }
}

// map 字段：Lua 表 key -> value
bool MapToMessage(lua_State* L, int idx, const FieldDescriptor* f, Message* msg, const Reflection* refl)
{
    if (lua_type(L, idx) != LUA_TTABLE)
        return false;

    const FieldDescriptor* keyF = f->message_type()->field(0);
    const FieldDescriptor* valF = f->message_type()->field(1);

    lua_pushnil(L);
    while (lua_next(L, idx) != 0)
    {
        // 栈：... key value
        Message* entry = refl->AddMessage(msg, f);
        const Reflection* erefl = entry->GetReflection();

        // map 的 key 一定是标量
        if (!SetScalar(L, -2, keyF, entry, erefl))
        {
            lua_pop(L, 2);
            return false;
        }
        if (valF->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
        {
            if (!TableToMessage(L, -1, erefl->MutableMessage(entry, valF)))
            {
                lua_pop(L, 2);
                return false;
            }
        }
        else if (!SetScalar(L, -1, valF, entry, erefl))
        {
            lua_pop(L, 2);
            return false;
        }

        lua_pop(L, 1);   // 弹 value，保留 key 供 lua_next
    }
    return true;
}

void MapToTable(lua_State* L, const FieldDescriptor* f, const Message& msg, const Reflection* refl)
{
    const Reflection* mrefl = msg.GetReflection();
    const int count = mrefl->FieldSize(msg, f);
    const FieldDescriptor* keyF = f->message_type()->field(0);
    const FieldDescriptor* valF = f->message_type()->field(1);

    lua_newtable(L);
    for (int i = 0; i < count; ++i)
    {
        const Message& entry = mrefl->GetRepeatedMessage(msg, f, i);
        const Reflection* erefl = entry.GetReflection();

        PushScalar(L, keyF, entry, erefl);
        if (valF->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
        {
            if (!MessageToTable(L, &erefl->GetMessage(entry, valF)))
                lua_newtable(L);
        }
        else
        {
            PushScalar(L, valF, entry, erefl);
        }
        lua_rawset(L, -3);
    }
    (void)refl;
}
}  // namespace

google::protobuf::Message* NewByName(const std::string& fullName)
{
    const Descriptor* desc = google::protobuf::DescriptorPool::generated_pool()
                                 ->FindMessageTypeByName(fullName);
    if (desc == nullptr)
    {
        LOG_WARNING << "pb_lua: unknown message type: " << fullName;
        return nullptr;
    }

    const Message* proto = google::protobuf::MessageFactory::generated_factory()->GetPrototype(desc);
    if (proto == nullptr)
    {
        LOG_WARNING << "pb_lua: no prototype for type: " << fullName;
        return nullptr;
    }
    return proto->New();
}

bool TypeExists(const std::string& fullName)
{
    return google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(fullName) != nullptr;
}

bool TableToMessage(lua_State* L, int idx, google::protobuf::Message* msg)
{
    if (L == nullptr || msg == nullptr)
        return false;
    if (lua_type(L, idx) != LUA_TTABLE)
        return false;

    idx = lua_absindex(L, idx);
    const Descriptor* desc = msg->GetDescriptor();
    const Reflection* refl = msg->GetReflection();

    const int fieldCount = desc->field_count();
    for (int i = 0; i < fieldCount; ++i)
    {
        const FieldDescriptor* f = desc->field(i);

        lua_getfield(L, idx, NameStr(f).c_str());
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            continue;   // 未提供的字段保持"未设置"，不写默认值
        }

        bool ok = true;
        if (f->is_map())
        {
            ok = MapToMessage(L, -1, f, msg, refl);
        }
        else if (f->is_repeated())
        {
            if (lua_type(L, -1) != LUA_TTABLE)
            {
                ok = false;
            }
            else
            {
                const size_t n = lua_rawlen(L, -1);
                for (size_t j = 1; j <= n && ok; ++j)
                {
                    lua_rawgeti(L, -1, static_cast<lua_Integer>(j));
                    if (f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
                        ok = TableToMessage(L, -1, refl->AddMessage(msg, f));
                    else if (f->cpp_type() == FieldDescriptor::CPPTYPE_ENUM)
                    {
                        // repeated enum 必须走 AddEnum，不能走 SetEnum（单值接口）
                        const EnumDescriptor* ed = f->enum_type();
                        const EnumValueDescriptor* ev = nullptr;
                        if (lua_type(L, -1) == LUA_TSTRING)
                        {
                            const char* name = lua_tostring(L, -1);
                            ev = ed->FindValueByName(name ? name : "");
                        }
                        else
                        {
                            ev = ed->FindValueByNumber(static_cast<int>(luaL_checkinteger(L, -1)));
                        }
                        if (ev == nullptr)
                            ok = false;
                        else
                            refl->AddEnum(msg, f, ev);
                    }
                    else
                    {
                        // 标量 repeated：先 AddMessage 不适用，改用 SetRepeated*
                        switch (f->cpp_type())
                        {
                            case FieldDescriptor::CPPTYPE_INT32:
                                refl->AddInt32(msg, f, static_cast<int32_t>(luaL_checkinteger(L, -1)));
                                break;
                            case FieldDescriptor::CPPTYPE_INT64:
                                refl->AddInt64(msg, f, static_cast<int64_t>(luaL_checkinteger(L, -1)));
                                break;
                            case FieldDescriptor::CPPTYPE_UINT32:
                                refl->AddUInt32(msg, f, static_cast<uint32_t>(luaL_checkinteger(L, -1)));
                                break;
                            case FieldDescriptor::CPPTYPE_UINT64:
                                refl->AddUInt64(msg, f, static_cast<uint64_t>(luaL_checkinteger(L, -1)));
                                break;
                            case FieldDescriptor::CPPTYPE_DOUBLE:
                                refl->AddDouble(msg, f, luaL_checknumber(L, -1));
                                break;
                            case FieldDescriptor::CPPTYPE_FLOAT:
                                refl->AddFloat(msg, f, static_cast<float>(luaL_checknumber(L, -1)));
                                break;
                            case FieldDescriptor::CPPTYPE_BOOL:
                                refl->AddBool(msg, f, lua_toboolean(L, -1) != 0);
                                break;
                            case FieldDescriptor::CPPTYPE_STRING:
                            {
                                size_t len = 0;
                                const char* s = lua_tolstring(L, -1, &len);
                                refl->AddString(msg, f, std::string(s ? s : "", len));
                                break;
                            }
                            default:
                                ok = false;
                                break;
                        }
                    }
                    lua_pop(L, 1);
                }
            }
        }
        else
        {
            ok = SetScalar(L, -1, f, msg, refl);
        }

        lua_pop(L, 1);
        if (!ok)
        {
            LOG_WARNING << "pb_lua: convert table->message failed at field " << FullNameStr(f);
            return false;
        }
    }
    return true;
}

bool MessageToTable(lua_State* L, const google::protobuf::Message* msg)
{
    if (L == nullptr || msg == nullptr)
        return false;

    const Descriptor* desc = msg->GetDescriptor();
    const Reflection* refl = msg->GetReflection();

    lua_newtable(L);   // 栈顶 = 结果表
    const int tableIdx = lua_gettop(L);

    const int fieldCount = desc->field_count();
    for (int i = 0; i < fieldCount; ++i)
    {
        const FieldDescriptor* f = desc->field(i);

        if (f->is_map())
        {
            // 空 map 也输出空表，脚本侧可以无脑 pairs
            MapToTable(L, f, *msg, refl);
            lua_setfield(L, tableIdx, NameStr(f).c_str());
            continue;
        }

        if (f->is_repeated())
        {
            const int n = refl->FieldSize(*msg, f);
            lua_newtable(L);
            for (int j = 0; j < n; ++j)
            {
                if (f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
                {
                    if (!MessageToTable(L, &refl->GetRepeatedMessage(*msg, f, j)))
                        lua_newtable(L);
                }
                else if (f->cpp_type() == FieldDescriptor::CPPTYPE_ENUM)
                {
                    lua_pushstring(L, NameStr(refl->GetRepeatedEnum(*msg, f, j)).c_str());
                }
                else
                {
                    switch (f->cpp_type())
                    {
                        case FieldDescriptor::CPPTYPE_INT32:
                            lua_pushinteger(L, refl->GetRepeatedInt32(*msg, f, j));
                            break;
                        case FieldDescriptor::CPPTYPE_INT64:
                            lua_pushinteger(L, static_cast<lua_Integer>(refl->GetRepeatedInt64(*msg, f, j)));
                            break;
                        case FieldDescriptor::CPPTYPE_UINT32:
                            lua_pushinteger(L, static_cast<lua_Integer>(refl->GetRepeatedUInt32(*msg, f, j)));
                            break;
                        case FieldDescriptor::CPPTYPE_UINT64:
                            lua_pushinteger(L, static_cast<lua_Integer>(refl->GetRepeatedUInt64(*msg, f, j)));
                            break;
                        case FieldDescriptor::CPPTYPE_DOUBLE:
                            lua_pushnumber(L, refl->GetRepeatedDouble(*msg, f, j));
                            break;
                        case FieldDescriptor::CPPTYPE_FLOAT:
                            lua_pushnumber(L, refl->GetRepeatedFloat(*msg, f, j));
                            break;
                        case FieldDescriptor::CPPTYPE_BOOL:
                            lua_pushboolean(L, refl->GetRepeatedBool(*msg, f, j) ? 1 : 0);
                            break;
                        case FieldDescriptor::CPPTYPE_STRING:
                        {
                            const std::string& s = refl->GetRepeatedString(*msg, f, j);
                            lua_pushlstring(L, s.data(), s.size());
                            break;
                        }
                        default:
                            lua_pushnil(L);
                            break;
                    }
                }
                lua_rawseti(L, -2, j + 1);
            }
            lua_setfield(L, tableIdx, NameStr(f).c_str());
            continue;
        }

        // 单值：proto3 下判 has 无意义（标量没有 presence），但 message 字段有
        if (f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE && !refl->HasField(*msg, f))
            continue;

        PushScalar(L, f, *msg, refl);
        lua_setfield(L, tableIdx, NameStr(f).c_str());
    }

    return true;
}

bool BodyToTable(lua_State* L, const std::string& fullName, const std::string& body)
{
    google::protobuf::Message* msg = NewByName(fullName);
    if (msg == nullptr)
        return false;

    const bool parsed =
        (!body.empty() && msg->ParseFromArray(body.data(), static_cast<int>(body.size()))) ||
        (body.empty() && msg->ParseFromArray(nullptr, 0));

    if (!parsed)
    {
        LOG_WARNING << "pb_lua: parse body failed, type=" << fullName << " size=" << body.size();
        delete msg;
        return false;
    }

    const bool ok = MessageToTable(L, msg);
    delete msg;
    return ok;
}

bool TableToBody(lua_State* L, int idx, const std::string& fullName, std::string& out)
{
    google::protobuf::Message* msg = NewByName(fullName);
    if (msg == nullptr)
        return false;

    if (!TableToMessage(L, idx, msg))
    {
        delete msg;
        return false;
    }

    out = msg->SerializeAsString();
    delete msg;
    return true;
}

}  // namespace pbl
