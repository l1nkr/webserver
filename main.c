#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./mysql/sqlpool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot,bool trigmode);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

void create_daemon() {
	pid_t pid;
	pid = fork();
	if(pid == -1) {
		printf("fork error\n");
		exit(1);
	} else if(pid) {
		exit(0);
	}
 	//setsid使子进程独立。摆脱会话控制、摆脱原进程组控制、摆脱终端控制
	if(-1 == setsid()) {
		printf("setsid error\n");
		exit(1);
	}
  	//通过再次创建子进程结束当前进程，使进程不再是会话首进程来禁止进程重新打开控制终端
	pid = fork();
	if(pid == -1) {
		printf("fork error\n");
		exit(1);
	} else if(pid) {
		exit(0);
	}
  	//子进程中调用chdir()让根目录成为子进程工作目录
	chdir("/");
	int i;
	//关闭文件描述符
	for(i = 0; i < 3; ++i) {
		close(i);
	}
	//重设文件掩码为0（将权限全部开放）
	umask(0);
	return;
}
//信号处理函数
void sig_handler(int sig) {
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

/*
sig:需要处理的信号
handler:信号处理函数
restart:是否重启系统调用
*/
void addsig(int sig, void(handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;//系统调用被信号打断时，重启系统调用（慢速系统调用）
    }
    sigfillset(&sa.sa_mask);//忽略所有信号
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}

void show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[]) {
    create_daemon();
    if (argc <= 1) {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    int port = atoi(argv[1]);

    addsig(SIGCHLD, SIG_IGN);
    addsig(SIGPIPE, SIG_IGN);

    //创建数据库连接池
    connection_pool* connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "", "", 3306, 8);

    //创建线程池
    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...) {
        return 1;
    }

    http_conn* users_http = new http_conn[MAX_FD];
    assert(users_http);

    //从数据库中取出用户名和密码，存到map中
    users_http->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    struct linger tmp = {1, 5};
    //SO_LINGER若有数据待发送，延迟关闭  优雅关闭连接
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    /*
    1）SOL_SOCKET:通用套接字选项
    2）IPPROTO_IP:IP选项
    3）IPPROTO_TCP:TCP选项
    SO_REUSEADDR      允许重用本地地址和端口   
    */

    int ret = 0;
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(1024);
    assert(epollfd != -1);
    http_conn::m_epollfd = epollfd;

    addfd(epollfd, listenfd, false, 0);//注册epoll内核中的事件，同时在这里设置listenfd是LT

    //创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false, 1);//注册epoll内核中的事件，同时在这里设置pipefd[0]是 ET/LT

    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    //统一信号源  将收到的信号通过信号回调函数 往管道里面写  而这个管道是由线程在监听的
    
    client_data* users_timer = new client_data[MAX_FD];

    bool stop_server = false;
    bool timeout = false;
    //最小超时单位
    alarm(TIMESLOT);

    while (!stop_server) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        //errno:  4  Interrupted system call
        if (number < 0 && errno != EINTR) {
            break;
        }
        for (int i = 0; i < number; i++) {
            int new_fd = events[i].data.fd;
            //处理新到的客户连接   LT模式
            if (new_fd == listenfd) {
                sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlen);

                if (connfd < 0) {
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD) {
                    show_error(connfd, "Internal server busy");
                    continue;
                }
                //初始化套接字地址,注册epollonshot et,初始化http_conn对象中的成员变量
                users_http[connfd].init(connfd, client_address);
                
                //设置定时器相关配置
                util_timer *timer = new util_timer();
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                timer->expire = time(NULL) + 3 * TIMESLOT;
                timer_lst.add_timer(timer);

                //设置相关配置
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                users_timer[connfd].timer = timer;
                continue;

            } else if ((new_fd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                //处理信号
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                //另一端已关闭则返回0。失败返回-1，
                if (ret == -1||ret == 0) {
                    continue;
                } else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                            case SIGALRM:
                                timeout = true;
                                break;
                            case SIGTERM:
                                stop_server = true;
                                break;
                            default:
                                break;
                        }
                    }
                }

            } else if (events[i].events & EPOLLIN) {
                util_timer* timer = users_timer[new_fd].timer;
                //在此处读取所有数据
                if (users_http[new_fd].read_once()) {
                    //将该事件放入请求队列
                    pool->append(users_http + new_fd);
                    //若有数据传输，则将定时器往后延迟3个单位，并对新的定时器在链表上的位置进行调整
                    if (timer) {
                        timer->expire = time(NULL) + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    timer->cb_func(&users_timer[new_fd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }

            } else if (events[i].events & EPOLLOUT) {
                util_timer *timer = users_timer[new_fd].timer;
                if (users_http[new_fd].write()) {
                    if (timer) {
                        timer->expire = time(NULL) + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    timer->cb_func(&users_timer[new_fd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }

            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                /*events可以是以下几个宏的集合：
                EPOLLIN ：表示对应的文件描述符可以读（包括对端SOCKET正常关闭）；
                EPOLLOUT：表示对应的文件描述符可以写；
                EPOLLPRI：表示对应的文件描述符有紧急的数据可读（这里应该表示有带外数据到来）；
                EPOLLERR：表示对应的文件描述符发生错误；
                EPOLLHUP：表示对应的文件描述符被挂断；
                EPOLLET： 将EPOLL设为边缘触发(Edge Triggered)模式，这是相对于水平触发(Level Triggered)来说的。
                EPOLLONESHOT：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里
                */
                util_timer *timer = users_timer[new_fd].timer;
                //从epoll内核删除，从定时器链表中删除
                timer->cb_func(&users_timer[new_fd]);
                if (timer) {
                    timer_lst.del_timer(timer);
                }

            } 
        }
        if (timeout) {
            timer_lst.tick();//遍历时间链表，将所有超时事件都删除
            alarm(TIMESLOT);//重新定时
            //超时的后果就是将这个连接的所有信息与资源释放掉
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users_http;
    delete[] users_timer;
    delete pool;
    return 0;
}
