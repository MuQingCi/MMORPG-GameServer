#ifndef CLEARMOON_COMMON_UTILS_H
#define CLEARMOON_COMMON_UTILS_H

#include <cstdint>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

static_assert(sizeof(uint16_t) == 2, "uint16_t must be 2 bytes");
static_assert(sizeof(uint32_t) == 4, "uint32_t must be 4 bytes");
static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");


//-----16------
inline uint16_t netToHost16(uint16_t v) {
#if defined (_WIN32)
    return ntohs(v);
#else
    return be16toh(v); 
#endif
}

inline uint16_t host16ToNet(uint16_t v) {
#if defined (_WIN32)
    return htons(v);
#else
    return htobe16(v); 
#endif
}

//-----32------
inline uint32_t netToHost32(uint32_t v) {
#if defined (_WIN32)
    return ntohl(v);
#else
    return be32toh(v);
#endif
}
inline uint32_t host32ToNet(uint32_t v) {
#if defined (_WIN32)
    return htonl(v);
#else
    return htobe32(v); 
#endif
}

//-----64-----
inline uint64_t netToHost64(uint64_t v) {
#if defined (_WIN32)
    uint32_t lo = static_cast<uint32_t>(v);
    uint32_t hi = static_cast<uint32_t>(v >> 32);
    return (static_cast<uint64_t>(ntohl(hi)) << 32) | static_cast<uint64_t>(ntohl(lo));
#else
    return be64toh(v); 
#endif
}

inline uint64_t host64ToNet(uint64_t v) {
#if defined (_WIN32)
    uint32_t lo = static_cast<uint32_t>(v);
    uint32_t hi = static_cast<uint32_t>(v >> 32);
    return (static_cast<uint64_t>(htonl(hi)) << 32) | static_cast<uint64_t>(htonl(lo));
#else
    return htobe64(v); 
#endif
}


#endif