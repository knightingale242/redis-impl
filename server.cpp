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

//states of a connection
enum {
    STATE_REQ = 0, //reading requests
    STATE_RES = 1, //Sending responses
    STATE_END = 2,
};

const size_t k_max_msg = 4096;

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
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen); //get file descriptor of new connection
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
    conn_put(fd2conn, conn); //add new connection to vector of connections

    return 0;
}

static bool try_flush_buffer(Conn *conn) {
    ssize_t rv = 0;
    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        //got EAGAIN stop - error in c indicating that a resource is temporarily unavailable
        return false;
    }
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }

    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size) {
        //response was fully sent, can change state back
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    //still data in the buffer try to write again
    return true;
}

static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

static bool try_one_request(Conn *conn) {
    if (conn->rbuf_size < 4) {
        return false; //need atleast 4 bits for a valid request
    }

    //get length from the request
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);

    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }

    //got enough bits to decipher length but the full message has not arrived yet
    if (4 + len > conn->rbuf_size) {
        return false;
    }

    //got full message, going to echo it
    printf("client says: %.*s\n", len, &conn->rbuf[4]);

    //generate response
    memcpy(&conn->wbuf, &len, 4);
    memcpy(&conn->wbuf[4], &conn->rbuf[4], len);
    conn->wbuf_size += 4 + len;

    //removing previous message from read buffer
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        /*copying everying after previous message into the start of the reading buffer
        using memmove because the regions of memory we are copying to overlap so we need the
        safety guarantees to avoid undefined behavior*/
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;
    conn->state = STATE_RES;
    state_res(conn);

    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn *conn) {
    //rbuf_size = temp value to keep track of how big the buffer currently is
    //sizeof(rbuf) gets the max size the buffer should be
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size; //max value able to read into the buffer right now
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        return false;
    }
    if (rv < 0){
        msg("read() error");
        return false;
    }
    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF");
        }
        else {
            msg("EOF");
        }
    }

    conn->rbuf_size += sizeof(rv); //add however much was read into the buffer to the current size of the buffer
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    while(try_one_request(conn)) {} //clients could send multiple requests without waiting for responses which is why we loop
    return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn) {
    while(try_fill_buffer(conn));
}

static void connection_io(Conn *conn){
    if (conn->state == STATE_REQ) {
        state_req(conn);
    }
    else if (conn->state == STATE_RES) {
        state_res(conn);
    }
    else {
        assert(0); //not expected
    }
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
        die("bind() error!");
    }

    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen() error");
    }

    //list of all client connections
    std::vector<Conn *> fd2conn;

    //set listening fd to noblocking
    fd_set_nb(fd);

    //event loop implementation
    std::vector<struct pollfd> poll_args;
    while (true) {
        //empty argument array and push listening fd to 0th index
        poll_args.clear();
        struct pollfd pfd = {fd, POLL_IN, 0};
        poll_args.push_back(pfd);
        //iteration through all current connections
        for (Conn *conn : fd2conn) {
            if(!conn){
                continue;
            }
            //building pfd from each connection and adding to the list of connections in poll args
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
        //todo: use rv value to count how many events you have processed until you match the rv value and stop the loop early
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