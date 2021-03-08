#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/poll.h>
#include <asm/uaccess.h>
#include <linux/wait.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/wait.h>
#include <linux/semaphore.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/device.h>

#define Major 12
#define Minor 0
#define Dev_Count 8
#define Pipe Dev_Count >> 1
#define MaxSize 15

static struct class *myClass;
int major = Major;

struct myfifo
{
	struct cdev cdev;
	char buff[Pipe][MaxSize];		   //缓冲区，最大长度为MaxSize，Maxsize在宏定义中给出
	size_t r_head[Pipe], w_head[Pipe]; //读写指针
	size_t flag[Pipe];
	struct semaphore mutex[Pipe];	//互斥信号量,对buff的读写不能并发执行
									//struct semaphore rwmutex;
	wait_queue_head_t r_wait[Pipe]; //读阻塞队列
	wait_queue_head_t w_wait[Pipe]; //写阻塞
} myFifo;

//总长减去现有数据长度
size_t getLength(int index)
{
	size_t len = 0;
	size_t r = myFifo.r_head[index];
	size_t w = myFifo.w_head[index];
	if (w > r)
		len = w - r;
	else if (w == r && myFifo.flag[index] == 0)
		len = 0;
	else
		len = MaxSize - r + w;
	return MaxSize - len;
}

//https://blog.csdn.net/xiaohendsc/article/details/9306049
//内核中用inode结构表示具体的文件，而用file结构表示打开的文件描述符
//struct *file{loff_t f_ops;偏移量;void *private_data;f_dentry:文件目录项入口.......}
//struct inode{dev_t i_rdev;struct cdev *i_cmuldev}
int myFifo_open(struct inode *inode, struct file *filp);
int myFifo_release(struct inode *inode, struct file *filp);
ssize_t myFifo_read(struct file *filp, char __user *buf, size_t count, loff_t *f_ops);
ssize_t myFifo_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);

static const struct file_operations myFifo_fops = {
	.open = myFifo_open,
	.release = myFifo_release,
	.write = myFifo_write,
	.read = myFifo_read,
};

//初始化设备
int myFifo_init(void)
{
	int success;
	dev_t dev = MKDEV(major, Minor); //通过主设备号和次设备号获得设备号,dev_t是一个无符号型32位整数
	success = register_chrdev_region(dev, Dev_Count, "myFifo");
	if (success < 0)
		return success;
	printk("init myFifo success\n");

	//注册驱动，将操作与dev结构体绑定
	cdev_init(&myFifo.cdev, &myFifo_fops);
	myFifo.cdev.owner = THIS_MODULE;
	cdev_add(&myFifo.cdev, dev, Dev_Count);

	//https://www.cnblogs.com/lifexy/p/7827559.html
	//创建设备节点,首先是注册类到内核，然后在类下注册设备，device_create()第2和4个参数分别代表指向这个新设备的父结构class_device的指针
	//和指向与这个类设备相关联的struct设备的指针，设为null,一个管道对应两个设备
	myClass = class_create(THIS_MODULE, "FifoTest");
	device_create(myClass, NULL, MKDEV(major, 0), NULL, "FifoTest0");
	device_create(myClass, NULL, MKDEV(major, 1), NULL, "FifoTest1");
	device_create(myClass, NULL, MKDEV(major, 2), NULL, "FifoTest2");
	device_create(myClass, NULL, MKDEV(major, 3), NULL, "FifoTest3");
	device_create(myClass, NULL, MKDEV(major, 4), NULL, "FifoTest4");
	device_create(myClass, NULL, MKDEV(major, 5), NULL, "FifoTest5");
	device_create(myClass, NULL, MKDEV(major, 6), NULL, "FifoTest6");
	device_create(myClass, NULL, MKDEV(major, 7), NULL, "FifoTest7");

	//初始化读阻塞队列、写阻塞队列与互斥信号量
	int i;
	for (i = 0; i < (Pipe); i++)
	{
		sema_init(&(myFifo.mutex[i]), 1); //初始化信号量
		init_waitqueue_head(&myFifo.r_wait[i]);
		init_waitqueue_head(&myFifo.w_wait[i]);
		myFifo.r_head[i] = myFifo.w_head[i] = 0;
		myFifo.flag[i] = 0;
	}
	//sema_init(&(myFifo.rwmutex),0);
	printk("Init success\n");
	return 0;
}
void myFifo_exit(void)
{
	device_destroy(myClass, MKDEV(major, 0));
	device_destroy(myClass, MKDEV(major, 1));
	device_destroy(myClass, MKDEV(major, 2));
	device_destroy(myClass, MKDEV(major, 3));
	device_destroy(myClass, MKDEV(major, 4));
	device_destroy(myClass, MKDEV(major, 5));
	device_destroy(myClass, MKDEV(major, 6));
	device_destroy(myClass, MKDEV(major, 7));
	class_destroy(myClass);
	cdev_del(&myFifo.cdev);
	unregister_chrdev_region(MKDEV(major, 0), Dev_Count);
	printk("MyFifo Exit\n");
}

