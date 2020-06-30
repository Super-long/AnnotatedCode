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
#include "co_routine_inner.h"
#include "co_epoll.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>

#include <poll.h>
#include <sys/time.h>
#include <errno.h>

#include <assert.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>

#include <iostream> // 测试完删了

extern "C"
{
	extern void coctx_swap( coctx_t *,coctx_t* ) asm("coctx_swap");
};
using namespace std;
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env );
struct stCoEpoll_t;

/*
 * 每当启动（resume）一个协程时，就将它的协程控制块 stCoRoutine_t 结构指针保存在 pCallStack 的“栈顶”，
 * 然后“栈指针” iCallStackSize 加 1，最后切换 context 到待启动协程运行。当协程要让出（yield）CPU 时，
 * 就将它的 stCoRoutine_t从pCallStack 弹出，“栈指针” iCallStackSize 减 1，
 * 然后切换 context 到当前栈顶的协程（原来被挂起的调用者）恢复执
 */ 
// stCoRoutineEnv_t结构一个线程只有一个
struct stCoRoutineEnv_t 
{
	// 如果将协程看成一种特殊的函数，那么这个 pCallStack 就时保存这些函数的调用链的栈。
	// 非对称协程最大特点就是协程间存在明确的调用关系；甚至在有些文献中，启动协程被称作 call，
	// 挂起协程叫 return。非对称协程机制下的被调协程只能返回到调用者协程，这种调用关系不能乱，
	// 因此必须将调用链保存下来
	stCoRoutine_t *pCallStack[ 128 ];
	int iCallStackSize; // 上面那个调用栈的栈顶指针 
	// epoll的一个封装结构
	stCoEpoll_t *pEpoll;

	// for copy stack log lastco and nextco
	// 对上次切换挂起的协程和嵌套调用的协程栈的拷贝,为了减少共享栈上数据的拷贝
	// 在不使用共享栈模式时 pending_co 和 ocupy_co 都是空指针
	// pengding是目前占用共享栈的协程
	// 想想看,如果不加的话,我们需要O(N)的时间复杂度分清楚Callback中current上一个共享栈的协程实体(可能共享栈与默认模式混合)
	stCoRoutine_t* pending_co;
	// 与pending在同一个共享栈上的上一个协程
	stCoRoutine_t* occupy_co;
};
//int socket(int domain, int type, int protocol);
void co_log_err( const char *fmt,... )
{
}


#if defined( __LIBCO_RDTSCP__) 
static unsigned long long counter(void)
{
	register uint32_t lo, hi;
	register unsigned long long o;
	__asm__ __volatile__ (
			"rdtscp" : "=a"(lo), "=d"(hi)::"%rcx"
			);
	o = hi;
	o <<= 32;
	return (o | lo);

}
static unsigned long long getCpuKhz()
{
	FILE *fp = fopen("/proc/cpuinfo","r");
	if(!fp) return 1;
	char buf[4096] = {0};
	fread(buf,1,sizeof(buf),fp);
	fclose(fp);

	char *lp = strstr(buf,"cpu MHz");
	if(!lp) return 1;
	lp += strlen("cpu MHz");
	while(*lp == ' ' || *lp == '\t' || *lp == ':')
	{
		++lp;
	}

	double mhz = atof(lp);
	unsigned long long u = (unsigned long long)(mhz * 1000);
	return u;
}
#endif

// 获取1970.1.1到现在的毫秒数
// https://stackoverflow.com/questions/28550750/better-way-to-get-absolute-time
static unsigned long long GetTickMS()
{
#if defined( __LIBCO_RDTSCP__) 
	static uint32_t khz = getCpuKhz();
	return counter() / khz;
#else
	struct timeval now = { 0 };
	gettimeofday( &now,NULL );
	unsigned long long u = now.tv_sec;
	u *= 1000;
	u += now.tv_usec / 1000; 
	return u;
#endif
}

/* no longer use
static pid_t GetPid()
{
    static __thread pid_t pid = 0;
    static __thread pid_t tid = 0;
    if( !pid || !tid || pid != getpid() )
    {
        pid = getpid();
#if defined( __APPLE__ )
		tid = syscall( SYS_gettid );
		if( -1 == (long)tid )
		{
			tid = pid;
		}
#elif defined( __FreeBSD__ )
		syscall(SYS_thr_self, &tid);
		if( tid < 0 )
		{
			tid = pid;
		}
#else 
        tid = syscall( __NR_gettid );
#endif

    }
    return tid;

}
static pid_t GetPid()
{
	char **p = (char**)pthread_self();
	return p ? *(pid_t*)(p + 18) : getpid();
}
*/
template <class T,class TLink>
void RemoveFromLink(T *ap)
{
	TLink *lst = ap->pLink;
	if(!lst) return ;
	assert( lst->head && lst->tail );

	if( ap == lst->head )
	{
		lst->head = ap->pNext;
		if(lst->head)
		{
			lst->head->pPrev = NULL;
		}
	}
	else
	{
		if(ap->pPrev)
		{
			ap->pPrev->pNext = ap->pNext;
		}
	}

	if( ap == lst->tail )
	{
		lst->tail = ap->pPrev;
		if(lst->tail)
		{
			lst->tail->pNext = NULL;
		}
	}
	else
	{
		ap->pNext->pPrev = ap->pPrev;
	}

	ap->pPrev = ap->pNext = NULL;
	ap->pLink = NULL;
}

