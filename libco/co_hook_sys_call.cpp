/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
* 
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#include <netinet/in.h>
#include <errno.h>
#include <time.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>

#include <resolv.h>
#include <netdb.h>

#include <time.h>
#include <map>
#include "co_routine.h"
#include "co_routine_inner.h"
#include "co_routine_specific.h"

typedef long long ll64_t;

/*
 * 原因: 套接字的O_NONBLOCK属性决定了hook后的read函数是直接调用系统的read函数,
 * 还是调用原本的系统函数,如果不把这些信息保存下来,我们就无法实现hook函数.
 * 
 * 当然这个结构也不是必要的,有这个结构可以少几次系统调用
 * 
 * 可以通过fcntl的F_GETFL参数获取套接字是否为阻塞的信息,超时时间可以全局存储
 * 
 * 补充:还有一个很重要的作用就是在libco中套接字在hook后的fcntl中都设置为非阻塞,这里保存了套接字原有的阻塞属性
 */
struct rpchook_t
{
	int user_flag;									// 记录套接字的状态
	struct sockaddr_in dest; //maybe sockaddr_un;	// 套机字目标地址
	int domain; //AF_LOCAL->域套接字 , AF_INET->IP	 // 套接字类型

	struct timeval read_timeout;					// 读超时时间
	struct timeval write_timeout;					// 写超时时间
};

static inline pid_t GetPid()
{
	char **p = (char**)pthread_self();
	return p ? *(pid_t*)(p + 18) : getpid();
}
/* 
 * 套接字hook信息数组 - 存储(该线程内)所有协程中的套接字hook信息, 
 * 便于套接字hook信息在被hook的系统调用之间传递,
 * 部分被hook函数用这些信息来控制函数逻辑(详见下面被hook的系统调用).
 *
 * 理解这个数组是重点, 一部分被hook的系统调用初始化这些数组中的元素,
 * 另一部分被hook的系统调用获取数组元素来控制函数逻辑.
 */
static rpchook_t *g_rpchook_socket_fd[ 102400 ] = { 0 };

typedef int (*socket_pfn_t)(int domain, int type, int protocol);
typedef int (*connect_pfn_t)(int socket, const struct sockaddr *address, socklen_t address_len);
typedef int (*close_pfn_t)(int fd);

typedef ssize_t (*read_pfn_t)(int fildes, void *buf, size_t nbyte);
typedef ssize_t (*write_pfn_t)(int fildes, const void *buf, size_t nbyte);

typedef ssize_t (*sendto_pfn_t)(int socket, const void *message, size_t length,
	                 int flags, const struct sockaddr *dest_addr,
					               socklen_t dest_len);

typedef ssize_t (*recvfrom_pfn_t)(int socket, void *buffer, size_t length,
	                 int flags, struct sockaddr *address,
					               socklen_t *address_len);

/* 
 * 这里算是一个比较明显的错误,库中send的返回值为ssize_t,不加的话会导致使用时编译错误,
 * 错误为无法重载只有返回值不同的函数
 */
typedef ssize_t (*send_pfn_t)(int socket, const void *buffer, size_t length, int flags);
typedef ssize_t (*recv_pfn_t)(int socket, void *buffer, size_t length, int flags);

typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
typedef int (*setsockopt_pfn_t)(int socket, int level, int option_name,
			                 const void *option_value, socklen_t option_len);

typedef int (*fcntl_pfn_t)(int fildes, int cmd, ...);
typedef struct tm *(*localtime_r_pfn_t)( const time_t *timep, struct tm *result );

typedef void *(*pthread_getspecific_pfn_t)(pthread_key_t key);
typedef int (*pthread_setspecific_pfn_t)(pthread_key_t key, const void *value);

typedef int (*setenv_pfn_t)(const char *name, const char *value, int overwrite);
typedef int (*unsetenv_pfn_t)(const char *name);
typedef char *(*getenv_pfn_t)(const char *name);
typedef hostent* (*gethostbyname_pfn_t)(const char *name);
typedef res_state (*__res_state_pfn_t)();
typedef int (*__poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);

static socket_pfn_t g_sys_socket_func 	= (socket_pfn_t)dlsym(RTLD_NEXT,"socket");
static connect_pfn_t g_sys_connect_func = (connect_pfn_t)dlsym(RTLD_NEXT,"connect");
static close_pfn_t g_sys_close_func 	= (close_pfn_t)dlsym(RTLD_NEXT,"close");

