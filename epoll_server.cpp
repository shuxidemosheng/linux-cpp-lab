// epoll_server.cpp —— 阶段 1 第 4 课（压轴）：单线程 epoll 事件循环版
//
// 前三课的矛盾：要么一次只等一个连接（阻塞），要么每个连接占一个线程（浪费）。
// epoll 的解法：把所有 fd 登记给内核，单线程睡眠等待，谁有事件就处理谁。
// 本版没有任何 worker 线程——一个线程伺候任意多连接。
//
// 两种触发模式：
//   ./epoll_server lt   —— LT 水平触发（默认）：数据没读完会反复通知，宽容
//   ./epoll_server et   —— ET 边缘触发：状态变化只通知一次，必须非阻塞+循环读干
//
// 编译：g++ -g -Wall -o epoll_server epoll_server.cpp   （单线程，不再需要 -pthread）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>      // fcntl：把 fd 设为非阻塞
#include <sys/socket.h>
#include <sys/epoll.h>  // epoll_create1 / epoll_ctl / epoll_wait 三件套
#include <netinet/in.h>
#include <arpa/inet.h>

const int PORT = 8888;
const int MAX_EVENTS = 1024;   // 单次 epoll_wait 最多取回多少个就绪事件

// 打印 [秒.毫秒] 时间戳，观察事件顺序
static void ts() {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    fprintf(stderr, "[%ld.%03ld] ", t.tv_sec % 100000, t.tv_nsec / 1000000);
}

// 把 fd 设为非阻塞。返回后 read/write 不再"睡等"：
// 没数据时立刻返回 -1 且 errno = EAGAIN（= "暂时没有，你待会儿再来"）。
// ET 模式的硬性前提：通知只有一次，绝不能让一次 read 睡死整个线程。
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);          // 取出 fd 当前的标志位
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);  // 加上非阻塞位再写回
}

int main(int argc, char* argv[]) {
    bool et = (argc > 1 && strcmp(argv[1], "et") == 0);   // 命令行选触发模式
    signal(SIGPIPE, SIG_IGN);

    // ---------- 创建监听 socket 的六步与第 1 课相同 ----------
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) { perror("socket"); exit(1); }

    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) { perror("bind"); exit(1); }
    if (listen(listenfd, 128) == -1) { perror("listen"); exit(1); }
    set_nonblocking(listenfd);   // 两种模式都设非阻塞，写法统一

    // ---------- epoll 三步之一：创建事件登记处 ----------
    // 返回值又是一个 fd（epfd）——"一切皆文件"的再次体现。
    // 内部是一棵红黑树（存放所有登记的 fd）+ 一条就绪链表（事件发生时挂入）。
    int epfd = epoll_create1(0);
    if (epfd == -1) { perror("epoll_create1"); exit(1); }

    // ---------- 之二：登记 listenfd ----------
    // events 是"关心什么"的位掩码：EPOLLIN=可读；EPOLLRDHUP=对端关闭了写方向；
    // data 是"事件发生时原样带回"的 union，这里放 fd 本身，回来好认人。
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = listenfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) == -1) { perror("epoll_ctl"); exit(1); }

    struct epoll_event events[MAX_EVENTS];   // epoll_wait 返回时就绪事件填这里
    printf("epoll echo server (%s mode) listening on port %d...\n", et ? "ET" : "LT", PORT);

    // ---------- 事件循环：整个服务器的主体，只有这一个循环 ----------
    for (;;) {
        // ---------- 之三：等待。没有事件时整个进程在这里睡觉（不占 CPU），
        // 有事件则返回就绪个数，并把就绪条目填进 events[]。
        // 对比 select/poll：这里只返回"就绪的"，不需要遍历全部连接。
        int nready = epoll_wait(epfd, events, MAX_EVENTS, -1);   // -1 = 永久等待
        if (nready == -1) {
            if (errno == EINTR) continue;   // 被信号打断不算错误，回去重等
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;     // 从带回的信息认出"是谁的事件"

            if (fd == listenfd) {
                // ===== 监听 fd 可读 = 有新连接来了 =====
                // 循环 accept 直到 EAGAIN（取完为止）。原因：ET 下监听 fd 的
                // "可读"通知只有一次，若这批来了 3 个连接只 accept 1 个，
                // 剩下 2 个的事件不会再通知——必须一口气取干。LT 下这样写也无害。
                while (true) {
                    struct sockaddr_in cli;
                    socklen_t clen = sizeof(cli);
                    int connfd = accept(listenfd, (struct sockaddr*)&cli, &clen);
                    if (connfd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 取完了
                        perror("accept");
                        break;
                    }
                    set_nonblocking(connfd);
                    struct epoll_event cev;
                    // ET 模式给连接 fd 加 EPOLLET 标志；LT 不加
                    cev.events = EPOLLIN | EPOLLRDHUP | (et ? EPOLLET : 0);
                    cev.data.fd = connfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &cev);
                    ts();
                    fprintf(stderr, "接受新连接 connfd=%d\n", connfd);
                }
                continue;
            }

            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // ===== 对端关闭或连接异常：注销 + 关闭，一步都不能少 =====
                // 不 DEL 就 close 的话，内核虽然自动清理，但显式注销是
                // 防止"事件表里留野指针"的好习惯（TinyWebServer 同样处理）
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                ts();
                fprintf(stderr, "fd=%d 对端关闭/异常，已清理\n", fd);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                // ===== 连接 fd 可读：echo =====
                // 必须循环读到 EAGAIN。LT 下没读完内核会再通知，读一次也能活；
                // ET 下只有这一次机会，不读干就永远丢了 —— 统一用循环，两种模式通吃。
                char buf[1024];
                while (true) {
                    ssize_t n = read(fd, buf, sizeof(buf));
                    if (n > 0) {
                        ts();
                        fprintf(stderr, "fd=%d 读到 %zd 字节\n", fd, n);
                        // 压测友好：请求长得像 HTTP 方法就回一个最小合法响应，
                        // wrk 才能把响应解析成功并统计——这就是"解析请求→回响应"
                        // 的最简雏形，TinyWebServer 的 http/ 模块把它做成了状态机
                        const char* resp = "HTTP/1.1 200 OK\r\n"
                                           "Content-Length: 0\r\n"
                                           "Connection: keep-alive\r\n\r\n";
                        if (n >= 4 && (strncmp(buf, "GET ", 4) == 0 ||
                                       strncmp(buf, "POST", 4) == 0 ||
                                       strncmp(buf, "HEAD", 4) == 0)) {
                            if (write(fd, resp, strlen(resp)) != (ssize_t)strlen(resp))
                                perror("write");
                        } else {
                            // 普通文本仍是 echo
                            if (write(fd, buf, n) != n) perror("write");
                        }
#ifdef READ_ONCE
                        // 破坏性实验专用（g++ -DREAD_ONCE 编译时生效）：
                        // 每次事件只读一次就收手，用来演示 ET "只通知一次"的后果
                        break;
#endif
                    } else if (n == 0) {
                        // 读到 0 = 对端正常关闭（FIN），清理并退出内层循环
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        ts();
                        fprintf(stderr, "fd=%d 客户端断开，已清理\n", fd);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读干了
                        if (errno == EINTR) continue;                        // 被信号打断，重试
                        perror("read");
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        break;
                    }
                }
            }
        }
    }
    return 0;
}
