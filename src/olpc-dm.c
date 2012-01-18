/*
 * olpc-dm: A completely uninteractive desktop manager which just executes
 * the given X session script as a predetermined user.
 *
 * Copyright (C) 2009 One Laptop per Child
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <paths.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include <ck-connector.h>

//#define DEBUG
#define X_DISPLAY ":0"
#define X_VT "vt01"
#define X_SESSION "/usr/bin/olpc-session"
#define OLPC_USER "olpc"

#define AUTH_HOME        "/var/tmp/olpc-auth"
#define XAUTHORITY       AUTH_HOME "/.Xauthority"
#define XSERVERAUTH      AUTH_HOME "/.Xserverauth"
#define ICEAUTHORITY     AUTH_HOME "/.ICEauthority"

static CkConnector *ckc = NULL;
static const char *ck_cookie = NULL;
static pid_t client_pid = -1;
static pid_t server_pid = -1;
static volatile int signal_caught = 0;
static volatile int got_usr1 = 0;

static void shutdown(void);

#define die() _die(__LINE__, NULL)
#define die_msg(fmt...) _die(__LINE__, fmt)
#define die_perror(msg) _die_perror(__LINE__, msg)

__attribute__((noreturn))
static void _die(int line, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "olpc-dm: failure condition encountered on line %d\n",
		line);

	if (fmt != NULL) {
		fprintf(stderr, "olpc-dm: ");
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fputc('\n', stderr);
	}

	_exit(1);
}

__attribute__((noreturn))
static void _die_perror(int line, const char *msg)
{
	fprintf(stderr, "olpc-dm: failure condition encountered on line %d\n",
		line);
	perror(msg);
	_exit(1);
}

#ifdef DEBUG
#define dbg(msg...) _debug(msg)

static void _debug(const char *fmt, ...)
{
	va_list ap;

	printf("olpc-dm: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

#else
#define dbg(msg...)
#endif

/* start X server as the leader of a process group, and wait until it has
 * come online before returning */
static void start_server(void)
{
	pid_t pid;
	sigset_t mask, old;

	/* we use a trick to make the X server send us a SIGUSR1 to let us know
	 * when it is ready to start accepting connections. */
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigprocmask(SIG_BLOCK, &mask, &old);

	pid = vfork();
	if (pid == -1)
		die();

	if (pid == 0) {
		/* child */
		sigprocmask(SIG_SETMASK, &old, NULL);

		/* Don't hang on read/write to tty */
		signal(SIGTTIN, SIG_IGN);
		signal(SIGTTOU, SIG_IGN);

		/* Ignore SIGUSR1. This way the server will send the parent a SIGUSR1
		 * when it is ready to accept connections. */
		signal(SIGUSR1, SIG_IGN);

		if (setpgid(0, getpid()))
			die_perror("setpgid server");

		/* Boost the nice value of the X server for better responsiveness */
		setpriority(PRIO_PROCESS, 0, -10);

#ifndef DEBUG
		freopen("/dev/null", "r", stdin);
		freopen("/var/log/olpc-dm-X.log", "w", stdout);
		freopen("/var/log/olpc-dm-X.error.log", "w", stderr);
#endif
		execl("/usr/bin/X", "X", X_DISPLAY, X_VT, "-wr", "-nolisten", "tcp", "-auth", XSERVERAUTH,
			NULL);
		die_perror("exec X");
	}

	/* parent */
	dbg("server pid is %d", pid);
	server_pid = pid;

	/* wait for SIGUSR1 to indicate readiness, but with a 15 second timeout */
	alarm(15);
	got_usr1 = 0;
	sigsuspend(&old);
	alarm(0);
	sigprocmask(SIG_SETMASK, &old, NULL);

	if (got_usr1)
		return;

	fprintf(stderr, "olpc-dm: Timeout waiting for server to become ready\n");
	shutdown();
	die();
}

/* get the tty number that the X server is using from the 7th field in
 * /proc/123/stat */
static int get_server_tty(void)
{
#define STAT_BUFSIZE 128
	char *buf = malloc(STAT_BUFSIZE);
	char filename[17];
	int r;
	size_t rd;
	FILE *fd;
	char *base = buf;
	char *next;
	int i;

	if (!buf)
		die();

	r = snprintf(filename, sizeof(filename), "/proc/%d/stat", server_pid);
	if (r >= sizeof(filename))
		die();

	fd = fopen(filename, "r");
	if (!fd)
		die_perror("open stat");

	rd = fread(buf, 1, STAT_BUFSIZE, fd);
	fclose(fd);

	if (rd < STAT_BUFSIZE)
		buf[rd] = 0;

	/* we want the 7th field */
	i = 6;
	while (i--) {
		base = index(base, ' ');
		if (!base)
			die_msg("error finding X terminal");
		base++;
	}

	/* make sure the field wasn't truncated */
	next = index(base, ' ');
	if (!next)
		die_msg("error parsing terminal");

	*next = 0;
	r = atoi(base);
	
	if (major(r) != 4)
		die_msg("X not running on a tty?");

	free(buf);
	dbg("X is running on tty%d", minor(r));
	return minor(r);
}