static read_pfn_t g_sys_read_func 		= (read_pfn_t)dlsym(RTLD_NEXT,"read");
static write_pfn_t g_sys_write_func 	= (write_pfn_t)dlsym(RTLD_NEXT,"write");

static sendto_pfn_t g_sys_sendto_func 	= (sendto_pfn_t)dlsym(RTLD_NEXT,"sendto");
static recvfrom_pfn_t g_sys_recvfrom_func = (recvfrom_pfn_t)dlsym(RTLD_NEXT,"recvfrom");

static send_pfn_t g_sys_send_func 		= (send_pfn_t)dlsym(RTLD_NEXT,"send");
static recv_pfn_t g_sys_recv_func 		= (recv_pfn_t)dlsym(RTLD_NEXT,"recv");

static poll_pfn_t g_sys_poll_func 		= (poll_pfn_t)dlsym(RTLD_NEXT,"poll");

static setsockopt_pfn_t g_sys_setsockopt_func 
										= (setsockopt_pfn_t)dlsym(RTLD_NEXT,"setsockopt");
static fcntl_pfn_t g_sys_fcntl_func 	= (fcntl_pfn_t)dlsym(RTLD_NEXT,"fcntl");

static setenv_pfn_t g_sys_setenv_func   = (setenv_pfn_t)dlsym(RTLD_NEXT,"setenv");
static unsetenv_pfn_t g_sys_unsetenv_func = (unsetenv_pfn_t)dlsym(RTLD_NEXT,"unsetenv");
static getenv_pfn_t g_sys_getenv_func   =  (getenv_pfn_t)dlsym(RTLD_NEXT,"getenv");
static __res_state_pfn_t g_sys___res_state_func  = (__res_state_pfn_t)dlsym(RTLD_NEXT,"__res_state");

static gethostbyname_pfn_t g_sys_gethostbyname_func = (gethostbyname_pfn_t)dlsym(RTLD_NEXT, "gethostbyname");

static __poll_pfn_t g_sys___poll_func = (__poll_pfn_t)dlsym(RTLD_NEXT, "__poll");


/*
static pthread_getspecific_pfn_t g_sys_pthread_getspecific_func 
			= (pthread_getspecific_pfn_t)dlsym(RTLD_NEXT,"pthread_getspecific");

static pthread_setspecific_pfn_t g_sys_pthread_setspecific_func 
			= (pthread_setspecific_pfn_t)dlsym(RTLD_NEXT,"pthread_setspecific");

static pthread_rwlock_rdlock_pfn_t g_sys_pthread_rwlock_rdlock_func  
			= (pthread_rwlock_rdlock_pfn_t)dlsym(RTLD_NEXT,"pthread_rwlock_rdlock");

static pthread_rwlock_wrlock_pfn_t g_sys_pthread_rwlock_wrlock_func  
			= (pthread_rwlock_wrlock_pfn_t)dlsym(RTLD_NEXT,"pthread_rwlock_wrlock");

static pthread_rwlock_unlock_pfn_t g_sys_pthread_rwlock_unlock_func  
			= (pthread_rwlock_unlock_pfn_t)dlsym(RTLD_NEXT,"pthread_rwlock_unlock");
*/



static inline unsigned long long get_tick_count()
{
	uint32_t lo, hi;
	__asm__ __volatile__ (
			"rdtscp" : "=a"(lo), "=d"(hi)
			);
	return ((unsigned long long)lo) | (((unsigned long long)hi) << 32);
}

struct rpchook_connagent_head_t
{
    unsigned char    bVersion;
    struct in_addr   iIP;
    unsigned short   hPort;
    unsigned int     iBodyLen;
    unsigned int     iOssAttrID;
    unsigned char    bIsRespNotExist;
	unsigned char    sReserved[6];
}__attribute__((packed));


#define HOOK_SYS_FUNC(name) if( !g_sys_##name##_func ) { g_sys_##name##_func = (name##_pfn_t)dlsym(RTLD_NEXT,#name); }

static inline ll64_t diff_ms(struct timeval &begin,struct timeval &end)
{
	ll64_t u = (end.tv_sec - begin.tv_sec) ;
	u *= 1000 * 10;
	u += ( end.tv_usec - begin.tv_usec ) / (  100 );
	return u;
}



