#ifndef INTRINSICS_H
#define INTRINSICS_H

#include <stddef.h>

/*
 * Initialize intrinsics subsystem (loads persistent history).
 * Returns 0 on success, -1 on error.
 */
int intrinsics_init(void);

/*
 * Free intrinsics resources (write history if changed).
 */
void intrinsics_cleanup(void);

/*
 * Record a user-entered shell_cmd into history if it should be recorded
 * according to the rules (max 15, no consecutive duplicates, never store
 * commands containing an atomic 'log' name).
 *
 * This function is intended to be called once per user-entered command
 * (after syntax validation). If you are re-executing a command returned
 * by log execute, DO NOT call this (that re-executed command must not be stored).
 *
 * Returns 1 if added, 0 if skipped (not added), -1 on error.
 */
int intrinsics_record_command(const char *line);

/*
 * Try to handle a command if it is an intrinsic (hop/reveal/log).
 *
 * Parameters:
 *   line           - the original user-entered line (nul-terminated)
 *   out_reexec_cmd - if intrinsics_handle returns 2, *out_reexec_cmd will
 *                    be set to a malloc'd string containing the stored
 *                    command to re-execute (caller must free). Otherwise
 *                    *out_reexec_cmd is left untouched.
 *
 * Return values:
 *   0 => not an intrinsic
 *   1 => intrinsic handled, nothing to re-execute
 *   2 => intrinsic handled and requested re-execution; out_reexec_cmd set
 *  -1 => intrinsic handled but an error occurred (message already printed)
 */
int intrinsics_handle(const char *line, char **out_reexec_cmd);

int handle_hop_args(char **args, size_t nargs);
int handle_reveal_args(char **args, size_t nargs);
int handle_log_args(char **args, size_t nargs, char **out_reexec_cmd);

#endif /* INTRINSICS_H */
