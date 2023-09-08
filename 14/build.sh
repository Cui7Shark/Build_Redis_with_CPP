g++ -Wall -Wextra -O2 -g -std=c++17 server.cpp avl.cpp hashtable.cpp heap.cpp thread_pool.cpp zset.cpp -o Server_redis -lpthread
g++ -Wall -Wextra -O2 -g -std=c++17 client.cpp -o Client_redis