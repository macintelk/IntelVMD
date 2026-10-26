

#include "kern_nblue.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>

static NBlue nb;

static const char *bootargDebug = "-NvmdDebug";


PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	nullptr,
	0,
	&bootargDebug,
	1,
	nullptr,
	0,
	KernelVersion::Ventura,
	KernelVersion::Tahoe,
	[]() { nb.init(); },
};