/* open a ConsoleKit session */
static void init_ck(void)
{
	DBusError error;
	dbus_bool_t ret;
	dbus_int32_t uid;
	const char *display = X_DISPLAY;
	char device[11];
	char *device_ptr = device;
	dbus_bool_t is_local = TRUE;
	int r;
	struct passwd *pwd = getpwnam(OLPC_USER);
	if (!pwd)
		die_perror("getpwnam");

	ckc = ck_connector_new();
	if (!ckc)
		die();

	uid = pwd->pw_uid;
	r = snprintf(device, sizeof(device), "/dev/tty%d", get_server_tty());
	if (r >= sizeof(device))
		die();

	dbus_error_init(&error);
	ret = ck_connector_open_session_with_parameters (ckc, &error,
		"unix-user", &uid,
		"x11-display", &display,
		"x11-display-device", &device_ptr,
		"is-local", &is_local,
		NULL);
	if (!ret)
		die();

	dbg("CK session opened");
	ck_cookie = ck_connector_get_cookie(ckc);
	if (!ck_cookie)
		die_msg("couldn't get ConsoleKit cookie");

	/* copy it, just to be sure that it's safe to access after a fork */
	ck_cookie = strdup(ck_cookie);
	if (!ck_cookie)
		die();
}

/* close ConsoleKit session */
static void deinit_ck(void)
{
	DBusError error;

	if (!ck_cookie || !ckc)
		return;

	dbg("deinit ck");
	free((void *) ck_cookie);
	dbus_error_init(&error);
	if (!ck_connector_close_session(ckc, &error))
		fprintf(stderr, "olpc-dm: ck_connector_close_session() failed\n");
}

static void setenv_chk(const char *name, const char *value)
{
	if (setenv(name, value, 1))
		die();
}

/* setup execution environment ready for executing something as the target user:
 * correct environment, privs dropped, etc. */
static void setup_client_env(void)
{
	struct passwd *pwd = getpwnam(OLPC_USER);
	if (!pwd)
		die_perror("getpwnam");

	if (setpgid(0, getpid()))
		die_perror("setpgid");

	if (chdir(pwd->pw_dir))
		die_perror("chdir");

	clearenv();
	setenv_chk("HOME", pwd->pw_dir);
	setenv_chk("LOGNAME", pwd->pw_name);
	setenv_chk("USERNAME", pwd->pw_name);
	setenv_chk("USER", pwd->pw_name);
	setenv_chk("SHELL", pwd->pw_shell);
	setenv_chk("PATH", _PATH_DEFPATH);
	setenv_chk("DISPLAY", X_DISPLAY);
	setenv_chk("XAUTHORITY", XAUTHORITY);
	setenv_chk("XSERVERAUTH", XSERVERAUTH);
	setenv_chk("ICEAUTHORITY", ICEAUTHORITY);
	if (ck_cookie)
		setenv_chk("XDG_SESSION_COOKIE", ck_cookie);

	/* Drop privs */
	if (initgroups(pwd->pw_name, pwd->pw_gid))
		die_perror("initgroups");

	if (setresgid(pwd->pw_gid, pwd->pw_gid, pwd->pw_gid))
		die_perror("setresgid");

	if (setresuid(pwd->pw_uid, pwd->pw_uid, pwd->pw_uid))
		die_perror("setresuid");
}

/* start X11 client as the leader of a process group, with privileges dropped
 * to olpc user. */
static void start_client(void)
{
	pid_t pid = vfork();
	if (pid < 0)
		die();

	if (pid != 0) {
		/* parent */
		dbg("client pid is %d", pid);
		client_pid = pid;
		return;
	}

	/* child */
	setup_client_env();

	dbg("launch client now");
#ifndef DEBUG
	freopen("/dev/null", "r", stdin);
	freopen("/tmp/olpc-dm-client.log", "w", stdout);
	freopen("/tmp/olpc-dm-client.error.log", "w", stderr);
#endif
	execl(X_SESSION,  X_SESSION, NULL);
	die_perror("exec client");
}

static void signal_catcher(int signo)
{
	dbg("caught signal %d", signo);
	if (signo == SIGUSR1)
		got_usr1++;
	else if (signo != SIGALRM)
		signal_caught = signo;
}

