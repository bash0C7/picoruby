/* Apple/Darwin port of picoruby-io-console. Implements the io_* TTY-mode ABI
 * (mirroring ports/posix/io-console.c). Apple has two Darwin sub-platforms with
 * opposite needs, split by TargetConditionals:
 *
 *   - macOS: a real controlling terminal on stdin — reuse the termios logic
 *     verbatim from the posix port.
 *   - iOS / iOS Simulator: the REPL reads from the app UI, not a TTY; stdin is
 *     not a terminal, so termios calls are meaningless. Provide no-TTY stubs with
 *     sane defaults (never raw, echo always on) so the gem links and the REPL runs.
 */
#include <stdbool.h>
#include <TargetConditionals.h>

#if TARGET_OS_IPHONE

/* iOS / iOS Simulator: no controlling TTY. */
bool io_raw_q(void)            { return false; }
void io_raw_bang(bool nonblock) { (void)nonblock; }
void io_cooked_bang(void)      { }
void io_echo_eq(bool flag)     { (void)flag; }
bool io_echo_q(void)           { return true; }
void io__restore_termios(void) { }

#else

/* macOS: real terminal — same termios implementation as the posix port. */
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

static struct termios save_settings;
static int save_flags;

bool
io_raw_q(void)
{
  struct termios settings;
  tcgetattr(fileno(stdin), &settings);
  if ((settings.c_iflag & (BRKINT | ISTRIP | IXON)) == 0 &&
      (settings.c_lflag & (ICANON | IEXTEN | ECHO | ECHOE | ECHOK | ECHONL)) == 0) {
    return true;
  }
  else {
    return false;
  }
}

void
io_raw_bang(bool nonblock)
{
  struct termios settings;
  tcgetattr(fileno(stdin), &save_settings);
  settings = save_settings;
  settings.c_iflag &= ~(BRKINT | ISTRIP | IXON);
  settings.c_lflag &= ~(ICANON | IEXTEN | ECHO | ECHOE | ECHOK | ECHONL);
  settings.c_cc[VMIN]  = 1;
  settings.c_cc[VTIME] = 0;
  tcsetattr(fileno(stdin), TCSANOW, &settings);
  save_flags = fcntl(fileno(stdin), F_GETFL, 0);
  if (nonblock) {
    fcntl(fileno(stdin), F_SETFL, save_flags | O_NONBLOCK);
  }
  else {
    fcntl(fileno(stdin), F_SETFL, save_flags);
  }
}

void
io_cooked_bang(void)
{
  struct termios settings;
  tcgetattr(fileno(stdin), &save_settings);
  settings = save_settings;
  settings.c_iflag |= (BRKINT | ISTRIP | IXON);
  settings.c_lflag |= (ICANON | IEXTEN | ECHO | ECHOE | ECHOK | ECHONL);
  settings.c_cc[VMIN] = 1;
  settings.c_cc[VTIME] = 0;
  tcsetattr(fileno(stdin), TCSANOW, &settings);
  save_flags = fcntl(fileno(stdin), F_GETFL, 0);
  fcntl(fileno(stdin), F_SETFL, save_flags & ~O_NONBLOCK);
}

void
io_echo_eq(bool flag)
{
  struct termios settings;
  tcgetattr(fileno(stdin), &settings);
  if (flag) {
    settings.c_lflag |= ECHO;
  }
  else {
    settings.c_lflag &= ~ECHO;
  }
  tcsetattr(fileno(stdin), TCSANOW, &settings);
}

bool
io_echo_q(void)
{
  struct termios settings;
  tcgetattr(fileno(stdin), &settings);
  if (settings.c_lflag & ECHO) {
    return true;
  }
  else {
    return false;
  }
}

void
io__restore_termios(void)
{
  fcntl(fileno(stdin), F_SETFL, save_flags);
  tcsetattr(fileno(stdin), TCSANOW, &save_settings);
}

#endif
