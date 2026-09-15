#ifndef CLEARMOON_COMMON_NONCOPY_H
#define CLEARMOON_COMMON_NONCOPY_H

class noncopyable
{
public:
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;
protected:
    noncopyable() = default;
    virtual ~noncopyable() = default;
};

#endif