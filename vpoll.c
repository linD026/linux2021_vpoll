#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define VPOLL_IOC_MAGIC '^'
#define VPOLL_IO_ADDEVENTS _IO(VPOLL_IOC_MAGIC, 1)
#define VPOLL_IO_DELEVENTS _IO(VPOLL_IOC_MAGIC, 2)
typedef struct state {
  /* child  */
  int efd;
  /* parent */
  int epfd;
  struct epoll_event ev;
} state;

state *new_state() {
  state *state = malloc(sizeof(struct state));

  return state;
};
void free_state(state *state __attribute__((unused))) { free(state); };

int pre_fork_setup(state *state __attribute__((unused))) {
  state->efd = open("/dev/vpoll", O_RDWR | O_CLOEXEC);
  state->epfd = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event tmp = {
      .events =
          EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLOUT | EPOLLHUP | EPOLLPRI,
      .data.u64 = 123456789,
  };
  state->ev = tmp;

  epoll_ctl(state->epfd, EPOLL_CTL_ADD, state->efd, &state->ev);

  printf("setup efd %d epfd %d\n", state->efd, state->epfd);

  return 0;
};

int cleanup(state *state) {
  close(state->efd);
  close(state->epfd);
  return 0;
};

int child_post_fork_setup(state *state __attribute__((unused))) { return 0; }

int child_warmup(int warmup_iters __attribute__((unused)),
                 state *state __attribute__((unused))) {
  return 0;
}

int child_loop(int iters, state *state) {
  printf("child efd %d \n", state->efd);
  for (int i = 0; i < iters; ++i) {
    if (i % 2)
        ioctl(state->efd, VPOLL_IO_ADDEVENTS, EPOLLIN);
    else
        ioctl(state->efd, VPOLL_IO_ADDEVENTS, EPOLLOUT);
  }
  ioctl(state->efd, VPOLL_IO_ADDEVENTS, EPOLLHUP);

  return 0;
}

int child_cleanup(state *state __attribute__((unused))) { return 0; }

int parent_post_fork_setup(state *state __attribute__((unused))) { return 0; }

int parent_warmup(int warmup_iters __attribute__((unused)),
                  state *state __attribute__((unused))) {
  return 0;
}
int parent_loop(int iters __attribute__((unused)), state *state) {
  printf("parent epfd %d \n", state->epfd);
  while (1) {
    int nfds = epoll_wait(state->epfd, &state->ev, 1, 1000);
    if (nfds == 0)
      printf("timeout...\n");
    else {
      ioctl(state->efd, VPOLL_IO_DELEVENTS, state->ev.events);
      if (state->ev.events & EPOLLHUP) break;
    }
  }
  return 0;
}
int parent_cleanup(state *state __attribute__((unused))) { return 0; }