static void setup_signals(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = signal_catcher;
	sigemptyset(&sa.sa_mask);

	/* These signals can interrupt the wait() loop */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);

	/* we wait for this signal from the X server to indicate readiness */
	sigaction(SIGUSR1, &sa, NULL);

	/* we listen but silently handle this without acting on it */
	sigaction(SIGALRM, &sa, NULL);

	/* Don't hang on read/write to tty */
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
}

static void wait_for_exit(void)
{
	pid_t pid = -1;
	dbg("wait for child");
	while (pid != client_pid && pid != server_pid && !signal_caught) {
		pid = wait(NULL);
		dbg("wait() returned %d", pid);
	}
}

/* wait until X server has gone away */
static int wait_for_server_shutdown(int timeout)
{
	while (timeout-- > 0) {
		pid_t r;
		dbg("wait for server shutdown (pid %d), timeout %d", server_pid,
			timeout);
		r = waitpid(server_pid, NULL, WNOHANG);
		dbg("waitpid ret %d", r);
		if (r == server_pid) {
			dbg("server has shut down");
			return 0;
		}
		sleep(1);
	}

	return 1;
}

/* kill server by a gentle SIGTERM, followed by a SIGKILL if it doesn't comply.
 * doesn't return until the process has definitely gone away. */
static void kill_server(void)
{
	dbg("send SIGTERM to server");
	if (killpg(server_pid, SIGTERM)) {
		if (errno == ESRCH)
			return;
		die_perror("Can't kill X server");
	}

	if (wait_for_server_shutdown(10) == 0)
		return;

	fprintf(stderr, "Server not shutting down, sending SIGKILL\n");
	if (killpg(server_pid, SIGKILL))
		if (errno == ESRCH)
			return;

	if (wait_for_server_shutdown(3))
		die_msg("Can't kill server");
}

/* kill server and client */
static void shutdown(void)
{
	signal(SIGTERM, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	if (signal_caught)
		fprintf(stderr, "olpc-dm: Unexpected signal %d.\n", signal_caught);

	if (client_pid > 0) {
		dbg("send SIGHUP to client process group");
		if (killpg(client_pid, SIGHUP))
			if (errno != ESRCH)
				perror("killpg client");
	}

	if (server_pid > 0)
		kill_server();
}

/* create MIT cookie for xauth. result must be freed after use */
static char *generate_xauth_cookie(void)
{
#define MIT_COOKIE_LENGTH 32
	char *cookie = malloc(MIT_COOKIE_LENGTH + 1);
	int num_hex = 0;
	int r;
	int i;

	if (!cookie)
		die();

	/* use 2 as the size value for snprintf, because it doesn't seem to
	 * only want to write 1 character, also that means we end up naturally
	 * being NULL-terminated */

	srand(time(NULL));
	for (i = 0; i < MIT_COOKIE_LENGTH - 1; i++) {
		r = rand() % 16;
		snprintf(cookie + i, 2, "%x", r);
		if (r >= 10)
			num_hex++;
    }

	/* must have an even number of digits vs hex characters */
	if (num_hex % 2 == 0)
		r = rand() % 10;
	else
		r = 10 + (rand() % 5);
	snprintf(&cookie[MIT_COOKIE_LENGTH - 1], 2, "%x", r);

	dbg("generated cookie %s", cookie);
	return cookie;
}

static void generate_xauth(void)
{
	char *cookie;

	pid_t pid = vfork();
	if (pid < 0)
		die();

	if (pid != 0) {
		/* parent */
		int status = 0;

		dbg("xauth maker pid %d", pid);
		if (waitpid(pid, &status, 0) < 0)
			die_perror("waitpid xauth maker");
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			return;
		die_msg("xauth maker failed, code %d", status);
	}

	/* child */
	setup_client_env();
	cookie = generate_xauth_cookie();

	/* Create directory for the .Xauthority files (dlo trac #317), and
	 * remove any stale data file from there. */
	/* intentionally ignore retval; we are confident that xauth will correctly
	 * report any truly fatal errors. */
	mkdir(AUTH_HOME, 0700);
	unlink(XAUTHORITY);

	/* invoke xauth program to create the file */
	execl("/usr/bin/xauth", "xauth", "-q", "-f", XAUTHORITY, "add", X_DISPLAY, ".", cookie, NULL);
	die_perror("exec xauth");
}

int main(int argc, char *argv[])
{
	generate_xauth();
	setup_signals();
	start_server();
	init_ck();
	start_client();
	wait_for_exit();
	deinit_ck();
	shutdown();
	_exit(0);
}