static inline rpchook_t * get_by_fd( int fd )
{
	if( fd > -1 && fd < (int)sizeof(g_rpchook_socket_fd) / (int)sizeof(g_rpchook_socket_fd[0]) )
	{
		return g_rpchook_socket_fd[ fd ];
	}
	return NULL;
}
static inline rpchook_t * alloc_by_fd( int fd )
{
	if( fd > -1 && fd < (int)sizeof(g_rpchook_socket_fd) / (int)sizeof(g_rpchook_socket_fd[0]) )
	{
		rpchook_t *lp = (rpchook_t*)calloc( 1,sizeof(rpchook_t) );
		lp->read_timeout.tv_sec = 1;
		lp->write_timeout.tv_sec = 1;
		g_rpchook_socket_fd[ fd ] = lp;
		return lp;
	}
	return NULL;
}
static inline void free_by_fd( int fd )
{
	if( fd > -1 && fd < (int)sizeof(g_rpchook_socket_fd) / (int)sizeof(g_rpchook_socket_fd[0]) )
	{
		rpchook_t *lp = g_rpchook_socket_fd[ fd ];
		if( lp )
		{
			g_rpchook_socket_fd[ fd ] = NULL;
			free(lp);	
		}
	}
	return;

}
int socket(int domain, int type, int protocol)
{
	HOOK_SYS_FUNC( socket );

	if( !co_is_enable_sys_hook() )
	{
		return g_sys_socket_func( domain,type,protocol );
	}
	int fd = g_sys_socket_func(domain,type,protocol);
	if( fd < 0 )
	{
		return fd;
	}

	rpchook_t *lp = alloc_by_fd( fd );
	lp->domain = domain;
	
	fcntl( fd, F_SETFL, g_sys_fcntl_func(fd, F_GETFL,0 ) );

	return fd;
}

int co_accept( int fd, struct sockaddr *addr, socklen_t *len )
{
	int cli = accept( fd,addr,len );
	if( cli < 0 )
	{
		return cli;
	}
	alloc_by_fd( cli );
	return cli;
}
int connect(int fd, const struct sockaddr *address, socklen_t address_len)
{
	HOOK_SYS_FUNC( connect );

	if( !co_is_enable_sys_hook() )
	{
		return g_sys_connect_func(fd,address,address_len);
	}

	//1.sys call
	// 看似迷惑,为什么直接调用?
	// 其实在socket这个函数中套接字已经被转换成非阻塞的了
	int ret = g_sys_connect_func( fd,address,address_len );

	// 获取当前fd对应套接字的信息,
	rpchook_t *lp = get_by_fd( fd );
	// 返回为空的话证明这个套接字不是由HOOK后的socket创建的,那自然不该由这个hook后的connect管理
	if( !lp ) return ret; 

	if( sizeof(lp->dest) >= address_len )
	{
		 memcpy( &(lp->dest),address,(int)address_len );
	}
	if( O_NONBLOCK & lp->user_flag ) 
	{
		return ret;	// 该套接字是非阻塞的
	}
	// 如果非阻塞connect下error为EINPROGRESS那么就代表连接还在进行中。
	// 后面可以通过poll或者select来判断socket是否可写，如果可以写，说明连接完成了。
	if (!(ret < 0 && errno == EINPROGRESS)) // 出现错误
	{
		return ret;
	}

	//2.wait
	int pollret = 0;
	struct pollfd pf = { 0 };

	for(int i=0;i<3;i++) //25s * 3 = 75s
	{
		memset( &pf,0,sizeof(pf) );
		pf.fd = fd;
		pf.events = ( POLLOUT | POLLERR | POLLHUP );

		// 从这里CPU的执行权就被切换了
		pollret = poll( &pf,1,25000 );	// 超时时间为25s

		if( 1 == pollret  ) // 返回为1证明要么超时要么可写,感觉还应该判断不超时才break,
		{
			break;
		}
	}

	if( pf.revents & POLLOUT ) // 连接成功
	{
    // 3.check getsockopt ret
    int err = 0;
    socklen_t errlen = sizeof(err);
	// 可写不一定连接成功 使用getsockopt确认一下
    ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (ret < 0) {
      return ret;
    } else if (err != 0) {
      errno = err;
      return -1;
    }
    errno = 0;
    return 0;
  }

  errno = ETIMEDOUT;	// 连接超时
	return ret;
}