int myFifo_open(struct inode *inode, struct file *filp)
{
	try_module_get(THIS_MODULE);
	printk("open success\n");
	return 0;
}

int myFifo_release(struct inode *inode, struct file *filp)
{
	module_put(THIS_MODULE);
	printk("myFifo closed\n");
	return 0;
}

//偶数读,奇数写

ssize_t myFifo_read(struct file *filp, char __user *buf, size_t count, loff_t *f_ops)
{
	//获取设备文件设备号，判断是否是只读设备,file->f_dentry->d_inode->i_rdev
	int minor = MINOR(file_inode(filp)->i_rdev);
	//判断是第几个管道，例如0,1对应0, 2,3对应1 即 (次设备号/2)
	int index = minor >> 1;
	if (!(minor & 1))
	{
		//https://www.cnblogs.com/reality-soul/articles/4730133.html
		//缓冲区空，不可读，阻塞进程,condition为真返回的是0
		if (wait_event_interruptible(myFifo.r_wait[index], getLength(index) != 0))
		{
			return -1;
			//return -ERESTARTSYS;
			//-EFAULT (无效地址)
			//-EINTR(系统调用被中断)
			//-ERESTARTSYS(操作被信号中断)
		}

		/*
		wait(rwmutex);
			
		*/

		//wait(mutex)
		if (down_interruptible(&(myFifo.mutex[index])))
		{
			return -1;
		}
		//查看读取长度是否超过数据长度
		size_t len = MaxSize - getLength(index);
		printk("now have %dbytes\n", len);
		if (count > len)
			count = len;

		//因为fifo管道实际是在内核中，所以不能和用户空间直接访问,并且需要考虑读到队尾还未结束
		if (myFifo.r_head[index] + count >= MaxSize)
		{
			size_t first = MaxSize - myFifo.r_head[index];
			copy_to_user(buf, myFifo.buff[index] + myFifo.r_head[index], first);
			copy_to_user(buf + first, myFifo.buff[index], count - first);
		}
		else
		{
			copy_to_user(buf, myFifo.buff[index] + myFifo.r_head[index], count);
		}

		//更新读指针
		myFifo.r_head[index] = (myFifo.r_head[index] + count) % MaxSize;
		if (count != 0)
			myFifo.flag[index] = 0;
		//唤醒w_wait
		up(&(myFifo.mutex[index]));
		wake_up_interruptible(&myFifo.w_wait[index]);
		printk("read success\n");
		printk("now r_head is %d,w_head is%d\n\n", myFifo.r_head[index], myFifo.w_head[index]);
		return count;
	}
	else
	{
		printk("cannot read buff\n");
		return -1;
	}
}

ssize_t myFifo_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	int minor = MINOR(file_inode(filp)->i_rdev);
	int index = minor >> 1;
	if ((minor & 1) && (count <= MaxSize))
	{
		//缓冲区剩余区域数小于count，不可写，阻塞进程
		//size_t len=getLength(index);
		if (wait_event_interruptible(myFifo.w_wait[index], count <= getLength(index)))
		{
			return -1;
		}
		//wait(mutex)
		if (down_interruptible(&(myFifo.mutex[index])))
		{
			return -1;
		}

		if (myFifo.w_head[index] + count >= MaxSize)
		{
			size_t first = MaxSize - myFifo.w_head[index];
			copy_from_user(myFifo.buff[index] + myFifo.w_head[index], buf, first);
			copy_from_user(myFifo.buff[index], buf + first, count - first);
		}
		else
		{
			copy_from_user(myFifo.buff[index] + myFifo.w_head[index], buf, count);
		}

		//更新写指针
		myFifo.w_head[index] = (myFifo.w_head[index] + count) % MaxSize;
		myFifo.flag[index] = 1;
		//唤醒r_wait
		up(&(myFifo.mutex[index]));
		wake_up_interruptible(&myFifo.r_wait[index]);
		printk("write success\n");
		printk("now r_head is %d,w_head is%d\n\n", myFifo.r_head[index], myFifo.w_head[index]);
		return count;
	}
	else
	{
		printk("cannot write buff\n");
		return -1;
	}
}

module_init(myFifo_init);
module_exit(myFifo_exit);
MODULE_LICENSE("GPL");
