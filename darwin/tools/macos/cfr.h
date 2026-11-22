/* macOS has no copy_file_range; ENOSYS sends pacstrip to its own read/write copy. */
#include <errno.h>
#include <sys/types.h>
static inline ssize_t copy_file_range(int a, off_t *b, int c, off_t *d, size_t e, unsigned f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; errno = ENOSYS; return -1; }
