#include<stdio.h>
#include<pthread.h>
#include<unistd.h>


/* windows情况下的提前运行 */
#ifdef _WIN32
static __attribute__((constructor)) void before_main()
{
	// 设置为utf8格式 防止乱码
	system("chcp 65001");
	// clear();	// 清屏
}
#endif


volatile int x = 0;
volatile int y = 0;
volatile int a = 0;
volatile int b = 0;

void *thread_01(void *arg){
	// 等下线程2，空占CPU
	for(int i=0;i<=80000;i++);
	a = 1;
//	__sync_synchronize();
	x = b;
}

void *thread_02(void *arg){
	b = 1;
//	__sync_synchronize();
	y = a;
}



int main(){
	// 计算循环多少次
	int i = 0;
	while(1){
		i++;
		x=0;y=0;a=0;b=0;
		// 开启两个线程
		pthread_t t1,t2;
		// 第一个参数 pthread_t 类型的指针变量
		// 第二个参数 限制该线程的一些属性
		// 第三个参数 函数指针
		// 第四个参数 传入函数的参数
		pthread_create(&t1, NULL, thread_01, NULL);
		pthread_create(&t2, NULL, thread_02, NULL);
		
		// 等待两个线程完成操作
		pthread_join(t1,NULL);
		pthread_join(t2,NULL);
		
		if (x==0 && y==0){
			printf("第 %d 次执行 x = %d y = %d\n",i,x,y);
			break;
		}else{
			printf("第 %d 次执行 x = %d y = %d\n",i,x,y);
		}
	}
	
	return 0;
}
