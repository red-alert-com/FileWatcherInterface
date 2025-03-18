/**
 * Enhanced File System Watcher
 * 
 * A daemon-capable file system monitoring utility using inotify
 * Provides pattern matching, callbacks, and recursive monitoring
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <fnmatch.h>
#include <syslog.h>
#include <getopt.h>
#include <dirent.h>
#include <limits.h>
#include <ftw.h>
#include "daemon_utils.h"

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))
#define MAX_CALLBACKS 20
#define DEFAULT_PID_FILE "/var/run/fswatcher.pid"
#define MAX_WATCHES 512

// Watch descriptor mapping
typedef struct {
    int wd;                 // Watch descriptor
    char path[PATH_MAX];    // Full path being watched
} watch_info;

// Callback function type
typedef void (*event_callback)(const char *path, const char *filename);

// Callback structure
typedef struct {
    uint32_t mask;              // Event mask to trigger on
    char *pattern;              // Pattern to match
    event_callback callback;    // Function to call
} callback_info;

// Global variables
static int fd = -1;                             // inotify file descriptor
static int daemon_mode = 0;                     // Running as daemon?
static int recursive_mode = 0;                  // Watch directories recursively
static watch_info watches[MAX_WATCHES];         // Watch descriptor mapping
static int watch_count = 0;                     // Number of active watches
static callback_info callbacks[MAX_CALLBACKS];  // Callback registry
static int callback_count = 0;                  // Number of registered callbacks
static char **patterns = NULL;                  // Filename patterns to match
static int pattern_count = 0;                   // Number of patterns

/**
 * Register a callback function for specific events
 */
int register_callback(uint32_t event_mask, const char *pattern, event_callback cb) {
    if (callback_count >= MAX_CALLBACKS) {
        return -1;
    }
    
    callbacks[callback_count].mask = event_mask;
    callbacks[callback_count].pattern = pattern ? strdup(pattern) : NULL;
    callbacks[callback_count].callback = cb;
    
    return callback_count++;
}

/**
 * Add a watch for a specific directory
 */
int add_watch(const char *path) {
    // Check if we've reached the maximum number of watches
    if (watch_count >= MAX_WATCHES) {
        if (daemon_mode) {
            syslog(LOG_ERR, "Maximum number of watches reached (max=%d)", MAX_WATCHES);
        } else {
            fprintf(stderr, "Maximum number of watches reached (max=%d)\n", MAX_WATCHES);
        }
        return -1;
    }
    
    // Add the watch
    int wd = inotify_add_watch(fd, path, 
                  IN_CREATE | IN_MODIFY | IN_DELETE | 
                  IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB);
    
    if (wd < 0) {
        if (daemon_mode) {
            syslog(LOG_ERR, "Failed to add watch for %s: %s", path, strerror(errno));
        } else {
            fprintf(stderr, "Failed to add watch for %s: %s\n", path, strerror(errno));
        }
        return -1;
    }
    
    // Store the watch info
    watches[watch_count].wd = wd;
    strncpy(watches[watch_count].path, path, PATH_MAX - 1);
    watches[watch_count].path[PATH_MAX - 1] = '\0';
    
    if (daemon_mode) {
        syslog(LOG_INFO, "Watching directory: %s (wd=%d)", path, wd);
    } else {
        printf("Watching directory: %s (wd=%d)\n", path, wd);
    }
    
    watch_count++;
    return wd;
}

/**
 * Look up the path for a given watch descriptor
 */
const char* get_path_by_wd(int wd) {
    for (int i = 0; i < watch_count; i++) {
        if (watches[i].wd == wd) {
            return watches[i].path;
        }
    }
    return NULL;
}

/**
 * Recursively add watches for a directory and all its subdirectories
 */
static int ftw_callback(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    if (typeflag == FTW_D && ftwbuf->level >= 0) {  // Directory and not the root (which is already watched)
        add_watch(path);
    }
    return 0;  // Continue traversal
}

void watch_recursively(const char *path) {
    if (nftw(path, ftw_callback, 16, FTW_PHYS) == -1) {
        if (daemon_mode) {
            syslog(LOG_ERR, "Failed to recursively watch %s: %s", path, strerror(errno));
        } else {
            fprintf(stderr, "Failed to recursively watch %s: %s\n", path, strerror(errno));
        }
    }
}

