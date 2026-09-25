#include "tcpConnection.h"
#include "log/logger.h"
#include "timer/timerId.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <random>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>   // ::close（tcpConnection 的发送/关闭路径需要）


const uint32_t TcpConnection::RetransmitEntry::kBaseTimeoutMs = 200;    //初始超时重传时间为1000ms

const uint32_t TcpConnection::RetransmitEntry::kMaxTimeoutMs = 10000;    //最大延迟为10s

thread_local std::mt19937 rng(std::random_device{}());

//指数退避计时函数
//随机抖动版本
std::chrono::milliseconds caculate_backoff_jitter(
    uint32_t base_delay_ms,
    uint32_t retries,
    uint32_t max_delay_ms,
    std::mt19937& rng)
{
    uint64_t cap_ms = base_delay_ms;

    for(uint32_t i = 0; i < retries; ++i)
    {
        if(cap_ms > max_delay_ms / 2)
        {
            cap_ms = max_delay_ms;
            break;
        }
        cap_ms *=2;
    }

    cap_ms = std::min(cap_ms, static_cast<uint64_t>(max_delay_ms));

    std::uniform_int_distribution<uint64_t> dist(0, cap_ms);
    auto jitter_ms = dist(rng);

    return std::chrono::milliseconds(jitter_ms);
}


TcpConnection::TcpConnection(EventLoop* loop, std::string name, 
                            Socket socket, InetAddress& localAddr, 
                            InetAddress& peerAddr) 
                            : loop_(loop), 
                              channel_(loop,socket.Fd()),
                              socket_(std::move(socket)), 
                              localAddr_(std::move(localAddr)), 
                              peerAddr_(std::move(peerAddr)),
                              name_(name)
                              
{
    channel_.setReadCallback([this]{ handleRead(); });
    channel_.setWriteCallback([this] { handleWrite(); });
    channel_.setErrorCallback([this] { handleError(); });
    socket_.setNonBlocking(true);

    //关闭 Nagle 算法：
    // ① 消除 Nagle×延迟ACK 交互造成的 ~40ms 单次 RPC 延迟下限
    // ② 避免请求总长超过一个 MSS 时，Nagle 扣住尾段等 ACK、而应用层因包不完整
    //    无法回复应用层 ACK，造成双向等待的永久死锁
    socket_.setTcpNoDelay(true);
}

TcpConnection::~TcpConnection()
{
    assert(state_.load(std::memory_order_acquire) == kDisConnected);
    if(state_.load(std::memory_order_acquire) != kDisConnected)
        forceClose();

    if(idleTimerId_.valid())
    {
        loop_->cancel(idleTimerId_);
        idleTimerId_ = TimerId();
    }
    
    if(!pendingRetrans_.empty())
    {
        for(auto&kv : pendingRetrans_)
        {
            if(kv.second.timerId.valid())
            {
                loop_->cancel(kv.second.timerId);
            }
        }
        pendingRetrans_.clear();
    }
}

void TcpConnection::connectEstablelished()
{
    loop_->assertInLoopThread();
    assert(state_.load(std::memory_order_acquire) == kConnecting);

    channel_.enableReading();
    setState(kConnected);

    //连接建立时初始化最近活动时间并启动 IdleTimer（此后读写只更新 lastActive_，
    //不再反复 cancel+add，避免 timerfd 风暴）
    lastActive_ = Timestamp::now();
    resetIdleTimer();

    if(connectionCallback_) 
        connectionCallback_(shared_from_this());
}

void TcpConnection::connectDestroyed()
{
    loop_->assertInLoopThread();

    // 修复说明（0.13 附带）：原实现只在 connectEstablished() 里调用 connectionCallback_，
    // 关闭路径**从不通知**上层 —— 业务侧（会话表、后端连接状态机）永远收不到"连接已关闭"，
    // 只能靠内部的 closeCallback_ 回收连接对象，无法清理自己的会话状态。
    // 现在按 muduo 契约补齐：只要此前不是"已断开"，就发一次断开通知。
    bool needNotify = false;
    if(state_.load(std::memory_order_acquire) != kDisConnected)
    {
        setState(kDisConnected);
        needNotify = true;
    }

    //把空闲清理定时器置空
    if(idleTimerId_.valid())
    {
        loop_->cancel(idleTimerId_);
        idleTimerId_ = TimerId();
    }
    
    if(!pendingRetrans_.empty())
    {
        for(auto&kv : pendingRetrans_)
        {
            if(kv.second.timerId.valid())
            {
                loop_->cancel(kv.second.timerId);
            }
        }
        pendingRetrans_.clear();
    }

    // 不需要先 disableAll()，channel_.remove() 内部会完成 epoll 摘除
    channel_.remove();

    // 放在最后通知：此时定时器已清、channel 已摘除、state_ 已是 kDisConnected，
    // 回调里 conn->connected() 会正确返回 false。
    if(needNotify && connectionCallback_)
        connectionCallback_(shared_from_this());
}


