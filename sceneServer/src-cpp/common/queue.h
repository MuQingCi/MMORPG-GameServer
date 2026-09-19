#ifndef CLEARMOON_COMMON_QUEUE_H
#define CLEARMOON_COMMON_QUEUE_H

#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>
#include <cstddef>
#include <chrono>

/**
 * @brief 阻塞MPSC消息队列封装

语义约束（必须遵守）：
 *   - 多生产者：任意线程可调用 try_push / try_push_bulk
 *   - 单消费者：try_pop / wait_pop / wait_pop_for / try_pop_bulk
 *     只能由 [唯一] 消费者线程调用
 * 
 */
template<typename T>
class MsgQueue
{
public:
    explicit MsgQueue(size_t initialCapacity = 1024)
        : queue_(initialCapacity),                 // 1. 底层队列
          consumerToken_(queue_)                     // 2. 消费者 token（单消费者，可放内部）
    {}
     
    // ---- 生产者侧接口 ----
    // 需要调用方传入自己线程持有的 ProducerToken
    bool try_push(moodycamel::ProducerToken& ptok, T&& msg) 
    {
        return queue_.try_enqueue(ptok, std::move(msg));
    }
    bool try_push_bulk(moodycamel::ProducerToken& ptok, T* msgs, size_t n) 
    {
        return queue_.try_enqueue_bulk(ptok, msgs, n);
    }

    // 便捷接口：不带 Token（使用隐式 TLA），性能略低
    bool try_push(T&& msg) 
    {
        return queue_.try_enqueue(std::move(msg));
    }

    // ---- 消费者侧接口（只能在唯一的消费者线程调用）----
    bool try_pop(T& out) 
    {
        return queue_.try_dequeue(consumerToken_, out);
    }
    size_t try_pop_bulk(T* out, size_t maxN) 
    {
        return queue_.try_dequeue_bulk(consumerToken_, out, maxN);
    }

    void wait_pop(T& out) 
    {
        queue_.wait_dequeue(consumerToken_, out);
    }

    bool wait_pop_for(T& out, std::chrono::milliseconds timeout) {
        return queue_.wait_dequeue_timed(consumerToken_, out, std::chrono::duration_cast<std::chrono::microseconds>(timeout));
    }


    bool empty() const { return queue_.size_approx() == 0; }

    size_t current_size() const { return queue_.size_approx(); }

    moodycamel::ProducerToken createProducerToken() 
    {
        return moodycamel::ProducerToken(queue_);
    }
private:
    ////非阻塞队列成员
    // moodycamel::CncurrentQueue<T> queue_;
    moodycamel::BlockingConcurrentQueue<T> queue_;
    moodycamel::ConsumerToken consumerToken_;
};
#endif