template <class TNode,class TLink>
void inline AddTail(TLink*apLink,TNode *ap)
{
	if( ap->pLink )
	{
		return ;
	}
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)ap;
		ap->pNext = NULL;
		ap->pPrev = apLink->tail;
		apLink->tail = ap;
	}
	else
	{
		apLink->head = apLink->tail = ap;
		ap->pNext = ap->pPrev = NULL;
	}
	ap->pLink = apLink;
}
template <class TNode,class TLink>
void inline PopHead( TLink*apLink )
{
	if( !apLink->head ) 
	{
		return ;
	}
	TNode *lp = apLink->head;
	if( apLink->head == apLink->tail )
	{
		apLink->head = apLink->tail = NULL;
	}
	else
	{
		apLink->head = apLink->head->pNext;
	}

	lp->pPrev = lp->pNext = NULL;
	lp->pLink = NULL;

	if( apLink->head )
	{
		apLink->head->pPrev = NULL;
	}
}

template <class TNode,class TLink>
void inline Join( TLink*apLink,TLink *apOther )
{
	//printf("apOther %p\n",apOther);
	if( !apOther->head )
	{
		return ;
	}
	TNode *lp = apOther->head;
	while( lp )
	{
		lp->pLink = apLink;
		lp = lp->pNext;
	}
	lp = apOther->head;
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)lp;
		lp->pPrev = apLink->tail;
		apLink->tail = apOther->tail;
	}
	else
	{
		apLink->head = apOther->head;
		apLink->tail = apOther->tail;
	}

	apOther->head = apOther->tail = NULL;
}

/////////////////for copy stack //////////////////////////
stStackMem_t* co_alloc_stackmem(unsigned int stack_size)
{
	stStackMem_t* stack_mem = (stStackMem_t*)malloc(sizeof(stStackMem_t));
	stack_mem->occupy_co= NULL;
	stack_mem->stack_size = stack_size;
	stack_mem->stack_buffer = (char*)malloc(stack_size);
	// Temp->stack_buffer实际上是堆,地址从小到大,而程序运行过程中ebp指针由大向小移动
	stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;
	return stack_mem;
}

stShareStack_t* co_alloc_sharestack(int count, int stack_size)
{
	stShareStack_t* share_stack = (stShareStack_t*)malloc(sizeof(stShareStack_t));
	share_stack->alloc_idx = 0;
	share_stack->stack_size = stack_size;

	//alloc stack array
	share_stack->count = count;
	stStackMem_t** stack_array = (stStackMem_t**)calloc(count, sizeof(stStackMem_t*));
	for (int i = 0; i < count; i++)
	{
		stack_array[i] = co_alloc_stackmem(stack_size);
	}
	share_stack->stack_array = stack_array;
	return share_stack;
}

static stStackMem_t* co_get_stackmem(stShareStack_t* share_stack)
{
	if (!share_stack)
	{
		return NULL;
	}
	// 共享栈相关，共享栈中设置多个栈是为了减少拷贝
	int idx = share_stack->alloc_idx % share_stack->count;
	share_stack->alloc_idx++;

	return share_stack->stack_array[idx];
}


// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t;
struct stTimeoutItem_t;
struct stCoEpoll_t
{
	int iEpollFd; 								// epollfd
	static const int _EPOLL_SIZE = 1024 * 10;   // 一次 epoll_wait 最多返回的就绪事件个数

	struct stTimeout_t *pTimeout; 				// 时间轮

	struct stTimeoutItemLink_t *pstTimeoutList;	// 链表用于临时存放超时事件的item

	struct stTimeoutItemLink_t *pstActiveList;	// 该链表用于存放epoll_wait得到的就绪事件和定时器超时事件

	// 对 epoll_wait() 第二个参数的封装，即一次 epoll_wait 得到的结果集
	co_epoll_res *result;

};
typedef void (*OnPreparePfn_t)( stTimeoutItem_t *,struct epoll_event &ev, stTimeoutItemLink_t *active );
typedef void (*OnProcessPfn_t)( stTimeoutItem_t *);

// 超时链表中的一项
struct stTimeoutItem_t
{

	enum
	{
		eMaxTimeout = 40 * 1000 //40s
	};
	stTimeoutItem_t *pPrev; // 前一个元素
	stTimeoutItem_t *pNext; // 后一个元素
	stTimeoutItemLink_t *pLink; // 该链表项所属的链表

	unsigned long long ullExpireTime; // 在这个时间点会超时,在co_poll_inner中设定

	OnPreparePfn_t pfnPrepare; // 预处理函数，在eventloop中会被调用
	OnProcessPfn_t pfnProcess; // 处理函数 在eventloop中会被调用

	void *pArg; // routine的实体 
	bool bTimeout; // 是否已经超时bo
};
struct stTimeoutItemLink_t
{
	stTimeoutItem_t *head;
	stTimeoutItem_t *tail;

};
/*
* 毫秒级的超时管理器
* 使用时间轮实现
* 但是是有限制的，最长超时时间不可以超过iItemSize毫秒
*/
struct stTimeout_t
{
	/*
	   时间轮
	   超时事件数组，总长度为iItemSize,每一项代表1毫秒，为一个链表，代表这个时间所超时的事件。
	   这个数组在使用的过程中，会使用取模的方式，把它当做一个循环数组来使用，虽然并不是用循环链表来实现的
	*/
	stTimeoutItemLink_t *pItems;
	int iItemSize;		// 数组长度

