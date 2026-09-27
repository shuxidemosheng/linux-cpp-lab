// echo_server_pool.cpp —— 阶段 1 第 3 课：线程池版 echo 服务器
//
// 第 2 课"每连接一线程"模型的账：1000 个客户端 = 1000 个线程。
//   1) 内存：每线程默认 8MB 栈，1000 个 = 8GB
//   2) 创建/销毁各有系统调用开销
//   3) 调度器在大量线程间来回切换的开销
// 解法：固定 8 个工人线程 + 一个共享任务队列，即经典"生产者-消费者"模型：
//   主线程（生产者）accept 后把 connfd 塞进队列；工人（消费者）轮流取任务。
// 队列是所有线程共享的数据，必须用 mutex 保护；"队列空了工人怎么办"
// 则靠条件变量解决。对应 TinyWebServer 的 threadpool/ 和 lock/ 目录。
//
// 编译：g++ -g -Wall -pthread -o echo_server_pool echo_server_pool.cpp

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <ctime>        // clock_gettime，给日志打时间戳用
#include <deque>        // std::deque 双端队列，当任务队列用
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

const int PORT = 8888;
const int POOL_SIZE = 8;   // 工人数量（TinyWebServer 默认也是 8）

// ---------- MutexLockGuard：RAII 锁守卫（阶段 2 作业 1 的核心） ----------
// 构造函数加锁，析构函数解锁：持锁范围 = guard 对象的作用域。
// 无论这段代码从哪里退出（正常走完 / break / return / 抛异常），
// C++ 保证析构函数一定执行 → "漏解锁死锁"这类 bug 从机制上被消灭。
// 这是 TinyWebServer locker.h 缺失、而 muduo 等成熟库标配的组件。
class MutexLockGuard {
public:
    explicit MutexLockGuard(pthread_mutex_t& m) : mutex_(m) {
        pthread_mutex_lock(&mutex_);
    }
    ~MutexLockGuard() {
        pthread_mutex_unlock(&mutex_);
    }
    // 禁止拷贝：守卫的语义是"我负责这把锁的这一个作用域"，
    // 若允许拷贝，两个守卫管同一把锁会出现二次解锁（未定义行为）
    MutexLockGuard(const MutexLockGuard&) = delete;
    MutexLockGuard& operator=(const MutexLockGuard&) = delete;

private:
    pthread_mutex_t& mutex_;   // 引用成员：守卫不拥有锁，只是引用外部那把
};

// ---------- 互斥锁 + 条件变量：理解成"门锁 + 呼叫铃" ----------
// mutex：同一时刻只允许一个线程进出"操作队列"这段代码（临界区）
// cond ：呼叫铃。工人发现队列空就"睡在铃上"，主线程塞入任务后"摇铃"叫醒
struct thread_pool {
    std::deque<int> tasks;      // 任务队列：只存 connfd
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;  // 静态初始化，省去 init 调用
    pthread_cond_t  cond  = PTHREAD_COND_INITIALIZER;
};

thread_pool pool;   // 全局唯一实例（TinyWebServer 里是 threadpool<T> 类的成员，原理相同）

// 打印 [秒.毫秒] 时间戳，用来观察"任务何时被哪个工人取走"的先后顺序
static void ts() {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);   // 实时墙钟，秒 + 纳秒
    fprintf(stderr, "[%ld.%03ld] ", t.tv_sec % 100000, t.tv_nsec / 1000000);
}

// ---------- 工人线程：取 connfd，伺候到客户端断开，再回来排队取下一个 ----------
void* worker(void* /*arg*/) {
    for (;;) {
        // ---- 拿锁进门：guard 构造即加锁；下面这对大括号结束时（哪条路径都算）
        //      guard 析构自动解锁。对比 TinyWebServer 手动 unlock 的三处出口写法。
        int connfd;
        {
            MutexLockGuard guard(pool.mutex);

            // ---- 队列空就睡。注意必须用 while 而不是 if，两个原因：
            //   1) 虚假唤醒(spurious wakeup)：POSIX 明文允许 wait 无缘无故返回
            //   2) 惊群：signal 唤醒多个工人时，第一个抢到锁的取走任务，
            //      后面拿锁的工人发现队列已空，若用 if 就会带着"有任务"的错觉往下走
            while (pool.tasks.empty()) {
                pthread_cond_wait(&pool.cond, &pool.mutex);
                // wait 内部(三步为一个原子操作)：解锁 → 睡眠 → 被唤醒后重新拿锁再返回
            }

            connfd = pool.tasks.front();
            pool.tasks.pop_front();
        }   // <-- guard 在这一行析构，自动解锁
        ts();
        fprintf(stderr, "[worker %lu] 取到 connfd=%d\n",
                (unsigned long)pthread_self(), connfd);

        // ---- 干活：echo 循环和前两课完全相同 ----
        // 关键局限：worker 阻塞在 read 上时，这个工人被这条连接独占。
        // 8 个工人 = 最多同时伺候 8 条连接，第 9 条只能在队列里等——
        // 这个局限留到第 4 课用 epoll 解决。
        char buf[1024];
        ssize_t n;
        while ((n = read(connfd, buf, sizeof(buf))) > 0) {
            fprintf(stderr, "[worker %lu] connfd=%d read %zd 字节\n",
                    (unsigned long)pthread_self(), connfd, n);
            if (write(connfd, buf, n) != n) { perror("write"); break; }
        }
        if (n < 0) perror("read");
        close(connfd);
        // 客户端断开，回到 for 循环开头继续等下一个任务
    }
    return nullptr;
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    // ---------- 创建监听 socket 的六步与第 1 课完全相同 ----------
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
    printf("echo server (thread pool, %d workers) listening on port %d...\n", POOL_SIZE, PORT);

    // ---------- 开业前先雇好 8 个工人，此后创建线程的开销为零 ----------
    for (int i = 0; i < POOL_SIZE; i++) {
        pthread_t tid;
        pthread_create(&tid, nullptr, worker, nullptr);
        pthread_detach(tid);    // 分离态：结束自动回收，主线程不 join
    }

    // ---------- 主循环 = 生产者：只 accept + 塞队列，不干活 ----------
    while (true) {
        struct sockaddr_in cliaddr;
        socklen_t cliaddr_len = sizeof(cliaddr);
        int connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &cliaddr_len);
        if (connfd == -1) { perror("accept"); continue; }

        {
            MutexLockGuard guard(pool.mutex);   // RAII：出大括号自动解锁
            pool.tasks.push_back(connfd);       // 塞任务
        }

        // 摇铃叫醒"一个"睡觉的工人。
        // signal 唤醒一个，broadcast 唤醒全部——唤醒的工人还要重新抢锁，
        // 没抢到锁又得睡回去，所以任务少时 broadcast 只是白白惊动所有人（惊群）。
        pthread_cond_signal(&pool.cond);
    }
    return 0;   // 不可达
}
