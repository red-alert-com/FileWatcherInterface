// daemon_utils.h
#ifndef DAEMON_UTILS_H
#define DAEMON_UTILS_H

// Daemonize the process
int daemonize();

// Write PID to file
int write_pid_file(const char *pid_file);

// Remove PID file
void remove_pid_file(const char *pid_file);

// Setup signal handlers for daemon
void setup_daemon_signal_handlers();

#endif // DAEMON_UTILS_H