	unsigned long long ullStart; // 时间轮第一次使用的时间
	long long llStartIdx;	// 目前正在使用的下标
};
stTimeout_t *AllocTimeout( int iSize )
{
	stTimeout_t *lp = (stTimeout_t*)calloc( 1,sizeof(stTimeout_t) );	

	lp->iItemSize = iSize;
	lp->pItems = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) * lp->iItemSize );

	lp->ullStart = GetTickMS();
	lp->llStartIdx = 0;

	return lp;
}
void FreeTimeout( stTimeout_t *apTimeout )
{
	free( apTimeout->pItems );
	free ( apTimeout );
}

/*
* 将事件添加到定时器中
* @param apTimeout - (ref) 超时管理器
* @param apItem    - (in) 即将插入的超时事件
* @param allNow    - (in) 当前时间
*/
int AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,unsigned long long allNow )
{
	// 当前时间管理器的最早超时时间
	if( apTimeout->ullStart == 0 )
	{
		// 设置时间轮的最早时间是当前时间
		apTimeout->ullStart = allNow;
		// 设置最早时间对应的index 为 0
		apTimeout->llStartIdx = 0;
	}
	// 插入时间小于初始时间肯定是错的
	if( allNow < apTimeout->ullStart )
	{
		co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
					__LINE__,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	// 预期时间小于插入时间也是有问题的
	if( apItem->ullExpireTime < allNow )
	{
		co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu",
					__LINE__,apItem->ullExpireTime,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	// 计算事件还有多长时间会超时
	unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart; 

	// 预期时间到现在不能超过时间轮的大小
	// 其实是可以的，只需要取余放进去并加上一个圈数的成员就可以了
	// 遍历时圈数不为零就说明实际超时时间还有一个时间轮的长度，
	// 遍历完一项以后圈数不为零就减1即可
	if( diff >= (unsigned long long)apTimeout->iItemSize )
	{
		diff = apTimeout->iItemSize - 1;
		co_log_err("CO_ERR: AddTimeout line %d diff %d",
					__LINE__,diff);

		//return __LINE__;
	}
	AddTail( apTimeout->pItems + ( apTimeout->llStartIdx + diff ) % apTimeout->iItemSize , apItem );

	return 0;
}
inline void TakeAllTimeout( stTimeout_t *apTimeout,unsigned long long allNow,stTimeoutItemLink_t *apResult )
{
	// 第一次调用是设置初始时间
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}

	// 当前时间小于初始时间显然是有问题的
	if( allNow < apTimeout->ullStart )
	{
		return ;
	}
	// 求一个取出事件的有效区间
	int cnt = allNow - apTimeout->ullStart + 1;
	if( cnt > apTimeout->iItemSize )
	{
		cnt = apTimeout->iItemSize;
	}
	if( cnt < 0 )
	{
		return;
	}
	for( int i = 0;i<cnt;i++)
	{	// 把上面求的有效区间过一遍，某一项存在数据的话插入到超时链表中
		int idx = ( apTimeout->llStartIdx + i) % apTimeout->iItemSize;
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( apResult,apTimeout->pItems + idx  );
	}
	// 更新时间轮属性
	apTimeout->ullStart = allNow;
	apTimeout->llStartIdx += cnt - 1;

}
static int CoRoutineFunc( stCoRoutine_t *co,void * )
{
	if( co->pfn )
	{
		co->pfn( co->arg );
	}
	co->cEnd = 1;

	stCoRoutineEnv_t *env = co->env;

	co_yield_env( env );

	return 0;
}


/**
 * @env  环境变量
 * @attr 协程信息
 * @pfn  函数指针
 * @arg  函数参数
*/
struct stCoRoutine_t *co_create_env( stCoRoutineEnv_t * env, const stCoRoutineAttr_t* attr,
		pfn_co_routine_t pfn,void *arg )
{

	stCoRoutineAttr_t at;
	if( attr ) // 如果指定了attr的话就执行拷贝
	{
		memcpy( &at,attr,sizeof(at) );
	}
	// stack_size 有效区间为[0, 1024 * 1024 * 8]
	if( at.stack_size <= 0 )
	{
		at.stack_size = 128 * 1024;
	}
	else if( at.stack_size > 1024 * 1024 * 8 )
	{
		at.stack_size = 1024 * 1024 * 8;
	}

	// 4KB对齐,也就是说如果对stacksize取余不为零的时候对齐为4KB
	// 例如本来5KB,经过了这里就变为8KB了
	if( at.stack_size & 0xFFF ) 
	{	
		at.stack_size &= ~0xFFF;
		at.stack_size += 0x1000;
	}

	stCoRoutine_t *lp = (stCoRoutine_t*)malloc( sizeof(stCoRoutine_t) );
	
	memset( lp,0,(long)(sizeof(stCoRoutine_t))); 


	lp->env = env;
	lp->pfn = pfn;
	lp->arg = arg;

	stStackMem_t* stack_mem = NULL;
	if( at.share_stack ) // 共享栈模式 栈需要自己指定
	{
		stack_mem = co_get_stackmem( at.share_stack);
		at.stack_size = at.share_stack->stack_size;
	}
	else // 每个协程有一个私有的栈
	{
		stack_mem = co_alloc_stackmem(at.stack_size);
	}
	lp->stack_mem = stack_mem;

	lp->ctx.ss_sp = stack_mem->stack_buffer; // 这个协程栈的基址
	lp->ctx.ss_size = at.stack_size;// 未使用大小,与前者相加为esp指针,见coctx_make解释

	lp->cStart = 0;
	lp->cEnd = 0;
	lp->cIsMain = 0;
	lp->cEnableSysHook = 0;
	lp->cIsShareStack = at.share_stack != NULL;

	lp->save_size = 0;
	lp->save_buffer = NULL; 

	return lp;
}

//
int co_create( stCoRoutine_t **ppco,const stCoRoutineAttr_t *attr,pfn_co_routine_t pfn,void *arg )
{
	if( !co_get_curr_thread_env() ) // 是一个线程私有的变量 
	{
		co_init_curr_thread_env();
	}
	stCoRoutine_t *co = co_create_env( co_get_curr_thread_env(), attr, pfn,arg );
	*ppco = co;
	return 0;
}
void co_free( stCoRoutine_t *co )
{
    if (!co->cIsShareStack) 
    {    
        free(co->stack_mem->stack_buffer);
        free(co->stack_mem);
    }   
    //walkerdu fix at 2018-01-20
    //存在内存泄漏
    else 
    {
        if(co->save_buffer)
            free(co->save_buffer);

        if(co->stack_mem->occupy_co == co)
            co->stack_mem->occupy_co = NULL;
    }

    free( co );
}
void co_release( stCoRoutine_t *co )
{
    co_free( co );
}

void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co);

