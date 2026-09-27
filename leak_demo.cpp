// leak_demo.cpp —— 演示 LeakSanitizer / valgrind 的泄漏报告格式
// 三种情况各占一种：正常释放 / 忘记释放（泄漏）/ 还被指针引用着（reachable）
#include <cstdio>
#include <cstring>

int main() {
    // 情况 1：正确释放 —— 不出现在报告里
    char* ok = new char[100];
    memset(ok, 0, 100);
    //delete[] ok;

    // 情况 2：泄漏 —— 指针被覆盖，这块内存再也找不回来了
    char* leak = new char[64];
    memset(leak, 1, 64);
    leak = nullptr;             // 模拟"把唯一的钥匙弄丢了"
    (void)leak;

    // 情况 3：程序结束时指针还活着（比如全局变量引用着）——
    // 报告为 still reachable，不算丢失，但严格的服务器代码也应释放
    static char* reachable = new char[32];
    memset(reachable, 2, 32);
    (void)reachable;

    printf("done\n");
    return 0;   // 正常退出 —— 泄漏检查在这里执行
}
