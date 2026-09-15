#ifndef CLEARMOON_COMMON_QUEUE_H
#define CLEARMOON_COMMON_QUEUE_H

#include "msg.h"
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <cstddef>

/**
 * @brief MPSC消息队列封装，使用此队列需要确保满足多生产者，单消费者的前提
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
    bool try_push(moodycamel::ProducerToken& ptok, T&& msg) {
        return queue_.try_enqueue(ptok, std::move(msg));
    }
    bool try_push_bulk(moodycamel::ProducerToken& ptok, T* msgs, size_t n) {
        return queue_.try_enqueue_bulk(ptok, msgs, n);
    }

    // 便捷接口：不带 Token（使用隐式 TLA），性能略低
    bool try_push(T&& msg) {
        return queue_.try_enqueue(std::move(msg));
    }

    // ---- 消费者侧接口（只能在唯一的消费者线程调用）----
    bool try_pop(T& out) {
        return queue_.try_dequeue(consumerToken_, out);
    }
    size_t try_pop_bulk(T* out, size_t maxN) {
        return queue_.try_dequeue_bulk(consumerToken_, out, maxN);
    }

    bool empty() const { return queue_.size_approx() == 0; }

    size_t current_size() const { return queue_.size_approx(); }

    moodycamel::ProducerToken createProducerToken() 
    {
        return moodycamel::ProducerToken(queue_);
    }
private:
    moodycamel::ConcurrentQueue<T> queue_;
    moodycamel::ConsumerToken consumerToken_;
};
#endif