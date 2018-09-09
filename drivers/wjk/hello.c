#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/delay.h>

static int __init hello_init(void)
{
	int i;
	for(i = 0; i < 1; i++)
	{
		printk("-----------------------------hello wjk !!!---------------------%d\n",i);
//		mdelay(800);
	}
return 0;
}

static void __exit hello_exit(void)
{
	printk("Exit Hello wjk\n");
}

subsys_initcall(hello_init);
module_exit(hello_exit);

MODULE_AUTHOR("mo");
MODULE_DESCRIPTION("hello world wjk");
MODULE_LICENSE("GPL");
