#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <pthread.h>
#include "server.c"

// Function for the child monitoring thread
void *monitor_child(void *arg) {
    pid_t pid = *((pid_t *)arg);

    // Wait for the child process to finish
    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status)) {
        printf("Child process terminated abnormally\n");
    }

    // Signal the FIFO monitoring thread to exit
    child_exited = 1;
    child_exit_status = WEXITSTATUS(status);

    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    // Check if there are enough arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <child_program> [args...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Create the named FIFO
    if (mkfifo(FIFO_PATH, 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo");
            exit(EXIT_FAILURE);
        }
    }
    printf("FIFO created\n");

    // Create the recovery FIFO
    if (mkfifo(FIFO_RECOVER_PATH, 0666) == -1) {
        printf("FIFO recover failed to create\n");
        fflush(stdout);
        if (errno != EEXIST) {
            perror("mkfifo");
            exit(EXIT_FAILURE);
        }
    }
    printf("Recovery FIFO created\n");
    fflush(stdout);

    // Fork a child process
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) { // Child process
        // Set the FIFO path as an environment variable
        int fd;
        if (setenv("SLOWPOKE_FIFO_PATH", FIFO_PATH, 1) == -1) {
            perror("setenv");
            exit(EXIT_FAILURE);
        }

        if (setenv("SLOWPOKE_FIFO_RECOVER_PATH", FIFO_RECOVER_PATH, 1) == -1) {
            perror("setenv");
            exit(EXIT_FAILURE);
        }

        // Execute the child program with the remaining arguments
        if (setsid() == -1) {
            perror("setsid");
            exit(EXIT_FAILURE);
        }

        execvp(argv[1], &argv[1]);
        perror("execvp"); // If execvp fails
        exit(EXIT_FAILURE);
    } else { // Parent process
        // Create threads
        pthread_t fifo_thread, child_thread;
        pid_t pgid = getpgid(pid);
        if (pthread_create(&fifo_thread, NULL, poker, &pgid) != 0) {
            perror("pthread_create (fifo_thread)");
            exit(EXIT_FAILURE);
        }

        if (pthread_create(&child_thread, NULL, monitor_child, &pid) != 0) {
            perror("pthread_create (child_thread)");
            exit(EXIT_FAILURE);
        }

        // Wait for the child monitoring thread to finish
        pthread_join(child_thread, NULL);

        // Wait for the FIFO monitoring thread to finish
        pthread_join(fifo_thread, NULL);

        // Clean up
        unlink(FIFO_PATH); // Remove the FIFO file
    }

    return 0;
}
