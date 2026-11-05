#include "vn_platform.h"

const VnPlatformOps *vn_ops;

void vn_platform_set(const VnPlatformOps *ops)
{
	vn_ops = ops;
}
