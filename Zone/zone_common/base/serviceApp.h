#ifndef CLEARMOON_SERVICE_APP_H
#define CLEARMOON_SERVICE_APP_H

#include "base/proto.h"

#include <cstdint>
#include <string>

// Chat 请求/推送体：目标/发送者 ID(u64 大端) + 角色名(12B UTF-8，右侧零填充)
// + 文本。角色名由客户端提交，仅用于展示，不可作为可信身份。
std::string ChatBody(uint64_t playerId, const std::string& name, const std::string& text);
bool ParseChatBody(const std::string& body, uint64_t& playerId, std::string& name, std::string& text);

// 最小区服服务：网络线程独占 socket/读写缓冲，逻辑线程独占业务处理。
// 用法：chatServer|globalServer <网关内网IP> <端口> <zoneId>
int RunService(int argc, char** argv, uint8_t serviceId);

#endif