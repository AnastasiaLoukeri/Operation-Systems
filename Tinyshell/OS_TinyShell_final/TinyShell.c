#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>

// Για την μεταγλώττιση: Απαιτούνται οι εξής ενημερώσεις:
// sudo apt update
// sudo apt install libreadline-dev για τα αρχεία κεφαλίδας <readline/readline.h> & <readline/history.h>
// sudo apt install build-essential για το αρχείο κεφαλίδας <sys/wait.h> των POSIX εντολών
// sudo apt install trace
// Το αρχείο μεταγλωττίζεται : gcc -Wall -Wextra -g tinyshell.c -o tinyshell -lreadline


#define MAX_ARG_CAPACITY 50          // Μέγιστος αριθμός ορισμάτων
#define ARG_DELIMITERS " \t\r\n\a"  // Οριοθέτες για τον τεμαχισμό της εισόδου
#define MAX_JOBS 20                 // Μέγιστος αριθμός ταυτόχρονων εργασιών

// Καταστάσεις εργασίας
#define JOB_RUNNING 1
#define JOB_STOPPED 2

// job για τη διαχείριση διεργασιών και ομάδων διεργασιών
typedef struct job {
    int id;              // Job ID
    pid_t pid;           // Process ID της κύριας διεργασίας
    pid_t pgid;          // Process group ID για signal isolation
    int state;           // RUNNING ή STOPPED
    char cmdLine[1024];
} job_t;

job_t jobs[MAX_JOBS];
pid_t shell_pgid;
int shell_terminal;

// job management

void init_jobs() {
    for (int i = 0; i < MAX_JOBS; i++) jobs[i].id = 0;
}

// Προσθήκη νέας εργασίας με ανακύκλωση IDs
int add_job(pid_t pid, pid_t pgid, int state, char *cmd) {
    int candidate_id = 1;

    // Εύρεση του μικρότερου διαθέσιμου Job ID
    while (candidate_id <= MAX_JOBS) {
        int found = 0;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].id == candidate_id) {
                found = 1;
                break;
            }
        }
        if (!found) break; // Το candidate_id είναι ελεύθερο
        candidate_id++;
    }

    if (candidate_id > MAX_JOBS) return -1; // Η λίστα είναι γεμάτη

    // Τοποθέτηση στην πρώτη κενή θέση του πίνακα
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id == 0) {
            jobs[i].id = candidate_id;
            jobs[i].pid = pid;
            jobs[i].pgid = pgid;
            jobs[i].state = state;
            strncpy(jobs[i].cmdLine, cmd, 1023);
            return jobs[i].id;
        }
    }
    return -1;
}

void delete_job(pid_t pid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id != 0 && jobs[i].pid == pid) {
            jobs[i].id = 0;
            return;
        }
    }
}

job_t *get_job_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id != 0 && jobs[i].pid == pid) return &jobs[i];
    }
    return NULL;
}

job_t *get_job_by_jid(int jid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id == jid) return &jobs[i];
    }
    return NULL;
}

// signal handling

// Handler για SIGCHLD: Καθαρισμός zombie-jobs
void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;

    // Χρήση WNOHANG για non-blocking και WUNTRACED για stop
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            delete_job(pid);
        } else if (WIFSTOPPED(status)) {
            job_t *j = get_job_by_pid(pid);
            if (j) j->state = JOB_STOPPED;
        }
    }
}

// Ρύθμιση σημάτων: Το Shell αγνοεί τα σήματα terminal
void setup_signals() {
    struct sigaction sa;
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
}

//

char *getInputLine() {
    char currentDir[1024];
    char promptBuffer[1024 + 30];
    if (getcwd(currentDir, sizeof(currentDir)) == NULL) strcpy(currentDir, "UnknownPath");
    snprintf(promptBuffer, sizeof(promptBuffer), "[%s] TinyShell> ", currentDir);

    char *line = readline(promptBuffer);
    if (line == NULL) return NULL;
    if (*line) add_history(line);
    return line;
}

char **splitInputIntoArguments(char *inputLine) {
    int capacity = MAX_ARG_CAPACITY;
    int currentPosition = 0;
    char **arguments = malloc(capacity * sizeof(char*));
    if (!arguments) exit(EXIT_FAILURE);

    char *inputCopy = strdup(inputLine);
    char *currentArg = strtok(inputCopy, ARG_DELIMITERS);
    while (currentArg != NULL) {
        arguments[currentPosition++] = strdup(currentArg);
        if (currentPosition >= capacity) {
            capacity += MAX_ARG_CAPACITY;
            arguments = realloc(arguments, capacity * sizeof(char*));
        }
        currentArg = strtok(NULL, ARG_DELIMITERS);
    }
    arguments[currentPosition] = NULL;
    free(inputCopy);
    return arguments;
}