void co_resume( stCoRoutine_t *co )
{
	// stCoRoutine_t结构需要我们在我们的代码中自行调用co_release或者co_free
	stCoRoutineEnv_t *env = co->env;

	// 获取当前正在进行的协程主体 
	stCoRoutine_t *lpCurrRoutine = env->pCallStack[ env->iCallStackSize - 1 ];
	if( !co->cStart ) // 
	{
		coctx_make( &co->ctx,(coctx_pfn_t)CoRoutineFunc,co,0 ); //保存上一个协程的上下文
		co->cStart = 1;
	}
	// 把此次执行的协程控制块放入调用栈中
	env->pCallStack[ env->iCallStackSize++ ] = co;
	// co_swap() 内部已经切换了 CPU 执行上下文
	co_swap( lpCurrRoutine, co );
	
}


// walkerdu 2018-01-14                                                                              
// 用于reset超时无法重复使用的协程                                                                  
void co_reset(stCoRoutine_t * co)
{
    if(!co->cStart || co->cIsMain)
        return;

    co->cStart = 0;
    co->cEnd = 0;

    // 如果当前协程有共享栈被切出的buff，要进行释放
    if(co->save_buffer)
    {
        free(co->save_buffer);
        co->save_buffer = NULL;
        co->save_size = 0;
    }

    // 如果共享栈被当前协程占用，要释放占用标志，否则被切换，会执行save_stack_buffer()
    if(co->stack_mem->occupy_co == co)
        co->stack_mem->occupy_co = NULL;
}

// 当前协程让出CPU
void co_yield_env( stCoRoutineEnv_t *env )
{
	
	stCoRoutine_t *last = env->pCallStack[ env->iCallStackSize - 2 ]; // 要切换的协程
	stCoRoutine_t *curr = env->pCallStack[ env->iCallStackSize - 1 ]; // 即当前正在执行的协程

	env->iCallStackSize--;

	co_swap( curr, last);
}

void co_yield_ct()
{

	co_yield_env( co_get_curr_thread_env() );
}
void co_yield( stCoRoutine_t *co )
{
	co_yield_env( co->env );
}

/**
* 将原本占用共享栈的协程的内存保存起来。
* @param occupy_co 原本占用共享栈的协程
*/
void save_stack_buffer(stCoRoutine_t* occupy_co)
{
	///copy out
	stStackMem_t* stack_mem = occupy_co->stack_mem;
	// 目前所使用的栈帧大小
	// bp是在创建的时候决定的,也就是初始分配的那块连续空间的最高地址
	// sp就比较厉害了,看看co_swap中如何取stack_sp
	int len = stack_mem->stack_bp - occupy_co->stack_sp;

	if (occupy_co->save_buffer)
	{
		free(occupy_co->save_buffer), occupy_co->save_buffer = NULL;
	}

	occupy_co->save_buffer = (char*)malloc(len); //malloc buf;
	occupy_co->save_size = len;

	memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}

#include<iostream>

