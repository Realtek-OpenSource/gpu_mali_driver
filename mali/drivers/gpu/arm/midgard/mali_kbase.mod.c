#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

__visible struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";

MODULE_ALIAS("of:N*T*Carm,malit6xx");
MODULE_ALIAS("of:N*T*Carm,malit6xxC*");
MODULE_ALIAS("of:N*T*Carm,mali-midgard");
MODULE_ALIAS("of:N*T*Carm,mali-midgardC*");

MODULE_INFO(srcversion, "533BB7E5866E52F63B9ACCB");
