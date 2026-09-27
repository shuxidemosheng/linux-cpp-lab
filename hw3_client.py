# hw3_client.py —— 作业3专用客户端
# slow 线程：发 5MB 后从不接收（模拟慢客户端）
# fast 线程：期间做 5 次正常 echo，观察服务器是否被 slow 拖累
import socket, threading, time

def slow():
    s = socket.socket()
    s.connect(("127.0.0.1", 8888))
    t0 = time.time()
    s.sendall(b"a" * 5_000_000)     # 发 500 万字节，之后绝不 recv
    print(f"slow: 5MB 已发出(耗时 {time.time()-t0:.2f}s)，接下来 5 秒拒绝接收")
    time.sleep(5)
    s.close()
    print("slow: 关闭连接")

def fast():
    time.sleep(1)                   # 等 slow 先跑起来再开始
    for i in range(5):
        t0 = time.time()
        s = socket.socket()
        s.connect(("127.0.0.1", 8888))
        s.sendall(b"fast-client\n")
        s.settimeout(2)
        resp = s.recv(100)
        print(f"fast: 第{i+1}次 echo={resp!r} 耗时 {(time.time()-t0)*1000:.1f}ms")
        s.close()
        time.sleep(0.5)

threading.Thread(target=slow).start()
fast()
