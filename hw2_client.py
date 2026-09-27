# hw2_client.py —— 作业2专用客户端：发 2000 字节后保持连接，统计收到的回显
import socket, time, sys

s = socket.socket()
s.connect(("127.0.0.1", 8888))
s.sendall(b"a" * 2000)      # 一次发送 2000 字节，故意超过服务器 1024 的 buf
time.sleep(3)               # 关键：保持连接 3 秒，不给服务器发 FIN

got = b""
s.settimeout(2)             # 最多再等 2 秒回显
try:
    while len(got) < 2000:
        chunk = s.recv(4096)
        if not chunk:
            break
        got += chunk
except socket.timeout:
    pass
print(f"收到回显: {len(got)} 字节 (期望 2000)")
s.close()
