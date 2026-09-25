-- ===========================================================================
-- core/ 目录：框架级脚本（最先加载）
--   core 里只放"与玩法无关"的基础设施：通用工具、事件分发辅助。
--   加载顺序固定为 core/ -> 根目录 -> module/，并且每个目录内按文件名排序，
--   保证所有逻辑线程的加载顺序完全一致（否则会出现同名函数行为不一致）。
-- ===========================================================================

-- 通用小工具
util = util or {}

function util.clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- 服务端日志（C++ 绑定：log.i / log.w / log.e）
function util.log_info(fmt, ...)
    local ok, msg = pcall(string.format, fmt, ...)
    log.i(ok and msg or tostring(fmt))
end

-- 拆包时用：把 C++ 侧的 (ok, err) 结果统一成 "值或 nil+错误"
function util.check(ok, err)
    if not ok then
        util.log_info("call failed: %s", tostring(err))
        return nil, err
    end
    return true
end

util.log_info("core script loaded")
