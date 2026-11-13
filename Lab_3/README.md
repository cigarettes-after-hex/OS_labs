
# Operating Systems - Lab 3

### Variant 3

## Compiling

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic child.c -o child -pthread -lrt
gcc -std=c11 -Wall -Wextra -Werror -pedantic parent.c -o parent -pthread -lrt
```

## Running

```bash
./parent
```

## Example

```text
result.txt
100 2 5
10
20 2 2
5
7 2
3
-100 2 -5
10
```

Press `Ctrl + D` to finish input.

### Check the results

```bash
cat result.txt
```

Expected output:

```text
10
5
3
10
```

## Division by Zero

Example:

```text
zero.txt
18 3
6
8 0
Division by zero
```

The child process detects division by zero and terminates with an error. The parent process also terminates.

## Important

- `parent.c` and `child.c` must be in the same directory.
- The compiled executables `parent` and `child` must also be in the same directory.
- POSIX shared memory (`shm_open`, `mmap`) and named semaphores are used for interprocess communication.
- Division by zero terminates both processes.
