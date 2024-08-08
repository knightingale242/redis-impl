#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <vector>

#define PORT "6446"

#define BACKLOG 10
    
void sigchild_handler(int s) {
    //waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;
    while(waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}

void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int get_listener_socket(void) {
    int listener;
    int yes = 1;
    int rv;

    struct addrinfo hints, *ai, *p;

    //get socket and bind it
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if ((rv = getaddrinfo(NULL, PORT, &hints, &ai)) != 0) {
        fprintf(stderr, "pollserver: %s\n", gai_strerror(rv));
        exit(1);
    }

    for (p = ai; p != NULL; p = p->ai_next) {
        listener = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listener < 0) {
            continue;
        }

        //fixes address in use error
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

        if (bind(listener, p->ai_addr, p->ai_addrlen) < 0) {
            close(listener);
            continue;
        }

        break;
    }

    freeaddrinfo(ai);

    if (p == NULL) {
        return -1;
    }

    if (listen(listener, 10) == -1) {
        return -1;
    }

    return listener;
}

void add_to_pfds(std::vector<struct pollfd> &pfds, int newfd) {
    struct pollfd pfd = {newfd, POLLIN, 0};
    pfds.push_back(pfd);
}

void del_from_pfds(std::vector<struct pollfd> &pfds, int i) {
    if (i < pfds.size()) {
        pfds.erase(pfds.begin() + i);
    }
}

int main(void) {
    int listener;

    int newfd;
    struct sockaddr_storage remoteaddr;
    socklen_t addrlen;

    char buf[256];

    char remoteIP[INET6_ADDRSTRLEN];

    std::vector<struct pollfd> pfds;

    listener = get_listener_socket();
    if (listener == -1) {
        fprintf(stderr, "error getting listening socket\n");
        exit(1);
    }

    struct pollfd lis_pfd = {listener, POLLIN, 0};
    pfds.push_back(lis_pfd);
    //event loop 
    for (;;) {
        //know how many events we are looking for
        int poll_count = poll(pfds.data(), (nfds_t)pfds.size(), -1);

        if (poll_count == -1) {
            perror("poll");
            exit(1);
        }

        for (int i = 0; i < pfds.size(); i++) {
            if (pfds.at(i).revents & POLLIN) {
                if (pfds.at(i).fd == listener) {
                    addrlen = sizeof remoteaddr;
                    newfd = accept(listener, (struct sockaddr *)&remoteaddr, &addrlen);
                    if (newfd == -1) {
                        perror("accept");
                    }
                    else {
                        add_to_pfds(pfds, newfd);
                        printf("pollserver: new connection from %s on socket %d\n", inet_ntop(remoteaddr.ss_family, get_in_addr((struct sockaddr *)&remoteaddr), remoteIP, INET6_ADDRSTRLEN), newfd);
                    }
                }
                else {
                    int nbytes = recv(pfds[i].fd, buf, sizeof(buf) - 1, 0);
                    int sender_fd = pfds[i].fd;

                    if (nbytes <= 0) {
                        if (nbytes == 0) {
                            printf("pollserver: socket %d hung up\n", sender_fd);
                        } else {
                            perror("recv() error");
                        }
                        close(pfds.at(i).fd);
                        del_from_pfds(pfds, i);
                    }
                    else {
                        printf("server: received '%s'\n", buf);
                        if (send(sender_fd, buf, nbytes, 0) == -1) {
                            perror("send");
                        }
                    }
                } //read from poll
            } //looping through file descriptors
        } //infinite eventloop
    }
    return 0;
}