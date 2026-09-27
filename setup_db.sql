-- setup_db.sql —— TinyWebServer 所需的数据库初始化脚本
-- 执行方式（装好 MySQL 后）：sudo mysql < /mnt/d/AIcoding/linux-cpp-lab/setup_db.sql
-- 或者逐行在 mysql 提示符里粘贴执行

-- 1. 让 root 用户允许"密码方式"从本机 TCP 登录（Ubuntu 8.x 默认 root 走 auth_socket，
--    只允许 sudo 免密进，TinyWebServer 用 TCP + 密码连接会被拒）
--    注意：先在 sudo mysql 提示符里单独执行这三行！
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '<在这里填你的root密码>';
FLUSH PRIVILEGES;

-- 2. 建库建表（表结构按 TinyWebServer 的 README）
CREATE DATABASE IF NOT EXISTS qgydb;
USE qgydb;
CREATE TABLE user(
    username char(50) NULL,
    passwd char(50) NULL
) ENGINE=InnoDB;
INSERT INTO user(username, passwd) VALUES('name', 'passwd');
