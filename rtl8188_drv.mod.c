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
	{ 0xc6d09aa9, "release_firmware" },
	{ 0x74fb0024, "skb_put" },
	{ 0x52c5c991, "__kmalloc_noprof" },
	{ 0x28efd081, "ieee80211_free_hw" },
	{ 0x916afca2, "request_firmware" },
	{ 0x9aa5d9a0, "usb_register_driver" },
	{ 0x4829a47e, "memcpy" },
	{ 0x37a0cba, "kfree" },
	{ 0xf9e2ef3f, "ieee80211_register_hw" },
	{ 0x81f99cb6, "ieee80211_emulate_change_chanctx" },
	{ 0xc3055d20, "usleep_range_state" },
	{ 0xdbc38e7b, "ieee80211_handle_wake_tx_queue" },
	{ 0xa3f598f8, "ieee80211_tx_status_irqsafe" },
	{ 0xcea2e02d, "usb_put_dev" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x441d0de9, "__kmalloc_large_noprof" },
	{ 0x162e4fde, "usb_get_dev" },
	{ 0x81be4f80, "usb_submit_urb" },
	{ 0xf6fe5b67, "_dev_info" },
	{ 0x870dc3d4, "_dev_err" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0x1f1963ad, "usb_control_msg" },
	{ 0xe5fced77, "ieee80211_free_txskb" },
	{ 0xcefb0c9f, "__mutex_init" },
	{ 0x4c060af4, "ieee80211_alloc_hw_nm" },
	{ 0xc2f70a0a, "usb_deregister" },
	{ 0x75ca79b5, "__fortify_panic" },
	{ 0xdcb764ad, "memset" },
	{ 0xb52deb27, "_dev_warn" },
	{ 0xb8a218ab, "ieee80211_emulate_add_chanctx" },
	{ 0x8fa6400d, "ieee80211_emulate_switch_vif_chanctx" },
	{ 0x9662262f, "__netdev_alloc_skb" },
	{ 0xf690c200, "usb_disable_autosuspend" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0xeae3dfd6, "__const_udelay" },
	{ 0x98cfe1fb, "ieee80211_unregister_hw" },
	{ 0x48a97f91, "__kmalloc_cache_noprof" },
	{ 0xaf48b6b7, "usb_kill_urb" },
	{ 0x41ed3709, "get_random_bytes" },
	{ 0xe324d2f5, "usb_init_urb" },
	{ 0x7b98c545, "ieee80211_emulate_remove_chanctx" },
	{ 0x1c78c90d, "kmalloc_caches" },
	{ 0x48954fd5, "ieee80211_rx_irqsafe" },
	{ 0x5a081165, "module_layout" },
};

MODULE_INFO(depends, "mac80211");

MODULE_ALIAS("usb:v0BDAp0179d*dc*dsc*dp*ic*isc*ip*in*");

MODULE_INFO(srcversion, "54E6AFB5355DCA0F5547197");
MODULE_INFO(rhelversion, "10.3");
