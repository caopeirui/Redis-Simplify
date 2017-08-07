/* Linux epoll(2) based ae.c module */
#include "aeepoll.h"
#include <sys/time.h>

/*
 * 创建一个新的 epoll 实例，并将它赋值给 eventLoop
 */
int aeApiCreate(aeEventLoop *eventLoop) {

    aeApiState *state = zmalloc(sizeof(aeApiState)); /* aeApiState这个东西是用来干什么的? */

    if (!state) return -1;

    /* 初始化事件槽空间 */
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }

    /* 创建 epoll 实例 */
    state->epfd = epoll_create(1024); /* 1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }

    /* 赋值给 eventLoop
	 * apidata这个玩意被设置为了私有数据 */
    eventLoop->apidata = state;
    return 0;
}

/*
 * 调整事件槽大小
 */
int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    state->events = zrealloc(state->events, sizeof(struct epoll_event)*setsize); /* 重新分配槽的大小 */
    return 0;
}

/*
 * 释放 epoll 实例和事件槽
 */
void aeApiFree(aeEventLoop *eventLoop) { /* 我发现这里的一套东西,既要有初始化的函数,也要有析构的函数,差不多每个东西都要这样吧. */
    aeApiState *state = eventLoop->apidata;
    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

/*
 * 关联给定事件到 fd
 */
int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) { /* 添加到某个事件之上,好吧,主要是看封装,道理我们都懂是吧. */
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;

    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. 
     *
     * 如果 fd 没有关联任何事件，那么这是一个 ADD 操作。
     *
     * 如果已经关联了某个/某些事件，那么这是一个 MOD 操作。
     */
    int op = eventLoop->events[fd].mask == AE_NONE ?
            EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    /* 注册事件到 epoll */
    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.fd = fd;

    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) return -1; /* -1代表失败. */

    return 0; /* 0代表成功. */
}

/*
 * 从 fd 中删除给定事件
 */
void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;

    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd,EPOLL_CTL_MOD,fd,&ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee);
    }
}

/*
 * 获取可执行事件
 */
int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) { 
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

	int timeout = tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : -1;
    /* 等待时间 */
	retval = epoll_wait(state->epfd, state->events, eventLoop->setsize, timeout);

    /* 有至少一个事件就绪？ */
    if (retval > 0) {
        int j;

        /* 为已就绪事件设置相应的模式
         * 并加入到 eventLoop 的 fired 数组中 */
        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;

            if (e->events & EPOLLIN) mask |= AE_READABLE; /* 可读 */
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE; /* 可写 */
            if (e->events & EPOLLERR) mask |= AE_WRITABLE; /* 出错 */
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;

            eventLoop->fired[j].fd = e->data.fd; /* 放入到就绪队列之中 */
            eventLoop->fired[j].mask = mask; /* 干的漂亮,我想说的一点是,既然是这么放的话,那么应该一次性处理完所有的事件,然后才能继续调用epoll_wait */
        }
    }
    
    /* 返回已就绪事件个数 */
    return numevents;
}

/*
 * 返回当前正在使用的 poll 库的名字
 */
char *aeApiName(void) {
    return "epoll";
}