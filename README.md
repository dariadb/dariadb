# dariadb - numeric time-series database.
# Continuous Integration
<a href="https://scan.coverity.com/projects/dariadb">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/10983/badge.svg"/>
</a>

|  version | build & tests | test coverage |
|---------------------|---------|----------|
| `master`   | [![Build Status](https://travis-ci.org/lysevi/dariadb.svg?branch=master)](https://travis-ci.org/lysevi/dariadb) |  [![Coverage Status](https://coveralls.io/repos/github/lysevi/dariadb/badge.svg?branch=master)](https://coveralls.io/github/lysevi/dariadb?branch=master) |
| `develop` | [![Build Status](https://travis-ci.org/lysevi/dariadb.svg?branch=dev)](https://travis-ci.org/lysevi/dariadb) | [![Coverage Status](https://coveralls.io/repos/github/lysevi/dariadb/badge.svg?branch=dev)](https://coveralls.io/github/lysevi/dariadb?branch=dev)
 
# Features
* Can be used as a server application or an embedded library.
* Accept unordered data.
* Each measurement contains:
  - Id - x32 unsigned integer value.
  - Time - x64 timestamp.
  - Value - x64 float.
  - Flag - x32 unsigned integer.
* Write strategies:
  - wal - little cache and all values storing to disk in write ahead log. optimised for big write load(but slower than 'memory' strategy).
  - compressed - all values compressed for good disk usage without writing to sorted layer.
  - memory - all values stored in memory and dropped to disk when memory limit is ended.
  - cache - all values stored in memory with async writes to disk.
* LSM-like storage struct with three layers:
  - Memory cache or Append-only files layer, for fast write speed and crash-safety(if strategy is 'fast write').
  - Old values stored in compressed block for better disk space usage.
* High write speed:
  -  as embedded engine - to disk - 2.5 - 3.5 millions values per second to disk
  -  as memory storage(when strategy is 'memory') - 7-9 millions.
  -  across the network - 700k - 800k values per second
* Crash recovery.
* CRC32 for all values.
* Two variants of API:
  - Functor API (async) -  engine apply given function to each measurement in the incoming request.
  - Standard API - You can Query interval as list or values in time point as dictionary.
* Compaction old data with filtration support:
  - in engine api.
  - in network protocol.
* By step storage: for values with predefined write interval (per millisecond, second, minute, hour).

# Dependencies
* Boost 1.54.0 or higher: system, filesystem, date_time, unit_test_framework(to build tests), program_options, asio and regex(for server only)
* cmake 3.1 or higher
* c++ 14/17 compiler (MSVC 2015, gcc 6.0, clang 3.8)

##Build
### Install dependencies
---
```shell
$ sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
$ sudo apt-get update
$ sudo apt-get install -y libboost-dev  libboost-coroutine-dev libboost-context-dev libboost-filesystem-dev libboost-test-dev libboost-program-options-dev libasio-dev libboost-log-dev libboost-regex-dev libboost-date-time-dev cmake  g++-6  gcc-6 cpp-6
$ export CC="gcc-6"
$ export CXX="g++-6"
```
### Git submodules
```shell
$ cd dariadb
$ git submodules init 
$ git submodules update
```
### Available build options
- ENABLE_TESTS - Enable testing of the dariadb. - ON
- ENABLE_METRICS - Enable code metrics. - ON
- ENABLE_INTEGRATION_TESTS - Enable integration test. - ON
- ENABLE_SERVER - Enable build dariadb server. - ON
- ENABLE_BENCHMARKS - Enable build dariadb benchmarks. - ON
- CLANG_ASAN_UBSAN  - Enable Clang address & undefined behavior sanitizer for binary. - OFF
- CLANG_MSAN - Enable Clang memory sanitizer for binary. - OFF

#### Example
Configure to build with all benchmarks, but without tests and server.
```shell
$ cmake  -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DENABLE_INTEGRATION_TESTS=OFF -DENABLE_BENCHMARKS=ON -DENABLE_SERVER=OFF . 
```

###clang
---
```shell
$ cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="${CMAKE_CXX_FLAGS_RELEASE} -stdlib=libc++" -DCMAKE_EXE_LINKER_FLAGS="${CMAKE_EXE_LINKER_FLAGS} -lstdc++" .
$ make
```

###gcc
---
```shell
$ cmake -DCMAKE_BUILD_TYPE=Release .
$ make
```
###Microsoft Visual Studio
---
```cmd
$ cmake -G "Visual Studio 14 2015 Win64" .
$ cmake --build .
```
### build with non system installed boost
---
```shell
$ cmake  -DCMAKE_BUILD_TYPE=Release -DBOOST_ROOT="path/to/boost/" .
$ make
```

