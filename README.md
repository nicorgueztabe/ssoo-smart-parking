# Smart parking lot: a multithreaded simulation in C

A simulation of a parking lot with 4 entry gates (3 priority lanes each) and 2 exit gates, written with POSIX threads, mutexes and condition variables, for an Operating Systems course. It is a producer-consumer design: a manager thread reads the cars from a CSV file and feeds 12 circular queues, and the lane and exit threads consume them.

## Build and run

Linux only.

```
gcc -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2 -pthread -o parking parking_manager.c queue.c
./parking <csv file> <queue size> <parking capacity>
```

The CSV file has a header line, which is ignored, and one car per line: `Number Plate,Gate,Priority`.

- The plate is four digits, a dash and three capital letters (`1111-AAA`).
- The gate is 0 to 3 and the priority is 0 to 2. Priority 0 is the highest (zero-emission cars), then residents (1) and visitors (2).
- Lines that do not follow this are skipped.

## How it works

- **`parking_manager`** (`main`) creates the shared `Lot`, the 12 entry queues and the 2 exit queues, and starts the 12 entry threads and the 2 exit threads. It reads the CSV with `open`, `read` and `close`, releases the entry threads, puts each car in the queue of its gate and priority, marks the queues as finished and joins every thread.
- **`entry_lane_thread`** takes the next car from its queue and calls `lot_claim_space`, which blocks while the lot is full or while a car with a higher priority is waiting. Then it puts the car in the exit queue of gate `entry gate % 2`.
- **`exit_gate_thread`** takes a car from its queue and calls `lot_release_space`, which frees a space and wakes the waiting lanes.
- **`queue.c`** is a circular buffer protected by a mutex and two condition variables (not full and not empty), plus a flag that lets the consumers stop once the producer is done.
- **`Lot`** keeps the occupied spaces and one condition variable per priority. A car waits while the lot is full or any car of a higher priority is waiting.

The code was checked with ThreadSanitizer and AddressSanitizer. Error messages are in Spanish.

## Example

`cars.csv`, where the line with `zzzz-XXX` is not a valid plate and is skipped:

```
Number Plate,Gate,Priority
1111-AAA,0,2
2222-BBB,1,0
3333-CCC,2,1
4444-DDD,3,2
zzzz-XXX,1,1
5555-EEE,0,0
```

A lot with 2 spaces and queues of size 2. The command hides the `Waiting` and `Ending` lines of the threads, and the order between threads can change from run to run:

```
$ ./parking cars.csv 2 2 | grep -v -e Waiting -e Ending
[LANE 2][GATE 0] Car 1111-AAA
[INFO] LOT CLAIMED 1/2 BY 1111-AAA
[LANE 0][GATE 1] Car 2222-BBB
[INFO] LOT CLAIMED 2/2 BY 2222-BBB
[LANE 1][GATE 2] Car 3333-CCC
[LANE 2][GATE 3] Car 4444-DDD
[EXIT 0] CAR 1111-AAA
[INFO] LOT RELEASED 1/2 BY 1111-AAA
[EXIT 1] CAR 2222-BBB
[INFO] LOT RELEASED 0/2 BY 2222-BBB
[LANE 0][GATE 0] Car 5555-EEE
[INFO] LOT CLAIMED 1/2 BY 5555-EEE
[EXIT 0] CAR 5555-EEE
[INFO] LOT RELEASED 0/2 BY 5555-EEE
[INFO] LOT CLAIMED 1/2 BY 3333-CCC
[INFO] LOT CLAIMED 2/2 BY 4444-DDD
[EXIT 0] CAR 3333-CCC
[INFO] LOT RELEASED 1/2 BY 3333-CCC
[EXIT 1] CAR 4444-DDD
[INFO] LOT RELEASED 0/2 BY 4444-DDD
[OK][parking_manager] Finishing
```

In this run `3333-CCC` and `4444-DDD` are already waiting for a space when `5555-EEE`, a priority 0 car, arrives, and it still takes the next free space. The lot never goes over 2/2.

Errors go to stderr and the program exits with -1:

```
$ ./parking cars.csv 0 2
ERROR: El tamaño de la cola debe ser un número mayor que 0
$ ./parking missing.csv 2 2
ERROR: fallo al abrir el archivo CSV: No such file or directory
```