// 当前准备让出 CPU 的协程叫做 current 协程，把即将调入执行的叫做 pending 协程
void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co)
{
 	stCoRoutineEnv_t* env = co_get_curr_thread_env();

	// get curr stack sp
	// 这个获取esp的方法也太溜了吧, 
	char c;
	curr->stack_sp= &c; 

	if (!pending_co->cIsShareStack)
	{ 
		env->pending_co = NULL;
		env->occupy_co = NULL;
	}
	else // 如果采用了共享栈
	{
		env->pending_co = pending_co;
		// get last occupy co on the same stack mem
		// 获取pending使用的栈空间的执行协程
		stCoRoutine_t* occupy_co = pending_co->stack_mem->occupy_co;
		// 也就是当前正在执行的进程
		// set pending co to occupy thest stack mem
		// 将该共享栈的占用者改为pending_co
		pending_co->stack_mem->occupy_co = pending_co;

		env->occupy_co = occupy_co;
		if (occupy_co && occupy_co != pending_co)
		{
			// 如果上一个使用协程不为空,则需要把它的栈内容保存起来
			save_stack_buffer(occupy_co); 
		}
	}

	// -----------------------------------

/*         coctx_t p1 = curr->ctx;
        {
            printf("%d %p\n",p1.ss_size, p1.ss_sp);
            for (int i = 0; i < 14; ++i) {
                std::cout << p1.regs[i] << " ";;
            }
            putchar('\n');
        }
        coctx_t p2 = pending_co->ctx;
        {
            printf("%d %p\n",p2.ss_size, p2.ss_sp);
            for (int i = 0; i < 14; ++i) {
                std::cout << p2.regs[i] << " ";;
            }
            putchar('\n');
        } */
	// -----------------------------------

	//swap context 这个函数执行完, 就切入下一个协程了
	coctx_swap(&(curr->ctx),&(pending_co->ctx) );
	
	//cout << "down------------\n";

	//stack buffer may be overwrite, so get again;
	stCoRoutineEnv_t* curr_env = co_get_curr_thread_env();
	stCoRoutine_t* update_occupy_co =  curr_env->occupy_co; 
	stCoRoutine_t* update_pending_co = curr_env->pending_co;
	
	if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co)
	{
		//resume stack buffer
		if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
		{
			// 如果是一个协程执行到一半然后被切换出去然后又切换回来,这个时候需要恢复栈空间
			memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer, update_pending_co->save_size);
		}
	}
}



//int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
struct stPollItem_t ;
struct stPoll_t : public stTimeoutItem_t 
{
	struct pollfd *fds; // 描述poll中的事件
	nfds_t nfds; // typedef unsigned long int nfds_t;

	stPollItem_t *pPollItems; // 要加入epoll的事件 长度为nfds

	int iAllEventDetach; // 标识是否已经处理过了这个对象了

	int iEpollFd; // epoll fd

	int iRaiseCnt; // 此次触发的事件数


};
struct stPollItem_t : public stTimeoutItem_t
{
	struct pollfd *pSelf;// 对应的poll结构
	stPoll_t *pPoll;	// 所属的stPoll_t

	struct epoll_event stEvent;	// poll结构所转换的epoll结构
};
/*
 *   EPOLLPRI 		POLLPRI    // There is urgent data to read.  
 *   EPOLLMSG 		POLLMSG
 *
 *   				POLLREMOVE
 *   				POLLRDHUP
 *   				POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll( short events )
{
	uint32_t e = 0;	
	if( events & POLLIN ) 	e |= EPOLLIN;
	if( events & POLLOUT )  e |= EPOLLOUT;
	if( events & POLLHUP ) 	e |= EPOLLHUP;
	if( events & POLLERR )	e |= EPOLLERR;
	if( events & POLLRDNORM ) e |= EPOLLRDNORM;
	if( events & POLLWRNORM ) e |= EPOLLWRNORM;
	return e;
}
static short EpollEvent2Poll( uint32_t events )
{
	short e = 0;	
	if( events & EPOLLIN ) 	e |= POLLIN;
	if( events & EPOLLOUT ) e |= POLLOUT;
	if( events & EPOLLHUP ) e |= POLLHUP;
	if( events & EPOLLERR ) e |= POLLERR;
	if( events & EPOLLRDNORM ) e |= POLLRDNORM;
	if( events & EPOLLWRNORM ) e |= POLLWRNORM;
	return e;
}

static __thread stCoRoutineEnv_t* gCoEnvPerThread = NULL;

void co_init_curr_thread_env()
{
	gCoEnvPerThread = (stCoRoutineEnv_t*)calloc( 1, sizeof(stCoRoutineEnv_t) );
	stCoRoutineEnv_t *env = gCoEnvPerThread;

	env->iCallStackSize = 0;	// 修改"调用栈"顶指针
	struct stCoRoutine_t *self = co_create_env( env, NULL, NULL,NULL );
	self->cIsMain = 1;			// 一个线程调用这个函数的肯定是主协程喽

	env->pending_co = NULL;
	env->occupy_co = NULL;

	coctx_init( &self->ctx ); // 能跑到这里一定是main,所以清空上下文

	env->pCallStack[ env->iCallStackSize++ ] = self; // 放入线程独有环境中

	stCoEpoll_t *ev = AllocEpoll();
	SetEpoll( env,ev );
}
stCoRoutineEnv_t *co_get_curr_thread_env()
{
	return gCoEnvPerThread;
}

void OnPollProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

void OnPollPreparePfn( stTimeoutItem_t * ap,struct epoll_event &e,stTimeoutItemLink_t *active )
{
	stPollItem_t *lp = (stPollItem_t *)ap;
	// 把epoll此次触发的事件转换成poll中的事件
	lp->pSelf->revents = EpollEvent2Poll( e.events );


	stPoll_t *pPoll = lp->pPoll;
	// 已经触发的事件数加一
	pPoll->iRaiseCnt++;

	// 若此事件还未被触发过
	if( !pPoll->iAllEventDetach )
	{
		// 设置已经被触发的标志
		pPoll->iAllEventDetach = 1;

		// 将该事件从时间轮中移除
		// 因为事件已经触发了，肯定不能再超时了
		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( pPoll );

		// 将该事件添加到active列表中
		AddTail( active,pPoll );

	}
}


/*
* libco的核心调度
* 在此处调度三种事件：
* 1. 被hook的io事件，该io事件是通过co_poll_inner注册进来的
* 2. 超时事件
* 3. 用户主动使用poll的事件
* 所以，如果用户用到了三种事件，必须得配合使用co_eventloop
*
* @param ctx epoll管理器
* @param pfn 每轮事件循环的最后会调用该函数
* @param arg pfn的参数
*/