void TcpConnection::shutdown()
{
    if(state_.load(std::memory_order_acquire) == kConnected)
    {
        setState(kDisConnecting);
        auto self = shared_from_this();
        loop_->runInLoop([self] { self->shutdownInLoop(); });
    }
}

void TcpConnection::forceClose()
{
    if(state_.load(std::memory_order_acquire) == kConnected || state_.load(std::memory_order_acquire) == kDisConnecting)
    {
        setState(kDisConnecting);
        auto self = shared_from_this();
        loop_->runInLoop([self]{ self->forceCloseInLoop(); });
    }
}

void TcpConnection::send(Buffer* buff)
{
    send(buff->peek(), buff->readableBytes());
}

void TcpConnection::send(const std::string& message)
{
    send(message.data(), message.size());
}
void TcpConnection::send(const void* data, size_t len)
{
    if(state_.load(std::memory_order_acquire) != kConnected) return;

    if (loop_->isInThread()) {
        sendInLoop(data, len);
    } else {
        // 为保证跨线程发送时数据有效性，拷贝数据到 shared_ptr<string>
        auto buf = std::make_shared<std::string>(static_cast<const char*>(data), len);
        // 跨线程投递：本任务由**调用方线程**投到本连接的 IO 线程，
        // 而连接可能在任务执行前就被关闭并在 IO 线程析构（connectDestroyed）。
        // 裸 this 会 use-after-free；捕获 self（shared_from_this）让对象至少活到任务执行完。
        auto self = shared_from_this();
        loop_->runInLoop([self, buf]{ self->sendInLoop(buf->data(), buf->size()); });
    }
}

//可靠传输（支持超时重传)
void TcpConnection::sendWithRetransmit(const void*data, size_t len, uint64_t seq)
{
    //构建名为SendFunc的lambda函数
    // self：SendFunc 既可能在 IO 线程内直接调用，也可能被跨线程 runInLoop 投递
    //（见本函数末尾的 else 分支），因此必须让对象活到任务执行完。
    auto self = shared_from_this();
    auto SendFunc = [self, data = std::string(static_cast<const char*>(data), len), seq] {
        //构造超时重传上下文
        RetransmitEntry entry;
        entry.seq = seq;
        entry.data = data;
        entry.retries = 0;
        self->send(entry.data.data(), entry.data.size());

        //填充待处理重传map并启动超时重传定时器
        self->pendingRetrans_[seq] = std::move(entry);
        self->resetRetransmitTimer(self->pendingRetrans_[seq]);
    };

    //确保SendFunc位于io线程中被执行
    if(loop_->isInThread())
    {
        SendFunc();
    }else {
        loop_->runInLoop(std::move(SendFunc));
    }
}

void TcpConnection::sendWithRetransmit(Buffer* buffer, uint64_t seq)
{
    sendWithRetransmit(buffer->peek(),buffer->readableBytes(), seq);
}

void TcpConnection::ackReceived(uint64_t seq)
{
    if(loop_->isInThread())
    {
        auto it = pendingRetrans_.find(seq);
        if(it != pendingRetrans_.end())
        {
            if(it->second.timerId.valid())
            {
                loop_->cancel(it->second.timerId);
            }
            pendingRetrans_.erase(seq);
        }
    }
    else {
        // 跨线程投递：捕获 self，避免连接提前析构导致 UAF（同 send()）
        auto self = shared_from_this();
        loop_->runInLoop([self, seq]{
            auto it = self->pendingRetrans_.find(seq);
            if(it != self->pendingRetrans_.end())
            {
                if(it->second.timerId.valid())
                {
                    self->loop_->cancel(it->second.timerId);
                }
                self->pendingRetrans_.erase(seq);
            }
        });

    }
}


