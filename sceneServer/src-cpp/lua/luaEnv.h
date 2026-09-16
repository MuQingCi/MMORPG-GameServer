#ifndef CLEARMOON_SCENE_LUA_LUAENV_H
#define CLEARMOON_SCENE_LUA_LUAENV_H

#include "db/dbValue.h"
#include "common/ITimerService.h"

#include <cstddef>
#include <string>
#include <cstdint>
#include <vector>
#include <map>

struct lua_State;


/**
 * @brief 负责整个场景服务器脚本的处理，逻辑线程调用内部函数时传入逻辑线程独持Lua_state进行脚本调用
		  每个逻辑线程内部在start函数中通过本类将C++函数API 挂载到lua虚拟机全局变量中
 * 
 */
 
class LuaEnv
{
public:
//一个线程持有一个实例
		LuaEnv(const std::string& luaDir) : root_(luaDir)
		{}

		~LuaEnv()
		{
			Shutdown();
		}

		// C2S 分发结果: 供逻辑层决定是否回 RetTip
		enum class DispatchResult { kOk, kNoFn, kLuaError };

		bool Init(const std::string& root);
		void Shutdown();
		// lua_State* State() { return L_; }
		bool Reload();  // 热更: 重建 state 并重新加载全部脚本

		// 把 "a.b.c" 路径的函数压到栈顶; 返回是否可调用
		bool PushFn(const char* path);
		// 调用栈顶函数, nargs 为已压参数个数; 成功返回 true
		bool Call(int nargs);  // 失败时输出带脚本行号的栈回溯

		// C2S 业务: 调用 path(session, pid, req_table)
		DispatchResult DispatchC2S(const char* path, uint32_t session, uint32_t pid,
						int req_slot);
		// DB 回调: path(ctx, errcode, errmsg, data)
		int64_t RegisterDbAck(const std::string& fn_path);
		void DispatchDbAck(int64_t ctx, const dbv::DbResult& res);
		// 以 int64 参数调用 "a.b.c" 路径函数(C++ 驱动 Lua 决策, 如敌人 AI tick)
		// 返回 false 表示函数缺失或执行出错(调用方需兜底)
		bool CallI64(const char* path, const std::vector<int64_t>& args);

		// Lua 定时器(内部转成 Mod::TIMER 的定时器消息)
		uint32_t AddLuaTimer(uint64_t workId, uint64_t playerId, uint64_t session,int64_t delay_ms, bool repeat, const std::string& fn);
		void CancelLuaTimer(uint32_t timer_id);
		void OnLuaTimerFire(uint32_t timer_id);

		// 玩家下线钩子: 清理 Lua 侧与该玩家相关的定时器(如自动行走链)
		void OnPlayerLogout(uint32_t pid);

		void bindTimer(ITimerService* timer)
		{
			timer_ = timer;
		}
private:
    bool LoadScripts(lua_State* L);
	bool DoFile(const std::string& rel_path);

	lua_State* L_ = nullptr;

    //脚本根路径
    std::string root_;
	int msgh_ref_ = -2;  // LUA_NOREF: 错误处理器(debug.traceback)注册表引用
	uint64_t call_count_ = 0;
	uint64_t error_count_ = 0;
    //根据配置读取脚本路径
	std::vector<std::string> script_files_; 

	std::map<int64_t, std::string> db_pending_;
	int64_t next_ctx_ = 10000;
	std::map<uint64_t, std::pair<std::string, bool>> lua_timers_;

	ITimerService* timer_;
};

#endif