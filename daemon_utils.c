// daemon_utils.c
#include "daemon_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

static const char *current_pid_file = NULL;

// Signal handler for termination signals
static void signal_handler(int sig) {
    switch (sig) {
        case SIGTERM:
        case SIGINT:
            syslog(LOG_NOTICE, "Received signal %d, shutting down...", sig);
            // Remove PID file before exiting
            if (current_pid_file) {
                remove_pid_file(current_pid_file);
            }
            closelog();
            exit(EXIT_SUCCESS);
            break;
        case SIGHUP:
            // Could implement config reload here
            syslog(LOG_NOTICE, "Received SIGHUP, reloading configuration...");
            break;
    }
}

// Daemonize the process
int daemonize() {
    pid_t pid, sid;
    
    // Fork off the parent process
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    
    // If we got a good PID, then we can exit the parent process
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    
    // Change the file mode mask
    umask(0);
    
    // Open logs
    openlog("fswatcher", LOG_PID, LOG_DAEMON);
    syslog(LOG_NOTICE, "Daemon started");
    
    // Create a new SID for the child process
    sid = setsid();
    if (sid < 0) {
        syslog(LOG_ERR, "Failed to create new session, code %d (%s)",
               errno, strerror(errno));
        return -1;
    }
    
    // Change the current working directory.
    // This prevents the current directory from being locked
    if (chdir("/") < 0) {
        syslog(LOG_ERR, "Failed to change working directory, code %d (%s)",
               errno, strerror(errno));
        return -1;
    }
    
    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // Redirect standard file descriptors to /dev/null
    int fd = open("/dev/null", O_RDWR);
    if (fd != STDIN_FILENO) {
        dup2(fd, STDIN_FILENO);
    }
    if (fd != STDOUT_FILENO) {
        dup2(fd, STDOUT_FILENO);
    }
    if (fd != STDERR_FILENO) {
        dup2(fd, STDERR_FILENO);
    }
    if (fd > STDERR_FILENO) {
        close(fd);
    }
    
    return 0;
}

// Write PID to file
int write_pid_file(const char *pid_file) {
    FILE *fp;
    
    fp = fopen(pid_file, "w");
    if (fp == NULL) {
        syslog(LOG_ERR, "Failed to open PID file %s, code %d (%s)",
               pid_file, errno, strerror(errno));
        return -1;
    }
    
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    
    // Store for later usage when handling signals
    current_pid_file = pid_file;
    
    return 0;
}

// Remove PID file
void remove_pid_file(const char *pid_file) {
    if (unlink(pid_file) < 0) {
        syslog(LOG_ERR, "Failed to remove PID file %s, code %d (%s)",
               pid_file, errno, strerror(errno));
    }
}

// Setup signal handlers for daemon
void setup_daemon_signal_handlers() {
    struct sigaction sa;
    
    // Setup signal handler
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}