/**
 * Check if a file matches any of the patterns
 */
int matches_pattern(const char *filename) {
    if (pattern_count == 0) {
        return 1;  // No patterns means match everything
    }
    
    for (int i = 0; i < pattern_count; i++) {
        if (fnmatch(patterns[i], filename, 0) == 0) {
            return 1;
        }
    }
    
    return 0;
}

/**
 * Process an event and trigger appropriate callbacks
 */
void process_event(uint32_t event_mask, const char *path, const char *filename) {
    // Check if this is a new directory and we're in recursive mode
    if (recursive_mode && (event_mask & IN_CREATE) && (event_mask & IN_ISDIR)) {
        char full_path[PATH_MAX];
        snprintf(full_path, PATH_MAX, "%s/%s", path, filename);
        add_watch(full_path);
        
        if (daemon_mode) {
            syslog(LOG_INFO, "Added watch for new directory: %s", full_path);
        } else {
            printf("Added watch for new directory: %s\n", full_path);
        }
    }
    
    // Log the event if in daemon mode
    if (daemon_mode) {
        if (event_mask & IN_CREATE)
            syslog(LOG_INFO, "File created: %s/%s", path, filename);
        if (event_mask & IN_DELETE)
            syslog(LOG_INFO, "File deleted: %s/%s", path, filename);
        if (event_mask & IN_MODIFY)
            syslog(LOG_INFO, "File modified: %s/%s", path, filename);
        if (event_mask & IN_MOVED_FROM)
            syslog(LOG_INFO, "File moved from: %s/%s", path, filename);
        if (event_mask & IN_MOVED_TO)
            syslog(LOG_INFO, "File moved to: %s/%s", path, filename);
    }
    
    // Process through callbacks
    for (int i = 0; i < callback_count; i++) {
        // Check if event mask matches
        if (callbacks[i].mask & event_mask) {
            // Check if pattern matches
            if (!callbacks[i].pattern || 
                fnmatch(callbacks[i].pattern, filename, 0) == 0) {
                callbacks[i].callback(path, filename);
            }
        }
    }
}

/**
 * Clean up all resources
 */
void cleanup() {
    // Remove all watches
    for (int i = 0; i < watch_count; i++) {
        inotify_rm_watch(fd, watches[i].wd);
    }
    
    // Close the inotify file descriptor
    if (fd >= 0) {
        close(fd);
    }
    
    // Free allocated memory for callbacks
    for (int i = 0; i < callback_count; i++) {
        free(callbacks[i].pattern);
    }
}

/**
 * Example callbacks
 */
void on_file_created(const char *path, const char *filename) {
    if (!daemon_mode) {
        printf("CALLBACK: File created: %s/%s\n", path, filename);
    }
    // Add custom logic here
}

void on_file_deleted(const char *path, const char *filename) {
    if (!daemon_mode) {
        printf("CALLBACK: File deleted: %s/%s\n", path, filename);
    }
    // Add custom logic here
}

void on_file_modified(const char *path, const char *filename) {
    if (!daemon_mode) {
        printf("CALLBACK: File modified: %s/%s\n", path, filename);
    }
    // Add custom logic here
}

/**
 * Print usage information
 */
void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] PATH_TO_WATCH [PATTERN...]\n", program_name);
    printf("Options:\n");
    printf("  -d, --daemon        Run as a daemon\n");
    printf("  -r, --recursive     Watch directories recursively\n");
    printf("  -p, --pid=FILE      PID file location (default: %s)\n", DEFAULT_PID_FILE);
    printf("  -h, --help          Display this help message\n");
    printf("\nExamples:\n");
    printf("  %s /home/user/docs             # Watch all files in docs\n", program_name);
    printf("  %s -r /var/log \"*.log\"         # Watch log files recursively\n", program_name);
    printf("  %s -d -p /tmp/fw.pid /etc      # Watch /etc as a daemon\n", program_name);
}

/**
 * Main program
 */