int close(int fd)
{
	HOOK_SYS_FUNC( close );
	
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_close_func( fd );
	}

	free_by_fd( fd );
	int ret = g_sys_close_func(fd);

	return ret;
}

ssize_t read( int fd, void *buf, size_t nbyte )
{
	HOOK_SYS_FUNC( read );
	
	// 如果目前线程没有一个协程, 则直接执行系统调用
	if( !co_is_enable_sys_hook() )
	{	// dlsym以后得到的原函数
		return g_sys_read_func( fd,buf,nbyte );
	}
	// 获取这个文件描述符的详细信息
	rpchook_t *lp = get_by_fd( fd );

	// 套接字为非阻塞的,直接进行系统调用
	if( !lp || ( O_NONBLOCK & lp->user_flag ) ) 
	{
		ssize_t ret = g_sys_read_func( fd,buf,nbyte );
		return ret;
	}

	// 套接字阻塞
	int timeout = ( lp->read_timeout.tv_sec * 1000 ) 
				+ ( lp->read_timeout.tv_usec / 1000 );

	struct pollfd pf = { 0 };
	pf.fd = fd;
	pf.events = ( POLLIN | POLLERR | POLLHUP );

	// 调用co_poll, co_poll中会切换协程, 
	// 协程被恢复时将会从co_poll中的挂起点继续运行
	int pollret = poll( &pf,1,timeout );

	// 套接字准备就绪或者超时 执行hook前的系统调用
	ssize_t readret = g_sys_read_func( fd,(char*)buf ,nbyte );

	if( readret < 0 ) // 超时
	{
		co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d",
					fd,readret,errno,pollret,timeout);
	}
	// 成功读取
	return readret; 
	
}
ssize_t write( int fd, const void *buf, size_t nbyte )
{
	HOOK_SYS_FUNC( write );
	
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_write_func( fd,buf,nbyte );
	}
	rpchook_t *lp = get_by_fd( fd );

	// 我觉得这里有必要再强调一遍,user_flag是用户设定的,但对于libco来说,
	// 所有由hook函数创建的套接字对系统来说都是非阻塞的
	if( !lp || ( O_NONBLOCK & lp->user_flag ) )
	{
		ssize_t ret = g_sys_write_func( fd,buf,nbyte );
		return ret;
	}
	size_t wrotelen = 0;	//已写的长度
	int timeout = ( lp->write_timeout.tv_sec * 1000 ) // 计算超时时间
				+ ( lp->write_timeout.tv_usec / 1000 );

	// 因为TCP协议的原因,有时可能因为ask中接收方窗口小于write大小的原因无法发满
	ssize_t writeret = g_sys_write_func( fd,(const char*)buf + wrotelen,nbyte - wrotelen );

	if (writeret == 0)
	{
		return writeret;
	}

	if( writeret > 0 )
	{
		wrotelen += writeret;	
	}
	// 一次可能无法写入完全，发生在TCP发送窗口小于要发送的数据大小的时候，通常是对端数据堆积
	while( wrotelen < nbyte )
	{

		struct pollfd pf = { 0 };
		pf.fd = fd;
		pf.events = ( POLLOUT | POLLERR | POLLHUP );
		poll( &pf,1,timeout );

		writeret = g_sys_write_func( fd,(const char*)buf + wrotelen,nbyte - wrotelen );
		
		if( writeret <= 0 )
		{
			break;
		}
		wrotelen += writeret ;
	}
	if (writeret <= 0 && wrotelen == 0)
	{
		return writeret;
	}
	return wrotelen;
}

ssize_t sendto(int socket, const void *message, size_t length,
	                 int flags, const struct sockaddr *dest_addr,
					               socklen_t dest_len)
{
	/*
		1.no enable sys call ? sys
		2.( !lp || lp is non block ) ? sys
		3.try
		4.wait
		5.try
	*/
	HOOK_SYS_FUNC( sendto );
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_sendto_func( socket,message,length,flags,dest_addr,dest_len );
	}

	rpchook_t *lp = get_by_fd( socket );
	if( !lp || ( O_NONBLOCK & lp->user_flag ) )
	{
		return g_sys_sendto_func( socket,message,length,flags,dest_addr,dest_len );
	}

	ssize_t ret = g_sys_sendto_func( socket,message,length,flags,dest_addr,dest_len );
	if( ret < 0 && EAGAIN == errno )
	{
		int timeout = ( lp->write_timeout.tv_sec * 1000 ) 
					+ ( lp->write_timeout.tv_usec / 1000 );


		struct pollfd pf = { 0 };
		pf.fd = socket;
		pf.events = ( POLLOUT | POLLERR | POLLHUP );
		poll( &pf,1,timeout );

		ret = g_sys_sendto_func( socket,message,length,flags,dest_addr,dest_len );

	}
	return ret;
}

