/*
 * Darwin HAL for mruby-task.
 *
 * The POSIX HAL arms setitimer(ITIMER_REAL). That SIGALRM is addressed to
 * the process, and the kernel hands it to whichever thread is not blocking
 * it; on Darwin, CoreBluetooth and dispatch timers run threads that never
 * block it, so mrb_tick (which moves tasks between the waiting and ready
 * lists) runs there, concurrently with the VM thread. mrb_task_disable_irq
 * cannot stop that: the signal mask belongs to the calling thread only.
 *
 * This HAL is the POSIX one with one addition: a handler that finds itself
 * on any thread other than the one that called mrb_hal_task_init re-sends
 * SIGALRM to that thread with pthread_kill and returns. A thread-directed
 * signal is delivered to that thread only, and while it is blocked there
 * by mrb_task_disable_irq it stays pending until mrb_task_enable_irq, so
 * mrb_tick runs on the VM thread and never inside an exclusion.
 *
 * Every registered VM is ticked from that one thread, as in the POSIX HAL;
 * a VM driven from a second thread would be ticked across threads again.
 */
#include <mruby.h>
#include "task_hal.h"
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define NSEC_PER_MSEC 1000000ULL
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000LL
#endif
#define USEC_PER_MSEC 1000ULL

static mrb_state *vm_list[MRB_TASK_MAX_VMS];
static volatile sig_atomic_t vm_count = 0;
static sigset_t alarm_mask;
static pthread_t vm_thread;

static void
sigalrm_handler(int sig)
{
  int i;
  if (!pthread_equal(pthread_self(), vm_thread)) {
    pthread_kill(vm_thread, sig);
    return;
  }
  for (i = 0; i < vm_count; i++) {
    if (vm_list[i]) {
      mrb_tick(vm_list[i]);
    }
  }
}

void
mrb_hal_task_init(mrb_state *mrb)
{
  int i;
  struct sigaction sa;
  struct itimerval timer;
  int vm_index = -1;

  for (i = 0; i < 4; i++) {
    mrb->task.queues[i] = NULL;
  }
  mrb->task.tick = 0;
  mrb->task.wakeup_tick = UINT32_MAX;
  mrb->task.switching = FALSE;

  sigemptyset(&alarm_mask);
  sigaddset(&alarm_mask, SIGALRM);
  pthread_sigmask(SIG_BLOCK, &alarm_mask, NULL);

  for (i = 0; i < vm_count; i++) {
    if (vm_list[i] == mrb) {
      vm_index = i;
      break;
    }
  }
  if (vm_index < 0) {
    if (vm_count >= MRB_TASK_MAX_VMS) {
      pthread_sigmask(SIG_UNBLOCK, &alarm_mask, NULL);
      mrb_raisef(mrb, E_RUNTIME_ERROR,
                 "too many mrb_states with task scheduler (max: %d)",
                 MRB_TASK_MAX_VMS);
    }
    vm_list[vm_count] = mrb;
    vm_count++;
  }

  if (vm_count == 1) {
    vm_thread = pthread_self();

    sa.sa_handler = sigalrm_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = MRB_TICK_UNIT * 1000;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = MRB_TICK_UNIT * 1000;
    setitimer(ITIMER_REAL, &timer, NULL);
  }

  pthread_sigmask(SIG_UNBLOCK, &alarm_mask, NULL);
}

void
mrb_task_enable_irq(void)
{
  pthread_sigmask(SIG_UNBLOCK, &alarm_mask, NULL);
}

void
mrb_task_disable_irq(void)
{
  pthread_sigmask(SIG_BLOCK, &alarm_mask, NULL);
}

void
mrb_hal_task_idle_cpu(mrb_state *mrb)
{
  (void)mrb;
  usleep(MRB_TICK_UNIT * 1000);
}

void
mrb_hal_task_sleep_us(mrb_state *mrb, mrb_int usec)
{
  struct timespec start, now, sleep_time;
  int ret;
  (void)mrb;

  if (usec < 0) {
    return;
  }
  ret = clock_gettime(CLOCK_MONOTONIC, &start);
  if (ret != 0) {
    usleep(usec);
    return;
  }
  uint64_t target_ns = (uint64_t)usec * USEC_PER_MSEC;
  while (1) {
    ret = clock_gettime(CLOCK_MONOTONIC, &now);
    if (ret != 0) {
      break;
    }
    uint64_t elapsed_ns = (uint64_t)(now.tv_sec - start.tv_sec) * NSEC_PER_SEC +
                          (uint64_t)(now.tv_nsec - start.tv_nsec);
    if (elapsed_ns >= target_ns) {
      break;
    }
    uint64_t remaining_ns = target_ns - elapsed_ns;
    if (remaining_ns > NSEC_PER_MSEC) {
      sleep_time.tv_sec = remaining_ns / NSEC_PER_SEC;
      sleep_time.tv_nsec = remaining_ns % NSEC_PER_SEC;
    }
    else {
      sleep_time.tv_sec = 0;
      sleep_time.tv_nsec = NSEC_PER_MSEC;
    }
    nanosleep(&sleep_time, NULL);
  }
}

void
mrb_hal_task_final(mrb_state *mrb)
{
  struct itimerval timer;
  int i, j;

  pthread_sigmask(SIG_BLOCK, &alarm_mask, NULL);
  for (i = 0; i < vm_count; i++) {
    if (vm_list[i] == mrb) {
      for (j = i; j < vm_count - 1; j++) {
        vm_list[j] = vm_list[j + 1];
      }
      vm_list[vm_count - 1] = NULL;
      vm_count--;
      break;
    }
  }
  if (vm_count == 0) {
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &timer, NULL);
  }
  pthread_sigmask(SIG_UNBLOCK, &alarm_mask, NULL);
}

/* mrbgem entry points (the generated gem_init.c calls them); the HAL itself
 * is initialised by mruby-task through mrb_hal_task_init above. */
void
mrb_hal_task_darwin_gem_init(mrb_state *mrb)
{
  (void)mrb;
}

void
mrb_hal_task_darwin_gem_final(mrb_state *mrb)
{
  (void)mrb;
}
