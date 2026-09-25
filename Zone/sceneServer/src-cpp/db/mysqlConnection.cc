#include "db/mysqlConnection.h"

#include "log/logger.h"

#include <cstring>
#include <string>
#include <vector>

#if defined(SCENE_HAS_MYSQL)
#include <mysql.h>
#endif

namespace
{
constexpr int32_t kErrNotConnected = 1000;
constexpr int32_t kErrBadTask      = 1001;
constexpr int32_t kErrPrepare      = 1002;
constexpr int32_t kErrExecute      = 1003;
constexpr int32_t kErrFetch        = 1004;

void Fail(dbv::DbResult& out, int32_t code, const std::string& msg)
{
    out.errcode = code;
    out.errmsg = msg;
    out.data = dbv::Val::Nil();
}
}  // namespace

struct MySqlConnection::Impl
{
#if defined(SCENE_HAS_MYSQL)
    MYSQL* conn = nullptr;
#endif
};

MySqlConnection::MySqlConnection()
    : impl_(std::make_unique<Impl>())
{
}

MySqlConnection::~MySqlConnection()
{
    Close();
}

bool MySqlConnection::IsConnected() const
{
#if defined(SCENE_HAS_MYSQL)
    return impl_->conn != nullptr;
#else
    return false;
#endif
}

void MySqlConnection::Close()
{
#if defined(SCENE_HAS_MYSQL)
    if (impl_->conn != nullptr)
    {
        ::mysql_close(impl_->conn);
        impl_->conn = nullptr;
    }
#endif
}

