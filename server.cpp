#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include<cassert>
#include<vector>
#include<fcntl.h>
#include<poll.h>

enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2,
};

struct Conn {
    int fd = -1;
    uint32_t state = 0;
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint32_t wbuf[4 + k_max_msg];
};

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void do_something(int connfd) {
    char rbuf[64] = {};
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);
    if (n < 0) {
        msg("read() error");
        return;
    }
    printf("client says: %s\n", rbuf);

    char wbuf[] = "world";
    write(connfd, wbuf, strlen(wbuf));
}

static int32_t read_full(int fd, char *buf, size_t n){
    while (n > 0){
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0){
            return -1; //error or unexpected EOF
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, char *buf, size_t n){
    while (n > 0){
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0){
            return -1; //error
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv; //decrements n by rv bytes
        buf += rv; //increments pointer by rv bytes
    }
    return 0;
}

static void fd_set_nb(int fd){
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
    }

    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if(errno){
        die("fnctl error");
    }
}

const size_t k_max_msg = 4096;

//parser to handle incoming requests
//same as client but start with reading and then writing
static int32_t one_request(int connfd){
    //4 byte header
    char rbuf[4 + k_max_msg + 1];
    errno = 0;
    int32_t err = read_full(connfd, rbuf, 4);
    if (err){
        if (errno == 0){
            msg("EOF");
        }
        else{
            msg("read() error");
            return err;
        }
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);
    if (len > k_max_msg){
        msg("too long");
        return -1;
    }
    
    //request body
    err = read_full(connfd, &rbuf[4], len);
    if (err){
        msg("read() error");
        return err;
    }

    //processing
    rbuf[4 + len] = '\0';
    printf("client says %s\n", &rbuf[4]);

    //reply using same protocol
    const char reply[] = "world";
    char wbuf[4 + sizeof(reply)];
    len = (uint32_t)strlen(reply);
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], reply, len);
    return write_all(connfd, wbuf, 4 + len);
}

static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn){
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd) {
    //accept
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0){
        msg("accept() error");
        return -1;
    }

    //set new connection to nonblocking mode
    fd_set_nb(connfd);

    //creating the struct conn
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn *));
    if (!conn) {
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

static void connection_io(Conn *conn){
    // if (conn->state == STATE_REQ) {
    //     state_req(conn);
    // }
    // else if (conn->state == STATE_RES) {
    //     state_res(conn);
    // }
    // else {
    //     assert(0); //not expected
    // }
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    // this is needed for most server applications
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0);    // wildcard address 0.0.0.0
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    //list of all client connections
    std::vector<Conn *>fd2conn;

    //set listening fd to noblocking
    fd_set_nb(fd);

    //event loop implementation
    std::vector<struct pollfd> poll_args;
    while (true) {
        //prepare arguments for poll
        poll_args.clear();
        //listening fd in first position
        struct pollfd pfd = {fd, POLL_IN, 0};
        poll_args.push_back(pfd);
        //connection file descriptors
        for (Conn *conn : fd2conn) {
            if(!conn){
                continue;
            }
            //building pfd from each connection
            struct pollfd pfd = {};
            pfd.fd = conn->fd;
            pfd.events = (conn->state == STATE_REQ) ? POLL_IN : POLL_OUT;
            pfd.events = pfd.events | POLL_ERR;
            poll_args.push_back(pfd);
        }

        //poll for active file descriptors
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), 1000);
        if (rv < 0) {
            die("poll");
        }

        //process active connections
        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn *conn = fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    //client closed normally or something bad happened
                    //destroy connection
                    fd2conn[conn->fd] = NULL;
                    (void)close(conn->fd);
                    free(conn);
                }
            }
        }

        //try to accept a new connection if listening fd is active
        if (poll_args[0].revents) {
            (void)accept_new_conn(fd2conn, fd);
        }
    }

    return 0;
}