#ifdef __APPLE__
#include_next <sys/errno.h>   /* (macOS' errno.h includes this name: the real one comes first) */
#else
#include <errno.h>
#endif
#include "../fakekernel.h"
