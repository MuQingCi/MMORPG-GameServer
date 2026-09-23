#ifndef CLEARMOON_GATEWAY_TCPCONNECTION_H
#define CLEARMOON_GATEWAY_TCPCONNECTION_H

#include "common/noncopy.h"
#include "common/callbacks.h"
#include "event/channel.h"
#include "event/eventLoop.h"
#include "inetAddress.h"
#include "socket.h"
#include "common/buffer.h"
#include "timer/timerId.h"
#include "timer/timer.h"
#include "common/timestamp.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

class TcpConnection : public noncopyable,
                      public std::enable_shared_from_this<TcpConnection>
{
public:
    TcpConnection(EventLoop* loop, std::string name, Socket socket, InetAddress& localAddr, InetAddress& peerAddr);
    ~TcpConnection();

    //TcpServer调用
    void connectEstablelished();
    void connectDestroyed();
   

    void shutdown();   //优雅关闭
    void forceClose(); //强制关闭

    //设置回调函数
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }

    //获取本类成员变量
    const std::string name() const { return name_; }
    EventLoop* getLoop() const { return loop_; }
    const InetAddress& getLocalAddr() const { return localAddr_; }
    const InetAddress& getPeerAddr() const { return peerAddr_; }

    bool connected() const { return state_.load(std::memory_order_acquire) == kConnected; }

    void send(Buffer* buff);
    void send(const std::string& message);
    void send(const void*data, size_t len);

    //可靠传输（支持超时重传)
    void sendWithRetransmit(const void*data, size_t len, uint64_t seq);
    void sendWithRetransmit(Buffer* buffer, uint64_t seq);

    void ackReceived(uint64_t seq);

    // ========== 文件发送接口 ==========
    /**
     * @brief 使用 sendfile 零拷贝发送文件
     * @param filePath 文件路径
     */
    void sendFile(const std::string& filePath);

private:
    enum StateE{
        kConnecting,
        kConnected,
        kDisConnecting,
        kDisConnected
    };

    //发送函数
    void sendInLoop(const void*data, size_t len);

    //文件发送函数
    void sendFileInLoop(const std::string& filePath);

    //处理对应消息函数
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    //优雅关闭以及强制关闭
    void shutdownInLoop();
    void forceCloseInLoop();

    /**
     * @brief 定时器相关函数
            resetRetransmitTimer()
            onRetransmitTimeout()
            resetIdleTimer()
            onIdleTimeout
     * 
     */
    //超时重传相关
    struct RetransmitEntry{
        uint64_t seq; //消息序列号
        std::string data;
        uint32_t retries = 0;
        TimerId timerId;
        static const uint32_t kMaxRetries = 5;
        static const uint32_t kBaseTimeoutMs;
        static const uint32_t kMaxTimeoutMs;
    };

    void resetRetransmitTimer(RetransmitEntry& entry);
    void onRetransmitTimeout(uint64_t seq);

    std::map<uint64_t, RetransmitEntry> pendingRetrans_;

    //重置空闲处理定时器
    void resetIdleTimer();
    //执行清理空闲连接任务
    void onIdleTimeout();
    //更新最近活动时间戳（只更新时间戳、不操作定时器，避免高频 cancel+add 定时器风暴）
    void touchActivity();

    void setState(StateE s) { state_.store(s,std::memory_order_release); }

    EventLoop* loop_;
    Channel channel_;
    Socket socket_;
    InetAddress localAddr_;
    InetAddress peerAddr_;

    std::string name_;
    std::atomic<StateE> state_{kConnecting};

    //读写Buffer
    Buffer writeBuffer_;
    Buffer readBuffer_;

    //各类回调函数
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;

    // ========== 文件发送状态 ==========
    int fileFd_ = -1;            // 当前要发送的文件描述符
    off_t fileSentOffset_ = 0;   // 已发送的字节偏移
    off_t fileTotalSize_ = 0;    // 文件总大小
    bool sendingFile_ = false;   // 是否正在发送文件

    //========== 定时器Id ==========
    TimerId readTimerId_;
    TimerId writeTimerId_;

    //空闲定时器处理相关
    //定时清理空闲连接定时器
    TimerId idleTimerId_;
    Timestamp lastActive_;          //最近一次读写活动的时间戳（用于空闲判定）

    const double kTimeoutSeconds_ = 60;  //超时时间
};

#endif