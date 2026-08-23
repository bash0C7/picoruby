/*
  Original source code from mruby/mrubyc:
    Copyright (C) 2015- Kyushu Institute of Technology.
    Copyright (C) 2015- Shimane IT Open-Innovation Center.
  Modified source code for picoruby/femtoruby:
    Copyright (C) 2025 HASUMI Hitoshi.

  This file is distributed under BSD 3-Clause License.
*/

/*
 * Darwin (macOS host / iOS / watchOS) console HAL for picoruby-machine.
 *
 * A CrossBuild with `conf.ports :darwin, :posix` compiles only this
 * directory for the gem (first match), so everything ports/posix/hal.c
 * provides has to exist here as well. The Darwin/XNU userland is a BSD
 * libc, so the implementations are the POSIX ones.
 *
 * One deliberate difference: under PICORB_VM_MRUBY the scheduler tick,
 * IRQ masking and idle/sleep primitives (mrb_hal_task_init and friends)
 * are owned by mruby-task's own port (ports/posix on a macOS host) or by
 * the embedder (an iOS/watchOS app bridge links its own task HAL), so this
 * file does not define picorb_hal_init for the mruby VM. A second
 * mrb_hal_task_init in libmruby.a would be a duplicate symbol the moment
 * this object is pulled in for picorb_hal_write. The mruby/c VM has no
 * mruby-task, so its HAL entry points stay here, as in ports/posix.
 */

/***** System headers *******************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

/***** Local headers ********************************************************/
#include "../../include/hal.h"

/***** Local variables ******************************************************/
/* SIGALRM mask for the tick-based IRQ emulation. Only the mruby/c path
 * arms the tick here; for the mruby VM the set stays empty and the
 * picorb_*_irq entry points below are no-ops kept for symbol parity. */
static sigset_t sigset_, sigset2_;

#if defined(PICORB_VM_MRUBYC)
#define MRB_TICK_UNIT MRBC_TICK_UNIT

static void
sig_alarm(int dummy)
{
  (void)dummy;
  picorb_tick();
}

void
picorb_hal_init(void)
{
  sigemptyset(&sigset_);
  sigaddset(&sigset_, SIGALRM);

  struct sigaction sa;
  sa.sa_handler = sig_alarm;
  sa.sa_flags   = SA_RESTART;
  sa.sa_mask    = sigset_;
  sigaction(SIGALRM, &sa, 0);

  struct itimerval tval;
  int sec  = 0;
  int usec = MRB_TICK_UNIT * 1000;
  tval.it_interval.tv_sec  = sec;
  tval.it_interval.tv_usec = usec;
  tval.it_value.tv_sec     = sec;
  tval.it_value.tv_usec    = usec;
  setitimer(ITIMER_REAL, &tval, 0);
}

void
picorb_hal_enable_irq(void)
{
  sigprocmask(SIG_SETMASK, &sigset2_, 0);
}

void
picorb_hal_disable_irq(void)
{
  sigprocmask(SIG_BLOCK, &sigset_, &sigset2_);
}

void
picorb_hal_idle_cpu(void)
{
  sleep(1);
}
#endif /* PICORB_VM_MRUBYC */

/***** Global functions *****************************************************/

int
picorb_hal_write(int fd, const void *buf, int nbytes)
{
  return (int)write(fd == 2 ? STDERR_FILENO : STDOUT_FILENO, buf, nbytes);
}

int
picorb_hal_flush(int fd)
{
  /* write(2) above is unbuffered; this drains the stdio stream that
   * debug_printf uses on the POSIX path. */
  return fflush(fd == 2 ? stderr : stdout);
}

//================================================================
/*!@brief
  enable interrupt

*/
void
picorb_enable_irq(void)
{
  sigprocmask(SIG_SETMASK, &sigset2_, 0);
}


//================================================================
/*!@brief
  disable interrupt

*/
void
picorb_disable_irq(void)
{
  sigprocmask(SIG_BLOCK, &sigset_, &sigset2_);
}


//================================================================
/*!@brief
  abort program

*/
void
picorb_hal_abort(const char *s)
{
  if (s) {
    picorb_hal_write(2, s, (int)strlen(s));
  }
  exit(1);
}