int main(int argc, char **argv) {
    const char *pid_file = DEFAULT_PID_FILE;
    const char *watch_path = NULL;
    
    // Parse command line options
    int opt;
    static struct option long_options[] = {
        {"daemon",    no_argument,       NULL, 'd'},
        {"recursive", no_argument,       NULL, 'r'},
        {"pid",       required_argument, NULL, 'p'},
        {"help",      no_argument,       NULL, 'h'},
        {NULL,        0,                 NULL, 0}
    };
    
    while ((opt = getopt_long(argc, argv, "drp:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                daemon_mode = 1;
                break;
            case 'r':
                recursive_mode = 1;
                break;
            case 'p':
                pid_file = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    
    // Get watch path from remaining arguments
    if (optind < argc) {
        watch_path = argv[optind++];
    } else {
        fprintf(stderr, "Error: No watch path specified\n");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }
    
    // Process pattern arguments
    if (optind < argc) {
        pattern_count = argc - optind;
        patterns = &argv[optind];
        
        if (!daemon_mode) {
            printf("Filtering for patterns:\n");
            for (int i = 0; i < pattern_count; i++) {
                printf("  - %s\n", patterns[i]);
            }
        }
    }
    
    // Register example callbacks
    register_callback(IN_CREATE, NULL, on_file_created);
    register_callback(IN_DELETE, NULL, on_file_deleted);
    register_callback(IN_MODIFY, NULL, on_file_modified);
    
    // Set up atexit handler for cleanup
    atexit(cleanup);
    
    // Daemonize if requested
    if (daemon_mode) {
        if (daemonize() < 0) {
            fprintf(stderr, "Failed to daemonize\n");
            exit(EXIT_FAILURE);
        }
        
        // Write PID file
        if (write_pid_file(pid_file) < 0) {
            syslog(LOG_ERR, "Failed to write PID file");
            exit(EXIT_FAILURE);
        }
        
        // Setup signal handlers
        setup_daemon_signal_handlers();
    }
    
    // Initialize inotify
    fd = inotify_init();
    if (fd < 0) {
        if (daemon_mode) {
            syslog(LOG_ERR, "Failed to initialize inotify: %s", strerror(errno));
        } else {
            perror("inotify_init");
        }
        exit(EXIT_FAILURE);
    }
    
    // Add watch for the specified path
    int initial_wd = add_watch(watch_path);
    if (initial_wd < 0) {
        exit(EXIT_FAILURE);
    }
    
    // If recursive mode is enabled, add watches for all subdirectories
    if (recursive_mode) {
        if (!daemon_mode) {
            printf("Recursive mode enabled, watching all subdirectories\n");
        }
        
        watch_recursively(watch_path);
        
        if (!daemon_mode) {
            printf("Total watches: %d\n", watch_count);
        }
    }
    
    // Buffer for reading events
    char buffer[BUF_LEN];
    
    // Main event loop
    while (1) {
        int i = 0;
        int length = read(fd, buffer, BUF_LEN);
        
        if (length < 0) {
            if (daemon_mode) {
                syslog(LOG_ERR, "Read error: %s", strerror(errno));
            } else {
                perror("read");
            }
            exit(EXIT_FAILURE);
        }
        
        // Process events
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *) &buffer[i];
            
            if (event->len) {
                // Get the path for this watch descriptor
                const char *path = get_path_by_wd(event->wd);
                if (path) {
                    // Check if the file matches any of our patterns
                    if (matches_pattern(event->name)) {
                        // Process the event
                        process_event(event->mask, path, event->name);
                        
                        // Also print the raw event info if not in daemon mode
                        if (!daemon_mode) {
                            if (event->mask & IN_CREATE)
                                printf("File created: %s/%s\n", path, event->name);
                            if (event->mask & IN_DELETE)
                                printf("File deleted: %s/%s\n", path, event->name);
                            if (event->mask & IN_MODIFY)
                                printf("File modified: %s/%s\n", path, event->name);
                            if (event->mask & IN_MOVED_FROM)
                                printf("File moved from: %s/%s\n", path, event->name);
                            if (event->mask & IN_MOVED_TO)
                                printf("File moved to: %s/%s\n", path, event->name);
                        }
                    }
                } else {
                    if (daemon_mode) {
                        syslog(LOG_WARNING, "Received event for unknown watch descriptor: %d", event->wd);
                    } else {
                        fprintf(stderr, "Warning: Received event for unknown watch descriptor: %d\n", event->wd);
                    }
                }
            }
            
            i += EVENT_SIZE + event->len;
        }
    }
    
    // This point will never be reached in this simple version
    // Cleanup is handled by atexit function
    return 0;
}
