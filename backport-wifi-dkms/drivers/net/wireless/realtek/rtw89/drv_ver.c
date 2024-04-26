static const char drv_ver[]="v6.8-backport-5.15-43-g606c895b3";
#include <linux/module.h>
module_param_string(drv_ver, drv_ver, sizeof(drv_ver), 0444);
