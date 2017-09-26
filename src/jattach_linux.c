/*
 * Copyright 2016 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef __linux__
#define _GNU_SOURCE
#include <sched.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define PATH_MAX 1024

// See hotspot/src/os/bsd/vm/os_bsd.cpp
// This must be hard coded because it's the system's temporary
// directory not the java application's temp directory, ala java.io.tmpdir.
#ifdef __APPLE__
// macosx has a secure per-user temporary directory

char temp_path_storage[PATH_MAX];

const char* get_temp_directory() {
    static char *temp_path = NULL;
    if (temp_path == NULL) {
        int pathSize = confstr(_CS_DARWIN_USER_TEMP_DIR, temp_path_storage, PATH_MAX);
        if (pathSize == 0 || pathSize > PATH_MAX) {
            strlcpy(temp_path_storage, "/tmp", sizeof (temp_path_storage));
        }
        temp_path = temp_path_storage;
    }
    return temp_path;
}
#else // __APPLE__

const char* get_temp_directory() {
    return "/tmp";
}
#endif // __APPLE__

// Check if remote JVM has already opened socket for Dynamic Attach
static int check_socket(int pid) {
    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "%s/.java_pid%d", get_temp_directory(), pid);

    struct stat stats;
    return stat(path, &stats) == 0 && S_ISSOCK(stats.st_mode);
}

// Force remote JVM to start Attach listener.
// HotSpot will start Attach listener in response to SIGQUIT if it sees .attach_pid file
static int start_attach_mechanism(int pid, int nspid) {
    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "/proc/%d/cwd/.attach_pid%d", nspid, nspid);
    
    int fd = creat(path, 0660);
    if (fd == -1) {
        snprintf(path, PATH_MAX, "%s/.attach_pid%d", get_temp_directory(), nspid);
        fd = creat(path, 0660);
        if (fd == -1) {
            return 0;
        }
    }
    close(fd);
    
    // We have to still use the host namespace pid here for the kill() call
    kill(pid, SIGQUIT);
    
    int result;
    struct timespec ts = {0, 100000000};
    int retry = 0;
    do {
        nanosleep(&ts, NULL);
        result = check_socket(nspid);
    } while (!result && ++retry < 10);

    unlink(path);
    return result;
}

// Connect to UNIX domain socket created by JVM for Dynamic Attach
static int connect_socket(int pid) {
    int fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/.java_pid%d", get_temp_directory(), pid);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

// Send command with arguments to socket
static int write_command(int fd, int argc, char** argv) {
    // Protocol version
    if (write(fd, "1", 2) <= 0) {
        return 0;
    }

    int i;
    for (i = 0; i < 4; i++) {
        const char* arg = i < argc ? argv[i] : "";
        if (write(fd, arg, strlen(arg) + 1) <= 0) {
            return 0;
        }
    }
    return 1;
}

// Mirror response from remote JVM to stdout
static void read_response(int fd) {
    char buf[8192];
    ssize_t bytes;
    while ((bytes = read(fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, bytes, stdout);
    }
}

// On Linux, get the innermost pid namespace pid for the specified host pid
static int nspid_for_pid(int pid) {
#ifdef __linux__
    int nspid = pid;
    char status[64];
    char *line = NULL;
    FILE *status_file;
    size_t size;

    snprintf(status, sizeof(status), "/proc/%d/status", pid);
    status_file = fopen(status, "r");

    while (getline(&line, &size, status_file) != -1) {
        if (strstr(line, "NStgid:") != NULL) {
            // PID namespaces can be nested; the last one is the innermost one
            nspid = (int)strtol(strrchr(line, '\t'), NULL, 10);
        }
    }

    if (line != NULL) {
        free(line);
    }
    fclose(status_file);
    return nspid;
#else
    return pid;
#endif
}

static int enter_mount_ns(int pid) {
#ifdef __linux__
    // We're leaking the oldns and newns descriptors, but this is a short-running
    // tool, so they will be closed when the process exits anyway.
    int oldns, newns;
    char curnspath[128], newnspath[128];
    struct stat oldns_stat, newns_stat;

    snprintf(curnspath, sizeof(curnspath), "/proc/self/ns/mnt");
    snprintf(newnspath, sizeof(newnspath), "/proc/%d/ns/mnt", pid);

    if ((oldns = open(curnspath, O_RDONLY)) < 0 ||
        ((newns = open(newnspath, O_RDONLY)) < 0)) {
        return 0;
    }

    if (fstat(oldns, &oldns_stat) < 0 || fstat(newns, &newns_stat) < 0) {
        return 0;
    }
    if (oldns_stat.st_ino == newns_stat.st_ino) {
        // Don't try to call setns() if we're in the same namespace already.
        return 1;
    }

    return setns(newns, CLONE_NEWNS) < 0 ? 0 : 1;
#else
    return 1;
#endif
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: jattach <pid> <cmd> <args> ...\n");
        return 1;
    }
    
    int pid = atoi(argv[1]);
    if (pid == 0) {
        perror("Invalid pid provided");
        return 1;
    }

    int nspid = nspid_for_pid(pid);
    if (enter_mount_ns(pid) < 0) {
        fprintf(stderr, "WARNING: couldn't enter target process mnt namespace\n");
    }

    // Make write() return EPIPE instead of silent process termination
    signal(SIGPIPE, SIG_IGN);

    if (!check_socket(nspid) && !start_attach_mechanism(pid, nspid)) {
        perror("Could not start attach mechanism");
        return 1;
    }

    int fd = connect_socket(nspid);
    if (fd == -1) {
        perror("Could not connect to socket");
        return 1;
    }
    
    printf("Connected to remote JVM\n");
    if (!write_command(fd, argc - 2, argv + 2)) {
        perror("Error writing to socket");
        close(fd);
        return 1;
    }

    printf("Response code = ");
    read_response(fd);

    printf("\n");
    close(fd);

    return 0;
}
