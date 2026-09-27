// echo_server_mt.cpp —— 阶段 1 第 2 课：多线程版 echo 服务器
//
// 解决第 1 课遗留的问题：单线程版一次只能伺候一个客户端（第二个 nc 会卡住）。
// 方案：accept 到一个连接，就创建一个专属线程去伺候它，主线程立刻回去等下一个。
//
// 编译（必须加 -pthread，链接 POSIX 线程库）：
//   g++ -g -Wall -pthread -o echo_server_mt echo_server_mt.cpp
//
// 测试：开两个终端各自执行 nc 127.0.0.1 8888，这次谁都不用等谁。

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <pthread.h>     // POSIX 线程：pthread_create / pthread_detach / pthread_self
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

const int PORT = 8888;

// 传给线程的"连接档案"。为什么要单独搞一个结构体、还放在堆上？
// —— 这是本课最重要的坑，见 main 循环里的注释。
struct client_info {
    int connfd;                 // 这条通话线路的 fd
    struct sockaddr_in addr;    // 客户端的 IP 和端口
};

// ---------- 线程入口函数：每个客户端连接的一生都在这里度过 ----------
// 签名必须是 void* (*)(void*)，这是 pthread_create 硬性规定的。
// arg 就是 pthread_create 第 4 个参数原样传进来的指针。
void* echo_thread(void* arg) {
    // 把 void* 还原成本来的类型。C++ 里用 static_cast，C 里直接强转。
    struct client_info* cli = static_cast<struct client_info*>(arg);
    int connfd = cli->connfd;

    // inet_ntoa 用静态缓冲区存结果——两个线程同时调用会互相覆盖，
    // 这是线程不安全函数的典型。改用线程安全的 inet_ntop + 自己的栈缓冲。
    char ip[16];
    inet_ntop(AF_INET, &cli->addr.sin_addr, ip, sizeof(ip));
    // %lu 打印线程 ID：每个线程拿到的 pthread_self() 都不同
    fprintf(stderr, "[线程 %lu] 开始伺候 %s:%d\n",
            (unsigned long)pthread_self(), ip, ntohs(cli->addr.sin_port));

    char buf[1024];
    ssize_t n;
    while ((n = read(connfd, buf, sizeof(buf))) > 0) {
        // 每次实际读到的字节数都打印出来——发 2000 字节就能看到 read 被拆成几次
        fprintf(stderr, "[线程 %lu] 本次 read 到 %zd 字节\n",
                (unsigned long)pthread_self(), n);
        if (write(connfd, buf, n) != n) {       // 这次顺手检查了 write 是否全部写完
            perror("write");
            break;
        }
    }
    if (n < 0)
        perror("read");
    fprintf(stderr, "[线程 %lu] 客户端断开，线程退出\n", (unsigned long)pthread_self());

    close(connfd);      // 线程自己的通话线路自己关
    //delete cli;         // main 里 new 出来的档案，用完了由这里释放（谁申请谁记得谁）
    return nullptr;     // 线程到此结束；返回值没人用，因为下面选择了 detach
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    // ---------- 前六步和第 1 课完全一样：创建、复用、地址、绑定、监听 ----------
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
    printf("echo server (multi-thread) listening on port %d...\n", PORT);

    // ---------- 主循环：只做"接电话"，把"通话"全部交给新线程 ----------
    while (true) {
        struct sockaddr_in cliaddr;
        socklen_t cliaddr_len = sizeof(cliaddr);
        int connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &cliaddr_len);
        if (connfd == -1) { perror("accept"); continue; }

        // 坑（面试高频题）：为什么必须 new 一个 client_info，而不是传 &cliaddr 或 &connfd？
        // cliaddr/connfd 是 main 栈上的局部变量，主循环每 accept 一次就【覆盖】它们一次。
        // 如果把它们的地址交给线程，主线程跑得快时，第二个客户端的信息会把第一个
        // 线程还没读完的数据踩掉——数据竞争。堆上 new 出来的内存没有这个问题：
        // 每个连接独享一份，地址永远不变，由线程用完自己 delete。
        struct client_info* cli = new struct client_info;
        cli->connfd = connfd;
        cli->addr = cliaddr;

        pthread_t tid;
        // pthread_create(线程id存放处, 属性(0=默认), 入口函数, 传给入口函数的参数)
        // 返回 0 表示成功；注意：成功与否通过【返回值】判断，线程内出错才是 -1/errno 那套
        int err = pthread_create(&tid, nullptr, echo_thread, cli);
        if (err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));  // pthread 系列不设 errno
            close(connfd);
            delete cli;
            continue;
        }
        // 默认创建的是"可 join"线程：死后资源（栈等）要等别人 pthread_join 来收尸。
        // 我们不打算收尸，就声明为"分离态"：线程结束后内核自动回收全部资源。
        // 不 detach 的话，每来一个客户端就泄漏一个线程的资源，日积月累耗尽系统。
        pthread_detach(tid);
    }
    return 0;   // 不可达，示意
}
