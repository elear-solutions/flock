#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sysexits.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <paths.h>
#include <sys/file.h>

/* options descriptor */
static struct option longopts[] = {
  { "script",     required_argument,      NULL,           'c' },
  { "lock",       required_argument,      NULL,           'l' },
  { "shared",     no_argument,            NULL,           's' },
  { "exclusive",  no_argument,            NULL,           'x' },
  { "nb",         no_argument,            NULL,           'n' },
  { "verbose",    no_argument,            NULL,           'v' },
  { "timeout",    required_argument,      NULL,           'w' }
};

static bool timeout_expired = false;

/* Meant to be used atexit(close_stdout); */
static inline void close_stdout(void) {
	if (0 != ferror(stdout) || 0 != fclose(stdout)) {
		warn("write error");
		_exit(EXIT_FAILURE);
	}

	if (0 != ferror(stderr) || 0 != fclose(stderr)) {
		warn("write error");
		_exit(EXIT_FAILURE);
	}
}

static void timeout_handler(int /*@unused@*/ sig __attribute__((__unused__))) {
	timeout_expired = true;
}

int main(int argc, char *argv[]) {
	int type = LOCK_EX;
	bool have_timeout = false;
	bool verbose = false;
	double raw_timeval;
	int status_time_conflict = EXIT_FAILURE;
	struct itimerval timer, old_timer;
	struct sigaction sa, old_sa;
    struct timeval t_l_req, t_l_acq;
	int block = 0;
	bool close_before_exec = false;
	char *filename = NULL;
	char **cmd_argv = NULL, *sh_c_argv[4];
	int fd = -1;
	int opt;
	int status = EX_OK;
	int open_flags = 0;
	pid_t w, f;

	if (0 != atexit(close_stdout)) {
		err(EX_OSERR, "Could not attach atexit handler");
	}

	if (argc < 2) {
		exit(EX_USAGE);
	}

	memset(&timer, 0, sizeof timer);

  while (-1 != (opt = getopt_long(argc, argv, "c:l:sxnvw:", longopts, NULL))) {
		switch (opt) {
		case 'c':
			cmd_argv = sh_c_argv;
			cmd_argv[0] = getenv("SHELL");

			if (!cmd_argv[0] || !*cmd_argv[0]) {
				cmd_argv[0] = _PATH_BSHELL;
			}
			cmd_argv[1] = "-c";
			cmd_argv[2] = optarg;
			cmd_argv[3] = NULL;
			break;
		case 'l':
			filename = optarg;
			break;
		case 's':
			type = LOCK_SH;
			break;
		case 'x':
			type = LOCK_EX;
			break;
		case 'n':
			block = LOCK_NB;
			break;
		case 'v':
			verbose = true;
			break;
		case 'w':
			have_timeout = true;
			raw_timeval = strtod(optarg, NULL);
			if (0 >= raw_timeval)
				errx(EX_USAGE, "timeout must be greater than 0, was %f", raw_timeval);
			timer.it_value.tv_sec = (time_t) raw_timeval;
			timer.it_value.tv_usec = (suseconds_t) ((raw_timeval - timer.it_value.tv_sec) * 1000000);
			break;
		case '?':
		default:
			break;
		}
	}


	if (NULL != filename) {
		// some systems allow exclusive locks on read-only files
		if (LOCK_SH == type || 0 != access(filename, W_OK)) {
			open_flags = O_RDONLY | O_NOCTTY | O_CREAT;
		} else {
			open_flags = O_WRONLY | O_NOCTTY | O_CREAT;
		}

		if (true == verbose) {
			gettimeofday(&t_l_req,NULL);
			printf("flock: getting lock ");
		}
		fd = open(filename, open_flags, 0666);

		// directories don't like O_WRONLY (and sometimes O_CREAT)
		if (fd < 0 && EISDIR == errno) {
			open_flags = O_RDONLY | O_NOCTTY;
			fd = open(filename, open_flags);
		}

		if (fd < 0) {
			warn("cannot open lock file %s", filename);
			switch (errno) {
			case ENOMEM:
			case EMFILE:
			case ENFILE:
				err(EX_OSERR, "OS error");
			case EROFS:
			case ENOSPC:
				err(EX_CANTCREAT, "could not create file");
			default:
				err(EX_NOINPUT, "invalid input");
			}
		}
	}

	if (true == have_timeout) {
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = timeout_handler;
		sa.sa_flags = SA_RESETHAND;

		if (0 != sigaction(SIGALRM, &sa, &old_sa)) {
			err(EX_OSERR, "could not attach timeout handler");
		}

		if (0 != setitimer(ITIMER_REAL, &timer, &old_timer)) {
			err(EX_OSERR, "could not set interval timer");
		}
	}

	while (0 != flock(fd, type | block)) {
		switch (errno) {
		case EWOULDBLOCK: // non-blocking lock not available
			exit(status_time_conflict);
		case EINTR: // interrupted by signal
			if (timeout_expired) { // failed to acquire lock in time
				exit(status_time_conflict);
			}
			continue;
		case EIO:
		case ENOLCK:
			err(EX_OSERR, "OS error");
		default:
			printf("err : %d -> %s\n", errno, strerror(errno));
			err(EX_DATAERR, "data error");
		}
	}
	if (true == verbose) {
		gettimeofday(&t_l_acq,NULL);
		printf("took %1u microseconds\n", (t_l_acq.tv_usec - t_l_req.tv_usec)); // not adding due to time constraints
	}

	if (true == have_timeout) {
		if (0 != setitimer(ITIMER_REAL, &old_timer, NULL)) {
			err(EX_OSERR, "could not reset old interval timer");
		}

		if (0 != sigaction(SIGALRM, &old_sa, NULL)) {
			err(EX_OSERR, "could not reattach old timeout handler");
		}
	}

	if (NULL != cmd_argv) {
		// Clear any inherited settings
		signal(SIGCHLD, SIG_DFL);
		f = fork();

		if (f < 0) {
			err(EX_OSERR, "fork failed");
		} else if (0 == f) {
			if (close_before_exec)
				if (0 != close(fd)) {
					err(EX_OSERR, "could not close file descriptor");
				}

			if (true == verbose) {
				printf("flock: executing %s\n", cmd_argv[0]);
			}
			if (0 != execvp(cmd_argv[0], cmd_argv)) {
				warn("failed to execute command: %s", cmd_argv[0]);
				switch(errno) {
				case EIO:
				case ENOMEM:
					_exit(EX_OSERR);
				default:
					_exit(EX_NOINPUT);
				}
			}
		} else {
			do {
				w = waitpid(f, &status, 0);
				if (-1 == w && errno != EINTR) {
					break;
				}
			} while (w != f);

			if (-1 == w) {
				err(EXIT_FAILURE, "waidpid failed");
			}
			else if (0 != WIFEXITED(status)) {
				status = WEXITSTATUS(status);
			}
			else if (0 != WIFSIGNALED(status)) {
				status = WTERMSIG(status) + 128;
			}
			else {
				status = EX_OSERR;
			}
		}
	}

	return status;
}