void co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg )
{
	if( !ctx->result )	// 给结果集分配空间
	{													// epoll结果集大小
		ctx->result =  co_epoll_res_alloc( stCoEpoll_t::_EPOLL_SIZE );
	}
	co_epoll_res *result = ctx->result;


	for(;;)
	{
		// 最大超时时间设置为 1 ms
		// 所以最长1ms，epoll_wait就会被唤醒
		int ret = co_epoll_wait( ctx->iEpollFd,result,stCoEpoll_t::_EPOLL_SIZE, 1 );

		// 不使用局部变量的原因是epoll循环并不是元素的唯一来源.例如条件变量相关(co_routine.cpp stCoCondItem_t)
		stTimeoutItemLink_t *active = (ctx->pstActiveList);
		stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList);

		memset( timeout,0,sizeof(stTimeoutItemLink_t) );

		// 获取在co_poll_inner放入epoll_event中的stTimeoutItem_t结构体
		for(int i=0;i<ret;i++)
		{
			stTimeoutItem_t *item = (stTimeoutItem_t*)result->events[i].data.ptr;

			if( item->pfnPrepare ) // 如果用户设置预处理回调的话就执行
			{
				// 若是hook后的poll的话,会把此事件加入到active队列中,并更新一些状态
				item->pfnPrepare( item,result->events[i],active );
			}
			else
			{
				AddTail( active,item );
			}
		}


		// 从时间轮上取出超时事件
		unsigned long long now = GetTickMS();

		// 以当前时间为超时截止点
		// 从时间轮中取出超时的时间放入到timeout中
		TakeAllTimeout( ctx->pTimeout,now,timeout );

		stTimeoutItem_t *lp = timeout->head;
		while( lp ) // 遍历超时链表,设置超时标志,并加入active链表
		{
			//printf("raise timeout %p\n",lp);
			lp->bTimeout = true;
			lp = lp->pNext;
		}

		// 把timeout合并到active中
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( active,timeout );

		lp = active->head;
		// 开始遍历active链表
		//int IIIIII = 0;
		while( lp )
		{
			// 在链表不为空的时候删除active的第一个元素 如果删除成功,那个元素就是lp
			//std::cout << IIIIII++ << std::endl;
			PopHead<stTimeoutItem_t,stTimeoutItemLink_t>( active );
            if (lp->bTimeout && now < lp->ullExpireTime) 
			{ // 一种排错机制,在超时和所等待的时间内已经完成只有一个条件满足才是正确的
				int ret = AddTimeout(ctx->pTimeout, lp, now);
				if (!ret) //插入成功
				{
					lp->bTimeout = false;
					lp = active->head;
					continue;
				}
			}
			// TODO 有问题,如果同一个协程有两个事件在一次epoll循环中触发,
			// 那么第一个事件切回去执行协程,第二个呢,已提交issue
			if( lp->pfnProcess )
			{	// 默认为OnPollProcessEvent 会切换协程
				lp->pfnProcess( lp );
			}

			lp = active->head;
		}
		// 每次事件循环结束以后执行该函数, 用于终止协程
		if( pfn )
		{
			if( -1 == pfn( arg ) )
			{
				break;
			}
		}

	}
}
void OnCoroutineEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}


stCoEpoll_t *AllocEpoll()
{
	stCoEpoll_t *ctx = (stCoEpoll_t*)calloc( 1,sizeof(stCoEpoll_t) );

	ctx->iEpollFd = co_epoll_create( stCoEpoll_t::_EPOLL_SIZE );	// 就是epoll_create
	ctx->pTimeout = AllocTimeout( 60 * 1000 );
	
	ctx->pstActiveList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );
	ctx->pstTimeoutList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );


	return ctx;
}

void FreeEpoll( stCoEpoll_t *ctx )
{
	if( ctx )
	{
		free( ctx->pstActiveList );
		free( ctx->pstTimeoutList );
		FreeTimeout( ctx->pTimeout );
		co_epoll_res_free( ctx->result );
	}
	free( ctx );
}

stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env )
{
	return env->pCallStack[ env->iCallStackSize - 1 ];
}
// 获取当前线程正在运行的协程
stCoRoutine_t *GetCurrThreadCo( ) 
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();
	// 为什么返回0而不就地初始化呢?看似简单的一笔,与example_poll相关
	if( !env ) return 0;	
	return GetCurrCo(env);
}


/*
 * 1.这个函数非常重要, 大部分hook后的函数会使用这个函数把事件注册到epoll
 * 2.把poll事件转换成epoll事件
 * 
* @param ctx epoll上下文
* @param fds[] fds 要监听的文件描述符 原始poll函数的参数，
* @param nfds  nfds fds的数组长度 原始poll函数的参数
* @param timeout timeout 等待的毫秒数 原始poll函数的参数
* @param pollfunc 原始的poll函数, g_sys_poll_func
 * */
typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
int co_poll_inner( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout, poll_pfn_t pollfunc)
{
	// 超时时间为零 直接执行系统调用 感觉这直接在hook的poll中判断就好了
    if (timeout == 0) 
	{
		return pollfunc(fds, nfds, timeout);
	}
	if (timeout < 0) // 搞不懂这是什么意思,小于零就看做无限阻塞?
	{
		timeout = INT_MAX;
	}
	// epoll fd
	int epfd = ctx->iEpollFd;
	// 获取当前线程正在运行的协程
	stCoRoutine_t* self = co_self();

	//1.struct change
	// 一定要把这stPoll_t, stPollItem_t之间的关系看清楚
	stPoll_t& arg = *((stPoll_t*)malloc(sizeof(stPoll_t)));
	// 指针的初始化,非常关键,不加的话在addtail的条件判断中会出现问题
	memset( &arg,0,sizeof(arg) );

	arg.iEpollFd = epfd;
	arg.fds = (pollfd*)calloc(nfds, sizeof(pollfd));
	arg.nfds = nfds;

	// 一个小优化 数据量少的时候少一次系统调用
	stPollItem_t arr[2];
	if( nfds < sizeof(arr) / sizeof(arr[0]) && !self->cIsShareStack)
	{
		// 如果poll中监听的描述符只有1个或者0个， 并且目前的不是共享栈模型
		arg.pPollItems = arr;
	}	
	else
	{
		arg.pPollItems = (stPollItem_t*)malloc( nfds * sizeof( stPollItem_t ) );
	}
	memset( arg.pPollItems,0,nfds * sizeof(stPollItem_t) );

	// 在eventloop中调用的处理函数,功能是唤醒pArg中的协程,也就是这个调用poll的协程
	arg.pfnProcess = OnPollProcessEvent;  
	arg.pArg = GetCurrCo( co_get_curr_thread_env());
	
	
	//2. add epoll
	for(nfds_t i=0;i<nfds;i++)
	{
		arg.pPollItems[i].pSelf = arg.fds + i; // 第i个poll事件
		arg.pPollItems[i].pPoll = &arg;

		// 设置一个预处理回调 这个回调做的事情是把此事件从超时队列转到就绪队列
		arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;
		// ev是arg.pPollItems[i].stEvent的一个引用,这里就相当于是缩写了

		// epoll_event 就是epoll需要的事件类型
		// 这个结构直接插在红黑树中,时间到来或超时我们可以拿到其中的data
		// 一般我用的时候枚举中只使用fd,这里使用了一个指针
		struct epoll_event &ev = arg.pPollItems[i].stEvent;

		if( fds[i].fd > -1 ) // fd有效
		{
			ev.data.ptr = arg.pPollItems + i;
			ev.events = PollEvent2Epoll( fds[i].events );

			// 把事件加入poll中的事件进行封装以后加入epoll
			int ret = co_epoll_ctl( epfd,EPOLL_CTL_ADD, fds[i].fd, &ev );
			if (ret < 0 && errno == EPERM && nfds == 1 && pollfunc != NULL)
			{ //加入epoll失败 且nfds只有一个
				if( arg.pPollItems != arr )
				{
					free( arg.pPollItems );
					arg.pPollItems = NULL;
				}
				free(arg.fds);
				free(&arg);
				return pollfunc(fds, nfds, timeout);
			}
		}
		//if fail,the timeout would work
	}

	//3.add timeout

	// 获取当前时间
	unsigned long long now = GetTickMS();

	// 超时时间
	arg.ullExpireTime = now + timeout;
	
	// 添加到超时链表中 
	int ret = AddTimeout( ctx->pTimeout,&arg,now );
	int iRaiseCnt = 0;

	// 正常返回return 0
	if( ret != 0 )
	{
		co_log_err("CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
				ret,now,timeout,arg.ullExpireTime);
		errno = EINVAL;
		iRaiseCnt = -1;

	}
    else
	{
		// 让出CPU, 切换到其他协程, 当事件到来的时候就会调用callback,那里会唤醒此协程
		co_yield_env( co_get_curr_thread_env() );

		// --------------我是分割线---------------
		// 在预处理中执行+1, 也就是此次阻塞等待的事件中有几个是实际发生了
		iRaiseCnt = arg.iRaiseCnt;
	}

    {
		// clear epoll status and memory
		// 将该项从时间轮中删除
		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &arg );
		// 将此次poll中涉及到的时间全部从epoll中删除 
		// 这意味着有一些事件没有发生就被终止了 
		// 比如poll中3个事件,实际触发了两个,最后一个在这里就被移出epoll了
		for(nfds_t i = 0;i < nfds;i++)
		{
			int fd = fds[i].fd;
			if( fd > -1 )
			{
				co_epoll_ctl( epfd,EPOLL_CTL_DEL,fd,&arg.pPollItems[i].stEvent );
			}
			fds[i].revents = arg.fds[i].revents;
		}


		// 释放内存 当然使用智能指针就没这事了
		if( arg.pPollItems != arr )
		{
			free( arg.pPollItems );
			arg.pPollItems = NULL;
		}

		free(arg.fds);
		free(&arg);
	}
	// 返回此次就绪或者超时的事件
	return iRaiseCnt;
}