char *resolveCommandPath(char *commandName) {
    if (strchr(commandName, '/') != NULL) return (access(commandName, X_OK) == 0) ? strdup(commandName) : NULL;
    char *pathEnv = getenv("PATH");
    if (!pathEnv) return NULL;
    char *pathCopy = strdup(pathEnv);
    char *directory = strtok(pathCopy, ":");
    char fullPathBuffer[1024];
    char *foundPath = NULL;
    while (directory != NULL) {
        snprintf(fullPathBuffer, sizeof(fullPathBuffer), "%s/%s", directory, commandName);
        if (access(fullPathBuffer, X_OK) == 0) {
            foundPath = strdup(fullPathBuffer);
            break;
        }
        directory = strtok(NULL, ":");
    }
    free(pathCopy);
    return foundPath;
}

void handleRedirection(char **args) {
    for (int i = 0; args[i] != NULL; i++) {
        int fd;
        if (strcmp(args[i], ">") == 0) {
            fd = open(args[i+1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(fd, STDOUT_FILENO); close(fd);
            args[i] = NULL;
        } else if (strcmp(args[i], ">>") == 0) {
            fd = open(args[i+1], O_WRONLY | O_CREAT | O_APPEND, 0644);
            dup2(fd, STDOUT_FILENO); close(fd);
            args[i] = NULL;
        } else if (strcmp(args[i], "<") == 0) {
            fd = open(args[i+1], O_RDONLY);
            dup2(fd, STDIN_FILENO); close(fd);
            args[i] = NULL;
        }
    }
}

// Εκτέλεση Pipelines με υποστήριξη Job Control και Process Groups
void executePipedCommands(char *inputLine, int is_bg) {
    char *commands[MAX_ARG_CAPACITY];
    int cmd_count = 0;
    char *line_copy = strdup(inputLine);
    char *token = strtok(line_copy, "|");
    while (token != NULL) {
        commands[cmd_count++] = token;
        token = strtok(NULL, "|");
    }

    int pipefd[2];
    int prev_read = 0;
    pid_t first_pid = 0, last_pid = 0;
    sigset_t mask, prev_mask;

    // Μπλοκάρισμα SIGCHLD κατά την τροποποίηση της λίστας εργασιών
    sigemptyset(&mask); sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &prev_mask);

    for (int i = 0; i < cmd_count; i++) {
        if (i < cmd_count - 1) pipe(pipefd);
        pid_t pid = fork();
        if (pid == 0) {
            setpgid(0, (i == 0) ? 0 : first_pid); // Δημιουργία/ένταξη σε Process Group
            signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL); // Επαναφορά σημάτων στο child proc
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);

            if (prev_read != 0) { dup2(prev_read, STDIN_FILENO); close(prev_read); }
            if (i < cmd_count - 1) { dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]); close(pipefd[0]); }

            char **args = splitInputIntoArguments(commands[i]);
            handleRedirection(args);
            char *execPath = resolveCommandPath(args[0]);
            extern char **environ;
            execve(execPath, args, environ);
            exit(EXIT_FAILURE);
        } else {
            if (i == 0) first_pid = pid;
            setpgid(pid, first_pid);
            if (prev_read != 0) close(prev_read);
            if (i < cmd_count - 1) { close(pipefd[1]); prev_read = pipefd[0]; }
            last_pid = pid;
        }
    }

    if (!is_bg) {
        add_job(last_pid, first_pid, JOB_RUNNING, inputLine);
        tcsetpgrp(shell_terminal, first_pid); // Παραχώρηση temrinal
        int status;
        waitpid(last_pid, &status, WUNTRACED);
        tcsetpgrp(shell_terminal, shell_pgid); // Ανάκτηση terminal

        // Αναφορά status  Pipeline
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) != 0) fprintf(stderr, "[Pipeline Exit Status: %d]\n", WEXITSTATUS(status));
            delete_job(last_pid);
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[Pipeline terminated by signal: %d]\n", WTERMSIG(status));
            delete_job(last_pid);
        } else if (WIFSTOPPED(status)) {
            printf("\n[%d]+ Stopped \t%s\n", get_job_by_pid(last_pid)->id, inputLine);
            get_job_by_pid(last_pid)->state = JOB_STOPPED;
        }
    } else {
        int jid = add_job(last_pid, first_pid, JOB_RUNNING, inputLine);
        printf("[%d] %d\n", jid, last_pid);
    }
    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    free(line_copy);
}

//

// Μεταφορά εργασίας στο foreground
void do_fg(char **argv) {
    if (!argv[1] || argv[1][0] != '%') { printf("Usage: fg %%<job_id>\n"); return; }
    int jid = atoi(argv[1] + 1);
    job_t *job = get_job_by_jid(jid);
    if (!job) { printf("TinyShell: fg: %s: no such job\n", argv[1]); return; }

    job->state = JOB_RUNNING;
    tcsetpgrp(shell_terminal, job->pgid);
    kill(-job->pgid, SIGCONT);

    int status;
    waitpid(job->pid, &status, WUNTRACED);
    tcsetpgrp(shell_terminal, shell_pgid);

    if (WIFSTOPPED(status)) {
        printf("\n[%d]+ Stopped \t%s\n", job->id, job->cmdLine);
        job->state = JOB_STOPPED;
    } else {
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) fprintf(stderr, "[Exit Status: %d]\n", WEXITSTATUS(status));
        delete_job(job->pid);
    }
}

