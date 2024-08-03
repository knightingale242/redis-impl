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

//kills program if some strange error occurs
static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

//helper for logging msg to console
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

//function for reading in from buffer 
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

//function for writing to write buffer
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

const size_t k_max_msg = 4096;

/*
This function builds the query that is going to be sent to the server
message structure: | len | message |
The first 4 byes will contain the length of the message
the rest of the message will contain text of len variable length
*/

static int32_t query(int fd, const char *text){
    uint32_t len = (uint32_t)strlen(text); //get length of message being sent
    if (len > k_max_msg){
        return -1; //error if message is longer than max message length described
    }
    
    char wbuf[4 + k_max_msg];
    memcpy(wbuf, &len, 4); //copying the length into the first four bytes of the write buffer
    memcpy(&wbuf[4], text, len); //copying the actual text to be sent into the rest of the write buffer
    if(int32_t err = write_all(fd, wbuf, 4 + len)) { //writes to the server file descripter the length of the message plus 4 bytes to include header
        return err;
    }

    //receiving the response from the server
    char rbuf[4 + k_max_msg + 1]; //buffer to read in response in same define format: | len | message |
    errno = 0;
    int32_t err = read_full(fd, rbuf, 4); //reading in the first 4 bytes (len) from the server file descriptor
    if (err){
        if (errno == 0){
            msg("EOF");
        }
        else{
            msg("read() error");
        }
        return err;
    }
    memcpy(&len, rbuf, 4); //copying the length from read buffer into len variable
    if (len > k_max_msg){
        msg("too long");
        return -1;
    }

    //reply body
    err = read_full(fd, &rbuf[4], len); //reading the message into the read buffer from the server file descriptor
    if(err){
        msg("read error");
        return err;
    }

    //process
    rbuf[4 + len] = '\0'; //add null terminator
    printf("server responded %s\n", &rbuf[4]);
    return 0;
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);  // 127.0.0.1
    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("connect");
    }

    int32_t err = query(fd, "hello1");
    if (err){
        goto L_DONE;
    }
    err = query(fd, "hello2");
    if (err){
        goto L_DONE;
    }
    err = query(fd, "hello3");
    if (err){
        goto L_DONE;
    }

L_DONE:
    close(fd);
    return 0;
}