ssize_t recvfrom(int socket, void *buffer, size_t length,
	                 int flags, struct sockaddr *address,
					               socklen_t *address_len)
{
	HOOK_SYS_FUNC( recvfrom );
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_recvfrom_func( socket,buffer,length,flags,address,address_len );
	}

	rpchook_t *lp = get_by_fd( socket );
	if( !lp || ( O_NONBLOCK & lp->user_flag ) )
	{
		return g_sys_recvfrom_func( socket,buffer,length,flags,address,address_len );
	}

	int timeout = ( lp->read_timeout.tv_sec * 1000 ) 
				+ ( lp->read_timeout.tv_usec / 1000 );


	struct pollfd pf = { 0 };
	pf.fd = socket;
	pf.events = ( POLLIN | POLLERR | POLLHUP );
	poll( &pf,1,timeout );

	ssize_t ret = g_sys_recvfrom_func( socket,buffer,length,flags,address,address_len );
	return ret;
}

ssize_t send(int socket, const void *buffer, size_t length, int flags)
{
	HOOK_SYS_FUNC( send );
	
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_send_func( socket,buffer,length,flags );
	}
	rpchook_t *lp = get_by_fd( socket );

	if( !lp || ( O_NONBLOCK & lp->user_flag ) )
	{
		return g_sys_send_func( socket,buffer,length,flags );
	}
	size_t wrotelen = 0;
	int timeout = ( lp->write_timeout.tv_sec * 1000 ) 
				+ ( lp->write_timeout.tv_usec / 1000 );

	ssize_t writeret = g_sys_send_func( socket,buffer,length,flags );
	// socket突然断开会出现返回0
	if (writeret == 0)
	{
		return writeret;
	}

	if( writeret > 0 )
	{
		wrotelen += writeret;	
	}
	while( wrotelen < length )
	{

		struct pollfd pf = { 0 };
		pf.fd = socket;
		pf.events = ( POLLOUT | POLLERR | POLLHUP );
		poll( &pf,1,timeout );

		writeret = g_sys_send_func( socket,(const char*)buffer + wrotelen,length - wrotelen,flags );
		
		if( writeret <= 0 )
		{
			break;
		}
		wrotelen += writeret ;
	}
	if (writeret <= 0 && wrotelen == 0)
	{
		return writeret;
	}
	return wrotelen;
}

ssize_t recv( int socket, void *buffer, size_t length, int flags )
{
	HOOK_SYS_FUNC( recv );
	
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_recv_func( socket,buffer,length,flags );
	}
	rpchook_t *lp = get_by_fd( socket );

	if( !lp || ( O_NONBLOCK & lp->user_flag ) ) 
	{
		return g_sys_recv_func( socket,buffer,length,flags );
	}
	int timeout = ( lp->read_timeout.tv_sec * 1000 ) 
				+ ( lp->read_timeout.tv_usec / 1000 );

	struct pollfd pf = { 0 };
	pf.fd = socket;
	pf.events = ( POLLIN | POLLERR | POLLHUP );

	int pollret = poll( &pf,1,timeout );

	ssize_t readret = g_sys_recv_func( socket,buffer,length,flags );

	if( readret < 0 )
	{
		co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d",
					socket,readret,errno,pollret,timeout);
	}

	return readret;
	
}

extern int co_poll_inner( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout, poll_pfn_t pollfunc);

