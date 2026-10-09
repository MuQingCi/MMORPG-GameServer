#ifndef CLEARMOON_GATEWAY_PLAYER_H
#define CLEARMOON_GATEWAY_PLAYER_H
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// 场景服网络连接 session 不在此结构中；由本地 Conn/MsgHead.session 保存。
struct ClientData {
    uint64_t playerId = 0;
    uint64_t clientSession = 0;
    uint32_t clientEpoch = 0;
    uint32_t gatewayId = 0;
    uint32_t sceneId = 0;
};
inline bool SameClientBinding(const ClientData& a, const ClientData& b) {
    return a.playerId == b.playerId && a.clientSession == b.clientSession &&
           a.clientEpoch == b.clientEpoch && a.gatewayId == b.gatewayId && a.sceneId == b.sceneId;
}
namespace GatewayPlayer {
constexpr uint16_t kModule = 200;
constexpr uint16_t kOnline = 1;
constexpr uint16_t kOffline = 2;
constexpr size_t kHeaderSize = 32;
inline bool Valid(const ClientData& d) {
    return d.playerId != 0 && d.clientSession != 0 && d.clientEpoch != 0 && d.gatewayId != 0 && d.sceneId != 0;
}
inline void Put(std::string& s, uint64_t v, unsigned bytes) {
    for(unsigned i=0;i<bytes;++i) s.push_back(char((v>>(8*i))&255));
}
inline uint64_t Get(const std::string& s, size_t pos, unsigned bytes) {
    uint64_t v=0; for(unsigned i=0;i<bytes;++i) v|=uint64_t(uint8_t(s[pos+i]))<<(8*i); return v;
}
inline std::string Pack(const ClientData& d, const std::string& payload = {}) {
    std::string s; Put(s,0x4750,2); Put(s,1,2);
    Put(s,d.playerId,8); Put(s,d.clientSession,8); Put(s,d.clientEpoch,4);
    Put(s,d.gatewayId,4); Put(s,d.sceneId,4); s += payload; return s;
}
inline bool Unpack(const std::string& s, ClientData& out, std::string& payload) {
    if(s.size()<kHeaderSize || Get(s,0,2)!=0x4750 || Get(s,2,2)!=1) return false;
    ClientData d{Get(s,4,8),Get(s,12,8),uint32_t(Get(s,20,4)),uint32_t(Get(s,24,4)),uint32_t(Get(s,28,4))};
    if(!Valid(d)) return false;
    out=d; payload=s.substr(kHeaderSize); return true;
}
}

// 网络线程独占。连接 -> 多玩家绑定，同时保留玩家反向索引与离线水位。
class GatewayClientIndex {
public:
    bool Bind(uint64_t connection, const ClientData& d) {
        if(!connection || !GatewayPlayer::Valid(d)) return false;
        const auto old=latest_.find(d.playerId);
        if(old!=latest_.end()) {
            if(old->second.gatewayId!=d.gatewayId || old->second.sceneId!=d.sceneId) return false;
            if(d.clientEpoch<old->second.clientEpoch) return false;
            if(d.clientEpoch==old->second.clientEpoch) {
                if(!SameClientBinding(old->second,d)) return false;
                const auto route=connectionByPlayer_.find(d.playerId);
                return route!=connectionByPlayer_.end() && route->second==connection;
            }
        }
        const auto route=connectionByPlayer_.find(d.playerId);
        if(route!=connectionByPlayer_.end()) byConnection_[route->second].erase(d.playerId);
        latest_[d.playerId]=d; byConnection_[connection][d.playerId]=d;
        connectionByPlayer_[d.playerId]=connection; return true;
    }
    bool Matches(uint64_t connection,const ClientData& d) const {
        const auto c=byConnection_.find(connection); if(c==byConnection_.end()) return false;
        const auto p=c->second.find(d.playerId); return p!=c->second.end() && SameClientBinding(p->second,d);
    }
    bool Unbind(uint64_t connection,const ClientData& d) {
        if(!Matches(connection,d)) return false;
        byConnection_[connection].erase(d.playerId); connectionByPlayer_.erase(d.playerId); return true;
    }
    std::vector<ClientData> Close(uint64_t connection) {
        std::vector<ClientData> result; const auto c=byConnection_.find(connection);
        if(c==byConnection_.end()) return result;
        for(const auto& p:c->second) { result.push_back(p.second); connectionByPlayer_.erase(p.first); }
        byConnection_.erase(c); return result;
    }
private:
    std::unordered_map<uint64_t,std::unordered_map<uint64_t,ClientData>> byConnection_;
    std::unordered_map<uint64_t,uint64_t> connectionByPlayer_;
    std::unordered_map<uint64_t,ClientData> latest_;
};
#endif