// Συνέχιση εργασίας στο background
void do_bg(char **argv) {
    if (!argv[1] || argv[1][0] != '%') { printf("Usage: bg %%<job_id>\n"); return; }
    int jid = atoi(argv[1] + 1);
    job_t *job = get_job_by_jid(jid);
    if (!job) { printf("TinyShell: bg: %s: no such job\n", argv[1]); return; }

    job->state = JOB_RUNNING;
    kill(-job->pgid, SIGCONT);
    printf("[%d]+ %s &\n", job->id, job->cmdLine);
}

// Τερματισμός ομάδας διεργασιών μέσω του Job ID
void do_kill(char **argv) {
    if (!argv[1] || argv[1][0] != '%') { printf("Usage: kill %%<job_id>\n"); return; }
    int jid = atoi(argv[1] + 1);
    job_t *job = get_job_by_jid(jid);
    if (!job) { printf("TinyShell: kill: %s: no such job\n", argv[1]); return; }
    if (kill(-job->pgid, SIGTERM) < 0) perror("TinyShell: kill failed");
    else printf("Job [%d] (%d) terminated\n", job->id, job->pid);
}

// main shell
void shellMainLoop() {
    char *inputLine;
    extern char **environ;
    while (1) {
        inputLine = getInputLine();
        if (!inputLine) { printf("\nTerminating TinyShell...\n"); break; }
        if (strlen(inputLine) == 0) { free(inputLine); continue; }

        int is_bg = 0; // [cite: 8]
        if (inputLine[strlen(inputLine)-1] == '&') { is_bg = 1; inputLine[strlen(inputLine)-1] = '\0'; }

        if (strchr(inputLine, '|') != NULL) {
            executePipedCommands(inputLine, is_bg);
            free(inputLine); continue;
        }

        char **commandArgs = splitInputIntoArguments(inputLine);
        if (!commandArgs[0]) { free(inputLine); free(commandArgs); continue; }

        if (strcmp(commandArgs[0], "exit") == 0) exit(0);
        else if (strcmp(commandArgs[0], "cd") == 0) {
            char *dir = (commandArgs[1] == NULL || strcmp(commandArgs[1], "~") == 0) ? getenv("HOME") : commandArgs[1];
            if (chdir(dir) != 0) perror("TinyShell: cd failed");
        } else if (strcmp(commandArgs[0], "jobs") == 0) {
            for(int j=0; j<MAX_JOBS; j++)
                if(jobs[j].id != 0) printf("[%d] %s \t%s\n", jobs[j].id, (jobs[j].state==JOB_RUNNING)?"Running":"Stopped", jobs[j].cmdLine);
        } else if (strcmp(commandArgs[0], "fg") == 0) do_fg(commandArgs);
          else if (strcmp(commandArgs[0], "bg") == 0) do_bg(commandArgs);
          else if (strcmp(commandArgs[0], "kill") == 0) do_kill(commandArgs);
        else {
            char *execPath = resolveCommandPath(commandArgs[0]);
            if (execPath) {
                sigset_t mask, prev_mask;
                sigemptyset(&mask); sigaddset(&mask, SIGCHLD);
                sigprocmask(SIG_BLOCK, &mask, &prev_mask);

                pid_t pid = fork();
                if (pid == 0) {
                    setpgid(0, 0);
                    signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
                    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                    handleRedirection(commandArgs);
                    execve(execPath, commandArgs, environ);
                    exit(1);
                } else {
                    setpgid(pid, pid);
                    if (!is_bg) {
                        add_job(pid, pid, JOB_RUNNING, inputLine);
                        tcsetpgrp(shell_terminal, pid);
                        int status;
                        waitpid(pid, &status, WUNTRACED);
                        tcsetpgrp(shell_terminal, shell_pgid);

                        if (WIFEXITED(status)) {
                            if (WEXITSTATUS(status) != 0) fprintf(stderr, "[Exit Status: %d]\n", WEXITSTATUS(status));
                            delete_job(pid);
                        } else if (WIFSIGNALED(status)) {
                            fprintf(stderr, "[Terminated by signal: %d (%s)]\n", WTERMSIG(status), strsignal(WTERMSIG(status)));
                            delete_job(pid);
                        } else if (WIFSTOPPED(status)) {
                            printf("\n[%d]+ Stopped \t%s\n", get_job_by_pid(pid)->id, inputLine);
                            get_job_by_pid(pid)->state = JOB_STOPPED;
                        }
                    } else {
                        int jid = add_job(pid, pid, JOB_RUNNING, inputLine);
                        printf("[%d] %d\n", jid, pid);
                    }
                    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                }
                free(execPath);
            } else fprintf(stderr, "TinyShell: Command not found: %s\n", commandArgs[0]);
        }
        for(int k=0; commandArgs[k]; k++) { free(commandArgs[k]); }
        free(commandArgs); free(inputLine);
    }
}

int main(void) {
    setup_signals();
    init_jobs();
    shell_terminal = STDIN_FILENO;
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EPERM) perror("setpgid failed");
    tcsetpgrp(shell_terminal, shell_pgid);
    shellMainLoop();
    return EXIT_SUCCESS;
}
