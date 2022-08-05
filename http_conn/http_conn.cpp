#include "http_conn.h"

//定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
//网站根目录
const char* doc_root = "/home/laputa/WEB/2_BookWeb/web_2.0/resources";

//将文件描述符设置为非阻塞的,返回值没用
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

//将fd上的EPOLLIN和EPOLLET时间注册到epollfd指示的epoll内核时间表中
//,参数oneshot指定是否注册fd上二等EPOLLONESHOT事件
void addfd( int epollfd, int fd, bool one_shot, bool enable_et )
{
    epoll_event event;
    event.data.fd = fd;
    //EPOLLRDHUP可以方便地检测客户端是否断开(牛客视频说的)
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if( one_shot )
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

//删除事件表epollfd上的客户端fd
void removefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close( fd );
}

//重置fd上的事件。这样操作之后,尽管fd上的EPOLLONESHOT事件被注册
//,但是操作系统仍然会出发fd上的EPOLLIN事件,且只触发一次
void modfd( int epollfd, int fd, int ev )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭http连接
void http_conn::close_conn( bool real_close )
{
    if( real_close && ( m_sockfd != -1 ) )
    {
        //modfd( m_epollfd, m_sockfd, EPOLLIN );
        removefd( m_epollfd, m_sockfd );
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接进服务器的客户端的信息
void http_conn::init( int sockfd, const sockaddr_in& addr )
{
    m_sockfd = sockfd;
    m_address = addr;
    int error = 0;
    //书P88,获取并清除socket错误状态,error是回传的错误参数,但该程序中没有使用
    socklen_t len = sizeof( error );
    getsockopt( m_sockfd, SOL_SOCKET, SO_ERROR, &error, &len );

    //端口复用,是为了避免TIME_WAIT状态,仅用于调试,实际使用时应该去掉
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    addfd( m_epollfd, sockfd, true, true);
    m_user_count++;

    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = NULL;
    m_version = NULL;
    m_content_length = 0;
    m_host = NULL;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset( m_read_buf, '\0', READ_BUFFER_SIZE );
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE );
    memset( m_real_file, '\0', FILENAME_LEN );
}

//从状态机,用于解析出一行内容
//check_index指向buffer(应用程序的缓冲区)中当前正在分析的字节,read_index指向buffer中客户数据的尾部的下一字节
//,buffer中第0~checked_index字节都已分析完毕,第checked_index~(read_index-1)字节由本函数中的循环逐个进行分析
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx )
    {
        //获取当前要分析的字节
        temp = m_read_buf[ m_checked_idx ];
        //如果当前的字节是"\r",即回车符,则说明可能读到一个完整的行
        if ( temp == '\r' )
        {
            //如果"\r"字符碰巧是目前buffer中的最后一个已经被读入的客户数据,那么这次分析没有读到一个完整的行
            //,返回LINE_OPEN以表示还需要继续读取客户数据才能进一步分析
            if ( ( m_checked_idx + 1 ) == m_read_idx )
            {
                return LINE_OPEN;
            }
            //如果下一个字符是"\n",则说明我们成功读取到一个完整的行
            else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' )
            {
                m_read_buf[ m_checked_idx++ ] = '\0';//把回车符去掉
                m_read_buf[ m_checked_idx++ ] = '\0';//把换行符去掉
                return LINE_OK;
            }

            //否则的话,说明客户发送的HTTP请求存在语法错误问题
            return LINE_BAD;
        }
        //如果当前的字节是"\n",即换行符,则也说明可能读取到一个完整的行
        else if( temp == '\n' )
        {
            if( ( m_checked_idx >= 1/*感觉这里应该是大于等于1*/  ) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) )
            {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    //如果所有内容都分析完毕也没遇到"\r"字符,则返回LINE_OPEN,表示还需要继续读取客户数据才能进一步分析
    return LINE_OPEN;
}

//循环读取客户数据,直到无数据可读或对方关闭连接
bool http_conn::read()
{
    if( m_read_idx >= READ_BUFFER_SIZE )
    {
        return false;
    }

    int bytes_read = 0;
    while( true )
    {
        bytes_read = recv( m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        if ( bytes_read == -1 )
        {
            /*EAGAIN和 EWOULDBLOCK等效！
            从字面上来看，是提示再试一次
            。这个错误经常出现在当应用程序进行一些非阻塞(non-blocking)操作(对文件或socket)的时候
            。例如，以O_NONBLOCK的标志打开文件/socket/FIFO，如果你连续做read操作而没有数据可读
            。此时程序不会阻塞起来等待数据准备就绪返回，read函数会返回一个错误EAGAIN
            ，提示你的应用程序现在没有数据可读请稍后再试
            。又例如，当一个系统调用(比如fork)因为没有足够的资源(比如虚拟内存)而执行失败
            ，返回EAGAIN提示其再调用一次(也许下次就能成功)
            */
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return false;
        }
        else if ( bytes_read == 0 )
        {
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
}

//解析HTTP请求行,获得请求方法、目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line( char* text )
{
    //依次检验字符串s1中的字符，当被检验字符在字符串s2中也包含时，则停止检验，并返回该字符位置，空字符null不包括在内。
    //返回s1中第一个满足条件的字符的指针，如果没有匹配字符则返回空指针NULL。
    m_url = strpbrk( text, " \t" );
    //如果请求行中没有空白字符或"\t"字符,则HTTP请求必有问题
    if ( ! m_url )
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char* method = text;
    //strcasecmp用忽略大小写比较字符串.，通过strcasecmp函数可以指定每个字符串用于比较的字符数
    //，strcasecmp用来比较参数s1和s2字符串前n个字符，比较时会自动忽略大小写的差异。
    if ( strcasecmp( method, "GET" ) == 0 )
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }

    //size_t strspn (const char *s,const char * accept);
    //函数说明 strspn()从参数s 字符串的开头计算连续的字符，而这些字符都完全是accept 所指字符串中的字符
    //。简单的说，若strspn()返回的数值为n，则代表字符串s 开头连续有n 个字符都是属于字符串accept内的字符。
    m_url += strspn( m_url, " \t" );//我认为这里是为了越过上方" \t",以便开始寻找下一个" \t"
    m_version = strpbrk( m_url, " \t" );
    if ( ! m_version )
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn( m_version, " \t" );
    //仅支持HTTP/1.1
    if ( strcasecmp( m_version, "HTTP/1.1" ) != 0 )
    {
        return BAD_REQUEST;
    }

    //检查URL是否合法
    if ( strncasecmp( m_url, "http://", 7 ) == 0 )
    {
        m_url += 7;
        //strchr函数功能为在一个串中查找给定字符的第一个匹配之处
        //。函数原型为：char *strchr(const char *str, int c)
        //，即在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。 
        //返回一个指向该字符串中第一次出现的字符的指针，如果字符串中不包含该字符则返回NULL空指针
        m_url = strchr( m_url, '/' );//这是将url定位到的第一个/, 可能是防止用户在网页中输入了空格或其他字符
    }

    if ( ! m_url || m_url[ 0 ] != '/' )
    {
        return BAD_REQUEST;
    }

    printf( "The request URL is: %s\n", m_url );
    //HTTP请求行处理完毕,状态转移到头部字段的分析
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers( char* text )
{
    //遇到空行
    if( text[ 0 ] == '\0' )
    {
        //如果当前解析的是头部字段,则说明我们得到了一个正确完整的HTTP请求
        if ( m_method == HEAD )
        {
            return GET_REQUEST;
        }

        //如果HTTP请求有消息体,则还需要读取m_content_length字节的消息体,状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 )
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    //处理Connection头部字段
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 )
    {
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 )
        {
            m_linger = true;
        }
    }
    //处理Content-Length头部字段
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 )
    {
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol( text );
    }
    //处理Host头部字段
    else if ( strncasecmp( text, "Host:", 5 ) == 0 )
    {
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }
    else
    {
        printf( "oop! unknow header %s\n", text );
    }

    return NO_REQUEST;

}

//我们没有真正解析HTTP请求的消息体,只是判断它是否被完整地读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text )
{
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

//主机状态,其分析参考8.6节 解析HTTP请求 这是8-3httpparser_my.cpp的parse_content函数
http_conn::HTTP_CODE http_conn::process_read()
{
    //记录当前行的读取状态
    LINE_STATUS line_status = LINE_OK;
    //记录HTTP请求的处理结果
    HTTP_CODE ret = NO_REQUEST;
    char* text = NULL;

    //主状态机,用于从buffer中取出所有完整的行
    while ( ( ( m_check_state == CHECK_STATE_CONTENT ) && ( line_status == LINE_OK  ) )
                || ( ( line_status = parse_line() ) == LINE_OK ) )
    {
        text = get_line();
        //记录下一行的起始位置
        m_start_line = m_checked_idx;
        printf( "got 1 http line: %s\n", text );

        //m_check_state记录主状态机当前的状态
        switch ( m_check_state )
        {
            //第一个状态,分析请求行
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                break;
            }
            //第二个状态,分析请求头部字段
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                else if ( ret == GET_REQUEST )
                {
                    return do_request();
                }
                break;
            }
            //第三个状态,分析请求数据
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content( text );
                if ( ret == GET_REQUEST )
                {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }

    //表示还需要进一步分析
    return NO_REQUEST;
}

//当遇到一个完整、正确的HTTP请求时,我们就分析目标文件的属性,如果目标文件存在、对所有用户可读
//，且不是目录,则使用mmap将其映射到内存地址m_file_address处,并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    /*
    函数原型： char *strncpy(char *dest, const char *src, int n)
    返回值：dest字符串起始地址
    说明：
    1、当src字符串长度小于n时，则拷贝完字符串后
    ，剩余部分将用空字节填充(重点,这也是他问什么将n写成FILENAME_LEN - len - 1的原因)
    ，直到n个strncpy不会向dest追加’\0’。
    2、src和dest所指的内存区域不能重叠，且dest必须有足够的空间放置n个字符
    */
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    if ( stat( m_real_file, &m_file_stat ) < 0 )
    {
        return NO_RESOURCE;
    }

    if ( ! ( m_file_stat.st_mode & S_IROTH ) )//读取权限不足,S_IROTH的意思应该是"其他读"
    {
        return FORBIDDEN_REQUEST;
    }

    if ( S_ISDIR( m_file_stat.st_mode ) )//S_ISDIR()函数的作用是判断一个路径是不是目录
    {
        return BAD_REQUEST;
    }

    int fd = open( m_real_file, O_RDONLY );
    //mmap的用法看lesson25的mmap-parent-child-ipc.c
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

//对内存映射区执行munmap操作
void http_conn::unmap()
{
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = NULL;
    }
}

//写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if ( bytes_to_send == 0 )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        init();
        return true;
    }

    while( 1 )
    {
        //对于EPOLLIN : 如果状态改变了[ 比如 从无到有],那么只要输入缓冲区可读就会触发
        //对于EPOLLOUT: 如果状态改变了[比如 从满到不满],只要输出缓冲区可写就会触发;
        //详见https://blog.csdn.net/dashoumeixi/article/details/94406535 解释.c的第二个
        temp = writev( m_sockfd, m_iv, m_iv_count );
        if ( temp <= -1 )
        {
            //如果TCP写缓冲没有空间,则等待下一轮EPOLLOUT事件,虽然在此期间,服务器无法立即接收到同一客户的下一个请求
            //,但这可以保证连接的完整性
            if( errno == EAGAIN )
            {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            //响应成功,根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        //if ( bytes_to_send <= bytes_have_send )
        if( bytes_to_send <= 0 )
        //https://blog.csdn.net/ad838931963/article/details/118598882?
        //解释.c的第三个
        {
            unmap();
            if( m_linger )
            {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            }
            else
            {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

//往写缓冲区写入待发送的数据
//https://blog.csdn.net/weixin_40332490/article/details/105306188
//详情查看 解释.cpp
bool http_conn::add_response( const char* format, ... )
{
    if( m_write_idx >= WRITE_BUFFER_SIZE )
    {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) )
    {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title )
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers( int content_len )
{
    add_content_length( content_len );
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length( int content_len )
{
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

//根据服务器处理HTTP请求的结果,决定返回给客户端的内容
bool http_conn::process_write( HTTP_CODE ret )
{
    switch ( ret )
    {
        case INTERNAL_ERROR:
        {
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) )
            {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) )
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) )
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line( 403, error_403_title );
            add_headers( strlen( error_403_form ) );
            if ( ! add_content( error_403_form ) )
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line( 200, ok_200_title );
            if ( m_file_stat.st_size != 0 )
            {
                add_headers( m_file_stat.st_size );
                m_iv[ 0 ].iov_base = m_write_buf;
                m_iv[ 0 ].iov_len = m_write_idx;
                m_iv[ 1 ].iov_base = m_file_address;
                m_iv[ 1 ].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else
            {
                const char* ok_string = "<html><body></body></html>";
                add_headers( strlen( ok_string ) );
                if ( ! add_content( ok_string ) )
                {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

//由线程池中的工作线程调用,这是处理HTTP请求的入口函数
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }

    bool write_ret = process_write( read_ret );
    if ( ! write_ret )
    {
        close_conn();
    }

    modfd( m_epollfd, m_sockfd, EPOLLOUT );
}

