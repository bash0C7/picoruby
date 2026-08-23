/*
 * Darwin HAL for mruby-io.
 *
 * Every Apple platform is a BSD userland, so mruby-io's POSIX HAL is the
 * right implementation. The exception is process creation: the watchOS and
 * tvOS SDKs mark fork(2) / exec*(2) __WATCHOS_PROHIBITED / __TVOS_PROHIBITED
 * and clang rejects any reference to them, so a plain POSIX build of
 * mruby-io cannot compile for those targets.
 *
 * This gem is the external HAL provider (hal-io-<conf>) for such builds. It
 * reuses the POSIX source textually; where spawning is prohibited, fork()
 * and execl() resolve to failures, so IO.popen raises Errno::ENOTSUP at run
 * time instead of the build failing. On macOS and iOS the result is the
 * POSIX HAL unchanged.
 */
#include <TargetConditionals.h>

/* Same order as the POSIX source: io_hal.h before <sys/stat.h>, whose
 * st_atime / st_mtime / st_ctime macros would otherwise rewrite the
 * mrb_io_stat field names. Then every header that declares fork/exec*, so
 * the macros below never touch a declaration. */
#include <mruby.h>
#include "io_hal.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/param.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <libgen.h>

#if TARGET_OS_WATCH || TARGET_OS_TV
#define fork() (errno = ENOTSUP, (pid_t)-1)
#define execl(...) (-1)
#endif

#include "../../picoruby-mruby/lib/mruby/mrbgems/mruby-io/ports/posix/io_hal.c"