// ========== 文件发送接口 ==========
void TcpConnection::sendFile(const std::string& filePath)
{
    if(state_.load(std::memory_order_acquire) != kConnected) return;

    if(loop_->isInThread())
    {
        sendFileInLoop(filePath);
    }
    else
    {
        // 同 send()：跨线程投递必须捕获 self，避免对象提前析构（UAF）
        auto self = shared_from_this();
        loop_->runInLoop([self, filePath] { self->sendFileInLoop(filePath); });
    }
}


//---------------private---------------
void TcpConnection::sendInLoop(const void* data, size_t len)
{
    loop_->assertInLoopThread();
    if(state_.load(std::memory_order_acquire) == kDisConnected) return;

    ssize_t n = 0;
    if(!channel_.isWriting() && writeBuffer_.readableBytes() == 0)
    {
        // MSG_NOSIGNAL：对端已关闭时写不触发 SIGPIPE，避免整个进程被信号杀死
        n = ::send(channel_.getFd(), data, len, MSG_NOSIGNAL);
        if(n >= 0)
        {
            //触发超时重传时也需更新最近活动时间
            touchActivity();
            
            if(static_cast<size_t>(n) == len)
            {
                if(writeCompleteCallback_) writeCompleteCallback_(shared_from_this());
                return;
            }
        }
        else
        {
            n = 0;
            if(errno != EAGAIN)
            {
                //TODO 错误处理
            }
        }
    }

    if(static_cast<size_t>(n) < len)
    {
        writeBuffer_.append(static_cast<const char*>(data) + n, len - n);
        if(!channel_.isWriting())
        {
            channel_.enableWriting();
        }
    }

    if(state_.load(std::memory_order_acquire) == kDisConnecting && writeBuffer_.readableBytes() == 0)
        shutdownInLoop();
}

void TcpConnection::sendFileInLoop(const std::string& filePath)
{
    loop_->assertInLoopThread();
    if(state_.load(std::memory_order_acquire) == kDisConnected) return;

    // 打开文件
    int fd = ::open(filePath.c_str(), O_RDONLY);
    if(fd < 0)
    {
        LOG_INFO<< "Can't open the file!";
        // 文件无法打开，强制关闭连接
        forceCloseInLoop();
        return;
    }

    LOG_INFO << "Open The File!";

    // 获取文件大小
    struct stat st;
    if(::fstat(fd, &st) < 0)
    {
        ::close(fd);
        forceCloseInLoop();
        return;
    }

    // 设置文件发送状态
    fileFd_ = fd;
    fileSentOffset_ = 0;
    fileTotalSize_ = st.st_size;
    sendingFile_ = true;

    // 启用 EPOLLOUT 事件，handleWrite 中会用 sendfile 发送
    channel_.enableWriting();
}



void TcpConnection::handleRead()
{
    loop_->assertInLoopThread();

    int savedErrno = 0;
    ssize_t n = readBuffer_.readFd(channel_.getFd(), &savedErrno);

    Timestamp t = Timestamp::now();
    if(n > 0 )
    {
        //更新最近活动时间（不操作定时器）
        touchActivity();

        if(messageCallback_) messageCallback_(shared_from_this(), &readBuffer_, t);
    }
    else if(n == 0)
    {
        handleClose();
    }
    else {
        if(savedErrno != EAGAIN)
        LOG_ERROR << "read Buffer error!";
        handleError();
    }
        
}

