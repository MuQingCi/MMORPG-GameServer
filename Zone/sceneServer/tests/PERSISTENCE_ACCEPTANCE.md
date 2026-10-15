# 隔离持久化验收

本工具仅供显式执行，不默认加入 CTest，也不使用区服配置中的数据库。
需要本机 MariaDB 初始化工具、服务端、客户端，以及 Redis 服务端。

```bash
cmake -S /home/lanxiyuan/Project_Cpp/GameServer/Zone \
  -B /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance
cmake --build /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance \
  --target sceneServer gatewayServer persistence_dao_probe -j2
python3 /home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/persistence_e2e.py \
  /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance
```

工具使用 `/tmp/clearmoon-persistence-e2e-*` 独立目录、回环端口、数据库
`clearmoon_persistence_e2e` 和专用 DML 账号。不会访问系统现有数据库。
配置凭据使用随机密码和受限目录；退出时停止本次进程并删除临时数据。
成功摘要保存在构建目录的 `persistence_e2e_summary.txt`，不包含凭据。
失败时输出经密码遮蔽的服务日志。SIGKILL/主机故障无法执行 Python 清理，
这种情况下应先确认残留进程属于本次临时目录，再手工清理，不能停止其他实例。

## 验收内容

- 真驱动一致性加载，非零数据库出生点和非空背包。
- 外键失败时玩家、背包和版本整体回滚。
- 相同版本载荷重复保存，旧版本拒绝。
- 网关鉴权、进场、移动、主动离线、立即重新绑定和加载。
- 在线周期保存；修改后关服排空；进程重启恢复。
- 复用网关连接的另一玩家 TCP 断开后独立保存。
- SQL 查询核对位置、版本推进及物品内容；日志检查关服未完成错误。

快速重登测试覆盖真实正常时序，但不保证人为延迟保存的每个交错都被触发；
该交错还需对应单元测试。工具不验证持久任务日志、进程崩溃不丢失、死锁、
COMMIT 断链结果未知恢复、跨网关接管或客户端 AOI 视野推送。

回环端口动态选取，选取与启动之间仍可能存在竞争；端口被占用会使测试失败。
MariaDB 是当前机器上的验收后端，不应将结果当作所有 MySQL 版本兼容证明。