#ifdef __APPLE__
#include_next <sys/ioccom.h>
#else
#include <sys/ioctl.h>
#endif
#include "../fakekernel.h"