void TcpConnection::handleWrite()
{
    loop_->assertInLoopThread();

    if(!channel_.isWriting()) return;

    //更新最近活动时间（不操作定时器）
    touchActivity();

    // ========== 先排空 writeBuffer_（比如文件模式下响应头可能还未发送完）==========
    if(writeBuffer_.readableBytes() > 0)
    {
        int savedErrno = 0;
        ssize_t n = writeBuffer_.WriteFd(channel_.getFd(), &savedErrno);
        if(n > 0)
        {
            if(writeBuffer_.readableBytes() == 0 && !sendingFile_)
                channel_.disableWriting();
            if(writeCompleteCallback_)
                writeCompleteCallback_(shared_from_this());
        }
        else {
            if(savedErrno != EAGAIN)
                handleError();
        }
        // 如果 writeBuffer_ 还有数据未发完，等待下次 EPOLLOUT
        if(writeBuffer_.readableBytes() > 0)
            return;
    }

    // ========== 文件发送模式（sendfile 零拷贝）==========
    if(sendingFile_ && fileFd_ >= 0)
    {
        const size_t chunkSize = 64 * 1024; // 每次最多 64KB
        ssize_t sent = ::sendfile(channel_.getFd(), fileFd_, &fileSentOffset_, chunkSize);

        if(sent > 0)
        {
            if(fileSentOffset_ >= fileTotalSize_)
            {
                // 文件发送完毕
                ::close(fileFd_);
                fileFd_ = -1;
                sendingFile_ = false;
                channel_.disableWriting();

                LOG_INFO<<"Send the file Sucesslly";

                if(writeCompleteCallback_)
                    writeCompleteCallback_(shared_from_this());
            }
            // 否则继续等待下一次 EPOLLOUT 事件
        }
        else if(sent < 0)
        {
            int err = errno;
            if(err != EAGAIN)
            {
                LOG_ERROR<< "Error: Send the file";
                // 发送出错
                ::close(fileFd_);
                fileFd_ = -1;
                sendingFile_ = false;
                channel_.disableWriting();
                handleError();
            }
            // EAGAIN 仅等待下一次重试
        }

        return;
    }

    // ========== 普通 buffer 发送模式（仅在 writeBuffer_ 非空且非文件模式时进入）==========
    if(writeBuffer_.readableBytes() > 0)
    {
        int savedErrno = 0;
        ssize_t n = writeBuffer_.WriteFd(channel_.getFd(), &savedErrno);

        if(n > 0)
        {
            if(writeBuffer_.readableBytes() == 0)
                channel_.disableWriting();
            if(writeCompleteCallback_)
                writeCompleteCallback_(shared_from_this());
            if(state_.load(std::memory_order_acquire) == kDisConnecting)
                shutdownInLoop();
        }
        else {
            if(savedErrno != EAGAIN)
            //TODO 处理错误
            handleError();
        }
    }
}

void TcpConnection::handleClose()
{
    LOG_INFO << "Handle Close";
    
    loop_->assertInLoopThread();
    
    // 防重入：如果已经断开，不再重复处理
    if(state_.load(std::memory_order_acquire) == kDisConnected)
        return;
    
    assert(state_.load(std::memory_order_acquire) == kConnected || state_.load(std::memory_order_acquire) == kDisConnecting);
    setState(kDisConnected);
    // channel_.remove() 内部会调用 loop_->removeChannel() 完成 epoll 移除
    // 不需要先调用 disableAll()，避免重复 epoll_ctl DEL 导致的断言失败
    channel_.remove();

    // 清理文件发送状态
    if(fileFd_ >= 0)
    {
        ::close(fileFd_);
        fileFd_ = -1;
    }
    fileSentOffset_ = 0;
    fileTotalSize_ = 0;
    sendingFile_ = false;

    if(idleTimerId_.valid())
    {
        loop_->cancel(idleTimerId_);
        idleTimerId_ = TimerId();
    }
    
    if(!pendingRetrans_.empty())
    {
        for(auto&kv : pendingRetrans_)
        {
            if(kv.second.timerId.valid())
            {
                loop_->cancel(kv.second.timerId);
            }
        }
        pendingRetrans_.clear();
    }

    // 通知"连接已断开"，再交回内部回收（**顺序很重要：先业务、后内部**）。
    //
    // 修复说明（0.13）：原实现只调用 closeCallback_，而 connectDestroyed() 里也完全没有通知，
    // 于是业务侧（会话表、后端连接状态机）永远收不到"连接已关闭"。
    // 真实关闭路径是 handleClose()（对端 FIN、forceClose、shutdown 都会走到这里），
    // 它在本函数开头就把状态置成了 kDisConnected，因此"状态变化才通知"的判断在这里并不生效，
    // 必须在此处显式通知。connectDestroyed() 里保留的那次通知用于它自己完成状态迁移的场景，
    // 两处由状态守卫保证**一个连接只通知一次**。
    if(connectionCallback_)
        connectionCallback_(shared_from_this());

    if(closeCallback_)
        closeCallback_(shared_from_this());
}

void TcpConnection::handleError()
{
    //TODO 日志记录错误
    handleClose();
}

