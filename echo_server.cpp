// echo_server.cpp —— 阶段 1 第 1 课：最小的 TCP echo 服务器
//
// 功能：客户端发来什么文本，就把同样的文本发回去。
// 它是 TinyWebServer 的"胚胎"：listenfd/connfd、read/write 这套骨架
// 在后续所有版本（多线程版、epoll 版）中都不会变，变的是"怎么同时伺候多个 connfd"。
//
// 编译（在 WSL 中执行）：g++ -g -Wall -o echo_server echo_server.cpp
// 运行：./echo_server
// 测试：另开一个 WSL 终端，执行  nc 127.0.0.1 8888  ，输入任意文字后回车

#include <cstdio>        // perror, printf —— 标准输出/错误打印
#include <cstdlib>       // exit
#include <cstring>       // bzero —— 内存清零（写地址结构体前必须清空）
#include <csignal>       // signal —— 屏蔽 SIGPIPE 信号用（见后文"坑"说明）
#include <unistd.h>      // read, write, close —— 这几个其实是系统调用的薄封装
#include <sys/socket.h>  // socket, setsockopt, bind, listen, accept, sockaddr
#include <netinet/in.h>  // sockaddr_in, htons/htonl, INADDR_ANY —— IPv4 地址结构
#include <arpa/inet.h>   // inet_ntoa, ntohs —— 把网络字节序的 IP/端口转成可读字符串

const int PORT = 8888;   // 服务器监听的端口号，1024 以下需要 root 权限，所以选 8888

int main() {
    // ---------- 第一步：创建监听 socket（"买一部电话机"） ----------
    // socket(协议族, 套接字类型, 协议) 返回一个文件描述符(fd)。
    //   AF_INET    ：IPv4 协议族（AF_INET6 是 IPv6）
    //   SOCK_STREAM：字节流套接字，即 TCP（SOCK_DGRAM 是 UDP）
    //   0          ：让内核根据前两个参数选默认协议，这里自动就是 TCP
    // 返回值是 3、4、5... 这样的小整数，它和"打开一个文件得到的 fd"是同一套编号体系，
    // 所以内核对 socket 和文件用统一的接口管理——这就是 Linux "一切皆文件"的体现。
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {          // 系统调用失败统一返回 -1，错误码放在全局 errno 里
        perror("socket");          // perror 会根据 errno 打印人类可读的错误原因
        exit(1);
    }

    // ---------- 第二步：设置 SO_REUSEADDR 选项 ----------
    // 坑：TCP 连接主动关闭的一方会进入 TIME_WAIT 状态（约 1~2 分钟），
    // 期间服务器重启 bind 会报 "Address already in use"。加这个选项可跳过该限制。
    // 调试阶段频繁 Ctrl+C 重启服务器，几乎必踩这个坑。
    int optval = 1;  // 非 0 表示"启用"该选项
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // ---------- 第三步：填写本机地址结构体（"登记电话号码"） ----------
    // sockaddr_in 是 IPv4 专用的地址结构，字段含义如下。
    // 必须先整体清零：结构体里有内核要对齐/填充的保留字节，不清零是经典 bug 来源。
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;              // 地址族：IPv4，必须和 socket() 的 AF_INET 一致
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    // INADDR_ANY 就是 0.0.0.0：监听本机所有网卡的 8888 端口
    // （本机既有 lo 回环网卡 127.0.0.1，也有 eth0 的 172.x.x.x，都要能访问到）
    addr.sin_port = htons(PORT);
    // htons = host to network short。网络字节序规定为大端，而 x86 CPU 是小端，
    // 多字节数字（端口号 8888 = 0x22B8）不转换的话，网络上会读到完全不同的数字。
    // 结论：凡是写进协议头的多字节数字，一律用 htonl/htons 转换。

    // ---------- 第四步：bind（把号码贴到电话机上） ----------
    // 把"IP:端口"和 socket 绑定。第二个参数用 (struct sockaddr*) 强转是历史设计：
    // bind 被设计成通用接口（IPv4/IPv6/Unix 域套接字都走它），只能传一个"基类型"指针。
    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        exit(1);
    }

    // ---------- 第五步：listen（"开机等待铃响"） ----------
    // listen 做两件事：(1) 把 socket 从"可以主动 connect"切换为"被动监听"状态；
    // (2) 让内核开一个排队队列：客户端的连接请求先在队列里完成三次握手，
    // 等着你用 accept 取走。第二个参数 128 就是这个队列的长度上限（全连接队列）。
    if (listen(listenfd, 128) == -1) {
        perror("listen");
        exit(1);
    }
    printf("echo server listening on port %d...\n", PORT);

    // 屏蔽 SIGPIPE：如果客户端已断开而服务器还往 connfd 里 write，
    // 内核会向本进程发 SIGPIPE 信号，默认行为是"直接杀死进程"。
    // 写服务器程序几乎都要屏蔽它，改为感知 write 的 -1 返回值。
    signal(SIGPIPE, SIG_IGN);

    // ---------- 主循环：一个一个地接待客户端 ----------
    while (true) {
        // ---------- 第六步：accept（"接电话"，这是最容易被误解的一步） ----------
        // accept 是阻塞调用：队列里没有已完成的连接时，进程在这里睡觉。
        // 握手成功后，内核新建一个【新的】socket（connfd）专门负责和这个客户端通信，
        // listenfd 永远只负责"接电话"，connfd 才是"通话线路"——二者分工不同，
        // 这是新手最容易混淆的一点：一个服务器有 1 个 listenfd + N 个 connfd。
        struct sockaddr_in cliaddr;                    // 用于接收对方的 IP 和端口
        socklen_t cliaddr_len = sizeof(cliaddr);       // 注意是"传入传出"参数，必须初始化
        int connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &cliaddr_len);
        if (connfd == -1) {
            perror("accept");   // 个别连接失败不应让整个服务器退出，打日志后继续
            continue;
        }
        printf("client connected: %s:%d\n",
               inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port));

        // ---------- 第七步：echo 循环（"通话"） ----------
        char buf[1024];   // 应用层缓冲区：内核收到数据后躺在 connfd 的接收缓冲区里，
                          // read 就是把数据从"内核缓冲区"搬到"你自己的 buf"
        ssize_t n;
        while ((n = read(connfd, buf, sizeof(buf))) > 0) {
            // read 返回值有三种，必须区分：
            //   > 0 ：实际读到的字节数（注意：可能小于 1024，TCP 是字节流，来多少算多少）
            //   = 0 ：对端正常关闭了连接（收到 FIN，即 EOF）——退出循环
            //   = -1：出错（errno 说明原因）
            // 以下一行是 echo 的全部逻辑：原样把 n 个字节写回去。
            // （简化处理：没检查 write 是否写完——发送缓冲区满时 write 只写一部分，
            //   这个坑留给讲"非阻塞 IO"时再处理；阻塞模式下 write 几乎总是全部写完。）
            write(connfd, buf, n);
        }
        if (n < 0)          // 只有出错才 perror；n == 0 是客户端正常下线，不算错误
            perror("read");
        printf("client %s:%d disconnected\n",
               inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port));
        close(connfd);      // 挂断这一路通话，回收 connfd。fd 不 close 会耗尽（fd 泄漏）
    }

    close(listenfd);        // 实际永远执行不到（上面是死循环），仅作示意
    return 0;
}
