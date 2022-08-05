#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./locker/locker.h"
#include "./threadpool/threadpool.h"
#include "./http_conn/http_conn.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000

extern int addfd( int epollfd, int fd, bool one_shot, bool enable_et );
extern int removefd( int epollfd, int fd );

//进行信号捕捉与处理
void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart ) //牛客视频里这个就没写
    {
        sa.sa_flags |= SA_RESTART;//由此信号中断的系统调用自动重启,Unix环境高级编程P279?
    }
    sigfillset( &sa.sa_mask );//临时阻塞全部信号
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void show_error( int connfd, const char* info )
{
    printf( "%s", info );
    send( connfd, info, strlen( info ), 0 );
    close( connfd );
}


int main( int argc, char* argv[] )
{
    if( argc <= 2 )
    {
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );

    //对SIGPIE信号进行处理_2.14_管道的读写特点和管道设置为非阻塞_PPT2.19信号
    //向一个没有读端的管道写数据会导致SIGPIPE信号的产生,使用SIG_IGN忽略此信号
    addsig(SIGPIPE, SIG_IGN);

    threadpool< http_conn >* pool = NULL;
    try
    {
        pool = new threadpool< http_conn >;
    }
    catch( ... )
    {
        return 1;//?为啥是1
        //牛客视频里这里写的exit(-1);
    }

    //预先为每个可能的客户连接分配一个http_conn对象
    http_conn* users = new http_conn[ MAX_FD ];
    assert( users );
    int user_count = 0;

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );
    //书P87、88、93、94
    //这样设置后,此时close系统调用立即返回,TCP模块将丢弃关闭的socket对应的TCP发送缓冲区残留的数据
    //,同时给对方发送一个复位报文段。因此,这种情况给服务器提供了异常终止一个连接的方法。
    struct linger tmp = { 1, 0 };
    setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret >= 0 );

    ret = listen( listenfd, 5 );
    assert( ret >= 0 );

    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd( epollfd, listenfd, false, false);
    http_conn::m_epollfd = epollfd;

    while( true )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) /*某种信号机制引起的错误?*/)
        {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 )
                {
                    printf( "errno is: %d\n", errno );
                    continue;
                }
                if( http_conn::m_user_count >= MAX_FD )
                {
                    show_error( connfd, "Internal server busy" );
                    continue;
                }
                
                //初始化客户连接
                users[connfd].init( connfd, client_address );
            }
            //EPOLLHUP表示读写都关闭
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )
            {
                //如由异常,直接关闭客户连接
                users[sockfd].close_conn();
            }
            else if( events[i].events & EPOLLIN )
            {
                //根据读的结果,决定是将任务添加到线程池,还是关闭连接
                if( users[sockfd].read() )
                {
                    pool->append( users + sockfd );
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            else if( events[i].events & EPOLLOUT )
            {
                //根据写的结果,决定是否关闭连接
                if( !users[sockfd].write() )
                {
                    users[sockfd].close_conn();
                }
            }
            else
            {}
        }
    }

    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}
