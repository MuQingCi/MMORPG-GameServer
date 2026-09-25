#ifndef CLEARMOON_GATEWAY_COMMON_CALLBACKS_H
#define CLEARMOON_GATEWAY_COMMON_CALLBACKS_H

#include "base/buffer.h"
#include "common/timestamp.h"

#include <functional>
#include <memory>

class TcpConnection;

using ReadCallback = std::function<void()>;
using WriteCallback = std::function<void()>;
using ErrorCallback = std::function<void()>;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, Timestamp)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;

using CloseCallback = std::function<void(const TcpConnectionPtr&)>;

#endif