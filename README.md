# Linux + C++ 网络编程练习

> **初试后复习入口：[复试复习总纲.md](复试复习总纲.md)**（资产地图 / 三周复习路线 / 20 道自测 / 环境重建指南）

从零递进实现四个版本的 TCP 服务器，最终读通 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer)（Apache 2.0）源码并修复其 bug 的过程记录。所有代码在 WSL2 (Ubuntu) 下用 g++ 编译通过。

## 四个递进版本

| 文件 | 版本 | 解决的问题 |
|---|---|---|
| `echo_server.cpp` | 阻塞单线程 | socket 六步链路：`socket→bind→listen→accept→read/write`；TIME_WAIT 与 SO_REUSEADDR；字节流无边界 |
| `echo_server_mt.cpp` | 每连接一线程 | `pthread_create/detach`；堆上传参避免数据竞争；1000 连接 = 1000×8MB 栈的账 |
| `echo_server_pool.cpp` | 线程池 | 生产者-消费者；mutex+cond（while 重查）；RAII 锁守卫 `MutexLockGuard`；背压 |
| `epoll_server.cpp` | 单线程 epoll | `epoll_create1/ctl/wait`；LT/ET 双模式（`-DREAD_ONCE` 可编译破坏版复现 ET 只通知一次）；非阻塞 EAGAIN；EPOLLONESHOT；writev |

配套实验客户端：`hw2_client.py`（2000 字节拆包对比 LT/ET）、`hw3_client.py`（慢客户端拖垮线程池 vs 不拖垮事件循环的对照）。

## 调试工具实操记录

- **valgrind / LeakSanitizer**：泄漏报告解读、假阴性识别（两者互斥；被 kill 的进程无泄漏报告）
- **ThreadSanitizer**：注释线程池的入队锁 → 抓到 3 组 data race（含 fd 上的竞争）；恢复锁后归零
- **alarm_demo.cpp**：SIGALRM 异步打断 sleep 的可视化

## TinyWebServer 源码精读（fork 见个人仓库）

按 `lock → threadpool → http → log → timer → webserver` 顺序读通，修复/发现 3 个真实 bug：

1. **修复** 数据库连接 `localhost` 陷阱：MySQL 客户端把 localhost 特殊化为 Unix 域套接字，容器化数据库必须 `127.0.0.1` 走 TCP
2. **修复** POST 密码解析 off-by-2：硬编码 `i + 10` 撞上 `&passwd=` 的 8 字节长度，注册 `123456` 入库变 `3456`；改为 `strstr` 按字面定位
3. **发现** `block_queue` 超时版 `pop` 的纳秒换算错误（`(ms % 1000) * 1000` 应为 `* 1000000`）

知识点详见 [复习笔记.md](复习笔记.md)。

## 三项源码改造（tag 与基准数据见 fork 仓库）

每项遵循同一方法论：**问题 → 假设 → 方案 → 数据**，基准脚本随仓库可复现。

1. **keep-alive pipelining 残留数据丢失修复**
   - 问题：发完 keep-alive 响应后无条件清空 `m_read_buf`，pipeline 后续请求被无声丢弃
   - 验证：单连接连发两个 GET，修复前 1/2 响应，修复后 2/2

2. **定时器升序链表 → 最小堆**（tag `v1.1-timer-heap`）
   - 基准（1 万定时器）：add 7.61µs→0.03µs，adjust 6.77µs→0.03µs（约 250/225 倍）
   - 节点以 `heap_idx` 记录堆中位置，adjust/del 无需线性查找

3. **日志系统性能改造**（tag `v1.2-log-perf`）
   - 移除宏内每行 `fflush`（原版异步日志因此比同步更慢）、格式化移到栈缓冲、
     写盘线程"空闲 1 秒落盘"；顺带修复 `block_queue` 超时 pop 纳秒换算 bug
   - 基准（wrk -c100 -t2 -d5s）：同步日志代价 **-44% → -10%**，异步 -59% → -52%

调试工具链备忘：valgrind/ASan（泄漏与越界）、TSan（线程池入队竞态抓现行）、
GIT_CURL_VERBOSE + SSH over 443（HTTPS 被间歇 RST 时的推送方案）。

## 实测数据（WSL2）

- echo epoll 单线程：500 并发，LT 59,741 QPS / ET 66,975 QPS（ET 快约 12%）
- TinyWebServer 完整版（含 MySQL 查询）：webbench 200 并发 5 秒 35,451 请求，0 失败

## 构建

```bash
# 各版本（WSL 中）
g++ -g -Wall -pthread -o echo_server_pool echo_server_pool.cpp

# TinyWebServer（需 libmysqlclient-dev 与运行中的 MySQL）
make && TWS_DB_USER=xxx TWS_DB_PASS=xxx ./server -p 8888
```
