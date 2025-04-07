karin.lein, login2
Karine Lein (322644733),  Ronen Levy (319130241)
EX: 1

FILES:
myfile.c -- a file with some code
myfile.h -- a file with some headers


### ANSWERS:

#### Assignment 1
The program expects a single argument and is executed with it using the execve() system call. At the beginning of its
execution, the program sets up its memory environment.It uses the brk(NULL) call to query the current end of the heap,
and mmap() to allocate 8192 bytes of anonymous memory for internal use. Then, it attempts to access /etc/ld.so.preload 
(which does not exist), and proceeds to load the dynamic linker cache via openat() to find shared library paths. The
program uses newfstatat() to get metadata about the opened cache file, maps it into memory with mmap() for reading, and
then closes the file. SNext, the program opens the standard C library (libc.so.6) using openat(), reads ELF headers with
read() and pread64(), and uses multiple mmap() calls to map its different segments (read-only, executable, writable) 
into memory. After the library is mapped, the file descriptor is closed.

Following library loading, the program performs thread and memory initialization. It allocates memory for thread-local
storage using mmap(), sets up the thread-local segment with arch_prctl(), and configures thread identification using 
set_tid_address(), set_robust_list(), and rseq() (for restartable sequences). It also uses mprotect() to update memory
protections and prlimit64() to query stack size limits. Temporary memory used for the cache is then unmapped using
munmap().

Once initialization is complete, the program creates a directory named Welcome using the mkdir system call.  Then, it 
adjusts the program heap, requesting more space. Inside this directory, it creates three files: Welcome, To, and OS-2024
with write permissions. It then checks the file status (if it’s ready) and writes specific messages into each file. The 
first file, Welcome, contains the string "karin.lein\nIf you haven't read t...". The second file, To, is written with 
"Start exercises early!". The third file, OS-2024, includes "Good luck!".
After writing, the program cleans up by removing the three files using unlink(), and then deletes the Welcome directory 
itself with rmdir(). It concludes its execution cleanly by calling exit_group(0), signaling a successful exit.

#### Assignment 2