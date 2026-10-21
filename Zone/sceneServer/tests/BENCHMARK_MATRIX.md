# 显式六案例持久化压测

2026-10-17 历史批准范围：3 Gateway / 2 Scene；128/256/512总连接；20 RPS/连接；持久化开启；30/60秒，各1次。
网关连接分配43/43/42、86/85/85、171/171/170；每(Scene,Gateway)2条channel，共12条。
smoke仅1组：12总连接、20RPS、10秒、持久化开启，不计入正式结果。
旧480组已作废；无参数不能启动隐式480组。

在全新batch下执行，以下所有命令从仓库根目录执行，路径为相对路径：
```bash
cmake -S Zone -B out/zone -DCMAKE_BUILD_TYPE=Release
cmake --build out/zone -j6
ctest --test-dir out/zone --output-on-failure
python3 -B Zone/sceneServer/tests/test_benchmark_audit.py
python3 -B Zone/sceneServer/tests/benchmark_matrix.py --build out/zone --batch out/test-results/benchmark-3g2s-smoke --smoke
python3 -B Zone/sceneServer/tests/benchmark_matrix.py --build out/zone --batch out/test-results/benchmark-3g2s --approved-3g2s --resume
python3 -B Zone/sceneServer/tests/benchmark_audit.py --batch out/test-results/benchmark-3g2s --require-complete
python3 -B Zone/sceneServer/tests/benchmark_report.py --batch out/test-results/benchmark-3g2s --archive
```

每案例独立MariaDB/Redis临时目录、端口和凭据，不连接业务数据库。
Release、info日志、GW2IO线程、Scene4逻辑线程、1000ms flush、10秒响应drain。
帧限仅改生成配置：总连接×RPS>10000则100000，否则10000。
客户端闭环、每连接单在途、START后约250ms开始发送；目标RPS不等于实际吞吐。
成功延迟全局最近秩p50/p90/p99，含drain，不包含失败/未完成请求。
CPU单核=100%；网络lo为命名空间指标，不能归因到单进程；未绑核，DB/Redis/客户端同机。

--resume校验二进制哈希和精确矩阵，跳过已完成结果（包括失败），不覆盖中断目录。
原始数据变更须移出已知case-summary.json缓存重新聚合；独立审计从原始样本重算分位数。
--require-complete验证所选案例集合，拒绝缺失/重复/额外案例。
清理构建后使用--archived --require-complete审计归档，SHA256SUMS代替已删除二进制校验。

归档包含脱敏配置、环境、源码/二进制哈希、原始采样、请求结果、DB验证和压缩日志。
先逐文件SHA-256校验，后仅删除显式列出的本批工作/构建目录，不跟随符号链接，不删历史/用户文件。
失败案例原样保留，不重试，不通过调参覆盖失败；缺失的DB证明须明确标为未完成。

## 当前证据保留与执行门槛

上述命令是历史批次操作示例，不是本轮授权。A/B、长时间压测和全量回归必须先完成轻量观测及局部验证，再取得人工批准。最新吞吐摘要以服务 README 为准。2026-10-19 原始运行日志已精简，大诊断时间线改为 `.gz`；原 SHA256SUMS 仅为历史清单，旧归档审计不能再假定全量原始文件齐全。使用历史结果前需先核对文件是否齐全，不能把缺失的观测当作通过。
