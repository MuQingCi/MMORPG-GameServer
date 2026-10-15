#ifndef CLEARMOON_SCENE_AOI_CLIENT_VIEW_H
#define CLEARMOON_SCENE_AOI_CLIENT_VIEW_H

#include "service/aoi/aoiService.h"

// 在所属逻辑线程中把 AOI 权威视野转换为客户端增量或全量快照。
// 该适配器不访问网络对象；所有出站数据只经过 AoiPushSink。
bool ProcessAoiClientObserver(AoiService& service, EntityId observer);

#endif