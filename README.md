# Introduction

A tcp server that supports calculating mathematical expression

# Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

# Install
```sh
cmake --build build -t install
```

# Testing

```sh
cmake --build build -t test
```

# Technical design
