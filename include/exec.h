#ifndef EXEC_H
#define EXEC_H

#include <sys/types.h>

int exec_run_line(const char *line);
// Background job structure
typedef struct bg_job {
    pid_t pid;
    int job_id;
    char *command;
    int stopped; /* 1 if stopped, 0 if running */
    struct bg_job *next;
} bg_job;

extern bg_job *job_list;

/* Function declarations */
void execute_command(char *command);
void init_job_list(void);
void add_background_job(pid_t pid, const char *cmd);
void add_stopped_job(pid_t pid, const char *cmd);
void check_background_jobs(void);
void execute_sequential_commands(char **commands, int count);
void execute_background_command(char *command);

/* Ctrl-D helpers */
void kill_all_children(void);
void handle_eof_exit(void);

/* Activities */
void print_activities(void);

#endif
