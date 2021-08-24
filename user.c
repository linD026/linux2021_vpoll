#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define handle_error(msg)                                                      \
    do {                                                                       \
        perror(msg);                                                           \
        exit(EXIT_FAILURE);                                                    \
    } while (0)

#define VPOLL_IOC_MAGIC '^'
#define VPOLL_IO_ADDEVENTS _IO(VPOLL_IOC_MAGIC, 1)
#define VPOLL_IO_DELEVENTS _IO(VPOLL_IOC_MAGIC, 2)

int main(int argc, char *argv[])
{
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLOUT | EPOLLHUP |
                  EPOLLPRI,
        .data.u64 = 123456789,
    };

    int efd = open("/dev/vpoll", O_RDWR | O_CLOEXEC);
    if (efd == -1)
        handle_error("/dev/vpoll");
    int epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (efd == -1)
        handle_error("epoll_create1");

    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, efd, &ev) == -1)
        handle_error("epoll_ctl");

    switch (fork()) {
    // child
    case 0:
        sleep(1);
        ioctl(efd, VPOLL_IO_ADDEVENTS, EPOLLIN);
        sleep(1);
        ioctl(efd, VPOLL_IO_ADDEVENTS, EPOLLIN);
        sleep(1);
        ioctl(efd, VPOLL_IO_ADDEVENTS, EPOLLIN | EPOLLPRI);
        sleep(1);
        ioctl(efd, VPOLL_IO_ADDEVENTS, EPOLLPRI);
        sleep(1);
        ioctl(efd, VPOLL_IO_ADDEVENTS, EPOLLOUT);
        sleep(1);
        ioctl(efd, VPOLL_IO_ADDEVENTS, EPOLLHUP);
        exit(EXIT_SUCCESS);
    // parent
    default:
        while (1) {
            int nfds = epoll_wait(epollfd, &ev, 1, 1000);
            if (nfds < 0)
                handle_error("epoll_wait");
            else if (nfds == 0)
                printf("timeout...\n");
            else {
                printf("GOT event %x\n", ev.events);
                ioctl(efd, VPOLL_IO_DELEVENTS, ev.events);
                if (ev.events & EPOLLHUP)
                    break;
            }
        }
        break;

    case -1: /* should not happen */
        handle_error("fork");
    }

    close(epollfd);
    close(efd);
    return 0;
}