int	co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout_ms )
{
	return co_poll_inner(ctx, fds, nfds, timeout_ms, NULL);
}

void SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev )
{
	env->pEpoll = ev;
}

// 得到当前线程中的epoll结构体stCoRoutineEnv_t
stCoEpoll_t *co_get_epoll_ct()
{
	if( !co_get_curr_thread_env() )
	{
		co_init_curr_thread_env();
	}
	return co_get_curr_thread_env()->pEpoll;
}
struct stHookPThreadSpec_t
{
	stCoRoutine_t *co;
	void *value;

	enum 
	{
		size = 1024
	};
};
void *co_getspecific(pthread_key_t key)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_getspecific( key );
	}
	return co->aSpec[ key ].value;
}
int co_setspecific(pthread_key_t key, const void *value)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_setspecific( key,value );
	}
	co->aSpec[ key ].value = (void*)value;
	return 0;
}



void co_disable_hook_sys()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( co )
	{
		co->cEnableSysHook = 0;
	}
}
bool co_is_enable_sys_hook()
{	
	// co_enable_sys_hook 会把cEnableSysHook设置成1
	stCoRoutine_t *co = GetCurrThreadCo();
	return ( co && co->cEnableSysHook );
}

stCoRoutine_t *co_self()
{
	return GetCurrThreadCo();
}

//co cond
// 条件变量相关实现,没有办法直接hook,因为没办法放到epoll中,所以需要实现一下
struct stCoCond_t;
// 相当于条件变量的变量声明
struct stCoCondItem_t 
{
	stCoCondItem_t *pPrev;
	stCoCondItem_t *pNext;
	stCoCond_t *pLink;

	stTimeoutItem_t timeout;
};
struct stCoCond_t
{
	stCoCondItem_t *head;
	stCoCondItem_t *tail;
};
static void OnSignalProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

stCoCondItem_t *co_cond_pop( stCoCond_t *link );
// 等价于pthread_cond_signal,从等待队列上去第一个值唤醒
int co_cond_signal( stCoCond_t *si )
{
	stCoCondItem_t * sp = co_cond_pop( si );
	if( !sp ) 
	{
		return 0;
	}
	// 从时间轮中移除
	RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );

	// 加到active队列中
	AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );
	// 所以单线程运行生产者消费者我们在signal以后还需要调用阻塞类函数转移CPU控制权,例如poll
	return 0;
}
// 和pthread_cond_broadcast相同,与pthread_cond_signal相比多了个循环
int co_cond_broadcast( stCoCond_t *si )
{
	for(;;)
	{
		// 取si中的首元素并从si中删除
		stCoCondItem_t * sp = co_cond_pop( si );
		if( !sp ) return 0;

		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );

		AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );
	}

	return 0;
}

// 和pthread_cond_wait语义相同
// 条件变量的实体;超时时间
int co_cond_timedwait( stCoCond_t *link,int ms )
{
	stCoCondItem_t* psi = (stCoCondItem_t*)calloc(1, sizeof(stCoCondItem_t));
	psi->timeout.pArg = GetCurrThreadCo();
	// 实际还是执行resume,进行协程切换
	psi->timeout.pfnProcess = OnSignalProcessEvent;

	if( ms > 0 )
	{
		unsigned long long now = GetTickMS();
		// 定义超时时间
		psi->timeout.ullExpireTime = now + ms;

		// 加入时间轮
		int ret = AddTimeout( co_get_curr_thread_env()->pEpoll->pTimeout,&psi->timeout,now );
		if( ret != 0 )
		{
			free(psi);
			return ret;
		}
	}
	// 相当于timeout为负的话超时时间无限
	AddTail( link, psi);

	co_yield_ct(); // 切换CPU执行权,切换CPU执行权,在epoll中触发peocess回调以后回到这里

	// 这个条件要么被触发,要么已经超时,从条件变量实体中删除
	RemoveFromLink<stCoCondItem_t,stCoCond_t>( psi );
	free(psi);

	return 0;
}
stCoCond_t *co_cond_alloc()
{
	return (stCoCond_t*)calloc( 1,sizeof(stCoCond_t) );
}
int co_cond_free( stCoCond_t * cc )
{
	free( cc );
	return 0;
}


stCoCondItem_t *co_cond_pop( stCoCond_t *link )
{
	stCoCondItem_t *p = link->head;
	if( p )
	{
		PopHead<stCoCondItem_t,stCoCond_t>( link );
	}
	return p;
}
