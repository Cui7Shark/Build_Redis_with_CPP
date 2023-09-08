# Build_Redis_with_CPP
> 参考资料：build your own Redis with C/C++
> 个人学习
## 使用
```shell
cd 14/
sh build.sh

./Server_redis

./Client_redis set hello world
./Client_redis get hello
```
## 功能
- set k v 
- get k
- del k
## 说明
- 使用`thread_pool`, 异步队列, 支持多线程.
- 使用 `heap data` , 设置生存时间 TTL.
- 使用计时器, `list 存储`, 支持连接超时检测(`5s`).
- 使用`epoll` (`epoll`模式在11文件夹`11_server.cpp`)和`poll`支持`IO`多路复用.
- 使用`hashmap` 保存kv (string, hashtable, zset)
- 使用AVL树， 支持对数据按顺序存储和查找

