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

#include "co_routine.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <stack>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <vector>
#include <set>
#include <unistd.h>

#ifdef __FreeBSD__
#include <cstring>
#endif

using namespace std;

// 这个例子其实就是对poll的一个简单使用,
// 其中蕴含着一个隐蔽的测试情况,就是主线程调用含有co_enable_hook_sys的函数
// 与GetCurrThreadCo密切相关

struct task_t
{
	stCoRoutine_t *co;		// 协程的主结构体
	int fd;					// fd
	struct sockaddr_in addr;// 目标地址
};

static int SetNonBlock(int iSock)
{
    int iFlags;

    iFlags = fcntl(iSock, F_GETFL, 0);
    iFlags |= O_NONBLOCK;
    iFlags |= O_NDELAY;
    int ret = fcntl(iSock, F_SETFL, iFlags);
    return ret;
}



static void SetAddr(const char *pszIP,const unsigned short shPort,struct sockaddr_in &addr)
{
	bzero(&addr,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(shPort);
	int nIP = 0;
	if( !pszIP || '\0' == *pszIP   
	    || 0 == strcmp(pszIP,"0") || 0 == strcmp(pszIP,"0.0.0.0") 
		|| 0 == strcmp(pszIP,"*") 
	  )
	{
		nIP = htonl(INADDR_ANY);
	}
	else
	{
		nIP = inet_addr(pszIP);
	}
	addr.sin_addr.s_addr = nIP;

}

static int CreateTcpSocket(const unsigned short shPort  = 0 ,const char *pszIP  = "*" ,bool bReuse  = false )
{
	int fd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
	if( fd >= 0 )
	{
		if(shPort != 0)
		{
			if(bReuse)
			{
				int nReuseAddr = 1;
				setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&nReuseAddr,sizeof(nReuseAddr));
			}
			struct sockaddr_in addr ;
			SetAddr(pszIP,shPort,addr);
			int ret = bind(fd,(struct sockaddr*)&addr,sizeof(addr));
			if( ret != 0)
			{
				close(fd);
				return -1;
			}
		}
	}
	return fd;
}

static void *poll_routine( void *arg )
{
	co_enable_hook_sys();

	vector<task_t> &v = *(vector<task_t>*)arg;
	for(size_t i=0;i<v.size();i++)
	{
		int fd = CreateTcpSocket();	// 创建一个基于TCP,IP的套接字
		SetNonBlock( fd );			// 设置成非阻塞的
		v[i].fd = fd;

		// N个任务分别建立连接
		int ret = connect(fd,(struct sockaddr*)&v[i].addr,sizeof( v[i].addr )); 
		printf("co %p connect i %ld ret %d errno %d (%s)\n",
			co_self(),i,ret,errno,strerror(errno));
	}
	// 运行到这里的时候v.size()个连接已经完成了, V[i].fd现在都是有效的
	struct pollfd *pf = (struct pollfd*)calloc( 1,sizeof(struct pollfd) * v.size() );

	for(size_t i=0;i<v.size();i++)
	{
		pf[i].fd = v[i].fd;
		pf[i].events = ( POLLOUT | POLLERR | POLLHUP );
	}
	set<int> setRaiseFds;
	size_t iWaitCnt = v.size();
	// 用poll来监听这几个fd
	for(;;)
	{
		int ret = poll( pf,iWaitCnt,1000 );
		// 在iWaitCnt个poll事件中有ret个就绪
		// 运行这里时CPU执行权已经切换回来了.poll也完成,要么超时,要么fd时间就绪 
		printf("co %p poll wait %ld ret %d\n",
				co_self(),iWaitCnt,ret);
		
		for(int i=0;i<(int)iWaitCnt;i++)
		{
			// 这么输出是为了对比方便,不过直接&比较,输出true/false不香嘛
			printf("co %p fire fd %d revents 0x%X POLLOUT 0x%X POLLERR 0x%X POLLHUP 0x%X\n",
					co_self(),
					pf[i].fd,
					pf[i].revents,
					POLLOUT,
					POLLERR,
					POLLHUP
					);
			setRaiseFds.insert( pf[i].fd );
		}
		// 其实仔细一看,这个while循环永远值执行一次呀,
		// setRaiseFds.size() == v.size()在运行了以后一定是满足的
		if( setRaiseFds.size() == v.size())
		{
			// 等到所有的fd都返回消息或者超时
			break;
		}
		if( ret <= 0 )
		{
			break;
		}

		iWaitCnt = 0;
		for(size_t i=0;i<v.size();i++)
		{	// 在setRaiseFds找到了fd,感觉这里不太可能找不到,第二个for循环把所有的fd都插入了
			if( setRaiseFds.find( v[i].fd ) == setRaiseFds.end() )
			{
				pf[ iWaitCnt ].fd = v[i].fd;
				pf[ iWaitCnt ].events = ( POLLOUT | POLLERR | POLLHUP );
				++iWaitCnt;
			}
		}
	}
	for(size_t i=0;i<v.size();i++)
	{
		close( v[i].fd );
		v[i].fd = -1;
	}

	// 感觉这个打印的没什么意义,肯定永远是相同的
	printf("co %p task cnt %ld fire %ld\n",
			co_self(),v.size(),setRaiseFds.size() );
	return 0;
}
int main(int argc,char *argv[])
{
	vector<task_t> v;
	for(int i=1;i<argc;i+=2)
	{
		task_t task = { 0 };
		SetAddr( argv[i],atoi(argv[i+1]),task.addr );
		v.push_back( task );
	}

//------------------------------------------------------------------------------------
	printf("--------------------- main -------------------\n");
	// 这里是主协程,也就是我们主线程跑的函数,显然是串行的,跑完才会轮到208行
	vector<task_t> v2 = v;
	poll_routine( &v2 );
	printf("--------------------- routine -------------------\n");

	for(int i=0;i<10;i++)
	{
		stCoRoutine_t *co = 0;
		vector<task_t> *v2 = new vector<task_t>();
		*v2 = v;
		co_create( &co,NULL,poll_routine,v2 );
		printf("routine i %d\n",i);
		co_resume( co );
	}

	co_eventloop( co_get_epoll_ct(),0,0 );

	return 0;
}
//./example_poll 127.0.0.1 12365 127.0.0.1 12222 192.168.1.1 1000 192.168.1.2 1111

