#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xa916b694, "strnlen" },
	{ 0x476b165a, "sized_strscpy" },
	{ 0x92997ed8, "_printk" },
	{ 0xbfbbdcea, "dev_remove_pack" },
	{ 0x75ca79b5, "__fortify_panic" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0x593bbc5d, "seq_printf" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0xf9111961, "seq_write" },
	{ 0x656e4a6e, "snprintf" },
	{ 0xa7eedcc4, "call_usermodehelper" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0xe63fcb0, "filp_open" },
	{ 0xa7caabdd, "kernel_read" },
	{ 0x53fa2eef, "filp_close" },
	{ 0xdcb764ad, "memset" },
	{ 0xe91b6b40, "dev_get_stats" },
	{ 0x15ba50a6, "jiffies" },
	{ 0x77ae495d, "usb_speed_string" },
	{ 0xa61bcb6d, "sk_skb_reason_drop" },
	{ 0xa65c6def, "alt_cb_patch_nops" },
	{ 0x34db050b, "_raw_spin_lock_irqsave" },
	{ 0xd35cce70, "_raw_spin_unlock_irqrestore" },
	{ 0xe2ac00a9, "skb_copy_bits" },
	{ 0x4829a47e, "memcpy" },
	{ 0xe2964344, "__wake_up" },
	{ 0x98cf60b3, "strlen" },
	{ 0x1a146ec3, "usb_ep_type_string" },
	{ 0x3c12dfe, "cancel_work_sync" },
	{ 0x8c03d20c, "destroy_workqueue" },
	{ 0x9d0d6206, "unregister_netdevice_notifier" },
	{ 0x811dc334, "usb_unregister_notify" },
	{ 0x8eb47cbf, "remove_proc_entry" },
	{ 0x57d56270, "device_destroy" },
	{ 0xbe2a9902, "class_destroy" },
	{ 0x59634eed, "cdev_del" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0xcea2e02d, "usb_put_dev" },
	{ 0x37a0cba, "kfree" },
	{ 0x162e4fde, "usb_get_dev" },
	{ 0xc7a4fbed, "rtnl_lock" },
	{ 0x146af92f, "init_net" },
	{ 0x6e720ff2, "rtnl_unlock" },
	{ 0x441d0de9, "__kmalloc_large_noprof" },
	{ 0xcefb0c9f, "__mutex_init" },
	{ 0xd9a5ea54, "__init_waitqueue_head" },
	{ 0xf7830f93, "alloc_workqueue_noprof" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x8d2e9386, "cdev_init" },
	{ 0xa4993b40, "cdev_add" },
	{ 0x91202a5e, "class_create" },
	{ 0x221b68db, "device_create" },
	{ 0xa589609b, "proc_mkdir" },
	{ 0x35f77489, "proc_create_single_data" },
	{ 0x89bbafc6, "usb_register_notify" },
	{ 0xd2da1048, "register_netdevice_notifier" },
	{ 0xa605917b, "usb_for_each_dev" },
	{ 0x6ff3a485, "dynamic_might_resched" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0xfe487975, "init_wait_entry" },
	{ 0x8ddd8aad, "schedule_timeout" },
	{ 0x8c26d495, "prepare_to_wait_event" },
	{ 0x92540fbf, "finish_wait" },
	{ 0x6cbbfc54, "__arch_copy_to_user" },
	{ 0x1c78c90d, "kmalloc_caches" },
	{ 0x48a97f91, "__kmalloc_cache_noprof" },
	{ 0x12a4e128, "__arch_copy_from_user" },
	{ 0xc5b6f236, "queue_work_on" },
	{ 0x349cba85, "strchr" },
	{ 0x552a38c7, "dev_add_pack" },
	{ 0x7682ba4e, "__copy_overflow" },
	{ 0x3b6c41ea, "kstrtouint" },
	{ 0x5a081165, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "0084E40EF0081125A67F365");
MODULE_INFO(rhelversion, "10.3");