void TcpConnection::shutdownInLoop()
{
    loop_->assertInLoopThread();
    if(!channel_.isWriting())
    {
        // 无待写数据：关闭写端并立即清理连接
        // handleClose → kDisConnected（避免依赖对端 FIN，EventLoop 退出后不再 poll）
        socket_.shutdownWrite();
        handleClose();
    }
    else
    {
        // 有待写数据：先关闭写端，等 sendInLoop 排空 writeBuffer_ 后会再次调用
        // shutdownInLoop()，届时 isWriting 为 false 进入上面分支完成清理
        socket_.shutdownWrite();
    }
}

void TcpConnection::forceCloseInLoop()
{
    loop_->assertInLoopThread();
    if(state_.load(std::memory_order_acquire) == kConnected || state_.load(std::memory_order_acquire) == kDisConnecting)
        handleClose();
}

void TcpConnection::resetRetransmitTimer(RetransmitEntry& entry)
{
    if(entry.timerId.valid())
    {
        loop_->cancel(entry.timerId);
        entry.timerId = TimerId();
    }

    //指数退避计算时间
    auto timeout = caculate_backoff_jitter(RetransmitEntry::kBaseTimeoutMs, entry.retries, RetransmitEntry::kMaxTimeoutMs, rng);

    auto seconds = timeout.count() / 1000.0;
    // 说明（为什么这里的裸 this 是安全的）：定时器回调与对象**同线程**——都跑在本连接的
    // IO 线程；而且对象析构（同样发生在 IO 线程内）会先 cancel 掉相关 timerId，
    // 取消任务在下一轮事件处理之前生效，因此不会出现"对象已析构 + 回调仍触发"。
    // 跨线程投递的场景必须改用 shared_from_this（见 send() / sendWithRetransmit()）。
    entry.timerId = loop_->runAfter(seconds, [this, seq = entry.seq]{
        onRetransmitTimeout(seq);
    });
}

void TcpConnection::onRetransmitTimeout(uint64_t seq)
{
    loop_->assertInLoopThread();
    auto it = pendingRetrans_.find(seq);
    if(it == pendingRetrans_.end()) return;

    auto &entry = it->second;

    if(entry.retries >= RetransmitEntry::kMaxRetries) 
    {
        LOG_WARNING<< "超时重传超出最大重传数, seq = "<<seq;
        pendingRetrans_.erase(it);
        forceCloseInLoop();
        return;
    }
    
    sendInLoop(entry.data.data(), entry.data.size());
    entry.retries++;
    resetRetransmitTimer(entry);
}


void TcpConnection::resetIdleTimer()
{
    //若空闲处理定时器已经存在则先cancel再重新添加
    if(idleTimerId_.valid())
    {
        loop_->cancel(idleTimerId_);
        idleTimerId_ = TimerId();
    }

    // 说明（为什么这里的裸 this 是安全的）：定时器回调与对象**同线程**——都跑在本连接的
    // IO 线程；而且对象析构（同样发生在 IO 线程内）会先 cancel 掉相关 timerId，
    // 取消任务在下一轮事件处理之前生效，因此不会出现"对象已析构 + 回调仍触发"。
    // 跨线程投递的场景必须改用 shared_from_this（见 send() / sendWithRetransmit()）。
    idleTimerId_ = loop_->runAfter(kTimeoutSeconds_, [this] { onIdleTimeout(); });
}

void TcpConnection::touchActivity()
{
    //只更新时间戳；定时器到期时再按剩余时间重调度，避免高频 cancel+add 风暴
    lastActive_ = Timestamp::now();
}

void TcpConnection::onIdleTimeout()
{ 
    loop_->assertInLoopThread();

    const int64_t timeoutUs = static_cast<int64_t>(kTimeoutSeconds_ * 1000000);
    int64_t elapsedUs = Timestamp::now().getMicroSecond() - lastActive_.getMicroSecond();

    //距上次活动已超过空闲阈值：断开连接
    if(elapsedUs >= timeoutUs)
    {
        forceCloseInLoop();
        return;
    }

    //期间仍有活动：按剩余时间重新调度（定时器到期前零 syscall）
    double remain = (timeoutUs - elapsedUs) / 1000000.0;
    idleTimerId_ = loop_->runAfter(remain, [this]{ onIdleTimeout(); });
}