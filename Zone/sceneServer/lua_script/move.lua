-- ===========================================================================
-- move.lua —— 移动模块（对应 routeTable 里的 move.c2s_walk / move.c2s_path）
--
-- 处理函数签名：fn(session, pid, req_table)
--   session / pid 由框架传入（来自网络帧头，网关已校验身份，不可信客户端自报）
--   req_table     是 gs.WalkReq / gs.PathReq 反序列化出来的 Lua 表
--
-- 回包用 net.send(session, "WalkAck", table, seq?)：
--   1) 它是**异步**的，返回 true 只代表"已入队"，不代表"已经发出去"。
--      也正因为异步，逻辑线程不需要（也不允许）碰网络层的连接与缓冲；
--   2) 省略 seq 时框架会自动带回"本次请求的 seq"（请求序列号原样回传），
--      脚本通常不需要自己处理它。
-- ===========================================================================

move = move or {}

-- 自动行走的定时器句柄：pid -> timerId
local autoWalkTimers = {}

function move.c2s_walk(session, pid, req)
    local p = player.info(pid)
    if p == nil then
        -- 玩家不在本 worker 分片（或未上线）：直接回错误，不要跨分片直读
        net.send(session, "RetTip", { code = 1001, text = "player not ready" })
        return
    end

    local sx = req and req.sx or 0
    local sy = req and req.sy or 0
    local dx = req and req.dx or 0
    local dy = req and req.dy or 0

    -- 简化版距离校验：真实项目里应交给寻路/AOI 模块
    local dist = math.abs(dx - sx) + math.abs(dy - sy)
    if dist > 100 then
        net.send(session, "RetTip", { code = 1002, text = "too far" })
        return
    end

    player.move(pid, dx, dy, 0)

    net.send(session, "WalkAck", { code = 0, x = dx, y = dy, dir = 0 })

    -- 每 500ms 推一次状态，示范 Lua 定时器 + 玩家下线自动清理
    local old = autoWalkTimers[pid]
    if old then
        timer.cancel(old)
    end
    autoWalkTimers[pid] = timer.every(500, "move.on_auto_walk_tick")
end

function move.on_auto_walk_tick(timerId)
    -- LuaEnv 会给回调传 timerId；玩家身份由框架在发起时记录（回投到同一线程）
    util.log_info("auto walk tick, timerId=%s", tostring(timerId))
end

function move.c2s_path(session, pid, req)
    -- 这里演示 Lua 请求 protobuf 出站消息：PathAck 带 repeated 字段
    local xs, ys = {}, {}
    for i = 1, 5 do
        xs[i] = 10 + i
        ys[i] = 20 + i
    end
    net.send(session, "PathAck", { code = 0, xs = xs, ys = ys })
end

-- 玩家下线的脚本侧钩子（LuaEnv::OnPlayerLogout 会尝试调用它）
function player_on_logout(pid)
    local t = autoWalkTimers[pid]
    if t then
        timer.cancel(t)
        autoWalkTimers[pid] = nil
    end
    util.log_info("player %s logout, timers cleaned", tostring(pid))
end
