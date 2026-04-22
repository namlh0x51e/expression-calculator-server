# Introduction

Expression calculator server

# Building

## Installing dependencies
Dependencies:
- Standalone asio
- Spdlog
- Catch2
### Using your package manager
```sh
sudo apt install libasio-dev libspdlog-dev catch2 
```
### Using fetch content
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFORCE_FETCH_CONTENT=ON
```

## Building
```sh
cmake --build build --parallel
```

# Install
```sh
cmake --install build --prefix /usr/local
```

# Testing

## Run tests
```sh
ctest --test-dir build --output-on-failure
```

## Run a single test binary directly
```sh
./build/tests/test_tokenizer
./build/tests/test_evaluation_engine
```

# Expression calculation server

## Usage
```sh
./expression-calculator-server -h

Usage: ./build/src/expression-calculator-server [options]
  --host HOST  Server hostname/IP.
  --port PORT  Server port.
  --threads N  Number of IO threads (default to number of cpus).
  --help N     Print this message and exit.
```

## Running an example
```sh
printf "(2+3)*4\n1+1\n" | netcat localhost 8000
20
2
```

# Expression generator tool

## Usage
```sh
./expression-gen -h

Usage: build/tools/expression-gen [options]

  --size N[K|M|G]  Target output size (default: 1G).
  --max-depth N    Max parenthesisation depth (default: 6, range: 1-64).
  --seed N         PRNG seed (default: 42).
  --host HOST      Server hostname/IP (enables TCP client mode).
  --port PORT      Server port (required with --host).
  --connections N  Concurrent TCP connections in TCP mode (default: 1).
  --help           Print this message and exit.
```

## Generate expression to stdout
```sh
./expression-gen --seed $RANDOM --size 1K
```

## Create tcp client and generating expression against a server
```sh
./expression-gen --seed $RANDOM --host locahost --port 8000

--- TCP client summary ---
  connections   : 1
  elapsed       : 48.606 s
  exprs sent    : 520041
  bytes sent    : 1073742441 (1024.00 MB)
  responses rcvd: 528494
  throughput    : 10699 exprs/s, 21.07 MB/s
  errors        : 0
```

# Technical design
## Tcp server
- Shared nothing architecture: each connection is handled only on 1 thread over its lifetime and no synchronization between threads is involed. This architecture can achieved the highest parallelization and is also easy to write as the mental overhead of thread safe code is no more.
- Thread pinning (affinity): this is for reducing the chance of cache missed
- Zero copy: no buffer copying is involed in this implementation, this helps minimizing cpu cycles waste.
- Minimal allocation: I try to minimize the use of heap allocation as much as possible
- Expression evaluation algorithm: I use a modified version of Shunting-Yard algorithm.

### Limitations
- The server only supports operations on 64bits integer
- No unary operation supported
- Subject to failure during high load: currently each connection hold a buffer of 1Mbs. This buffer size is not optimal for all workload type so tunning must be done. Also if the number of concurrent connections is high, it could caused an OOM kill. To mitigate this a thread local buffer pool could be used so that the server is stable under high load.
- Thread pinning: this is a double edged sword. Works best on high workload, other than that I would consider not using it.
- No control over work distribution: I use Linux socket option `so_reuseport` to delegate the connection load balacing to the OS. It may cause uneven works distribution and thread starvation. Solution for this might be sophisticated EBpf load balacing or Work-stealing scheduler.

## Expression generator
- I use recursive descent technique to randomly generate the expression, this is a common technique for writing parser.
- Due to that fact that the server only support int64, the output must be controlled so that it doesn't overflow.
### Grammar
```txt
expr -> term ('+' | '-') term
      | term
term -> factor ('*' | '/') factor
      | factor
factor -> number
        | '(' expr ')'
number -> <64bit integer>
```

