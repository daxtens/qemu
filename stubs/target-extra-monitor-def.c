#include "stddef.h"
#include "qemu/typedefs.h"
#include <stdint.h>

int target_extra_monitor_def(uint64_t *pval, const char *name);

int target_extra_monitor_def(uint64_t *pval, const char *name)
{
    return -1;
}
