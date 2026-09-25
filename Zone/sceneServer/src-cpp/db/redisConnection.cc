#include "db/redisConnection.h"

#include "log/logger.h"

#include <string>
#include <vector>

#if defined(SCENE_HAS_REDIS)
#include "db/redisApi.h"
#endif

namespace
{
constexpr int32_t kErrNotConnected = 2000;
constexpr int32_t kErrBadTask      = 2001;
constexpr int32_t kErrCommand      = 2002;
constexpr int32_t kErrReply        = 2003;

void Fail(dbv::DbResult& out, int32_t code, const std::string& msg)
{
    out.errcode = code;
    out.errmsg = msg;
    out.data = dbv::Val::Nil();
}

#if defined(SCENE_HAS_REDIS)
// redisReply -> Val 递归转换
dbv::Val ReplyToVal(const redisReply* reply)
{
    if (reply == nullptr)
        return dbv::Val::Nil();

    switch (reply->type)
    {
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_STRING:
            return dbv::Val::Str(std::string(reply->str ? reply->str : "", reply->len));

        case REDIS_REPLY_INTEGER:
            return dbv::Val::Int(static_cast<int64_t>(reply->integer));

        case REDIS_REPLY_NIL:
            return dbv::Val::Nil();

        case REDIS_REPLY_ERROR:
            // 业务级错误：以 Val::Str 返回，errcode 由调用方按 errmsg 判断
            return dbv::Val::Str(std::string("ERR:") + (reply->str ? reply->str : ""));

        case REDIS_REPLY_ARRAY:
        {
            std::vector<dbv::Val> arr;
            arr.reserve(reply->elements);
            for (size_t i = 0; i < reply->elements; ++i)
                arr.push_back(ReplyToVal(reply->element[i]));
            return dbv::Val::Arr(std::move(arr));
        }

        default:
            return dbv::Val::Nil();
    }
}
#endif
}  // namespace

struct RedisConnection::Impl
{
#if defined(SCENE_HAS_REDIS)
    redisContext* ctx = nullptr;
#endif
};

RedisConnection::RedisConnection()
    : impl_(std::make_unique<Impl>())
{
}

RedisConnection::~RedisConnection()
{
    Close();
}

bool RedisConnection::IsConnected() const
{
#if defined(SCENE_HAS_REDIS)
    return impl_->ctx != nullptr && impl_->ctx->err == 0;
#else
    return false;
#endif
}

void RedisConnection::Close()
{
#if defined(SCENE_HAS_REDIS)
    if (impl_->ctx != nullptr)
    {
        ::redisFree(impl_->ctx);
        impl_->ctx = nullptr;
    }
#endif
}

bool RedisConnection::Connect(const RedisConfig& cfg)
{
    cfg_ = cfg;

#if !defined(SCENE_HAS_REDIS)
    LOG_WARNING << "redis support not compiled in, redis channel disabled";
    return false;
#else
    Close();

    timeval tv{};
    tv.tv_sec = static_cast<time_t>(cfg_.timeoutMs / 1000);
    tv.tv_usec = static_cast<suseconds_t>((cfg_.timeoutMs % 1000) * 1000);

    impl_->ctx = ::redisConnectWithTimeout(cfg_.host.c_str(), cfg_.port, tv);
    if (impl_->ctx == nullptr || impl_->ctx->err != 0)
    {
        LOG_ERROR << "redis connect " << cfg_.host << ":" << cfg_.port << " failed: "
                  << (impl_->ctx ? impl_->ctx->errstr : "alloc failed");
        Close();
        return false;
    }

    ::redisSetTimeout(impl_->ctx, tv);

    if (!cfg_.password.empty())
    {
        redisReply* r = static_cast<redisReply*>(
            ::redisCommand(impl_->ctx, "AUTH %s", cfg_.password.c_str()));
        if (r == nullptr || r->type == REDIS_REPLY_ERROR)
        {
            LOG_ERROR << "redis auth failed: " << (r && r->str ? r->str : "no reply");
            if (r) ::freeReplyObject(r);
            Close();
            return false;
        }
        ::freeReplyObject(r);
    }

    // 注意：这里不切换 database index（SELECT 会让将来无法迁移 Redis Cluster），
    // 隔离统一用 key 前缀：zone:{zoneId}:scene:{sceneId}:...
    // 为此 RedisConfig 里已经不提供 dbIndex 字段（见 db/dbConfig.h 的说明），
    // 前缀与分片的强制校验在 db/redisKey.h。

    LOG_INFO << "redis connected: " << cfg_.host << ":" << cfg_.port;
    return true;
#endif
}

bool RedisConnection::Reconnect()
{
    if (cfg_.host.empty())
        return false;
    return Connect(cfg_);
}

void RedisConnection::Execute(const dbv::DbTask& task, dbv::DbResult& out)
{
    out = dbv::DbResult{};

#if !defined(SCENE_HAS_REDIS)
    Fail(out, kErrNotConnected, "redis support not compiled in");
    return;
#else
    if (impl_->ctx == nullptr)
    {
        Fail(out, kErrNotConnected, "redis not connected");
        return;
    }
    if (task.args.empty() || task.args[0].empty())
    {
        Fail(out, kErrBadTask, "empty command");
        return;
    }

    std::vector<const char*> argv;
    std::vector<size_t> argvLen;
    argv.reserve(task.args.size());
    argvLen.reserve(task.args.size());
    for (const auto& a : task.args)
    {
        argv.push_back(a.c_str());
        argvLen.push_back(a.size());
    }

    redisReply* reply = static_cast<redisReply*>(
        ::redisCommandArgv(impl_->ctx, static_cast<int>(argv.size()), argv.data(), argvLen.data()));

    if (reply == nullptr)
    {
        const std::string err = impl_->ctx->errstr;
        Close();   // 连接已不可用，交给 DB 线程重连
        Fail(out, kErrCommand, err);
        return;
    }

    if (reply->type == REDIS_REPLY_ERROR)
    {
        const std::string err = reply->str ? reply->str : "redis error";
        ::freeReplyObject(reply);
        Fail(out, kErrReply, err);
        return;
    }

    out.errcode = 0;
    out.data = ReplyToVal(reply);
    ::freeReplyObject(reply);
#endif
}
