// alarm_demo.cpp —— 作业1：体验 SIGALRM 的异步打断
// 编译：g++ -g -Wall -o alarm_demo alarm_demo.cpp
// 现象：主线程 sleep(5) 期间，SIGALRM 每秒"凭空插入"一次，sleep 提前返回
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <ctime>

int count = 0;   // 故意用全局变量：演示 handler 在主线程上下文中执行

void handler(int sig) {
    // 注意：printf/localtime 并非异步信号安全函数，正式代码里 handler 不许这么写；
    // 这里是演示程序，允许任性。正式服务器的做法 = TinyWebServer 的 self-pipe
    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);
    printf("    [handler] 第 %d 次 SIGALRM，时刻 %02d:%02d:%02d\n",
           ++count, lt->tm_hour, lt->tm_min, lt->tm_sec);
    alarm(1);   // 重新武装，实现每秒一次
}

int main() {
    signal(SIGALRM, handler);
    alarm(1);                     // 1 秒后内核投递第一个 SIGALRM
    printf("main: 开始循环 sleep(5)，注意观察每次被信号打断\n");
    for (int i = 1; i <= 3; i++) {
        unsigned rest = sleep(5); // sleep 被 SIGALRM 打断时，返回"没睡够的秒数"
        printf("main: 第 %d 次 sleep(5) 提前结束，剩余 %u 秒没睡\n", i, rest);
    }
    printf("main: 结束\n");
    return 0;
}