// 在hook以后的poll中应该执行协程的调度
int poll(struct pollfd fds[], nfds_t nfds, int timeout)
{ 
	HOOK_SYS_FUNC( poll );

	// 如果本线程不存在协程或者超时时间为零的话调用hook前的poll
	if (!co_is_enable_sys_hook() || timeout == 0) {
		return g_sys_poll_func(fds, nfds, timeout);
	}

	pollfd *fds_merge = NULL;
	nfds_t nfds_merge = 0; // 相当于一个单调递增的标志位
	std::map<int, int> m;  // fd --> idx
	std::map<int, int>::iterator it;
	if (nfds > 1) {
		fds_merge = (pollfd *)malloc(sizeof(pollfd) * nfds);
		for (size_t i = 0; i < nfds; i++) {
			if ((it = m.find(fds[i].fd)) == m.end()) { // 在mp中没有找到
				fds_merge[nfds_merge] = fds[i]; // 放入merge链表
				m[fds[i].fd] = nfds_merge; // 没找到就放进去
				nfds_merge++; // 游标递增
			} else {
				int j = it->second;
				fds_merge[j].events |= fds[i].events;  // merge in j slot
			}
		} 
	} 
	// 以上就相当于是一个小优化,就是查看此次poll中是否有fd相同的事件,有的话合并一下,仅此而已

	int ret = 0; 
	if (nfds_merge == nfds || nfds == 1) {// 没有执行合并
		// fds为poll的事件;nfds为事件数;timeout为超时时间;g_sys_poll_func为未hook的poll函数
		// 返回值为此次就绪的事件数 
		// 在co_poll_inner中有一个协程的切换
		ret = co_poll_inner(co_get_epoll_ct(), fds, nfds, timeout, g_sys_poll_func);
	} else {
		ret = co_poll_inner(co_get_epoll_ct(), fds_merge, nfds_merge, timeout,
				g_sys_poll_func);
		if (ret > 0) {
			// 把merge的事件还原一下
			for (size_t i = 0; i < nfds; i++) {
				it = m.find(fds[i].fd);
				if (it != m.end()) {
					int j = it->second;
					fds[i].revents = fds_merge[j].revents & fds[i].events;
				}
			}
		}
	}
	free(fds_merge);
	return ret;
}

int setsockopt(int fd, int level, int option_name,
			                 const void *option_value, socklen_t option_len)
{
	HOOK_SYS_FUNC( setsockopt );

	if( !co_is_enable_sys_hook() )
	{
		return g_sys_setsockopt_func( fd,level,option_name,option_value,option_len );
	}
	rpchook_t *lp = get_by_fd( fd );

	if( lp && SOL_SOCKET == level )
	{
		struct timeval *val = (struct timeval*)option_value;
		if( SO_RCVTIMEO == option_name  ) 
		{
			memcpy( &lp->read_timeout,val,sizeof(*val) );
		}
		else if( SO_SNDTIMEO == option_name )
		{
			memcpy( &lp->write_timeout,val,sizeof(*val) );
		}
	}
	return g_sys_setsockopt_func( fd,level,option_name,option_value,option_len );
}


