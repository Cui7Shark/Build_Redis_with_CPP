#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
// #include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <vector>
#include <sys/epoll.h>

static void msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg)
{
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

/// @brief  fd 设置为非阻塞模式
/// @param fd
static void fd_set_nb(int fd)
{
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno)
    {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno)
    {
        die("fcntl error");
    }
}

const size_t k_max_msg = 4096;

enum
{
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2, // mark the connection for deletion
};

struct Conn
{
    int fd = -1;
    uint32_t state = 0; // either STATE_REQ or STATE_RES
    // buffer for reading
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];
    // buffer for writing
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
};

static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn)
{
    if (fd2conn.size() <= (size_t)conn->fd)
    {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd)
{
    // accept
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0)
    {
        msg("accept() error");
        return -1; // error
    }

    // set the new connection fd to nonblocking mode
    fd_set_nb(connfd);
    // creating the struct Conn
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn)
    {
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn_put(fd2conn, conn);
    return 0;
}

static void state_req(Conn *conn);
static void state_res(Conn *conn);

static bool try_one_request(Conn *conn)
{
    // try to parse a request from the buffer
    if (conn->rbuf_size < 4)
    {
        // not enough data in the buffer. Will retry in the next iteration
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg)
    {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    if (4 + len > conn->rbuf_size)
    {
        // not enough data in the buffer. Will retry in the next iteration
        return false;
    }

    // got one request, do something with it
    printf("client says: %.*s\n", len, &conn->rbuf[4]);

    // generating echoing response
    memcpy(&conn->wbuf[0], &len, 4);
    memcpy(&conn->wbuf[4], &conn->rbuf[4], len);
    conn->wbuf_size = 4 + len;

    // remove the request from the buffer.
    // note: frequent memmove is inefficient.
    // note: need better handling for production code.
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain)
    {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    // change state
    conn->state = STATE_RES;
    state_res(conn);

    // continue the outer loop if the request was fully processed
    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn *conn)
{
    // try to fill the buffer
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do
    {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN)
    {
        // got EAGAIN, stop.
        return false;
    }
    if (rv < 0)
    {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0)
    {
        if (conn->rbuf_size > 0)
        {
            msg("unexpected EOF");
        }
        else
        {
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    // Try to process requests one by one.
    // Why is there a loop? Please read the explanation of "pipelining".
    while (try_one_request(conn))
    {
    }
    return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn)
{
    while (try_fill_buffer(conn))
    {
    }
}

static bool try_flush_buffer(Conn *conn)
{
    ssize_t rv = 0;
    do
    {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN)
    {
        // got EAGAIN, stop.
        return false;
    }
    if (rv < 0)
    {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size)
    {
        // response was fully sent, change state back
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    // still got some data in wbuf, could try to write again
    return true;
}

static void state_res(Conn *conn)
{
    while (try_flush_buffer(conn))
    {
    }
}

static void connection_io(Conn *conn)
{
    if (conn->state == STATE_REQ)
    {
        state_req(conn);
    }
    else if (conn->state == STATE_RES)
    {
        state_res(conn);
    }
    else
    {
        assert(0); // not expected
    }
}

int main()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        die("socket()");
    }

    int val = 1;
    // 设置套接字选项SO_REUSEADDR，使用setsockopt()函数。
    // 这个选项允许在套接字关闭后立即重用端口。如果设置失败，则调用die()函数打印错误信息并退出。
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0); // wildcard address 0.0.0.0
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv)
    {
        die("bind()");
    }

    // listen
    // SOMAXCONN : 监听队列的长度默认1024个链接数
    rv = listen(fd, SOMAXCONN);
    if (rv)
    {
        die("listen()");
    }

    // a map of all client connections, keyed by fd
    std::vector<Conn *> fd2conn;

    // set the listen fd to nonblocking mode
    fd_set_nb(fd);

    // use  epoll
    // 创建epoll实例，并将监听套接字添加到epoll事件集合中：
    int epollfd = epoll_create(100);
    if (epollfd < 0)
    {
        die("epoll_create()");
    }
    // 将用于监听的套接字添加到epoll实例中
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET; // 检测fd读缓冲区是否有数据 边缘触发模式
    ev.data.fd = fd;
    int ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    if (ret < 0)
    {
        die("epoll_ctl");
    }

    /////////////////////////////////////////////
    // the event loop
    std::vector<struct epoll_event> epoll_events; 
    //存储epoll_wait返回的就绪的文件描述符个数，
    epoll_events.resize(1024);
    while (true)
    {
        // wait for events
        int num_events = epoll_wait(epollfd, epoll_events.data(), epoll_events.size(), -1);
        if (num_events < 0)
        {
            die("epoll_wait()");
        }
        // process events
        for (int i = 0; i < num_events; ++i)
        {
            //取出当前的文件描述符
            int event_fd = epoll_events[i].data.fd;
            //判断当前的文件描述符是不是用于监听的
            if (event_fd == fd)
            {
                // event on the listening fd, accept new connections
                while (true)
                {
                    //接收新的连接请求到达
                    int connfd = accept(fd, nullptr, nullptr);
                    if (connfd < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break; // no more connections to accept
                        }
                        else
                        {
                            die("accept()");
                        }
                    }

                    // set the new connection fd to nonblocking mode
                    fd_set_nb(connfd);

                    // create the struct Conn
                    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
                    if (!conn)
                    {
                        close(connfd);
                        continue;
                    }
                    conn->fd = connfd;
                    conn->state = STATE_REQ;
                    conn->rbuf_size = 0;
                    conn->wbuf_size = 0;
                    conn->wbuf_sent = 0;
                    conn_put(fd2conn, conn);

                    // add the new connection fd to the epoll set
                    struct epoll_event event = {};
                    event.data.fd = connfd;
                    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
                    rv = epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &event);
                    if (rv < 0)
                    {
                        die("epoll_ctl()");
                    }
                }
            }
            else
            {
                // event on a connection fd
                Conn *conn = fd2conn[event_fd];

                if (epoll_events[i].events & EPOLLIN)
                {
                    // read event
                    state_req(conn);
                    if (conn->state == STATE_END)
                    {
                        // client closed normally, or something bad happened.
                        // destroy this connection
                        fd2conn[conn->fd] = nullptr;
                        (void)close(conn->fd);
                        free(conn);
                    }
                }

                if (epoll_events[i].events & EPOLLOUT)
                {
                    // write event
                    state_res(conn);
                    if (conn->state == STATE_REQ)
                    {
                        // response was fully sent, change state back
                        conn->wbuf_sent = 0;
                        conn->wbuf_size = 0;

                        // modify the event to listen for read events only
                        struct epoll_event event = {};
                        event.data.fd = conn->fd;
                        event.events = EPOLLIN | EPOLLET;
                        rv = epoll_ctl(epollfd, EPOLL_CTL_MOD, conn->fd, &event);
                        if (rv < 0)
                        {
                            die("epoll_ctl()");
                        }
                    }
                }
            }
        }
    }
}