bool MySqlConnection::Connect(const MySqlConfig& cfg)
{
    cfg_ = cfg;

#if !defined(SCENE_HAS_MYSQL)
    LOG_WARNING << "mysql support not compiled in, db channel disabled";
    return false;
#else
    Close();

    impl_->conn = ::mysql_init(nullptr);
    if (impl_->conn == nullptr)
    {
        LOG_ERROR << "mysql_init failed";
        return false;
    }

    const uint32_t connectTimeout = cfg_.connectTimeoutSec;
    const uint32_t readTimeout = cfg_.readTimeoutSec;
    const uint32_t writeTimeout = cfg_.writeTimeoutSec;
    ::mysql_options(impl_->conn, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    ::mysql_options(impl_->conn, MYSQL_OPT_READ_TIMEOUT, &readTimeout);
    ::mysql_options(impl_->conn, MYSQL_OPT_WRITE_TIMEOUT, &writeTimeout);

    if (::mysql_real_connect(impl_->conn, cfg_.host.c_str(), cfg_.user.c_str(),
                             cfg_.password.c_str(), cfg_.dbName.c_str(), cfg_.port,
                             nullptr, 0) == nullptr)
    {
        LOG_ERROR << "mysql connect " << cfg_.host << ":" << cfg_.port
                  << " failed: " << ::mysql_error(impl_->conn);
        Close();
        return false;
    }

    if (::mysql_set_character_set(impl_->conn, cfg_.charset.c_str()) != 0)
        LOG_WARNING << "mysql set charset failed: " << ::mysql_error(impl_->conn);

    LOG_INFO << "mysql connected: " << cfg_.host << ":" << cfg_.port << "/" << cfg_.dbName;
    return true;
#endif
}

bool MySqlConnection::Reconnect()
{
    if (cfg_.host.empty())
        return false;
    return Connect(cfg_);
}

void MySqlConnection::Execute(const dbv::DbTask& task, dbv::DbResult& out)
{
    out = dbv::DbResult{};

#if !defined(SCENE_HAS_MYSQL)
    Fail(out, kErrNotConnected, "mysql support not compiled in");
    return;
#else
    if (impl_->conn == nullptr)
    {
        Fail(out, kErrNotConnected, "mysql not connected");
        return;
    }
    if (task.args.empty() || task.args[0].empty())
    {
        Fail(out, kErrBadTask, "empty sql");
        return;
    }
    if (task.kind != 1 && task.kind != 2)
    {
        Fail(out, kErrBadTask, "unsupported task kind");
        return;
    }

    MYSQL_STMT* stmt = ::mysql_stmt_init(impl_->conn);
    if (stmt == nullptr)
    {
        Fail(out, kErrPrepare, "mysql_stmt_init failed");
        return;
    }

    const std::string& sql = task.args[0];
    if (::mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
    {
        const std::string err = ::mysql_stmt_error(stmt);
        ::mysql_stmt_close(stmt);
        Fail(out, kErrPrepare, err);
        return;
    }

    // ---- 绑定占位参数：统一按字符串绑定，由服务器按目标列类型隐式转换 ----
    const size_t paramCount = task.args.size() - 1;
    std::vector<MYSQL_BIND> paramBinds(paramCount);
    std::vector<unsigned long> paramLens(paramCount);
    std::vector<my_bool> paramNulls(paramCount, 0);
    for (size_t i = 0; i < paramCount; ++i)
    {
        std::memset(&paramBinds[i], 0, sizeof(MYSQL_BIND));
        paramLens[i] = static_cast<unsigned long>(task.args[i + 1].size());
        paramBinds[i].buffer_type = MYSQL_TYPE_STRING;
        paramBinds[i].buffer = const_cast<char*>(task.args[i + 1].data());
        paramBinds[i].buffer_length = paramLens[i];
        paramBinds[i].length = &paramLens[i];
        paramBinds[i].is_null = &paramNulls[i];
    }
    if (paramCount > 0 && ::mysql_stmt_bind_param(stmt, paramBinds.data()) != 0)
    {
        const std::string err = ::mysql_stmt_error(stmt);
        ::mysql_stmt_close(stmt);
        Fail(out, kErrPrepare, "bind param failed: " + err);
        return;
    }

    if (::mysql_stmt_execute(stmt) != 0)
    {
        const std::string err = ::mysql_stmt_error(stmt);
        // 连接已断：标记为未连接，让 DB 线程走重连
        const unsigned int eno = ::mysql_stmt_errno(stmt);
        ::mysql_stmt_close(stmt);
        if (eno == 2006 || eno == 2013)
            Close();
        Fail(out, kErrExecute, err);
        return;
    }

    // ---- exec：返回 [影响行数, 自增id] ----
    if (task.kind == 2)
    {
        std::vector<dbv::Val> row;
        row.push_back(dbv::Val::Int(static_cast<int64_t>(::mysql_stmt_affected_rows(stmt))));
        row.push_back(dbv::Val::Int(static_cast<int64_t>(::mysql_stmt_insert_id(stmt))));
        out.errcode = 0;
        out.data = dbv::Val::Arr(std::move(row));
        ::mysql_stmt_close(stmt);
        return;
    }

    // ---- select：按结果元数据决定每列的绑定类型（数值走 LONGLONG，其余走字符串）----
    MYSQL_RES* meta = ::mysql_stmt_result_metadata(stmt);
    if (meta == nullptr)
    {
        const std::string err = ::mysql_stmt_error(stmt);
        ::mysql_stmt_close(stmt);
        Fail(out, kErrFetch, "no result metadata: " + err);
        return;
    }

    const unsigned int fieldCount = ::mysql_num_fields(meta);
    MYSQL_FIELD* fields = ::mysql_fetch_fields(meta);

    std::vector<MYSQL_BIND> colBinds(fieldCount);
    std::vector<std::vector<char>> colBufs(fieldCount);
    std::vector<unsigned long> colLens(fieldCount, 0);
    std::vector<my_bool> colNulls(fieldCount, 0);
    std::vector<int64_t> colInts(fieldCount, 0);
    std::vector<char> colIsNum(fieldCount, 0);
    std::vector<uint8_t> colIsUnsigned(fieldCount, 0);

    for (unsigned int i = 0; i < fieldCount; ++i)
    {
        std::memset(&colBinds[i], 0, sizeof(MYSQL_BIND));
        colBinds[i].is_null = &colNulls[i];
        colBinds[i].length = &colLens[i];

        if (IS_NUM_FIELD(&fields[i]))
        {
            colIsNum[i] = 1;
            colIsUnsigned[i] = (fields[i].flags & UNSIGNED_FLAG) ? 1 : 0;
            colBinds[i].buffer_type = MYSQL_TYPE_LONGLONG;
            colBinds[i].buffer = &colInts[i];
            colBinds[i].buffer_length = sizeof(int64_t);
            colBinds[i].is_unsigned = colIsUnsigned[i];
        }
        else
        {
            // max_length 在未 store_result 时可能为 0：给一个够用的默认值，
            // 真被截断时下面用 mysql_stmt_fetch_column 取全量，不会静默丢数据
            size_t cap = fields[i].max_length > 0 ? fields[i].max_length + 1 : 1024;
            colBufs[i].resize(cap);
            colBinds[i].buffer_type = MYSQL_TYPE_STRING;
            colBinds[i].buffer = colBufs[i].data();
            colBinds[i].buffer_length = static_cast<unsigned long>(colBufs[i].size());
        }
    }

    if (::mysql_stmt_bind_result(stmt, colBinds.data()) != 0)
    {
        const std::string err = ::mysql_stmt_error(stmt);
        ::mysql_free_result(meta);
        ::mysql_stmt_close(stmt);
        Fail(out, kErrFetch, "bind result failed: " + err);
        return;
    }

    std::vector<dbv::Val> rows;
    for (;;)
    {
        const int rc = ::mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA)
            break;
        if (rc != 0 && rc != MYSQL_DATA_TRUNCATED)
        {
            const std::string err = ::mysql_stmt_error(stmt);
            ::mysql_free_result(meta);
            ::mysql_stmt_close(stmt);
            Fail(out, kErrFetch, err);
            return;
        }

        std::vector<dbv::Val> row;
        row.reserve(fieldCount);
        for (unsigned int i = 0; i < fieldCount; ++i)
        {
            if (colNulls[i])
            {
                row.push_back(dbv::Val::Nil());
                continue;
            }

            if (colIsNum[i])
            {
                row.push_back(dbv::Val::Int(colInts[i]));
                continue;
            }

            std::string text;
            if (rc == MYSQL_DATA_TRUNCATED && colLens[i] >= colBufs[i].size())
            {
                // 被截断：按真实长度重新取这一列
                std::vector<char> big(colLens[i] + 1);
                MYSQL_BIND one;
                std::memset(&one, 0, sizeof(MYSQL_BIND));
                unsigned long realLen = colLens[i];
                one.buffer_type = MYSQL_TYPE_STRING;
                one.buffer = big.data();
                one.buffer_length = static_cast<unsigned long>(big.size());
                one.length = &realLen;
                if (::mysql_stmt_fetch_column(stmt, &one, i, 0) == 0)
                    text.assign(big.data(), realLen);
            }
            else
            {
                text.assign(colBufs[i].data(), colLens[i]);
            }
            row.push_back(dbv::Val::Str(std::move(text)));
        }
        rows.push_back(dbv::Val::Arr(std::move(row)));
    }

    ::mysql_free_result(meta);
    ::mysql_stmt_close(stmt);

    out.errcode = 0;
    out.data = dbv::Val::Arr(std::move(rows));
#endif
}