int fcntl(int fildes, int cmd, ...)
{
	HOOK_SYS_FUNC( fcntl );

	if( fildes < 0 )
	{
		return __LINE__;
	}

	va_list arg_list;
	va_start( arg_list,cmd );

	int ret = -1;
	rpchook_t *lp = get_by_fd( fildes );
	switch( cmd )
	{
		case F_DUPFD:
		{
			int param = va_arg(arg_list,int); // 指向arg_list当前元素的下一个
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_GETFD:
		{
			// 用户创建的套接字如果不包含O_NONBLOCK的话,返回值也不包含
			ret = g_sys_fcntl_func( fildes,cmd );
      		if (lp && !(lp->user_flag & O_NONBLOCK)) {
          	ret = ret & (~O_NONBLOCK);
      		}
			break;
		}
		case F_SETFD:
		{
			int param = va_arg(arg_list,int);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_GETFL:
		{
			ret = g_sys_fcntl_func( fildes,cmd );
			break;
		}
		case F_SETFL:
		{
			int param = va_arg(arg_list,int);
			int flag = param;
			if( co_is_enable_sys_hook() && lp )
			{
				flag |= O_NONBLOCK;
			}
			ret = g_sys_fcntl_func( fildes,cmd,flag );
			if( 0 == ret && lp )
			{
				lp->user_flag = param;
			}
			break;
		}
		case F_GETOWN:
		{
			ret = g_sys_fcntl_func( fildes,cmd );
			break;
		}
		case F_SETOWN:
		{
			int param = va_arg(arg_list,int);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_GETLK:
		{
			struct flock *param = va_arg(arg_list,struct flock *);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_SETLK:
		{
			struct flock *param = va_arg(arg_list,struct flock *);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_SETLKW:
		{
			struct flock *param = va_arg(arg_list,struct flock *);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
	}

	va_end( arg_list );

	return ret;
}

struct stCoSysEnv_t
{
	char *name;	
	char *value;
};
struct stCoSysEnvArr_t //记录全局的环境变量
{
	stCoSysEnv_t *data;
	size_t cnt;
};
static stCoSysEnvArr_t *dup_co_sysenv_arr( stCoSysEnvArr_t * arr )
{
	stCoSysEnvArr_t *lp = (stCoSysEnvArr_t*)calloc( sizeof(stCoSysEnvArr_t),1 );	
	if( arr->cnt )
	{
		lp->data = (stCoSysEnv_t*)calloc( sizeof(stCoSysEnv_t) * arr->cnt,1 );
		lp->cnt = arr->cnt;
		memcpy( lp->data,arr->data,sizeof( stCoSysEnv_t ) * arr->cnt );
	}
	return lp;
}

static int co_sysenv_comp(const void *a, const void *b)
{
	return strcmp(((stCoSysEnv_t*)a)->name, ((stCoSysEnv_t*)b)->name); 
}
static stCoSysEnvArr_t g_co_sysenv = { 0 };


/**
 * name : 指针数组,每一个元素是一个环境变量名
 * cnt  : 指针数组的长度
*/
void co_set_env_list( const char *name[],size_t cnt)
{
	if( g_co_sysenv.data ) // 此函数只会被调用一次
	{
		return ;
	}
	g_co_sysenv.data = (stCoSysEnv_t*)calloc( 1,sizeof(stCoSysEnv_t) * cnt  );

	for(size_t i=0;i<cnt;i++)
	{	// 传入的指针数组中的指针有效
		if( name[i] && name[i][0] )
		{	// strdup用于字符串拷贝;char *strdup(char *s)
			g_co_sysenv.data[ g_co_sysenv.cnt++ ].name = strdup( name[i] );
		}
	}
	if( g_co_sysenv.cnt > 1 )
	{	// 使整个序列有序
		qsort( g_co_sysenv.data,g_co_sysenv.cnt,sizeof(stCoSysEnv_t),co_sysenv_comp );
		// 因为cnt大于1, lp,lq总是有效的
		stCoSysEnv_t *lp = g_co_sysenv.data;
		stCoSysEnv_t *lq = g_co_sysenv.data + 1;
		// 感觉这个循环像是在去重,如果是去重的话就有bug
		for(size_t i=1;i<g_co_sysenv.cnt;i++)
		{
			if( strcmp( lp->name,lq->name ) ) // 两个环境变量名不相同
			{
				++lp;
				if( lq != lp  )
				{
					*lp = *lq;
				}
			}
			++lq;
		}
		// 实际不同环境变量的数量
		g_co_sysenv.cnt = lp - g_co_sysenv.data + 1;
	}

}

// 这一系列函数还是通过设定一个用户态的

int setenv(const char *n, const char *value, int overwrite)
{
	HOOK_SYS_FUNC( setenv )
	// 已hook且调用过co_set_env_list来初始化g_co_sysenv
	if( co_is_enable_sys_hook() && g_co_sysenv.data )
	{
		stCoRoutine_t *self = co_self();
		if( self )
		{
			if( !self->pvEnv ) 	// 保存系统环境变量的指针
			{					// 把全局环境变量g_co_sysenv拷贝一份到此协程中
				self->pvEnv = dup_co_sysenv_arr( &g_co_sysenv );
			}
			stCoSysEnvArr_t *arr = (stCoSysEnvArr_t*)(self->pvEnv);

			// n就是环境变量名称,
			stCoSysEnv_t name = { (char*)n,0 };
			// 我们前面排过序,这里二分就可以了
			stCoSysEnv_t *e = (stCoSysEnv_t*)bsearch( &name,arr->data,arr->cnt,sizeof(name),co_sysenv_comp );

			if( e ) // 如果查找到的话
			{
				if( overwrite || !e->value  ) // 可覆盖或原变量无value
				{
					if( e->value ) free( e->value );
					e->value = ( value ? strdup( value ) : 0 );
				}
				return 0;
			}
		}

	}
	return g_sys_setenv_func( n,value,overwrite );
}
// 删除一个环境变量
int unsetenv(const char *n)
{
	HOOK_SYS_FUNC( unsetenv )
	if( co_is_enable_sys_hook() && g_co_sysenv.data )
	{
		stCoRoutine_t *self = co_self();
		if( self )
		{
			if( !self->pvEnv )
			{
				self->pvEnv = dup_co_sysenv_arr( &g_co_sysenv );
			}
			stCoSysEnvArr_t *arr = (stCoSysEnvArr_t*)(self->pvEnv);

			stCoSysEnv_t name = { (char*)n,0 };

			stCoSysEnv_t *e = (stCoSysEnv_t*)bsearch( &name,arr->data,arr->cnt,sizeof(name),co_sysenv_comp );

			if( e )
			{
				if( e->value )
				{
					free( e->value );
					e->value = 0;
				}
				return 0;
			}
		}

	}
	return g_sys_unsetenv_func( n );
}
char *getenv( const char *n )
{
	HOOK_SYS_FUNC( getenv )
	if( co_is_enable_sys_hook() && g_co_sysenv.data )
	{
		stCoRoutine_t *self = co_self();

		stCoSysEnv_t name = { (char*)n,0 };

		if( !self->pvEnv )
		{
			self->pvEnv = dup_co_sysenv_arr( &g_co_sysenv );
		}
		stCoSysEnvArr_t *arr = (stCoSysEnvArr_t*)(self->pvEnv);

		stCoSysEnv_t *e = (stCoSysEnv_t*)bsearch( &name,arr->data,arr->cnt,sizeof(name),co_sysenv_comp );

		if( e )
		{
			return e->value;
		}

	}
	return g_sys_getenv_func( n );

}
struct hostent* co_gethostbyname(const char *name);

struct hostent *gethostbyname(const char *name)
{
	HOOK_SYS_FUNC( gethostbyname );

#if defined( __APPLE__ ) || defined( __FreeBSD__ )
	return g_sys_gethostbyname_func( name );
#else
	if (!co_is_enable_sys_hook())
	{
		return g_sys_gethostbyname_func(name);
	}
	return co_gethostbyname(name);
#endif

}


struct res_state_wrap
{
	struct __res_state state;
};
CO_ROUTINE_SPECIFIC(res_state_wrap, __co_state_wrap);

extern "C"
{
	res_state __res_state() 
	{
		HOOK_SYS_FUNC(__res_state);

		if (!co_is_enable_sys_hook()) 
		{
			return g_sys___res_state_func();
		}

		return &(__co_state_wrap->state);
	}
	int __poll(struct pollfd fds[], nfds_t nfds, int timeout)
	{
		return poll(fds, nfds, timeout);
	}
}

struct hostbuf_wrap 
{
	struct hostent host;
	char* buffer;
	size_t iBufferSize;
	int host_errno;
};

CO_ROUTINE_SPECIFIC(hostbuf_wrap, __co_hostbuf_wrap);

#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
struct hostent *co_gethostbyname(const char *name)
{
	if (!name)
	{
		return NULL;
	}

	if (__co_hostbuf_wrap->buffer && __co_hostbuf_wrap->iBufferSize > 1024)
	{
		free(__co_hostbuf_wrap->buffer);
		__co_hostbuf_wrap->buffer = NULL;
	}
	if (!__co_hostbuf_wrap->buffer)
	{
		__co_hostbuf_wrap->buffer = (char*)malloc(1024);
		__co_hostbuf_wrap->iBufferSize = 1024;
	}

	struct hostent *host = &__co_hostbuf_wrap->host;
	struct hostent *result = NULL;
	int *h_errnop = &(__co_hostbuf_wrap->host_errno);

	int ret = -1;
	while (ret = gethostbyname_r(name, host, __co_hostbuf_wrap->buffer, 
				__co_hostbuf_wrap->iBufferSize, &result, h_errnop) == ERANGE && 
				*h_errnop == NETDB_INTERNAL )
	{
		free(__co_hostbuf_wrap->buffer);
		__co_hostbuf_wrap->iBufferSize = __co_hostbuf_wrap->iBufferSize * 2;
		__co_hostbuf_wrap->buffer = (char*)malloc(__co_hostbuf_wrap->iBufferSize);
	}

	if (ret == 0 && (host == result)) 
	{
		return host;
	}
	return NULL;
}
#endif


void co_enable_hook_sys() //�⺯������������,�����ļ��ᱻ���ԣ�����
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( co )
	{
		co->cEnableSysHook = 1;
	}
}

