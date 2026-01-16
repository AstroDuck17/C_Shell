# C_Shell - A Custom Unix Shell Implementation

A feature-rich Unix shell implementation written in C that provides standard shell functionality along with custom built-in commands, job control, I/O redirection, and command history management.

## 📋 Table of Contents

- [Features](#features)
- [Architecture](#architecture)
- [Building and Running](#building-and-running)
- [Built-in Commands](#built-in-commands)
- [Features in Detail](#features-in-detail)
- [Implementation Details](#implementation-details)
- [File Structure](#file-structure)
- [Technical Specifications](#technical-specifications)

## ✨ Features

### Core Shell Capabilities
- **Custom Prompt**: Displays `<username@hostname:current_directory>` format with tilde expansion for home directory
- **Command Execution**: Execute system commands with full path resolution
- **Command Chaining**: Support for sequential command execution using `;`
- **Background Processes**: Execute commands in background using `&`
- **Piping**: Chain commands using `|` for pipeline execution
- **I/O Redirection**: 
  - Input redirection with `<`
  - Output redirection with `>` (truncate)
  - Append output with `>>`

### Built-in Commands
1. **hop** - Enhanced directory navigation
2. **reveal** - Advanced directory listing
3. **log** - Command history management
4. **activities** - Process activity monitoring
5. **ping** - Send signals to processes
6. **fg/bg** - Job control commands

### Advanced Features
- **Signal Handling**: Proper handling of `Ctrl+C`, `Ctrl+Z`, and `Ctrl+D`
- **Job Control**: Full background/foreground job management
- **Command History**: Persistent history with smart duplicate handling
- **Process Management**: Track and manage spawned processes
- **Non-canonical Input**: Character-by-character input processing with immediate Ctrl+D detection

## 🏗️ Architecture

The shell is organized into modular components:

```
C_Shell/
├── src/
│   ├── main.c          # Main shell loop and input handling
│   ├── prompt.c        # Prompt generation and display
│   ├── parser.c        # Command syntax validation
│   ├── intrinsics.c    # Built-in command implementations
│   └── exec.c          # Command execution and job control
├── include/
│   ├── prompt.h
│   ├── parser.h
│   ├── intrinsics.h
│   └── exec.h
└── Makefile
```

## 🚀 Building and Running

### Prerequisites
- GCC compiler
- POSIX-compliant Unix/Linux system
- Standard C library

### Build
```bash
make
```

### Run
```bash
./shell.out
```

### Clean
```bash
make clean
```

## 📖 Built-in Commands

### 1. hop - Directory Navigation

Navigate through directories with enhanced features.

**Syntax:**
```bash
hop [path1] [path2] ... [pathN]
```

**Special Arguments:**
- `~` - Navigate to home directory
- `.` - Current directory (no change)
- `..` - Parent directory
- `-` - Previous directory (toggles between current and previous)

**Examples:**
```bash
hop ~              # Go to home directory
hop ..             # Go to parent directory
hop -              # Go to previous directory
hop dir1 dir2      # Navigate through dir1, then dir2
```

**Features:**
- Sequential navigation through multiple directories
- Maintains previous directory for `-` argument
- Prints "No such directory!" for invalid paths

---

### 2. reveal - Directory Listing

List directory contents with sorting and filtering options.

**Syntax:**
```bash
reveal [flags] [path]
```

**Flags:**
- `-a` - Show hidden files (files starting with `.`)
- `-l` - Line-by-line output (one entry per line)
- Flags can be combined: `-al` or `-la`

**Path Arguments:**
- `~` - Home directory
- `.` - Current directory
- `..` - Parent directory
- `-` - Previous directory
- Any valid absolute/relative path

**Examples:**
```bash
reveal              # List current directory
reveal -a           # Show hidden files
reveal -l           # Line-by-line format
reveal -al ~        # Show all files in home, line-by-line
reveal ../projects  # List specific directory
```

**Features:**
- Alphabetically sorted output (lexicographic order)
- Excludes `.` and `..` from output
- Space-separated default output, line-by-line with `-l`

---

### 3. log - Command History

Manage and replay command history with persistence.

**Syntax:**
```bash
log                    # Display all stored commands
log purge              # Clear all history
log execute <index>    # Re-execute a command from history
```

**Examples:**
```bash
log                    # Show history (oldest to newest)
log execute 3          # Execute the 3rd most recent command
log execute 1 | grep x # Execute command and pipe output
log purge              # Clear all history
```

**Features:**
- Stores up to 15 most recent commands
- Persistent across shell sessions (saved in `~/.osh_history`)
- Automatic duplicate removal (consecutive duplicates skipped)
- Commands containing `log` are never stored
- Index is 1-based, where 1 = most recent command
- Can chain `log execute` output with pipes and redirections

**Special Behavior:**
- Re-executed commands are NOT added to history again
- History is immediately persisted after each command
- Duplicate commands are moved to the most recent position

---

### 4. activities - Process Monitoring

Display all processes spawned by the shell that are currently running or stopped.

**Syntax:**
```bash
activities
```

**Output Format:**
```
[pid] : command_name - State
```

**Example Output:**
```
[1234] : sleep 100 - Running
[1235] : vim file.txt - Stopped
[1240] : find / -name test - Running
```

**Features:**
- Shows only shell-spawned processes
- Lists both running and stopped jobs
- Sorted alphabetically by command name
- Automatically removes terminated processes from display

---

### 5. ping - Signal Sending

Send signals to processes by PID.

**Syntax:**
```bash
ping <pid> <signal_number>
```

**Examples:**
```bash
ping 1234 9      # Send SIGKILL to process 1234
ping 5678 15     # Send SIGTERM to process 5678
ping 9999 19     # Send SIGSTOP to process 9999
```

**Features:**
- Signal number modulo 32 mapping (e.g., 33 → 1)
- Prints confirmation: "Sent signal X to process with pid Y"
- Error handling: "No such process found" for invalid PIDs

---

### 6. fg / bg - Job Control

Bring background jobs to foreground or resume stopped jobs.

**fg - Foreground**

**Syntax:**
```bash
fg [job_number]
```

**Behavior:**
- Without argument: brings most recent job to foreground
- With argument: brings specified job to foreground
- Resumes stopped jobs automatically (sends SIGCONT)
- Waits for job to complete or stop
- If job stops (Ctrl+Z), moves it back to background

**bg - Background**

**Syntax:**
```bash
bg [job_number]
```

**Behavior:**
- Without argument: resumes most recent stopped job
- With argument: resumes specified stopped job
- Prints: `[job_id] command &`
- Error if job is already running

**Examples:**
```bash
fg              # Bring most recent job to foreground
fg 2            # Bring job #2 to foreground
bg              # Resume most recent stopped job in background
bg 3            # Resume job #3 in background
```

## 🔧 Features in Detail

### Command Syntax and Grammar

The shell validates all commands according to a strict grammar:

```
<command_group> ::= <atomic> [ '|' <atomic> ]* [ '&' ]
<line>          ::= <command_group> [ ';' <command_group> ]*
<atomic>        ::= <name> [ <arg> | '<' <name> | '>' <name> | '>>' <name> ]*
```

**Valid Examples:**
```bash
ls -la
cat file.txt | grep error | wc -l
ls > output.txt ; cat output.txt
sleep 100 &
cat < input.txt > output.txt
ls | grep txt ; cat file.txt &
```

**Invalid Examples:**
```bash
| ls           # Pipe at start
ls |           # Pipe at end
ls > ; cat     # Missing filename
& ls           # Background at start
```

### I/O Redirection

**Input Redirection (`<`):**
```bash
sort < unsorted.txt         # Read from file
cat < input.txt | grep x    # Redirect input in pipeline
```

**Output Redirection (`>`):**
```bash
ls > files.txt              # Truncate and write
echo "Hello" > output.txt   # Overwrite file
```

**Append Output (`>>`):**
```bash
echo "Line 1" >> log.txt    # Append to file
date >> log.txt             # Append more data
```

**Combined:**
```bash
sort < input.txt > sorted.txt
cat < file1.txt >> file2.txt
```

### Piping

Chain multiple commands where output of one becomes input of the next:

```bash
ls -l | grep ".txt" | wc -l              # Count txt files
ps aux | grep chrome | awk '{print $2}'  # Get Chrome PIDs
cat file.txt | sort | uniq | wc -l       # Count unique sorted lines
```

**Pipeline Features:**
- Unlimited pipeline depth
- Each command runs in separate process
- Proper pipe buffer management
- Error propagation through pipeline

### Background Processes

Execute long-running commands without blocking the shell:

```bash
sleep 100 &                              # Run in background
find / -name "*.txt" > results.txt &     # Search in background
./long_script.sh &                       # Script in background
```

**Background Process Features:**
- Prints `[job_id] pid` when started
- Tracked by shell for status monitoring
- STDIN redirected to `/dev/null`
- Can be brought to foreground with `fg`
- Notified when completed: `command with pid X exited normally/abnormally`

### Signal Handling

**Ctrl+C (SIGINT):**
- Shell ignores SIGINT
- Foreground process receives SIGINT
- Shell continues running

**Ctrl+Z (SIGTSTP):**
- Foreground process stops
- Process moved to background job list as "Stopped"
- Job can be resumed with `fg` or `bg`

**Ctrl+D (EOF):**
- Immediately detected in non-canonical mode
- Kills all background jobs
- Prints "logout"
- Exits shell cleanly

### Job Control

**Job Lifecycle:**
1. Process starts in foreground or background
2. If background: tracked in job list with job_id
3. If stopped (Ctrl+Z): added to job list as "Stopped"
4. Can be resumed with `fg` (foreground) or `bg` (background)
5. Removed from job list when terminated

**Job List Management:**
- Each job has unique job_id (sequential)
- Jobs tracked by PID and command
- Status: Running or Stopped
- Automatic cleanup of terminated jobs

### Command History

**Persistence:**
- Saved in `~/.osh_history` file
- Loaded on shell startup
- Saved after each command

**History Rules:**
1. Maximum 15 commands stored
2. Oldest commands dropped when limit reached
3. Consecutive duplicate commands not stored
4. Commands containing atomic `log` never stored
5. Re-executed commands (from `log execute`) not stored again
6. Duplicate commands moved to most recent position

**Storage Format:**
- Plain text, one command per line
- Most recent command at end of file

### Process Groups

- Each background process runs in its own process group
- Foreground pipelines run in a single process group
- Allows proper signal delivery to entire job
- Shell remains in its own process group

## 💻 Implementation Details

### Non-Canonical Input Mode

The shell operates in non-canonical terminal mode for immediate character processing:

- Characters processed immediately without waiting for newline
- Ctrl+D detected instantly (before any buffering)
- Backspace handled manually with visual feedback
- Echo manually controlled for accurate display

**Benefits:**
- Immediate Ctrl+D detection even during foreground processes
- Better user experience with instant feedback
- Proper EOF handling without line buffering delays

### Process Execution Flow

1. **Input Reading**: Non-canonical mode with immediate character processing
2. **Syntax Validation**: Grammar-based parsing before execution
3. **History Recording**: Store command if it meets criteria
4. **Intrinsic Check**: Determine if built-in or external command
5. **Execution**:
   - Built-ins: Execute in shell process
   - External: Fork, exec, and wait/track
6. **Job Management**: Update background job statuses
7. **Prompt Display**: Show updated prompt for next command

### Memory Management

- All dynamically allocated strings freed appropriately
- Token arrays cleaned up after parsing
- Job list entries freed when jobs terminate
- History persistence ensures no data loss

### Error Handling

**Graceful Degradation:**
- Invalid syntax: Print "Invalid Syntax!" and continue
- Missing files: Print "No such file or directory"
- Invalid directories: Print "No such directory!"
- Invalid commands: Print "Command not found!"
- Process errors: Print "No such process found"

**No Shell Crashes:**
- All system call failures handled
- Malloc failures checked and handled
- Signal handling prevents unexpected termination

## 📁 File Structure

### Source Files

**main.c**
- Main shell loop
- Non-canonical terminal mode setup
- Input reading with backspace and Ctrl+D handling
- Command re-execution logic
- Signal handler registration
- Terminal restoration on exit

**prompt.c**
- Prompt initialization: capture home directory, username, hostname
- Prompt display with path resolution
- Tilde expansion for paths under home directory
- PWD environment variable management

**parser.c**
- Lexical analysis: tokenization into grammar tokens
- Syntax validation: recursive descent parser
- Grammar enforcement: validates command structure
- Token utility functions

**intrinsics.c**
- Built-in command dispatch
- `hop`: Directory navigation with history
- `reveal`: Directory listing with flags
- `log`: History management (display, purge, execute)
- History persistence: load/save from `~/.osh_history`
- Duplicate detection and removal

**exec.c**
- External command execution with fork/exec
- Pipeline creation: pipe setup and management
- I/O redirection: file descriptor manipulation
- Background job tracking: job list management
- Signal handlers: SIGINT, SIGTSTP
- `activities`: Process status display
- `ping`: Signal sending
- `fg/bg`: Job control implementation
- Process group management
- Foreground process tracking for signals

### Header Files

All headers follow consistent structure:
- Include guards
- Function declarations
- Type definitions (where applicable)
- External variable declarations

## 🔬 Technical Specifications

### Compilation Flags

```makefile
-std=c99                  # C99 standard
-D_POSIX_C_SOURCE=200809L # POSIX.1-2008 features
-D_XOPEN_SOURCE=700       # X/Open 7 features
-Wall -Wextra -Werror     # Strict warnings
-Wno-unused-parameter     # Allow unused parameters
-fno-asm                  # No inline assembly
```

### System Calls Used

- **Process Control**: `fork`, `execvp`, `wait`, `waitpid`, `setpgid`, `getpid`
- **Signals**: `signal`, `sigaction`, `kill`
- **File Operations**: `open`, `close`, `read`, `write`, `dup2`
- **Directory**: `getcwd`, `chdir`, `opendir`, `readdir`, `closedir`
- **Terminal**: `tcgetattr`, `tcsetattr`
- **I/O Multiplexing**: `poll`
- **Other**: `pipe`, `stat`, `getenv`, `setenv`

### Data Structures

**bg_job (Background Job)**
```c
typedef struct bg_job {
    pid_t pid;              // Process ID
    int job_id;             // Shell-assigned job number
    char *command;          // Command string
    int stopped;            // 1 if stopped, 0 if running
    struct bg_job *next;    // Linked list next pointer
} bg_job;
```

**CmdNode (Pipeline Stage)**
```c
typedef struct {
    char **argv;     // NULL-terminated argument vector
    char *infile;    // Input redirection file (or NULL)
    char *outfile;   // Output redirection file (or NULL)
    int append;      // 1 for >>, 0 for >
} CmdNode;
```

### Limits and Constants

- **History Size**: 15 commands maximum
- **Path Length**: `PATH_MAX` (typically 4096 bytes)
- **Initial Token Buffer**: 16 tokens (grows dynamically)
- **Host Name Length**: System `_SC_HOST_NAME_MAX`

### Thread Safety

The shell is single-threaded but uses:
- `volatile sig_atomic_t` for signal handler variables
- Async-signal-safe functions in signal handlers
- No shared state between parent and child processes

## 🎯 Special Behaviors

### Tilde Expansion

Paths are displayed with `~` when under home directory:
```
/home/user          → ~
/home/user/docs     → ~/docs
/home/other/file    → /home/other/file
```

### Previous Directory Tracking

- Updated only after successful `chdir` operations
- Available for `hop -`, `reveal -`
- Initially unset; error if used before any directory change

### History Uniqueness

When a command is entered that already exists in history:
1. Previous occurrence is removed
2. Command is added as most recent
3. Result: no duplicates, most recent position maintained

### Background Process Management

**Process Group Isolation:**
- Each background job in separate process group
- Prevents terminal signals from affecting background jobs
- Allows independent job control

**Status Reporting:**
- Check jobs before each prompt
- Non-blocking status checks (`WNOHANG`)
- Print completion messages asynchronously

### Ctrl+D During Foreground Process

Thanks to non-canonical mode and polling:
1. Shell polls stdin while waiting for foreground process
2. EOF/Ctrl+D detected immediately
3. Calls `handle_eof_exit()` which kills all jobs
4. Prints "logout" and exits

This ensures clean shutdown even if a foreground process is running.

## 🐛 Error Messages

| Error | Message |
|-------|---------|
| Invalid command syntax | `Invalid Syntax!` |
| Directory not found | `No such directory!` |
| File not found for redirection | `No such file or directory` |
| Cannot create output file | `Unable to create file for writing` |
| Command not in PATH | `Command not found!` |
| Invalid process ID for ping | `No such process found` |
| Invalid job number for fg/bg | `No such job` |
| Job already running for bg | `Job already running` |

## 🔄 Example Session

```bash
<user@hostname:~> hop ~/projects
<user@hostname:~/projects> reveal -la
.git
.gitignore
README.md
src
tests
<user@hostname:~/projects> cat README.md | grep "TODO" > todos.txt &
[1] 12345
<user@hostname:~/projects> activities
[12345] : cat README.md - Running
<user@hostname:~/projects> sleep 30 &
[2] 12346
[2] 12346
<user@hostname:~/projects> fg 1
cat README.md
^Z
[3] Stopped cat README.md
<user@hostname:~/projects> bg 3
[3] cat README.md &
<user@hostname:~/projects> log
hop ~/projects
reveal -la
cat README.md | grep "TODO" > todos.txt &
sleep 30 &
fg 1
bg 3
<user@hostname:~/projects> log execute 2
.git
.gitignore
README.md
src
tests
<user@hostname:~/projects> ping 12346 9
Sent signal 9 to process with pid 12346

sleep 30 with pid 12346 exited abnormally

<user@hostname:~/projects> ^D
logout
```

## 📝 Notes

- The shell maintains POSIX compliance for maximum portability
- All commands are thoroughly validated before execution
- Memory leaks are prevented through careful resource management
- The implementation prioritizes robustness and user experience
- Signal handling ensures the shell remains responsive under all conditions

## 🤝 Contributing

When modifying the shell:
1. Maintain the existing code structure and style
2. Test all features thoroughly, especially edge cases
3. Update this README if adding new features
4. Ensure compilation with all warning flags enabled
5. Verify signal handling and job control behavior
