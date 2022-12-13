% Asynchronous Programming with Seastar
% Nadav Har'El - nyh@ScyllaDB.com
  Avi Kivity - avi@ScyllaDB.com

# Introduction
**Seastar**, which we introduce in this document, is a C++ library for writing highly efficient complex server applications on modern multi-core machines.

Traditionally, the programming languages libraries and frameworks used for writing server applications have been divided into two distinct camps: those focusing on efficiency, and those focusing on complexity. Some frameworks are extremely efficient and yet allow building only simple applications (e.g., DPDK allows applications which process packets individually), while other frameworks allow building extremely complex applications, at the cost of run-time efficiency. Seastar is our attempt to get the best of both worlds: To create a library which allows building highly complex server applications, and yet achieve optimal performance.

> 传统上，用于编写服务器应用程序的编程语言库和框架被分为两个截然不同的阵营: 一个关注效率，另一个关注复杂性。有些框架非常高效，但只允许构建简单的应用程序(例如 DPDK 允许单独处理数据包的应用程序)，而其他框架允许构建非常复杂的应用程序，代价是运行时效率。Seastar 是我们两全其美的尝试: 创建一个库，允许构建高度复杂的服务器应用程序，同时实现最佳性能。

The inspiration and first use case of Seastar was Scylla, a rewrite of Apache Cassandra. Cassandra is a very complex application, and yet, with Seastar we were able to re-implement it with as much as 10-fold throughput increase, as well as significantly lower and more consistent latencies.

> Seastar 的灵感和首例使用案例是 ScyllaDB，它是 Apache Cassandra 的重写版本。Cassandra 是一个非常复杂的应用程序，但是，通过使用 Seastar，我们能够以 10 倍的吞吐量增长重新实现它，并且显著降低和更一致的延迟。

Seastar offers a complete asynchronous programming framework, which uses two concepts - **futures** and **continuations** - to uniformly represent, and handle, every type of asynchronous event, including network I/O, disk I/O, and complex combinations of other events.

> Seastar 提供了一个完整的异步编程框架，它使用了两个概念 -- **futures** 和 **continuations** -- 来统一表示和处理每种类型的异步事件，包括网络I/O、磁盘I/O和其他事件的复杂组合。

Since modern multi-core and multi-socket machines have steep penalties for sharing data between cores (atomic instructions, cache line bouncing and memory fences), Seastar programs use the share-nothing programming model, i.e., the available memory is divided between the cores, each core works on data in its own part of memory, and communication between cores happens via explicit message passing (which itself happens using the SMP's shared memory hardware, of course).

> 由于现代的多核和多插槽机器在核心之间共享数据(原子指令，高速缓存行反弹和内存栅栏)有很高的代价，Seastar 使用无共享编程模型，即可用的内存在内核之间分配，每个内核处理内存中自己的部分的数据，内核之间的通信通过显式消息传递发生(当然，这本身使用 SMP 的共享内存硬件)。

## Asynchronous programming
A server for a network protocol, such as the classic HTTP (Web) or SMTP (e-mail) servers, inherently deals with parallelism: Multiple clients send requests in parallel, and we cannot finish handling one request before starting to handle the next: A request may, and often does, need to block because of various reasons --- a full TCP window (i.e., a slow connection), disk I/O, or even the client holding on to an inactive connection --- and the server needs to handle other connections as well.

> 用于网络协议的服务器，例如经典的 HTTP（Web）或 SMTP（电子邮件）服务器，天生需要处理并行性。会存在多个客户端并行地发送请求，我们没办法保证在开始处理下一个请求之前完成前一个请求的处理。一个请求可能而且经常确实需要阻塞，一个完整的 TCP 窗口（即慢速连接）、磁盘 I/O，甚至是维持非活动连接的客户端。但是服务器也还是要处理其他连接。

The most straightforward way to handle such parallel connections, employed by classic network servers such as Inetd, Apache Httpd and Sendmail, is to use a separate operating-system process per connection. This technique evolved over the years to improve its performance: At first, a new process was spawned to handle each new connection; Later, a pool of existing processes was kept and each new connection was assigned to an unemployed process from the pool; Finally, the processes were replaced by threads. However, the common idea behind all these implementations is that at each moment, each process handles exclusively a single connection. Therefore, the server code is free to use blocking system calls, such as reading or writing to a connection, or reading from disk, and if this process blocks, all is well because we have many additional processes ready to handle other connections.

> 经典网络服务器（如 Inetd、Apache Httpd 和 Sendmail）采用的处理这种并行连接的最直接方法是每个连接使用单独的操作系统进程。这种技术性能的提高经过了多年的发展：起初，每个新连接都产生一个新进程来处理；后来，保留了一个事先生成的进程池，并将每个新连接分配给该池中的一个未使用的进程；最后，进程被线程取代。然而，所有这些实现背后的共同想法是，在每个时刻，每个进程都只处理一个连接。因此，服务器代码可以自由使用阻塞系统调用，例如读取或写入连接，或从磁盘读取，如果此进程阻塞，也不会影响，因为我们有许多其他进程准备处理其他连接。

Programming a server which uses a process (or a thread) per connection is known as *synchronous* programming, because the code is written linearly, and one line of code starts to run after the previous line finished. For example, the code may read a request from a socket, parse the request, and then piecemeal read a file from disk and write it back to the socket. Such code is easy to write, almost like traditional non-parallel programs. In fact, it's even possible to run an external non-parallel program to handle each request --- this is for example how Apache HTTPd ran "CGI" programs, the first implementation of dynamic Web-page generation.

> 对每个连接使用一个进程（或线程）的服务器进行编程称为同步编程，因为代码是线性编写的，并且一行代码在前一行完成后开始运行。例如，代码可能从套接字读取请求，解析请求，然后从磁盘中读取文件并将其写回套接字。这样的代码很容易编写，几乎就像传统的非并行程序一样。事实上，甚至可以运行一个外部的非并行程序来处理每个请求，例如 Apache HTTPd 如何运行"CGI"程序，这是动态网页生成的第一个实现。

>NOTE: although the synchronous server application is written in a linear, non-parallel, fashion, behind the scenes the kernel helps ensure that everything happens in parallel and the machine's resources --- CPUs, disk and network --- are fully utilized. Beyond the process parallelism (we have multiple processes handling multiple connections in parallel), the kernel may even parallelize the work of one individual connection --- for example process an outstanding disk request (e.g., read from a disk file) in parallel with handling the network connection (send buffered-but-yet-unsent data, and buffer newly-received data until the application is ready to read it).
>
>注意：虽然同步服务器应用程序是以线性、非并行的方式编写的，但在幕后，内核有助于确保一切并行发生，并且机器的资源 -- CPU、磁盘和网络 -- 可以得到充分利用。除了进程并行（我们有多个进程并行处理多个连接）之外，内核甚至可以并行处理一个单独的连接的工作 -- 例如处理一个未完成的磁盘请求（例如，从磁盘文件读取）与处理并行网络连接（发送缓冲但尚未发送的数据，并缓冲新接收的数据，直到应用程序准备好读取它）。

But synchronous, process-per-connection, server programming didn't come without disadvantages and costs. Slowly but surely, server authors realized that starting a new process is slow, context switching is slow, and each process comes with significant overheads --- most notably the size of its stack. Server and kernel authors worked hard to mitigate these overheads: They switched from processes to threads, from creating new threads to thread pools, they lowered default stack size of each thread, and increased the virtual memory size to allow more partially-utilized stacks. But still, servers with synchronous designs had unsatisfactory performance, and scaled badly as the number of concurrent connections grew. In 1999, Dan Kigel popularized "the C10K problem", the need of a single server to efficiently handle 10,000 concurrent connections --- most of them slow or even inactive.

> 但是同步的、每个连接使用一个进程的服务器编程方式并非没有缺点和成本。服务器开发人员意识到启动一个新进程很慢，上下文切换很慢，并且每个进程都有很大的开销，最明显的是它的堆栈大小。服务器和内核开发人员努力减轻这些开销：他们从进程切换到线程，从创建新线程到线程池，他们降低了每个线程的默认堆栈大小，并增加了虚拟内存大小以允许更多可以使用的堆栈。但是，采用同步设计的服务器的性能仍不能令人满意，并且随着并发连接数量的增加，扩展性也很差。1999 年，Dan Kigel 普及了 "C10K 问题"，需要单台服务器高效处理 10k 个并发的连接——它们大多数很慢甚至是不活跃的。

The solution, which became popular in the following decade, was to abandon the cozy but inefficient synchronous server design, and switch to a new type of server design --- the *asynchronous*, or *event-driven*, server. An event-driven server has just one thread, or more accurately, one thread per CPU. This single thread runs a tight loop which, at each iteration, checks, using ```poll()``` (or the more efficient ```epoll```) for new events on many open file descriptors, e.g., sockets. For example, an event can be a socket becoming readable (new data has arrived from the remote end) or becoming writable (we can send more data on this connection). The application handles this event by doing some non-blocking operations, modifying one or more of the file descriptors, and maintaining its knowledge of the _state_ of this connection.

> 在接下来的十年中流行的解决方案是放弃舒适但低效的同步服务器设计，转而使用一种新型的服务器设计 -- 异步或事件驱动的服务器。事件驱动服务器只有一个线程，或者更准确地说，每个 CPU 一个线程。这个单线程运行一个紧密的循环，在每次迭代中，检查、使用 `poll()`（或更有效的 `epoll`) 用于许多打开文件描述符（例如套接字）上的新事件。例如，一个事件可以是一个套接字变得可读（新数据已经从远程端到达）或变得可写（我们可以在这个连接上发送更多数据）。应用程序通过执行一些非阻塞操作、修改一个或多个文件描述符以及维护该连接的状态信息来处理此事件。

However, writers of asynchronous server applications faced, and still face today, two significant challenges:

* **Complexity:** Writing a simple asynchronous server is straightforward. But writing a *complex* asynchronous server is notoriously difficult. The handling of a single connection, instead of being a simple easy-to-read function call, now involves a large number of small callback functions, and a complex state machine to remember which function needs to be called when each event occurs.
* **Non-blocking:** Having just one thread per core is important for the performance of the server application, because context switches are slow. However, if we only have one thread per core, the event-handling functions must _never_ block, or the core will remain idle. But some existing programming languages and frameworks leave the server author no choice but to use blocking functions, and therefore multiple threads.
For example, ```Cassandra``` was written as an asynchronous server application; But because disk I/O was implemented with ```mmap```ed files, which can uncontrollably block the whole thread when accessed, they are forced to run multiple threads per CPU.

> 然而，异步服务器应用程序的编写者到目前仍然面临两个重大挑战：
>
> * **复杂性:** 编写一个简单的异步服务器是直接的。但是编写一个“复杂的”异步服务器是出了名的困难。单个连接的处理不再是简单易读的函数调用，而是涉及大量的小回调函数，以及一个复杂的状态机来记住在每个事件发生时需要调用哪个函数。
>
> * **非阻塞:** 每个核心只有一个线程对于服务器应用程序的性能很重要，因为上下文切换很慢。然而，如果每个核心只有一个线程，事件处理函数决不能阻塞，否则核心将保持空闲。但是一些现有的编程语言和框架让服务器作者别无选择，只能使用阻塞函数，从而使用多线程。例如，`Cassandra` 是作为异步服务器应用程序编写的，但是由于磁盘 I/O 是使用 `mmap` 文件实现的，这些文件在访问时可以不受控制地阻塞整个线程，因此它们被迫在每个 CPU 上运行多个线程。

Moreover, when the best possible performance is desired, the server application, and its programming framework, has no choice but to also take the following into account:

* **Modern Machines**: Modern machines are very different from those of just 10 years ago. They have many cores and deep memory hierarchies (from L1 caches to NUMA) which reward certain programming practices and penalizes others: Unscalable programming practices (such as taking locks) can devastate performance on many cores; Shared memory and lock-free synchronization primitives are available (i.e., atomic operations and memory-ordering fences) but are dramatically slower than operations that involve only data in a single core's cache, and also prevent the application from scaling to many cores.

* **Programming Language:** High-level languages such Java, Javascript, and similar "modern" languages are convenient, but each comes with its own set of assumptions which conflict with the requirements listed above. These languages, aiming to be portable, also give the programmer less control over the performance of critical code. For really optimal performance, we need a programming language which gives the programmer full control, zero run-time overheads, and on the other hand --- sophisticated compile-time code generation and optimization.

> 此外，当希望获得最佳性能时，服务器应用程序及其编程框架别无选择，只能考虑以下因素:
>
> * **现代机器**: 现代机器与10年前的机器大不相同。它们有很多的内核以及很深的内存层次结构(从 L1 缓存到 NUMA)，这对某些编程方式是有益的，但是对于某些编程方式却是有害的：不可扩展的编码方式(比如使用锁)会破坏多核的性能；共享内存和无锁同步原语(例如，原子操作和内存栅栏)是可用的，但相对只操作单个内核缓存中的数据的操作要慢得多，并且还会阻止应用程序扩展到多个内核。
>
> * **编程语言**: Java、Javascript 等类似的现代高级语言很方便，但每种语言都有自己的一套假设，这些假设与上面列出的要求相冲突。这些语言的目标是可移植性，但也让程序员对关键代码的性能控制变得更少。为了获得真正的最佳性能，我们需要一种编程语言，它可以让程序员完全控制，零运行时开销，另一方面实现复杂的编译时代码生成和优化。

Seastar is a framework for writing asynchronous server applications which aims to solve all four of the above challenges: It is a framework for writing *complex* asynchronous applications involving both network and disk I/O.  The framework's fast path is entirely single-threaded (per core), scalable to many cores and minimizes the use of costly sharing of memory between cores. It is a C++14 library, giving the user sophisticated compile-time features and full control over performance, without run-time overhead.

> Seastar 是一个用于编写异步服务器应用程序的框架，旨在解决上述四个挑战：它是一个用于编写涉及网络和磁盘 I/O 的 “复杂” 异步应用程序的框架。该框架的快速路径完全是单线程的(每个内核)，可扩展到多个内核，并最大限度地减少内核之间昂贵的内存共享的使用。它是一个 C++14 库，为用户提供了复杂的编译时特性和对性能的完全控制，而没有运行时开销。

## Seastar


Seastar is an event-driven framework allowing you to write non-blocking, asynchronous code in a relatively straightforward manner (once understood). Its APIs are based on futures.  Seastar utilizes the following concepts to achieve extreme performance:

* **Cooperative micro-task scheduler**: instead of running threads, each core runs a cooperative task scheduler. Each task is typically very lightweight -- only running for as long as it takes to process the last I/O operation's result and to submit a new one.
* **Share-nothing SMP architecture**: each core runs independently of other cores in an SMP system. Memory, data structures, and CPU time are not shared; instead, inter-core communication uses explicit message passing. A Seastar core is often termed a shard. TODO: more here https://github.com/scylladb/seastar/wiki/SMP
* **Future based APIs**: futures allow you to submit an I/O operation and to chain tasks to be executed on completion of the I/O operation. It is easy to run multiple I/O operations in parallel - for example, in response to a request coming from a TCP connection, you can issue multiple disk I/O requests, send messages to other cores on the same system, or send requests to other nodes in the cluster, wait for some or all of the results to complete, aggregate the results, and send a response.
* **Share-nothing TCP stack**: while Seastar can use the host operating system's TCP stack, it also provides its own high-performance TCP/IP stack built on top of the task scheduler and the share-nothing architecture. The stack provides zero-copy in both directions: you can process data directly from the TCP stack's buffers, and send the contents of your own data structures as part of a message without incurring a copy. Read more...
* **DMA-based storage APIs**: as with the networking stack, Seastar provides zero-copy storage APIs, allowing you to DMA your data to and from your storage devices.

> Seastar 是一个事件驱动的框架，允许你以一种相对简单的方式(一旦理解)编写非阻塞的异步代码。它的 api 基于 future。Seastar 利用以下概念实现极限性能:
>
> * **协作时微任务调度器**: 每个核心运行一个协作时任务调度器，而不是运行线程。每个任务通常都是非常轻量级的：只在处理最后一个 I/O 操作的结果和提交一个新操作的时候运行。
> * **Share-nothing SMP 体系结构**: 在 SMP 系统中，每个核心独立于其他核心运行。内存、数据结构和 CPU 时间不共享；相反，内核间通信使用显式的消息传递。Seastar 核心通常被称为分片。TODO: 更多信息请点击这里https://github.com/scylladb/seastar/wiki/SMP
> * **基于 future 的 api**: future 允许你提交 I/O 操作，并在 I/O 操作完成时链接要执行的任务。并行运行多个 I/O 操作很容易：例如，为了响应来自 TCP 连接的请求，你可以发出多个磁盘 I/O 请求，向同一系统上的其他内核发送消息，或向集群中的其他节点发送请求，等待部分或全部结果完成，聚合结果，然后发送响应。
> * **Share-nothing TCP 栈**: 虽然 Seastar 可以使用主机操作系统的 TCP 栈，但它也提供了自己的高性能 TCP/IP 协议栈，构建在任务调度器和 share-nothing 架构之上。Seastar 的高性能 TCP/IP 协议栈实现了双向的零拷贝：你可以直接在 TCP 协议栈的缓冲区处理数据，并将你自己的数据结构的内容作为消息的一部分发送，而无需引起拷贝。阅读更多...
> * **基于DMA的存储 api **: 与网络堆栈一样，Seastar 提供零拷贝存储 API，允许你将数据直接通过 DMA 传输到存储设备。

This tutorial is intended for developers already familiar with the C++ language, and will cover how to use Seastar to create a new application.

TODO: copy text from https://github.com/scylladb/seastar/wiki/SMP
https://github.com/scylladb/seastar/wiki/Networking

# Getting started

The simplest Seastar program is this:

```cpp
#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <iostream>

int main(int argc, char** argv) {
    seastar::app_template app;
    app.run(argc, argv, [] {
            std::cout << "Hello world\n";
            return seastar::make_ready_future<>();
    });
}
```

As we do in this example, each Seastar program must define and run, an `app_template` object. This object starts the main event loop (the Seastar *engine*) on one or more CPUs, and then runs the given function - in this case an unnamed function, a *lambda* - once.

> 正如我们在本例中所做的那样，每个 Seastar 程序都必须定义并运行一个 `app_template` 对象。该对象在一个或多个 CPU 上启动主事件循环（Seastar engine），然后运行给定函数 --- 在本例中是一个未命名的函数，一个 lambda。

The `return make_ready_future<>();` causes the event loop, and the whole application, to exit immediately after printing the "Hello World" message. In a more typical Seastar application, we will want event loop to remain alive and process incoming packets (for example), until explicitly exited. Such applications will return a _future_ which determines when to exit the application. We will introduce futures and how to use them below. In any case, the regular C `exit()` should not be used, because it prevents Seastar or the application from cleaning up appropriately.

> `return make_ready_future<>();` 导致事件循环和整个应用程序在打印 "Hello World" 消息后立即退出。在更典型的 Seastar 应用程序中，我们希望事件循环保持活动状态并处理传入的数据包，直到显式退出。此类应用程序将返回一个确定何时退出应用程序的 future。我们将在下面介绍 future 以及如何使用它们。在任何情况下，都不应使用常规 C `exit()`，因为它会阻止 Seastar 或应用程序进行适当的清理。

As shown in this example, all Seastar functions and types live in the "`seastar`" namespace. An user can either type this namespace prefix every time, or use shortcuts like "`using seastar::app_template`" or even "`using namespace seastar`" to avoid typing this prefix. We generally recommend to use the namespace prefixes `seastar` and `std` explicitly, and will will follow this style in all the examples below.

> 如本例所示，所有 Seastar 函数和类型都位于 `seastar` 命名空间中。用户可以每次都输入这个命名空间前缀，或者使用 `using seastar::app_template` 甚至  `using namespace seastar` 之类的快捷方式来避免输入这个前缀。我们通常建议显式地使用命名空间前缀 seastar 和 std，并将在下面的所有示例中遵循这种风格。

To compile this program, first make sure you have downloaded, built, and optionally installed Seastar, and put the above program in a source file anywhere you want, let's call the file `getting-started.cc`.

> 要编译这个程序，首先要确保你已经下载、编译和安装了 Seastar，然后把上面的程序放在你想要的源文件中，我们把这个文件叫做 `getting-started.cc`.

Linux's [pkg-config](http://www.freedesktop.org/wiki/Software/pkg-config/) is one way for easily determining the compilation and linking parameters needed for using various libraries - such as Seastar.  For example, if Seastar was built in the directory `$SEASTAR` but not installed, one can compile `getting-started.cc` with it using the command:

> Linux 的 `pkg-config` 是一种轻松确定使用各种库（例如 Seastar）所需的编译和链接参数的方法。例如，如果 Seastar 已在 `$SEASTAR` 目录中构建但未安装，则可以使用以下命令对 `getting-started.cc` 进行编译：

```sh
c++ getting-started.cc `pkg-config --cflags --libs --static $SEASTAR/build/release/seastar.pc`
```
The "`--static`" is needed because currently, Seastar is built as a static library, so we need to tell `pkg-config` to include its dependencies in the link command (whereas, had Seastar been a shared library, it could have pulled in its own dependencies).

If Seastar _was_ installed, the `pkg-config` command line is even shorter:

> 之所以需要 `--static`，是因为目前 Seastar 是作为静态库构建的，所以我们需要告诉 `pkg-config` 在链接命令中包含它的依赖项（而如果 Seastar 是一个共享库，它可能会引入它自己的依赖项）。
>
> 如果安装了 Seastar，命令 `pkg-config` 会更短：

```
c++ getting-started.cc `pkg-config --cflags --libs --static seastar`
```

Alternatively, one can easily build a Seastar program with CMake. Given the following `CMakeLists.txt`

```cmake
cmake_minimum_required (VERSION 3.5)

project (SeastarExample)

find_package (Seastar REQUIRED)

add_executable (example
  getting-started.cc)

target_link_libraries (example
  PRIVATE Seastar::seastar)
```

you can compile the example with the following commands:

```none
$ mkdir build
$ cd build
$ cmake ..
$ make
```

The program now runs as expected:
```none
$ ./example
Hello world
$
```

# Threads and memory
## Seastar threads
As explained in the introduction, Seastar-based programs run a single thread on each CPU. Each of these threads runs its own event loop, known as the *engine* in Seastar nomenclature. By default, the Seastar application will take over all the available cores, starting one thread per core. We can see this with the following program, printing `seastar::smp::count` which is the number of started threads:

> 如简介中所述，基于 Seastar 的程序在每个 CPU 上运行一个线程。这些线程中的每一个都运行自己的事件循环，在 Seastar 命名法中称为 *engine*。默认情况下，Seastar 应用程序将接管所有可用 CPU，每个 CPU 启动一个线程。我们可以通过以下程序看到这一点，打印 `seastar::smp::count` 启动线程的数量：

```cpp
#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <iostream>

int main(int argc, char** argv) {
    seastar::app_template app;
    app.run(argc, argv, [] {
            std::cout << seastar::smp::count << "\n";
            return seastar::make_ready_future<>();
    });
}
```

On a machine with 4 hardware threads (two cores, and hyperthreading enabled), Seastar will by default start 4 engine threads:

> 在具有 4 个 CPU（两个内核，并启用超线程）的机器上，Seastar 将默认启动 4 个 engine 线程：

```none
$ ./a.out
4
```

Each of these 4 engine threads will be pinned (a la **taskset(1)**) to a different hardware thread. Note how, as we mentioned above, the app's initialization function is run only on one thread, so we see the ouput "4" only once. Later in the tutorial we'll see how to make use of all threads.

> 这 4 个 engine 线程中的每一个都将被绑定（`taskset(1)`）到不同的 CPU 上。请注意，如上所述，应用程序的初始化函数仅在一个线程上运行，因此我们只看到输出 "4" 一次。在本教程的后面，我们将看到如何使用所有线程。

The user can pass a command line parameter, `-c`, to tell Seastar to start fewer threads than the available number of hardware threads. For example, to start Seastar on only 2 threads, the user can do:

> 用户可以传递命令行参数 `-c` 来告诉 Seastar 启动的线程数少于可用的 CPU 数。例如，要仅在 2 个 CPU 上启动 Seastar，用户可以执行以下操作：

```none
$ ./a.out -c2
2
```
When the machine is configured as in the example above - two cores with two hyperthreads on each - and only two threads are requested, Seastar ensures that each thread is pinned to a different core, and we don't get the two threads competing as hyperthreads of the same core (which would, of course, damage performance).

> 假设机器有两个内核，每个内核各有两个超线程，当机器按照上面的示例进行配置只请求两个线程时，Seastar 确保每个线程都绑定到不同的内核，并且我们不会让两个线程作为超线程竞争相同的核心（当然，这会损害性能）。

We cannot start more threads than the number of hardware threads, as allowing this will be grossly inefficient. Trying it will result in an error:

> 我们不能启动比硬件线程数更多的线程，因为这样做会非常低效。尝试设置更大的值会导致错误：

```none
$ ./a.out -c5
terminate called after throwing an instance of 'std::runtime_error'
  what():  insufficient processing units
abort (core dumped)
```

The error is an exception thrown from app.run, which we did not catch, leading to this ugly uncaught-exception crash. It is better to catch this sort of startup exceptions, and exit gracefully without a core dump:

> 该错误是 `app.run` 抛出的异常，如果没有捕获这个异常，当导致崩溃。最好就是捕获这种类型的异常，然后优雅的退出，而不要使用 core dump.

```cpp
#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    seastar::app_template app;
    try {
        app.run(argc, argv, [] {
            std::cout << seastar::smp::count << "\n";
            return seastar::make_ready_future<>();
        });
    } catch(...) {
        std::cerr << "Failed to start application: "
                  << std::current_exception() << "\n";
        return 1;
    }
    return 0;
}
```
```none
$ ./a.out -c5
Couldn't start application: std::runtime_error (insufficient processing units)
```

Note that catching the exceptions this way does **not** catch exceptions thrown in the application's actual asynchronous code. We will discuss these later in this tutorial.

> 注意，以这种方式捕获异常 **不会** 捕获应用程序实际异步代码中抛出的异常。我们将在本教程后面讨论这些内容。

## Seastar memory
As explained in the introduction, Seastar applications shard their memory. Each thread is preallocated with a large piece of memory (on the same NUMA node it is running on), and uses only that memory for its allocations (such as `malloc()` or `new`).

> 正如介绍中所解释的，Seastar 应用程序对它们的内存进行分片。每个线程都预先分配了一大块内存（在它运行的同一个 NUMA 节点上），并且只使用该内存进行分配（例如 `malloc()` 或 `new`）。

By default, the machine's **entire memory** except a certain reservation left for the OS (defaulting to the maximum of 1.5G or 7% of total memory) is pre-allocated for the application in this manner. This default can be changed by *either* changing the amount reserved for the OS (not used by Seastar) with the `--reserve-memory` option, or by explicitly giving the amount of memory given to the Seastar application, with the `-m` option. This amount of memory can be in bytes, or using the units "k", "M", "G" or "T". These units use the power-of-two values: "M" is a **mebibyte**, 2^20 (=1,048,576) bytes, not a **megabyte** (10^6 or 1,000,000 bytes).

> 默认情况下，除了为操作系统预留的一部分（默认为最大 1.5G 或总内存的 7%），其他所有可用内存都以这种方式预分配给应用程序。可以通过使用 `--reserve-memory` 选项更改为操作系统预留内存的数量（Seastar 不使用）或通过使用 `-m` 选项显式指定给予 Seastar 应用程序的内存量来更改此默认值。此内存量可以以字节为单位，也可以使用单位 "K"、"M"、"G" 或 "T"。这些单位使用二的幂值："M" 是 `mebibyte`，2^20 (1,048,576) 字节，而不是 `megabyte`（10^6 或 1,000,000 字节）。

Trying to give Seastar more memory than physical memory immediately fails:

> 如果尝试为 Seastar 提供比物理内存更多的内存会立即失败：

```none
$ ./a.out -m10T
Couldn't start application: std::runtime_error (insufficient physical memory)
```

# Introducing futures and continuations
Futures and continuations, which we will introduce now, are the building blocks of asynchronous programming in Seastar. Their strength lies in the ease of composing them together into a large, complex, asynchronous program, while keeping the code fairly readable and understandable. 

> 我们现在将介绍的 Futures 和 continuations 是 Seastar 中异步编程的基础。它们的优势在于可以轻松地将它们组合成一个大型、复杂的异步程序，同时保持代码的可读性和可理解性。

A [future](\ref future) is a result of a computation that may not be available yet.
Examples include:

  * a data buffer that we are reading from the network
  * the expiration of a timer
  * the completion of a disk write
  * the result of a computation that requires the values from
    one or more other futures.

> `future` 是可能尚不可用的计算的结果。示例包括：
>
> - 我们从网络中读取的数据缓冲区
> - 计时器到期
> - 磁盘写入完成
> - 需要来自一个或多个其他 future 的值的计算结果

The type `future<int>` variable holds an int that will eventually be available - at this point might already be available, or might not be available yet. The method available() tests if a value is already available, and the method get() gets the value. The type `future<>` indicates something which will eventually complete, but not return any value.

> `future<int>` 变量包含一个最终可用的 `int` （此时可能已经可用，或者可能还不可用）。`available()` 方法测试一个值是否已经可用，`get()` 方法获取该值。类型 `future<>` 表示最终将完成但不返回任何值。

A future is usually returned by an **asynchronous function**, a function which returns a future and arranges for this future to be eventually resolved.  Because asynchrnous functions _promise_ to eventually resolve the future which they returned, asynchronous functions are sometimes called "promises"; But we will avoid this term because it tends to confuse more than it explains.

> `future` 通常由异步函数返回，该函数返回 `future` 并最终解决该 `future`。因为异步函数承诺最终解决它们返回的 `future`，所以异步函数有时被称为 "promises"；但是我们将避免使用这个术语，因为它往往会比它所解释的更容易混淆。

One simple example of an asynchronous function is Seastar's function sleep():

> 一个简单的异步函数示例是 Seastar 的函数 `sleep()`：

```cpp
future<> sleep(std::chrono::duration<Rep, Period> dur);
```

This function arranges a timer so that the returned future becomes available (without an associated value) when the given time duration elapses.

> 此函数有一个计时器，以便在给定的持续时间过去时返回的 future 变得可用（没有关联的值）。

A **continuation** is a callback (typically a lambda) to run when a future becomes available. A continuation is attached to a future with the `then()` method. Here is a simple example:

> `continuation` 是在 future 可用时运行的回调（通常是 lambda）。使用 `then()` 方法将 continuation 附加到 future。这是一个简单的例子：

```cpp
#include <seastar/core/app-template.hh>
#include <seastar/core/sleep.hh>
#include <iostream>

int main(int argc, char** argv) {
    seastar::app_template app;
    app.run(argc, argv, [] {
        std::cout << "Sleeping... " << std::flush;
        using namespace std::chrono_literals;
        return seastar::sleep(1s).then([] {
            std::cout << "Done.\n";
        });
    });
}
```

In this example we see us getting a future from `seastar::sleep(1s)`, and attaching to it a continuation which prints a "Done." message. The future will become available after 1 second has passed, at which point the continuation is executed. Running this program, we indeed see the message "Sleeping..." immediately, and one second later the message "Done." appears and the program exits.

> 在这个例子中，我们从 `seastar::sleep(1s)` 返回了一个 future, 该 future 绑定了一个打印 "Done." 信息的 `continuation`. `future` 将在 1 秒后变为可用。运行这个程序，我们确实立即看到消息 "Sleeping..."，一秒钟后看到消息 "Done." 出现并且程序退出。

The return value of `then()` is itself a future which is useful for chaining multiple continuations one after another, as we will explain below. But here we just note that we `return` this future from `app.run`'s function, so that the program will exit only after both the sleep and its continuation are done.

> `then()` 的返回值本身就是一个 `future`，它对于一个接一个的链接多个 continuation 很有用，我们将在下面解释。但是这里我们只从 `app.run` 中 `return` 这个 `future`，这样程序只有在 `sleep` 和它的 `continuation` 都完成后才会退出。

To avoid repeating the boilerplate "app_engine" part in every code example in this tutorial, let's create a simple main() with which we will compile the following examples. This main just calls function `future<> f()`, does the appropriate exception handling, and exits when the future returned by `f` is resolved:

> 为了避免在本教程的每个代码示例中重复样板 `app_engine` 部分，让我们创建一个简单的 `main()`，我们将使用它来编译以下示例。这个 `main` 只是调用 function  `future<> f()` 进行适当的异常处理，并在 `f` 解决返回的 `future` 时退出：

```cpp
#include <seastar/core/app-template.hh>
#include <seastar/util/log.hh>
#include <iostream>
#include <stdexcept>

extern seastar::future<> f();

int main(int argc, char** argv) {
    seastar::app_template app;
    try {
        app.run(argc, argv, f);
    } catch(...) {
        std::cerr << "Couldn't start application: "
                  << std::current_exception() << "\n";
        return 1;
    }
    return 0;
}
```

Compiling together with this `main.cc`, the above sleep() example code becomes:

> 与这个 `main.cc` 一起编译，上面的 sleep() 示例代码变为：

```cpp
#include <seastar/core/sleep.hh>
#include <iostream>

seastar::future<> f() {
    std::cout << "Sleeping... " << std::flush;
    using namespace std::chrono_literals;
    return seastar::sleep(1s).then([] {
        std::cout << "Done.\n";
    });
}
```

So far, this example was not very interesting - there is no parallelism, and the same thing could have been achieved by the normal blocking POSIX `sleep()`. Things become much more interesting when we start several sleep() futures in parallel, and attach a different continuation to each. Futures and continuation make parallelism very easy and natural:

> 到目前为止，这个例子并不是很有趣：没有并行性，同样的事情也可以通过普通的阻塞 POSIX `sleep()` 来实现。当我们并行启动多个 sleep() futures 并为每个 future 绑定不同的 continuation 时，事情变得更加有趣。futures 和 continuation 使并行性变得非常容易和自然：

```cpp
#include <seastar/core/sleep.hh>
#include <iostream>

seastar::future<> f() {
    std::cout << "Sleeping... " << std::flush;
    using namespace std::chrono_literals;
    seastar::sleep(200ms).then([] { std::cout << "200ms " << std::flush; });
    seastar::sleep(100ms).then([] { std::cout << "100ms " << std::flush; });
    return seastar::sleep(1s).then([] { std::cout << "Done.\n"; });
}
```

Each `sleep()` and `then()` call returns immediately: `sleep()` just starts the requested timer, and `then()` sets up the function to call when the timer expires. So all three lines happen immediately and f returns. Only then, the event loop starts to wait for the three outstanding futures to become ready, and when each one becomes ready, the continuation attached to it is run. The output of the above program is of course:

> 每个 `sleep()` 和 `then()` 调用立即返回：`sleep()` 只是启动请求的计时器，并通过 `then()` 方法设置在计时器到期时调用的函数。`f()` 返回之后，事件循环才开始等待三个未完成的 `future` 就绪，当每个 future 就绪时，附加到它的 `continuation` 开始运行。上述程序的输出当然是：

```none
$ ./a.out
Sleeping... 100ms 200ms Done.
```

`sleep()` returns `future<>`, meaning it will complete at a future time, but once complete, does not return any value. More interesting futures do specify a value of any type (or multiple values) that will become available later. In the following example, we have a function returning a `future<int>`, and a continuation to be run once this value becomes available. Note how the continuation gets the future's value as a parameter:

> `sleep()` 返回 `future<>`，这意味着它将在未来的某个时间完成，一旦完成，不会返回任何值。更有趣的是，`future` 指定了将来可用的任何类型（或多个值）的值。在下面的示例中，我们有一个返回 `future<int>` 的函数以及一个在该值可用时运行的 `continuation`。请注意 `continuation` 如何将 future 的值作为参数：

```cpp
#include <seastar/core/sleep.hh>
#include <iostream>

seastar::future<int> slow() {
    using namespace std::chrono_literals;
    return seastar::sleep(100ms).then([] { return 3; });
}

seastar::future<> f() {
    return slow().then([] (int val) {
        std::cout << "Got " << val << "\n";
    });
}
```

The function `slow()` deserves more explanation. As usual, this function returns a future<int> immediately, and doesn't wait for the sleep to complete, and the code in `f()` can chain a continuation to this future's completion. The future returned by `slow()` is itself a chain of futures: It will become ready once sleep's future becomes ready and then the value 3 is returned. We'll explain below in more details how `then()` returns a future, and how this allows *chaining* futures.

> 函数 `slow()` 立即返回 `future<int>`，并且不等待 `sleep` 完成，`f()` 可以将 `continuation` 链接到此 `future` 。`slow()` 返回的 `future` 本身就是一个 `future` 链：一旦 sleep 的 future 就绪，它就会就绪，然后返回 3。我们将在下面更详细地解释 `then()` 如何返回 `future`，以及这如何允许链接 `future`.

This example begins to show the convenience of the futures programming model, which allows the programmer to neatly encapsulate complex asynchronous operations. slow() might involve a complex asynchronous operation requiring multiple steps, but its user can use it just as easily as a simple sleep(), and Seastar's engine takes care of running the continuations whose futures have become ready at the right time.

> 这个例子开始展示 future 编程模型的便利性，它允许程序员巧妙地封装复杂的异步操作。slow() 可能涉及需要多个步骤的复杂异步操作，但它的用户可以像简单地使用 `sleep()` 一样轻松地使用它，并且 Seastar 的 engine 负责在正确时间运行其 `future` 已就绪的 `continuation`。

## Ready futures
A future value might already be ready when `then()` is called to chain a continuation to it. This important case is optimized, and *usually* the continuation is run immediately instead of being registered to run later in the next iteration of the event loop.

> 在调用 `then()` 将 `continuation` 链接到 `future` 时，`future` 值可能已经就绪了。Seastar 针对这种情况做了优化，通常会立即运行 `continuation`，而不是延迟到事件循环的下一次迭代中运行。

This optimization is done *usually*, though sometimes it is avoided: The implementation of `then()` holds a counter of such immediate continuations, and after many continuations have been run immediately without returning to the event loop (currently the limit is 256), the next continuation is deferred to the event loop in any case. This is important because in some cases (such as future loops, discussed later) we could find that each ready continuation spawns a new one, and without this limit we can starve the event loop. It important not to starve the event loop, as this would starve continuations of futures that weren't ready but have since become ready, and also starve the important **polling** done by the event loop (e.g., checking whether there is new activity on the network card).

> 这种优化通常会进行，但有时候不会：`then()` 持有一个立即运行的 continuation 的计数器，在立即运行许多 `continuation` 而不返回事件循环（当前限制为 256）后，下一个 `continuation` 无论如何都会被推迟到事件循环。这一点很重要，因为在某些情况下（例如后面讨论的 future 循环），我们会发现每个就绪的 `continuation` 都会生成一个新的 `continuation`，如果没有这个限制，我们可能会饿死事件循环。重要的是不要让事件循环饿死，因为这会饿死那些当前还没有就绪但是以后会就绪的 `future` 的 `continuation`，也会饿死由事件循环完成的重要的轮询（例如，检查网卡上是否有新活动）。

`make_ready_future<>` can be used to return a future which is already ready. The following example is identical to the previous one, except the promise function `fast()` returns a future which is already ready, and not one which will be ready in a second as in the previous example. The nice thing is that the consumer of the future does not care, and uses the future in the same way in both cases.

> `make_ready_future<>` 可用于返回已经就绪的 `future`。下面的示例和前面的示例相同，只是函数 `fast()` 返回的是一个已经就绪的 `future`，而不是像上一个示例那样在一秒钟之后就绪的 future。好消息是 `future` 的消费者并不关心，并且在两种情况下都以相同的方式使用 `future`。

```cpp
#include <seastar/core/future.hh>
#include <iostream>

seastar::future<int> fast() {
    return seastar::make_ready_future<int>(3);
}

seastar::future<> f() {
    return fast().then([] (int val) {
        std::cout << "Got " << val << "\n";
    });
}
```

# Continuations
## Capturing state in continuations

We've already seen that Seastar *continuations* are lambdas, passed to the `then()` method of a future. In the examples we've seen so far, lambdas have been nothing more than anonymous functions. But C++11 lambdas have one more trick up their sleeve, which is extremely important for future-based asynchronous programming in Seastar: Lambdas can **capture** state. Consider the following example:

> 我们已经看到传递给 `future` 的 `then()` 方法中的 `continuation` 是 lambda。在我们目前看到的例子中，lambda 只不过是匿名函数。但是 C++11 的 lambda 还有一个技巧，这对于 Seastar 中基于 `future` 的异步编程非常重要：lambda 可以捕获状态。考虑以下示例：

```cpp
#include <seastar/core/sleep.hh>
#include <iostream>

seastar::future<int> incr(int i) {
    using namespace std::chrono_literals;
    return seastar::sleep(10ms).then([i] { return i + 1; });
}

seastar::future<> f() {
    return incr(3).then([] (int val) {
        std::cout << "Got " << val << "\n";
    });
}
```

The future operation `incr(i)` takes some time to complete (it needs to sleep a bit first...), and in that duration, it needs to save the `i` value it is working on. In the early event-driven programming models, the programmer needed to explicitly define an object for holding this state, and to manage all these objects. Everything is much simpler in Seastar, with C++11's lambdas: The *capture syntax* "`[i]`" in the above example means that the value of i, as it existed when incr() was called() is captured into the lambda. The lambda is not just a function - it is in fact an *object*, with both code and data. In essence, the compiler created for us automatically the state object, and we neither need to define it, nor to keep track of it (it gets saved together with the continuation, when the continuation is deferred, and gets deleted automatically after the continuation runs).

> future 的操作 `incr(i)` 需要一些时间才能完成（它需要先 sleep 一会儿......），在这段时间内，它需要保存它正在处理的值 `i`。在早期的事件驱动编程模型中，程序员需要显式定义一个对象来保持这种状态，并管理所有这些对象。使用 C++11 的 lambda，Seastar 中的一切都变得简单得多：上面示例中的捕获语法 `[i]` 意味着 i 的值被捕获到 lambda 中，因为它在 `incr()` 被调用时存在。lambda 不仅仅是一个函数，它实际上是一个对象，代码和数据。本质上，编译器自动为我们创建了 state 对象，我们不需要定义它，也不需要跟踪它（它当 `continuation` 被延迟时与 `continuation` 一起保存，并在 `continuation` 运行后自动删除）

One implementation detail worth understanding is that when a continuation has captured state and is run immediately, this capture incurs no runtime overhead. However, when the continuation cannot be run immediately (because the future is not yet ready) and needs to be saved till later, memory needs to be allocated on the heap for this data, and the continuation's captured data needs to be copied there. This has runtime overhead, but it is unavoidable, and is very small compared to the related overhead in the threaded programming model (in a threaded program, this sort of state usually resides on the stack of the blocked thread, but the stack is much larger than our tiny capture state, takes up a lot of memory and causes a lot of cache pollution on context switches between those threads).

> 一个值得理解的实现细节是，当一个 continuation 捕获状态并立即运行时，此捕获不会产生运行时开销。但是当 continuation 不能立即运行（因为 future 还没有就绪）并且需要保存一段时间，需要在堆上为这些数据分配内存，并且需要将 continuation 捕获的数据复制到那里。这有运行时开销，但这是不可避免的，与线程模型中的相关开销相比非常小（在线程中，这种状态通常驻留在阻塞线程的堆栈中，但堆栈要比我们微小的捕获状态大得多，占用大量内存并在这些线程之间的上下文切换上造成大量缓存污染）。

In the above example, we captured `i` *by value* - i.e., a copy of the value of `i` was saved into the continuation. C++ has two additional capture options: capturing by *reference* and capturing by *move*:

> 在上面的示例中，我们通过值捕获 `i`, 即将值的副本 `i` 保存到 `continuation` 中。C++ 有两个额外的捕获选项：通过 `reference` 捕获和通过 `move` 捕获：

Using capture-by-reference in a continuation is usually a mistake, and can lead to serious bugs. For example, if in the above example we captured a reference to i, instead of copying it,

> 在 continuation 中使用按 `reference` 捕获通常是错误的，并且可能导致严重的错误。例如，如果在上面的示例中，我们捕获了对 `i` 的引用，而不是复制它，

```cpp
seastar::future<int> incr(int i) {
    using namespace std::chrono_literals;
    // Oops, the "&" below is wrong:
    return seastar::sleep(10ms).then([&i] { return i + 1; });
}
```
this would have meant that the continuation would contain the address of `i`, not its value. But `i` is a stack variable, and the incr() function returns immediately, so when the continuation eventually gets to run, long after incr() returns, this address will contain unrelated content.

> 这意味着 `continuation` 将包含 `i` 的地址，而不是它的值。但是 `i` 是一个堆栈变量，而 `incr()` 函数会立即返回，所以在 `incr()` 返回很久之后，当 `continuation` 最终开始运行时，这个地址将包含不相关的内容。

An exception to the capture-by-reference-is-usually-a-mistake rule is the `do_with()` idiom, which we will introduce later. This idiom ensures that an object lives throughout the life of the continuation, and makes capture-by-reference possible, and very convenient.

> “reference 捕获通常是错误” 说法的一个例外是 `do_with()` 用法，我们将在后面介绍。`do_with()` 确保一个对象在 `continuation` 的整个生命周期中都存在，并且使得通过 `reference` 捕获成为可能，并且非常方便。

Using capture-by-*move* in continuations is also very useful in Seastar applications. By **moving** an object into a continuation, we transfer ownership of this object to the continuation, and make it easy for the object to be automatically deleted when the continuation ends. For example, consider a traditional function taking a std::unique_ptr<T>.

> 在 `continuation` 中使用 `move` 捕获也非常有用。通过将一个对象 `move` 到一个 `continuation` 中，我们将这个对象的所有权转移给 `continuation`，并且使对象在 `continuation` 结束时很容易被自动删除。例如，考虑一个使用 std::unique_ptr 的传统函数:

```cpp
int do_something(std::unique_ptr<T> obj) {
     // do some computation based on the contents of obj, let's say the result is 17
     return 17;
     // at this point, obj goes out of scope so the compiler delete()s it.  
```
By using unique_ptr in this way, the caller passes an object to the function, but tells it the object is now its exclusive responsibility - and when the function is done with the object, it automatically deletes it. How do we use unique_ptr in a continuation? The following won't work:

> 通过以这种方式使用 `unique_ptr`，调用者将一个对象传递给函数，意味着该对象现在是它专属的：当函数处理完该对象时，它会自动删除它。我们如何在`continuation` 中使用 `unique_ptr` ？以下是一个错误的示例：

```cpp
seastar::future<int> slow_do_something(std::unique_ptr<T> obj) {
    using namespace std::chrono_literals;
    // The following line won't compile...
    return seastar::sleep(10ms).then([obj] () mutable { return do_something(std::move(obj)); });
}
```

The problem is that a unique_ptr cannot be passed into a continuation by value, as this would require copying it, which is forbidden because it violates the guarantee that only one copy of this pointer exists. We can, however, *move* obj into the continuation:

> 问题是 `unique_ptr` 不能按值传递给 continuation，因为这需要复制它，这是被禁止的，因为它违反了该指针仅存在一个副本的保证。但是，我们可以将 obj `move` 到 `continuation` 中：

```cpp
seastar::future<int> slow_do_something(std::unique_ptr<T> obj) {
    using namespace std::chrono_literals;
    return seastar::sleep(10ms).then([obj = std::move(obj)] () mutable {
        return do_something(std::move(obj));
    });
}
```
Here the use of `std::move()` causes obj's move-assignment is used to move the object from the outer function into the continuation. The notion of move (*move semantics*), introduced in C++11, is similar to a shallow copy followed by invalidating the source copy (so that the two copies do not co-exist, as forbidden by unique_ptr). After moving obj into the continuation, the top-level function can no longer use it (in this case it's of course ok, because we return anyway).

> 这里使用 `std::move()` 引起 `obj` 的 `move-assignment`, 用于将对象从外部函数移动到 `continuation` 中。在 C++11 中引入的 `move`（移动语义）的概念类似于浅拷贝，然后使源拷贝无效（这样两个拷贝就不会共存，正如 `unique_ptr` 所禁止的那样）。将 `obj` 移入 `continuation` 之后，顶层函数就不能再使用它了（这种情况下当然没问题，因为我们无论如何都要返回）。

The `[obj = ...]` capture syntax we used here is new to C++14. This is the main reason why Seastar requires C++14, and does not support older C++11 compilers.

> 我们在这里使用的 `[obj = ...]` 捕获语法对于 C++14 来说是新的。这就是 Seastar 需要 C++14 且不支持较旧的 C++11 编译器的主要原因。

The extra `() mutable` syntax was needed here because by default when C++ captures a value (in this case, the value of std::move(obj)) into a lambda, it makes this value read-only, so our lambda cannot, in this example, move it again. Adding `mutable` removes this artificial restriction.

> 这里需要额外的 `() mutable` 语法，因为默认情况下，当 C++ 将一个值（在本例中为 std::move(obj) 的值）捕获到 lambda 中时，它会将此值设为只读，因此在此示例中，我们的 lambda 不能再次移动。添加 `mutable` 消除了这种的限制。

## Evaluation order considerations (C++14 only)

C++14 (and below) does *not* guarantee that lambda captures in continuations will be evaluated after the futures they relate to are evaluated(See https://en.cppreference.com/w/cpp/language/eval_order).

> c++ 14(及以下版本)不保证在 continuation 中的 lambda 捕获在与之相关的 future 被求值之后被求值

Consequently, avoid the programming pattern below:

> 因此，要避免以下编程模式:

```cpp
    return do_something(obj).then([obj = std::move(obj)] () mutable {
        return do_something_else(std::move(obj));
    });
```

In the example above, `[obj = std::move(obj)]` might be evaluated before `do_something(obj)` is called, potentially leading to use-after-move of `obj`.

> 在上面的例子中，`[obj = std::move(obj)]` 可能会在 `do_something(obj)` 之前执行，因此导致 `obj` 在 move 之后使用。

To guarantee the desired evaluation order, the expression above may be broken into separate statments as follows:

> 为了保证所需的求值顺序，可以将上面的表达式分解为如下的单独语句:

```cpp
    auto fut = do_something(obj);
    return fut.then([obj = std::move(obj)] () mutable {
        return do_something_else(std::move(obj));
    });
```

This was changed in C++17. The expression that creates the object the function `then` is called on (the future) is evaluated before all the arguments to the function, so this style is not required in C++17 and above.

> C++17 对此做了修改。创建对象的表达式，函数 `then` 被调用，在函数的所有参数之前求值，因此在 C++17 及以上版本中不需要这种样式。

## Chaining continuations
TODO: We already saw chaining example in slow() above. talk about the return from then, and returning a future and chaining more thens.

# Handling exceptions

An exception thrown in a continuation is implicitly captured by the system and stored in the future. A future that stores such an exception is similar to a ready future in that it can cause its continuation to be launched, but it does not contain a value -- only the exception.

> continuation 中抛出的异常被系统隐式捕获并存储在 future 中。存储此类异常的 future 类似于已经就绪的 future，因为它可以导致它的 continuation 被执行，但它不包含值，仅包含异常。

Calling `.then()` on such a future skips over the continuation, and transfers the exception for the input future (the object on which `.then()` is called) to the output future (`.then()`'s return value).

> 这样的 `future` 调用 `.then()` 会跳过 `continuation`，并将输入 `future`（`.then()`被调用的对象）的异常转移到输出 `future`（`.then()`的返回值）。

This default handling parallels normal exception behavior -- if an exception is thrown in straight-line code, all following lines are skipped:

> 默认处理与正常的异常行为相似：如果在直线代码中抛出异常，则跳过以下所有行：

```cpp
line1();
line2(); // throws!
line3(); // skipped
```

is similar to

```cpp
return line1().then([] {
    return line2(); // throws!
}).then([] {
    return line3(); // skipped
});
```

Usually, aborting the current chain of operations and returning an exception is what's needed, but sometimes more fine-grained control is required. There are several primitives for handling exceptions:

1. `.then_wrapped()`: instead of passing the values carried by the future into the continuation, `.then_wrapped()` passes the input future to the continuation. The future is guaranteed to be in ready state, so the continuation can examine whether it contains a value or an exception, and take appropriate action.
2. `.finally()`: similar to a Java finally block, a `.finally()` continuation is executed whether or not its input future carries an exception or not. The result of the finally continuation is its input future, so `.finally()` can be used to insert code in a flow that is executed unconditionally, but otherwise does not alter the flow.

> 通常，中止当前的操作链并返回异常是需要的，但有时需要更细粒度的控制。有几种处理异常的原语：
>
> 1. `.then_wrapped()`：不是将 future 携带的值传递给 continuation，而是将输入 future 传递给 continuation。这个 future 保证处于就绪状态，因此 continuation 可以检查它是否包含值或异常，并采取适当的行动。
> 2. `.finally()`: 类似于 Java 的 finally. `.finally()` 无论其输入 future 是否携带异常，都会执行 `continuation`。finally continuation 的结果是它的输入 future，因此 `.finally()` 可用于在无条件执行的流程中插入代码，但不会改变流程。

TODO: give example code for the above. Also mention handle_exception - although perhaps delay that to a later chapter?

## Exceptions vs. exceptional futures
An asynchronous function can fail in one of two ways: It can fail immediately, by throwing an exception, or it can return a future which will eventually fail (resolve to an exception). These two modes of failure appear similar to the uninitiated, but behave differently when attempting to handle exceptions using `finally()`, `handle_exception()`, or `then_wrapped()`. For example, consider the code:

> 异步函数在以下两种情况下会执行失败：抛出异常或者返回失败的 future（解析为异常）。这两种失败模式看起来很相似，但在使用 `finally()`、`handle_exception()` 或 `then_wrapped()` 处理异常时是不一样的行为。例如，考虑以下代码：

```cpp
#include <seastar/core/future.hh>
#include <iostream>
#include <exception>

class my_exception : public std::exception {
    virtual const char* what() const noexcept override { return "my exception"; }
};

seastar::future<> fail() {
    return seastar::make_exception_future<>(my_exception());
}

seastar::future<> f() {
    return fail().finally([] {
        std::cout << "cleaning up\n";
    });
}
```

This code will, as expected, print the "cleaning up" message - the asynchronous function `fail()` returns a future which resolves to a failure, and the `finally()` continuation is run despite this failure, as expected.

> 如预期的那样，此代码将打印 "cleaning up" 消息，异步函数 `fail()` 返回解析为失败的 future，并且 `finally()` 的 continuation 尽管出现此失败，但仍然会继续运行。

Now consider that in the above example we had a different definition for `fail()`:

> 现在考虑在上面的例子中我们有一个 `fail()` 不同的定义：

```cpp
seastar::future<> fail() {
    throw my_exception();
}
```

Here, `fail()` does not return a failing future. Rather, it fails to return a future at all! The exception it throws stops the entire function `f()`, and the `finally()` continuation does not not get attached to the future (which was never returned), and will never run. The "cleaning up" message is not printed now.

> 在这里，`fail()` 不返回失败的 `future`。相反，它根本无法返回 `future`！它抛出的异常会终止整个函数 `f()`，并且 `finally()` 的 continuation 不会附加到`future`（从未返回），并且永远不会运行。现在不打印 "cleaning up" 消息。

We recommend that to reduce the chance for such errors, asynchronous functions should always return a failed future rather than throw an actual exception. If the asynchronous function calls another function _before_ returning a future, and that second function might throw, it should use `try`/`catch` to catch the exception and convert it into a failed future:

> 我们建议为了减少此类错误的机会，异步函数应始终返回失败的 future，而不是抛出实际的异常。如果异步函数在返回 future 之前调用另一个函数，并且第二个函数可能会抛出，它应该使用 `try`/`catch` 来捕获异常并将其转换为失败的 future：

```cpp
void inner() {
    throw my_exception();
}
seastar::future<> fail() {
    try {
        inner();
    } catch(...) {
        return seastar::make_exception_future(std::current_exception());
    }
    return seastar::make_ready_future<>();
}
```

Here, `fail()` catches the exception thrown by `inner()`, whatever it might be, and returns a failed future with that failure. Written this way, the `finally()` continuation will be reached, and the "cleaning up" message printed.

>Despite this recommendation that asynchronous functions avoid throwing, some asynchronous functions do throw exceptions in addition to returning exceptional futures. A common example are functions which allocate memory and throw `std::bad_alloc` when running out of memory, instead of returning a future. The `future<> seastar::semaphore::wait()` method is one such function: It returns a future which may be exceptional if the semaphore was `broken()` or the wait timed out, but may also *throw* an exception when failing to allocate memory it needs to hold the list of waiters.
>Therefore, unless a function --- including asynchronous functions --- is explicitly tagged "`noexcept`", the application should be prepared to handle exceptions thrown from it. In modern C++, code usually uses RAII to be exception-safe without sprinkling it with `try`/`catch`.  `seastar::defer()` is a RAII-based idiom that ensures that some cleanup code is run even if an exception is thrown.
>
>
>
>尽管建议尽量避免异步函数抛出异常，但仍然有一些异步函数除了返回异常 future 外，还会抛出异常。一个常见的例子是内存申请，当内存不足时抛出 `std::bad_alloc` 的异常，而不是返回 `future`。`future<> seastar::semaphore::wait()` 方法就是这样一个函数：它返回一个 `future`，如果信号量 `broken()` 或等待超时，它可能返回异常的 `future`，但也可能在分配保存等待者列表的内存失败时抛出异常。因此，除非一个函数（包括异步函数）被显式标记为 `noexcept`，应用程序应该准备好处理从它抛出的异常。在现代 C++ 中，代码通常使用 RAII 来保证异常安全，而不是使用 `try`/`catch`。 `seastar::defer()` 是一个基于 RAII 的习惯用法，即使抛出异常也能确保运行一些清理代码）。

Seastar has a convenient generic function, `futurize_invoke()`, which can be useful here. `futurize_invoke(func, args...)` runs a function which may return either a future value or an immediate value, and in both cases convert the result into a future value. `futurize_invoke()` also converts an immediate exception thrown by the function, if any, into a failed future, just like we did above. So using `futurize_invoke()` we can make the above example work even if `fail()` did throw exceptions:

> Seastar 有一个方便的通用函数 `futurize_invoke()` 在这里很有用。`futurize_invoke(func, args...)` 运行一个可以返回 `future` 值或立即值的函数，并且在这两种情况下都将结果转换为 `future` 值。`futurize_invoke()` 还像我们上面所做的那样将函数抛出的异常（如果有）转换为失败的 `future`。因此使用`futurize_invoke()`，即使 `fail()` 抛出异常，我们也可以使上面的示例工作：

```cpp
seastar::future<> fail() {
    throw my_exception();
}
seastar::future<> f() {
    return seastar::futurize_invoke(fail).finally([] {
        std::cout << "cleaning up\n";
    });
}
```

Note that most of this discussion becomes moot if the risk of exception is inside a _continuation_. Consider the following code:

> 请注意，如果在 `continuation` 中抛出异常，则大部分讨论将变得毫无意义。考虑以下代码：

```cpp
seastar::future<> f() {
    return seastar::sleep(1s).then([] {
        throw my_exception();
    }).finally([] {
        std::cout << "cleaning up\n";
    });
}
```

Here, the lambda function of the first continuation does throw an exception instead of returning a failed future. However, we do _not_ have the same problem as before, which only happened because an asynchronous function threw an exception _before_ returning a valid future. Here, `f()` does return a valid future immediately - the failure will only be known later, after `sleep()` resolves. The message in `finally()` will be printed. The methods which attach continuations (such as `then()` and `finally()`) run the continuation the same way, so continuation functions may return immediate values or, in this case, throw an immediate exception, and still work properly.

> 在这里，第一个 continuation 的 lambda 函数确实抛出了一个异常，而不是返回一个失败的 future。和之前讨论不同的是，这次的异步函数在返回一个有效的 `future` 之前抛出了一个异常。函数 `f()` 会立即返回一个有效的 future，并且只有在 `sleep()` 执行结束之后才能知道失败。`finally()` 中的 "cleaning up" 信息仍然会被打印出来。附加 `continuation` 的方法（例如 `then()` 和 `finally()`）以相同的方式运行 `continuation`，因此 `continuation` 函数可能返回立即值，或者在这种情况下抛出立即异常，并且仍然正常工作。

# Lifetime management
An asynchronous function starts an operation which may continue long after the function returns: The function itself returns a `future<T>` almost immediately, but it may take a while until this future is resolved.

> 异步函数启动一个操作，该操作可能会在异步函数返回的很长时间后才执行：函数本身几乎立即返回 `future<T>`，但可能需要一段时间才能解决这个 `future`。

When such an asynchronous operation needs to operate on existing objects, or to use temporary objects, we need to worry about the *lifetime* of these objects: We need to ensure that these objects do not get destroyed before the asynchronous function completes (or it will try to use the freed object and malfunction or crash), and to also ensure that the object finally get destroyed when it is no longer needed (otherwise we will have a memory leak).
Seastar offers a variety of mechanisms for safely and efficiently keeping objects alive for the right duration. In this section we will explore these mechanisms, and when to use each mechanism.

> 当这样的异步操作需要对现有对象进行操作，或者使用临时对象时，我们需要考虑这些对象的生命周期：我们需要确保这些对象在异步函数完成之前不会被销毁（否则它会尝试使用释放的对象并发生故障或崩溃），并确保对象在不再需要时最终被销毁（否则我们将发生内存泄漏）。Seastar 提供了多种机制来安全有效地让对象在适当的时间内保持活动状态。在本节中，我们将探讨这些机制，以及何时使用每种机制。

## Passing ownership to continuation
The most straightforward way to ensure that an object is alive when a continuation runs and is destroyed afterwards is to pass its ownership to the continuation. When continuation *owns* the object, the object will be kept until the continuation runs, and will be destroyed as soon as the continuation is not needed (i.e., it may have run, or skipped in case of exception and `then()` continuation).

> 确保对象在 continuation 运行时处于活动状态，并在 continuation 不需要时被销毁的最直接方法是将其所有权传递给 continuation。当 continuation 拥有该对象时，该对象将一直保留到 continuation 运行，并在 continuation 不需要时被立即销毁（它可能已经运行，或者在出现异常和 `then()` continuation 时跳过）。

We already saw above that the way for a continuation to get ownership of an object is through *capturing*:

> 我们已经在上面看到，continuation 获取对象所有权的方法是通过捕获：

```cpp
seastar::future<> slow_incr(int i) {
    return seastar::sleep(10ms).then([i] { return i + 1; });
}
```
Here the continuation captures the value of `i`. In other words, the continuation includes a copy of `i`. When the continuation runs 10ms later, it will have access to this value, and as soon as the continuation finishes its object is destroyed, together with its captured copy of `i`. The continuation owns this copy of `i`.

> 这里 continuation 捕获了 `i` 的值。换句话说 continuation 包含了 `i` 的拷贝. 当 continuation 运行 10 毫秒后，它可以访问此值，当 continuation 完成时，其对象连同其捕获的 `i` 的拷贝都会被销毁。

Capturing by value as we did here - making a copy of the object we need in the continuation - is useful mainly for very small objects such as the integer in the previous example. Other objects are expensive to copy, or sometimes even cannot be copied. For example, the following is **not** a good idea:

> 我们在这里做的按值捕获 —— 拷贝我们在 continuation 中需要的对象 —— 主要用于非常小的对象，例如前面示例中的整数。其他对象的拷贝成本很高，有时甚至无法拷贝。例如，以下不是一个好主意：

```cpp
seastar::future<> slow_op(std::vector<int> v) {
    // this makes another copy of v:
    return seastar::sleep(10ms).then([v] { /* do something with v */ });
}
```
This would be inefficient - as the vector `v`, potentially very long, will be copied and the copy will be saved in the continuation. In this example, there is no reason to copy `v` - it was anyway passed to the function by value and will not be used again after capturing it into the continuation, as right after the capture, the function returns and destroys its copy of `v`.

> 在这个示例中，按值捕获 `std::vector<int> v` 将是低效的，因为 `v` 可能很大。在这个例子中，没有理由拷贝 v, 它无论如何都是按值传递给函数的，并且在将其捕获到 `continuation` 之后不会再次使用，因为在捕获之后，函数立即返回并销毁其副本 `v`.

For such cases, C++14 allows *moving* the object into the continuation:

> 对于这种情况，C++14 允许 move 到 continuation 中：

```cpp
seastar::future<> slow_op(std::vector<int> v) {
    // v is not copied again, but instead moved:
    return seastar::sleep(10ms).then([v = std::move(v)] { /* do something with v */ });
}
```
Now, instead of copying the object `v` into the continuation, it is *moved* into the continuation. The C++11-introduced move constructor moves the vector's data into the continuation and clears the original vector. Moving is a quick operation - for a vector it only requires copying a few small fields such as the pointer to the data. As before, once the continuation is dismissed the vector is destroyed - and its data array (which was moved in the move operation) is finally freed.

> 这里不将对象 `v` 拷贝到 continuation 中，而是将其移动到 continuation 中。C++11 引入的移动构造函数将 vector 的数据移动到 continuation 中并清除原始 vector。对于 vector 来说，移动是一种快速操作，它只需要复制一些小字段，例如指向数据的指针。和以前一样，一旦 continuation 被执行，vector 就会被销毁，最终被释放。

TODO: talk about temporary_buffer as an example of an object designed to be moved in this way.

In some cases, moving the object is undesirable. For example, some code keeps references to an object or one of its fields and the references become invalid if the object is moved. In some complex objects, even the move constructor is slow. For these cases, C++ provides the useful wrapper `std::unique_ptr<T>`. A `unique_ptr<T>` object owns an object of type `T` allocated on the heap. When a `unique_ptr<T>` is moved, the object of type T is not touched at all - just the pointer to it is moved. An example of using `std::unique_ptr<T>` in capture is:

> 在某些情况下，移动对象是不可取的。例如，某些代码保留对对象或其字段之一的引用，如果移动对象，引用将变为无效。在一些复杂的对象中，甚至移动构造函数也很慢。对于这些情况，C++ 提供了有用的封装 `std::unique_ptr<T>`。一个 `unique_ptr<T>` 对象拥有一个在堆上分配的 `T` 类型的对象。当 `unique_ptr<T>` 被移动时，类型 T 的对象根本没有被涉及，只是移动了指向它的指针。`std::unique_ptr<T>` 在捕获中使用的一个例子是：

```cpp
seastar::future<> slow_op(std::unique_ptr<T> p) {
    return seastar::sleep(10ms).then([p = std::move(p)] { /* do something with *p */ });
}
```

`std::unique_ptr<T>` is the standard C++ mechanism for passing unique ownership of an object to a function: The object is only owned by one piece of code at a time, and ownership is transferred by moving the `unique_ptr` object. A `unique_ptr` cannot be copied: If we try to capture p by value, not by move, we will get a compilation error.

> `std::unique_ptr<T>` 是将对象的唯一所有权传递给函数的标准 C++ 机制：对象一次仅由一段代码拥有，所有权通过移动 `unique_ptr` 对象来转移。`unique_ptr` 不能被复制：如果我们试图使用值捕获而不是移动捕获，我们会得到一个编译错误。

## Keeping ownership at the caller

The technique we described above - giving the continuation ownership of the object it needs to work on - is powerful and safe. But often it becomes hard and verbose to use. When an asynchronous operation involves not just one continuation but a chain of continations that each needs to work on the same object, we need to pass the ownership of the object between each successive continuation, which can become inconvenient. It is especially inconvenient when we need to pass the same object into two seperate asynchronous functions (or continuations) - after we move the object into one, the object needs to be returned so it can be moved again into the second. E.g.,

> 将对象的所有权移动给 continuation 是强大而安全的。但通常使用起来会变得困难和冗长。当异步操作不仅涉及一个 continuation，而是涉及每个都需要处理同一个对象的 `continuation` 链时，我们需要在每个 continuation 之间传递对象的所有权，这可能会变得不方便。当我们需要将同一个对象传递给两个单独的异步函数（或 continuation）时，尤其不方便，比如在我们将对象移入一个之后，需要返回该对象，以便它可以再次移入第二个。例如，

```cpp
seastar::future<> slow_op(T o) {
    return seastar::sleep(10ms).then([o = std::move(o)] {
        // first continuation, doing something with o
        ...
        // return o so the next continuation can use it!
        return std::move(o);
    }).then([](T o)) {
        // second continuation, doing something with o
        ...
    });
}
```

This complexity arises because we wanted asynchronous functions and continuations to take the ownership of the objects they operated on. A simpler approach would be to have the *caller* of the asynchronous function continue to be the owner of the object, and just pass *references* to the object to the various other asynchronous functions and continuations which need the object. For example:

> 之所以会出现这种复杂性，是因为我们希望异步函数和 continuation 都能够获取它们所操作的对象的所有权。一种更简单的方法是让异步函数的调用者继续成为对象的所有者，并将对该对象的引用传递给需要该对象的各种其他异步函数和 continuation。例如：

```cpp
seastar::future<> slow_op(T& o) {           // <-- pass by reference
    return seastar::sleep(10ms).then([&o] {// <-- capture by reference
        // first continuation, doing something with o
        ...
    }).then([&o]) {                        // <-- another capture by reference
        // second continuation, doing something with o
        ...
    });
}
```

This approach raises a question: The caller of `slow_op` is now responsible for keeping the object `o` alive while the asynchronous code started by `slow_op` needs this object. But how will this caller know how long this object is actually needed by the asynchronous operation it started?

> 这种方式提出了一个问题： `slow_op` 的调用者现在负责保持对象 `o` 处于活动状态，而由 `slow_op` 启动的异步代码需要这个对象。但是这个调用者如何知道它启动的异步操作实际需要这个对象多长时间呢？

The most reasonable answer is that an asynchronous function may need access to its parameters until the future it returns is resolved - at which point the asynchronous code completes and no longer needs access to its parameters. We therefore recommend that Seastar code adopt the following convention:

> 最合理的答案是异步函数可能需要访问它的参数，直到它返回的 `future` 被解析，此时异步代码完成并且不再需要访问它的参数。因此，我们建议 Seastar 代码采用以下约定：

> **Whenever an asynchronous function takes a parameter by reference, the caller must ensure that the referred object lives until the future returned by the function is resolved.**
>
> 
>
> **每当异步函数通过引用获取参数时，调用者必须确保被引用的对象存在，直到函数返回的 future 被解析。**

Note that this is merely a convention suggested by Seastar, and unfortunately nothing in the C++ language enforces it. C++ programmers in non-Seastar programs often pass large objects to functions as a const reference just to avoid a slow copy, and assume that the called function will *not* save this reference anywhere. But in Seastar code, that is a dangerous practice because even if the asynchronous function did not intend to save the reference anywhere, it may end up doing it implicitly by passing this reference to another function and eventually capturing it in a continuation.

> 请注意，这只是 Seastar 的建议，不幸的是，C++ 语言中没有强制执行它。非 Seastar 程序中的 C++ 程序员经常将大对象作为 const 引用传递给函数，只是为了避免慢速拷贝，并假设被调用的函数不会在任何地方保存此引用。但在 Seastar 代码中，这是一种危险的做法，因为即使异步函数不打算将引用保存在任何地方，它也可能会通过将此引用传递给另一个函数并最终在 continuation 中捕获它来隐式地执行此操作。

> It would be nice if future versions of C++ could help us catch incorrect uses of references. Perhaps we could have a tag for a special kind of reference, an "immediate reference" which a function can use use immediately (i.e, before returning a future), but cannot be captured into a continuation.
>
> 
>
> 如果未来的 C++ 版本可以帮助我们发现引用的不正确使用，那就太好了。也许我们可以为一种特殊的引用设置一个标签，一个函数可以立即使用的“立即引用”（即，在返回 future 之前），但不能被捕获到 continuation 中。

With this convention in place, it is easy to write complex asynchronous functions functions like `slow_op` which pass the object around, by reference, until the asynchronous operation is done. But how does the caller ensure that the object lives until the returned future is resolved? The following is *wrong*:

> 有了这个约定，就很容易编写复杂的异步函数函数，比如 `slow_op` 通过引用传递对象，直到异步操作完成。但是调用者如何确保对象在返回的 future 被解决之前一直存在？以下是错误的：

```cpp
seastar::future<> f() {
    T obj; // wrong! will be destroyed too soon!
    return slow_op(obj);
}
```
It is wrong because the object `obj` here is local to the call of `f`, and is destroyed as soon as `f` returns a future - not when this returned future is resolved! The correct thing for a caller to do would be to create the object `obj` on the heap (so it does not get destroyed as soon as `f` returns), and then run `slow_op(obj)` and when that future resolves (i.e., with `.finally()`), destroy the object.

> 这是错误的，因为这里的对象 `obj` 是调用 `f` 的局部变量，所以 `obj` 会在 `f` 返回 `future` 时立即销毁，而不是在返回的 `future` 被解决时！调用者要做的正确事情是在堆上创建 `obj` 对象（因此它不会在 `f` 返回时立即被销毁），然后运行 `slow_op(obj)`，当 `future` 解决（即使用`.finally()`）时，销毁对象。

Seastar provides a convenient idiom, `do_with()` for doing this correctly:

> Seastar 提供了一个方便的接口 `do_with()`, 用于正确执行此操作：

```cpp
seastar::future<> f() {
    return seastar::do_with(T(), [] (auto& obj) {
        // obj is passed by reference to slow_op, and this is fine:
        return slow_op(obj);
    }
}
```
`do_with` will *do* the given function *with* the given object alive. 

> `do_with` 将使用给定的对象执行特定的功能。

`do_with` saves the given object on the heap, and calls the given lambda with a reference to the new object. Finally it ensures that the new object is destroyed after the returned future is resolved. Usually, do_with is given an *rvalue*, i.e., an unnamed temporary object or an `std::move()`ed object, and `do_with` moves that object into its final place on the heap. `do_with` returns a future which resolves after everything described above is done (the lambda's future is resolved and the object is destroyed).

> `do_with` 将给定的对象保存在堆上，并使用新对象的引用调用给定的 lambda。最后，它确保在返回的 future 解决后销毁新对象。通常 `do_with` 被传入一个 `rvalue`，即一个未命名的临时对象或一个 `std::move()` 对象，`do_with` 将该对象移动到它在堆上的最终位置。`do_with` 返回一个在完成上述所有操作后解析的`future`（lambda 的 `future` 被解决并且对象被销毁）。

For convenience, `do_with` can also be given multiple objects to hold alive. For example here we create two objects and hold alive them until the future resolves:

> 为了方便起见，`do_with` 也可以传入多个对象并保持存活。例如在这里我们创建两个对象并维持它们的存活状态直到 future 解决：

```cpp
seastar::future<> f() {
    return seastar::do_with(T1(), T2(), [] (auto& obj1, auto& obj2) {
        return slow_op(obj1, obj2);
    }
}
```

While `do_with` can the lifetime of the objects it holds, if the user accidentally makes copies of these objects, these copies might have the wrong lifetime. Unfortunately, a simple typo like forgetting an "&" can cause such accidental copies. For example, the following code is broken:

> 虽然 `do_with` 打包了它拥有的对象的生命周期，但如果用户不小心复制了这些对象，这些副本可能具有错误的生命周期。不幸的是，像忘记 "&" 这样的简单错字可能会导致此类意外复制。例如，以下代码被破坏：

```cpp
seastar::future<> f() {
    return seastar::do_with(T(), [] (T obj) { // WRONG: should be T&, not T
        return slow_op(obj);
    }
}
```
In this wrong snippet, `obj` is mistakenly not a reference to the object which `do_with` allocated, but rather a copy of it - a copy which is destroyed as soon as the lambda function returns, rather than when the future it returns resolved. Such code will most likely crash because the object is used after being freed. Unfortunately the compiler will not warn about such mistakes. Users should get used to always using the type "auto&" with `do_with` - as in the above correct examples - to reduce the chance of such mistakes.

> 在这个错误的代码片段中，`obj` 不是对 `do_with` 分配对象的引用，而是它的副本：一个在 lambda 函数返回时被销毁的副本，而不是在它返回的 `future` 被解决时才销毁。这样的代码很可能会崩溃，因为对象在被释放后被使用。不幸的是，编译器不会警告此类错误。用户应该习惯于在 `do_with` 中总是使用 "auto&" 类型以减少发生此类错误的机会，如上面正确的例子。

 For the same reason, the following code snippet is also wrong:

> 同理，下面的代码片段也是错误的：

```cpp
seastar::future<> slow_op(T obj); // WRONG: should be T&, not T
seastar::future<> f() {
    return seastar::do_with(T(), [] (auto& obj) {
        return slow_op(obj);
    }
}
```
Here, although `obj` was correctly passed to the lambda by reference, we later acidentally passed `slow_op()` a copy of it (because here `slow_op` takes the object by value, not by reference), and this copy will be destroyed as soon as `slow_op` returns, not waiting until the returned future resolves.

> 在这里，虽然 `obj` 被正确的通过引用传递给了 lambda，但是我们后来不小心传递给 `slow_op()` 它的一个副本（因为这里 `slow_op` 是通过值而不是通过引用来捕获对象的），并且这个副本会在 `slow_op` 返回时立即销毁，而不是等到返回 future 解决。

When using `do_with`, always remember it requires adhering to the convention described above: The asynchronous function which we call inside `do_with` must not use the objects held by `do_with` *after* the returned future is resolved. It is a serious use-after-free bug for an asynchronous function to return a future which resolves while still having background operations using the `do_with()`ed objects.

> 使用 `do_with` 时，请始终记住它需要遵守上述约定：我们在 `do_with` 内部调用的异步函数不能在返回的 `future` 被解决后继续使用 `do_with` 所持有的对象。这是一个严重的 *use-after-free* 错误：异步函数返回一个 `future`，同时仍然使用 `do_with()` 的对象进行后台操作。

In general, it is rarely a good idea for an asynchronous function to resolve while leaving behind background operations - even if those operations do not use the `do_with()`ed objects. Background operations that we do not wait for may cause us to run out of memory (if we don't limit their number) and make it difficult to shut down the application cleanly.

> 通常，在保留后台操作的同时解决异步函数并不是一个好主意，即使这些操作不使用 `do_with()` 的对象。我们不等待的后台操作可能会导致我们内存不足（如果我们不限制它们的数量），并且很难干净地关闭应用程序。


## Sharing ownership (reference counting)
In the beginning of this chapter, we already noted that capturing a copy of an object into a continuation is the simplest way to ensure that the object is alive when the continuation runs and destoryed afterwards. However, complex objects are often expensive (in time and memory) to copy. Some objects cannot be copied at all, or are read-write and the continuation should modify the original object, not a new copy. The solution to all these issues are **reference counted**, a.k.a. **shared** objects:

> 在本章的开头，我们已经注意到将对象的副本捕获到 continuation 中是确保对象在 continuation 运行时处于活动状态并随后被销毁的最简单方法。但是，复杂对象的复制通常很昂贵（时间和内存）。有些对象根本无法复制或者读写，continuation 应该修改原始对象，而不是新副本。所有这些问题的解决方案都是引用计数，也就是共享对象：

A simple example of a reference-counted object in Seastar is a `seastar::file`, an object holding an open file object (we will introduce `seastar::file` in a later section). A `file` object can be copied, but copying does not involve copying the file descriptor (let alone the file). Instead, both copies point to the same open file, and a reference count is increased by 1. When a file object is destroyed, the file's reference count is decreased by one, and only when the reference count reaches 0 the underlying file is actually closed.

> Seastar 中引用计数对象的一个简单示例是 `seastar::file`，该对象包含一个打开的文件对象（我们将 `seastar::file` 在后面的部分中介绍）。`file` 对象可以被复制，但复制不涉及复制文件描述符（更不用说文件）。相反，两个副本都指向同一个打开的文件，并且引用计数增加 1。当文件对象被销毁时，文件的引用计数减少 1，只有当引用计数达到 0 时，底层文件才真正关闭.

The fact that `file` objects can be copied very quickly and all copies actually point to the same file, make it very convinient to pass them to asynchronous code; For example,

> `file` 对象可以非常快速地复制，并且所有副本实际上都指向同一个文件，这使得将它们传递给异步代码非常方便；例如，

```cpp
seastar::future<uint64_t> slow_size(file f) {
    return seastar::sleep(10ms).then([f] {
        return f.size();
    }); 
    // NOTE: something is wrong here! This will be explained below!
}
```

Note how calling `slow_size` is as simple as calling `slow_size(f)`, passing a copy of `f`, without  needing to do anything special to ensure that `f` is only destroyed when no longer needed. That simply happens naturally when nothing refers to `f` any more.

> 请注意，调用 `slow_size` 与调用 `slow_size(f)` 一样简单，传递 `f` 的副本，无需执行任何特殊操作以确保 `f` 仅在不再需要时才将其销毁。`f` 什么也没有做时，这很自然地发生了。

However, there is one complication. The above example is actually wrong, as the comment at the end of the function suggested. The problem is that the `f.size()` call started an asynchronous operation on `f` (the file's size may be stored on disk, so not immediately available) and yet at this point nothing is holding a copy of `f`... The method call does not increment the reference count of the object even if it an asynchronous method. (Perhaps this something we should rethink?)

> 然而，有一个问题很复杂。正如函数末尾的注释，上面的示例实际上是错误的。问题是 `f.size()` 调用启动了对 `f` 的异步操作(文件的大小可能存储在磁盘上，所以不能立即使用)，但没有任何地方保存 `f` 的副本... 函数调用不会增加对象的引用计数，即使它是异步方法。(也许我们应该重新考虑这一点?)

So we need to ensure that something does hold on to another copy of `f` until the asynchronous method call completes. This is how we typically do it:

> 因此，我们需要确保在异步函数调用完成之前，某些地方保存了 `f` 的另一个副本。我们通常是这样做的:

```cpp
seastar::future<uint64_t> slow_size(file f) {
    return seastar::sleep(10ms).then([f] {
        return f.size().finally([f] {});
    });
}
```
What we see here is that `f` is copied not only to the continuation which runs `f.size()`, but also into a continuation (a `finally`) which will run after it. So as long as `f.size()` does not complete, that second continuation holds `f` alive. Note how the second continuation seems to have no code (just a {}).  But the important thing is that the compiler automatically adds to it code to destroy its copy of `f` (and potentially the entire file if this reference count went down to 0).

> 我们在这里看到的是 `f` 不仅被拷贝到运行 `f.size()` 的 continuation 中，而且被拷贝到在它之后运行的 continuation 中(`finally`)。所以只要 `f.size() ` 没有完成，第二个 continuation 就会保持 `f` 存活。注意第二个 continuation  似乎没有代码(只有一个{})。但重要的是，编译器会自动向它添加代码，以销毁 `f` 的副本(如果引用计数减少到0，可能会销毁整个文件)。

The reference counting has a run-time cost, but it is usually very small; It is important to remember that Seastar objects are always used by a single CPU only, so the reference-count increment and decrement operations are not the slow atomic operations often used for reference counting, but just regular CPU-local integer operations. Moreover, judicious use of `std::move()` and the compiler's optimizer can reduce the number of unnecessary back-and-forth increment and decrement of the reference count.

> 引用计数有运行时开销，但通常很小；重要的是要记住，Seastar 对象总是只被单个 CPU 使用，因此引用计数的递增和递减操作通常不是缓慢的原子操作，而只是常规的 CPU 本地整数操作。而且，明智地使用 `std::move()` 和编译器的优化器可以减少可以减少不必要的来回增加和减少引用计数的次数。

C++11 offers a standard way of creating reference-counted shared objects - using the template `std::shared_ptr<T>`. A `shared_ptr` can be used to wrap any type into a reference-counted shared object like `seastar::file` above.  However, the standard `std::shared_ptr` was designed with multi-threaded applications in mind so it uses slow atomic increment/decrement operations for the reference count which we already noted is unnecessary in Seastar. For this reason Seastar offers its own single-threaded implementation of this template, `seastar::shared_ptr<T>`. It is similar to `std::shared_ptr<T>` except no atomic operations are used.

> C++11 提供了一种创建引用计数共享对象的标准方法：`std::shared_ptr<T>`. `shared_ptr` 可用于将任何类型包装到像上面的 `seastar::file` 的引用计数共享对象中。但是 `std::shared_ptr` 在设计时考虑了多线程，因此它对引用计数使用了缓慢的原子递增/递减操作，但这在 Seastar 中是不必要的。所以 Seastar 提供了自己的单线程实现 `seastar::shared_ptr<T>`。 除了不使用原子操作外，它类似于 `std::shared_ptr<T>`。

Additionally, Seastar also provides an even lower overhead variant of `shared_ptr`: `seastar::lw_shared_ptr<T>`. The full-featured `shared_ptr` is complicated by the need to support polymorphic types correctly (a shared object created of one class, and accessed through a pointer to a base class). It makes `shared_ptr` need to add two words to the shared object, and two words to each `shared_ptr` copy. The simplified `lw_shared_ptr` - which does **not** support polymorphic types - adds just one word in the object (the reference count) and each copy is just one word - just like copying a regular pointer. For this reason, the light-weight `seastar::lw_shared_ptr<T>` should be preferered when possible (`T` is not a polymorphic type), otherwise `seastar::shared_ptr<T>`. The slower `std::shared_ptr<T>` should never be used in sharded Seastar applications.

> 此外，Seastar 还提供了一种开销更低的变体 `shared_ptr`: `seastar::lw_shared_ptr<T>`. `seastar::shared_ptr` 由于需要支持多态类型（由一个类创建的共享对象，并通过指向基类的指针访问），因此功能变得复杂。`shared_ptr` 需要向共享对象添加两个字段，并为每个 `shared_ptr` 副本添加两个字段。简化版 `lw_shared_ptr` 不支持多态类型，只在对象中添加一个字段（引用计数），每个副本只有一个字段，就像拷贝常规指针一样。出于这个原因，如果可能（不是多态类型），应该首选轻量级 `seastar::lw_shared_ptr<T>`，否则使用 `seastar::shared_ptr<T>`。较慢的 `std::shared_ptr<T>` 绝不应在分片的 Seastar 应用程序中使用。

## Saving objects on the stack
Wouldn't it be convenient if we could save objects on a stack just like we normally do in synchronous code? I.e., something like:

> 如果我们可以像同步代码中那样将对象保存在堆栈中，那不是很方便吗？即：

```cpp
int i = ...;
seastar::sleep(10ms).get();
return i;
```
Seastar allows writing such code, by using a `seastar::thread` object which comes with its own stack.  A complete example using a `seastar::thread` might look like this:

> Seastar 允许通过使用带有自己堆栈的 `seastar::thread` 对象来编写此类代码。使用 `seastar::thread` 的完整示例可能如下所示：

```cpp
seastar::future<> slow_incr(int i) {
    return seastar::async([i] {
        seastar::sleep(10ms).get();
        // We get here after the 10ms of wait, i is still available.
        return i + 1;
    });
}
```
We present `seastar::thread`, `seastar::async()` and `seastar::future::get()` in the [seastar::thread] section.

> 我们在 `seastar::thread` 部分介绍 `seastar::thread`, `seastar::async()` 和 `seastar::future::get()`.

# Advanced futures
## Futures and interruption
TODO: A future, e.g., sleep(10s) cannot be interrupted. So if we need to, the promise needs to have a mechanism to interrupt it. Mention pipe's close feature, semaphore stop feature, etc.

> TODO: future 不能被打断（例如 sleep(10s)），如果有必要的话，我们需要一个机制来打断它。类似管道的关闭功能，信号量停止功能等。

## Futures are single use
TODO: Talk about if we have a future<int> variable, as soon as we get() or then() it, it becomes invalid - we need to store the value somewhere else. Think if there's an alternative we can suggest.

> TODO: 如果有一个 `future<int>` 变量，一旦我们执行 get() 或 then()，它就会变得无效，我们需要将值存储在其他地方。想想有没有别的办法

# Fibers
Seastar continuations are normally short, but often chained to one another, so that one continuation does a bit of work and then schedules another continuation for later. Such chains can be long, and often even involve loopings - see the following section, "Loops". We call such chains "fibers" of execution.

> Seastar 的 continuation 通常很短，但经常相互链接，因此一个 continuation 会做一些工作，然后安排另一个 continuation 供以后使用。这样的 continuation 链可以很长，甚至经常包含循环(参阅下一节 **Loops**)。我们称这种链为执行的 "fibers".

These fibers are not threads - each is just a string of continuations - but they share some common requirements with traditional threads.  For example, we want to avoid one fiber getting starved while a second fiber continuously runs its continuations one after another.  As another example, fibers may want to communicate - e.g., one fiber produces data that a second fiber consumes, and we wish to ensure that both fibers get a chance to run, and that if one stops prematurely, the other doesn't hang forever.  

> 这些 fibers 不是线程，每个都只是一串 continuations, 但它们与传统线程有一些共同的要求。例如，我们希望避免一个 fiber 在持续的运行它的 continuation 而另一个 fiber 被 "饿死" 的情况。另一个场景是，fiber 可能想要进行通信（例如一个 fiber 产生的数据被另一个 fiber 消费），我们希望确保两个 fiber 都有机会运行，如果一个过早停止，另一个不会永远挂起。

TODO: Mention fiber-related sections like loops, semaphores, gates, pipes, etc.

> TODO: 与 fiber 相关的部分，如 loops, semaphores, gates, pipes 等.

# Loops
A majority of time-consuming computations involve using loops. Seastar provides several primitives for expressing them in a way that composes nicely with the future/promise model. A very important aspect of Seastar loop primitives is that each iteration is followed by a preemption point, thus allowing other tasks to run inbetween iterations.

> 大多数耗时的计算都涉及到循环。Seastar 提供了几个原语，以一种与 future/promise 模型很好地结合的方式来表达它们。Seastar 循环原语的一个非常重要的方面是，每次迭代之后都有一个**抢占点**，因此允许其他任务在迭代之间运行。

## repeat
A loop created with `repeat` executes its body until it receives a `stop_iteration` object, which informs if the iteration should continue (`stop_iteration::no`) or stop (`stop_iteration::yes`). Next iteration will be launched only after the first one has finished. The loop body passed to `repeat` is expected to have a `future<stop_iteration>` return type.

> 用 `repeat` 创建的循环执行它的主体函数，直到它收到一个 `stop_iteration` 对象，该对象通知迭代是否应该继续(`stop_iteration::no`)或停止(`stop_iteration::yes`)。只有在第一个迭代完成之后才会启动下一个迭代。传递给 `repeat` 的循环体应该有一个 `future<stop_iteration>` 的返回类型。 

```cpp
seastar::future<int> recompute_number(int number);

seastar::future<> push_until_100(seastar::lw_shared_ptr<std::vector<int>> queue, int element) {
    return seastar::repeat([queue, element] {
        if (queue->size() == 100) {
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        }
        return recompute_number(element).then([queue] (int new_element) {
            queue->push_back(element);
            return stop_iteration::no;
        });
    });
}
```

## do_until
Do until is a close relative of `repeat`, but it uses an explicitly passed condition to decide whether it should stop iterating. The above example could be expressed with `do_until` as follows:

> `do_until` 是 `repeat` 的近亲，它使用显式传递的条件来决定是否应该停止迭代。上面的例子可以用 `do_until` 表示如下:

```cpp
seastar::future<int> recompute_number(int number);

seastar::future<> push_until_100(seastar::lw_shared_ptr<std::vector<int>> queue, int element) {
    return seastar::do_until([queue] { return queue->size() == 100; }, [queue, element] {
        return recompute_number(element).then([queue] (int new_element) {
            queue->push_back(new_element);
        });
    });
}
```
Note that the loop body is expected to return a `future<>`, which allows composing complex continuations inside the loop.

> 注意，循环体被期望返回 `future<>`，这允许在循环内部组合复杂的 continuations。

## do_for_each
A `do_for_each` is an equivalent of a `for` loop in Seastar world. It accepts a range (or a pair of iterators) and a function body, which it applies to each argument, in order, one by one. The next iteration will be launched only after the first one has finished, as was the case with `repeat`. As usual, `do_for_each` expects its loop body to return a `future<>`.

> `do_for_each` 相当于 Seastar 中的 `for` 循环。它接受一个范围(或一对迭代器)和一个函数体，并依次应用于每个实参。下一个迭代只有在第一个迭代完成后才会启动，就像 `repeat` 一样。和往常一样，`do_for_each` 期望它的循环体返回 `future<>`.

```cpp
seastar::future<> append(seastar::lw_shared_ptr<std::vector<int>> queue1, seastar::lw_shared_ptr<std::vector<int>> queue2) {
    return seastar::do_for_each(queue2, [queue1] (int element) {
        queue1->push_back(element);
    });
}

seastar::future<> append_iota(seastar::lw_shared_ptr<std::vector<int>> queue1, int n) {
    return seastar::do_for_each(boost::make_counting_iterator<size_t>(0), boost::make_counting_iterator<size_t>(n), [queue1] (int element) {
        queue1->push_back(element);
    });
}
```
`do_for_each` accepts either an lvalue reference to a container or a pair of iterators. It implies that the responsibility to ensure that the container is alive during the whole loop execution belongs to the caller. If the container needs its lifetime prolonged, it can be easily achieved with `do_with`:

> `do_for_each` 接受容器的左值引用或者一对迭代器。这意味着确保容器在整个循环执行期间处于活动状态的责任属于调用者。如果容器需要延长生命周期，可以通过 `do_with` 轻松实现:

```cpp
seastar::future<> do_something(int number);

seastar::future<> do_for_all(std::vector<int> numbers) {
    // Note that the "numbers" vector will be destroyed as soon as this function
    // returns, so we use do_with to guarantee it lives during the whole loop execution:
    return seastar::do_with(std::move(numbers), [] (std::vector<int>& numbers) {
        return seastar::do_for_each(numbers, [] (int number) {
            return do_something(number);
        });
    });
}

```

## parallel_for_each
Parallel for each is a high concurrency variant of `do_for_each`. When using `parallel_for_each`, all iterations are queued simultaneously - which means that there's no guarantee in which order they finish their operations.

> `parallel_for_each` 是 `do_for_each` 的高并发变体。当使用 `parallel_for_each` 时，所有迭代都同时排队, 这意味着不能保证它们以什么顺序完成操作。

```cpp
seastar::future<> flush_all_files(seastar::lw_shared_ptr<std::vector<seastar::file>> files) {
    return seastar::parallel_for_each(files, [] (seastar::file f) {
        // file::flush() returns a future<>
        return f.flush();
    });
}
```
`parallel_for_each` is a powerful tool, as it allows spawning many tasks in parallel. It can be a great performance gain, but there are also caveats. First of all, too high concurrency may be troublesome - the details can be found in chapter **Limiting parallelism of loops**.
Secondly, take note that the order in which iterations will be executed within a `parallel_for_each` loop is arbitrary - if a strict ordering is needed, consider using `do_for_each` instead.

> `parallel_for_each` 是一个强大的工具，因为它允许并行生成多个任务。它可以带来很大的性能提升，但也有一些需要注意的地方。首先，太高的并发性可能会带来麻烦, 详细信息可以在 **Limiting parallelism of loops** 章节中找到。
>
> 其次，请注意在 `parallel_for_each` 循环中执行迭代的顺序是任意的, 如果需要严格的顺序，可以考虑使用 `do_for_each` 代替。

TODO: map_reduce, as a shortcut (?) for parallel_for_each which needs to produce some results (e.g., logical_or of boolean results), so we don't need to create a lw_shared_ptr explicitly (or do_with).

> TODO: map_reduce，作为 `parallel_for_each` 的快捷方式(?)，它需要产生一些结果(例如，布尔结果的 logical_or)，所以我们不需要显式创建lw_shared_ptr(或do_with)。

TODO: See seastar commit "input_stream: Fix possible infinite recursion in consume()" for an example on why recursion is a possible, but bad, replacement for repeat(). See also my comment on https://groups.google.com/d/msg/seastar-dev/CUkLVBwva3Y/3DKGw-9aAQAJ on why Seastar's iteration primitives should be used over tail call optimization.

> TODO: 参见 seastar commit "input_stream: Fix possible infinite recursion in consume()" 的例子，说明为什么递归是 `repeat()` 的一种可能但糟糕的替代方法。关于为什么 Seastar 的迭代原语应该在尾部调用优化中使用，请参见我在 `https://groups.google.com/d/msg/seastar-dev/CUkLVBwva3Y/3DKGw-9aAQAJ` 上的评论。

# when_all: Waiting for multiple futures
Above we've seen `parallel_for_each()`, which starts a number of asynchronous operations, and then waits for all to complete. Seastar has another idiom, `when_all()`, for waiting for several already-existing futures to complete.

> 上面我们已经看到 `parallel_for_each()`，它启动了一些异步操作，然后等待所有操作完成。Seastar 还有另一个方法 `when_all()`，用于等待几个已经存在的 future 完成。

The first variant of `when_all()` is variadic, i.e., the futures are given as separate parameters, the exact number of which is known at compile time. The individual futures may have different types. For example,

> `when_all()` 的第一个参数是可变的，即 `future` 作为单独的参数给出，其确切数量在编译时是已知的。个别 `future` 可能有不同的类型。例如，

```cpp
#include <seastar/core/sleep.hh>

future<> f() {
    using namespace std::chrono_literals;
    future<int> slow_two = sleep(2s).then([] { return 2; });
    return when_all(sleep(1s), std::move(slow_two), 
                    make_ready_future<double>(3.5)
           ).discard_result();
}
```

This starts three futures - one which sleeps for one second (and doesn't return anything), one which sleeps for two seconds and returns the integer 2, and one which returns the double 3.5 immediately - and then waits for them. The `when_all()` function returns a future which resolves as soon as all three futures resolves, i.e., after two seconds. This future also has a value, which we shall explain below, but in this example, we simply waited for the future to resolve and discarded its value.

> 这里启动了三个 future（一个休眠一秒钟，并且不返回任何内容，一个休眠两秒钟并返回整数 2，以及一个立即返回双精度浮点数 3.5），然后等待它们就绪。该 `when_all()` 函数返回一个 `future`，它在所有三个 `future` 就绪后立即就绪，即两秒后。这个 `future` 也有一个值，我们将在下面解释，但在这个例子中，我们只是等待 future 就绪并丢弃它的值。

Note that `when_all()` accept only rvalues, which can be temporaries (like the return value of an asynchronous function or `make_ready_future`) or an `std::move()`'ed variable holding a future.

> 请注意，`when_all()` 只接受右值，它可以是临时的（如异步函数的返回值或 `make_ready_future`）或 `std::move()` 持有的 `future` 。

The future returned by `when_all()` resolves to a tuple of futures which are already resolved, and contain the results of the three input futures. Continuing the above example,

> 由 `when_all()` 返回的 `future` 为已经就绪的 `future` 元组，并包含三个输入 `future` 的结果。继续上面的例子，

```cpp
future<> f() {
    using namespace std::chrono_literals;
    future<int> slow_two = sleep(2s).then([] { return 2; });
    return when_all(sleep(1s), std::move(slow_two),
                    make_ready_future<double>(3.5)
           ).then([] (auto tup) {
            std::cout << std::get<0>(tup).available() << "\n";
            std::cout << std::get<1>(tup).get0() << "\n";
            std::cout << std::get<2>(tup).get0() << "\n";
    });
}
```

The output of this program (which comes after two seconds) is `1, 2, 3.5`: the first future in the tuple is available (but has no value), the second has the integer value 2, and the third a double value 3.5 - as expected.

> 该程序的输出（两秒后）是 1, 2, 3.5：元组中的第一个 future 可用（但没有值），第二个具有整数值 2，第三个是双精度值 3.5, 正如预期的那样。

One or more of the waited futures might resolve in an exception, but this does not change how `when_all()` works: It still waits for all the futures to resolve, each with either a value or an exception, and in the returned tuple some of the futures may contain an exception instead of a value. For example,

> 一个或多个等待的 `future` 可能会在异常中就绪，但这不会改变 `when_all()` 工作方式：它仍然等待所有 future 就绪，每个 future 都有一个值或一个异常，并且在返回的元组中，一些 future 可能包含异常而不是值。例如，

```cpp
future<> f() {
    using namespace std::chrono_literals;
    future<> slow_success = sleep(1s);
    future<> slow_exception = sleep(2s).then([] { throw 1; });
    return when_all(std::move(slow_success), std::move(slow_exception)
           ).then([] (auto tup) {
            std::cout << std::get<0>(tup).available() << "\n";
            std::cout << std::get<1>(tup).failed() << "\n";
            std::get<1>(tup).ignore_ready_future();
    });
}
```

Both futures are `available()` (resolved), but the second has `failed()` (resulted in an exception instead of a value). Note how we called `ignore_ready_future()` on this failed future, because silently ignoring a failed future is considered a bug, and will result in an "Exceptional future ignored" error message. More typically, an application will log the failed future instead of ignoring it.

> 两个 `future` 都 `available()`（已就绪），但第二个 `failed()`（异常而不是值）。注意我们如何在这个失败的 `future` 上调用 `ignore_ready_future()`，因为默默地忽略失败的 `future` 被认为是一个错误，并将导致 "Exceptional future ignored" 错误消息。更典型的是，应用程序将记录失败的 `future` 而不是忽略它。

The above example demonstrate that `when_all()` is inconvenient and verbose to use properly. The results are wrapped in a tuple, leading to verbose tuple syntax, and uses ready futures which must all be inspected individually for an exception to avoid error messages. 

> 上面的例子表明正确使用 `when_all()` 是不方便和冗长的。结果被包装在一个元组中，导致冗长的元组语法，要获取所有就绪的 `future`，必须单独检查所有的异常以避免错误消息。

So Seastar also provides an easier to use `when_all_succeed()` function. This function too returns a future which resolves when all the given futures have resolved. If all of them succeeded, it passes the resulting values to continuation, without wrapping them in futures or a tuple. If, however, one or more of the futures failed, `when_all_succeed()` resolves to a failed future, containing the exception from one of the failed futures. If more than one of the given future failed, one of those will be passed on (it is unspecified which one is chosen), and the rest will be silently ignored. For example,

> 所以 Seastar 也提供了一个更容易使用的 `when_all_succeed()` 方法。此函数也返回一个 future，当所有给定的 future 都已经就绪时，该 future 将就绪。如果它们都成功了，它将结果值传递给 continuation, 而不将它们包装在 future 或元组中。但是如果一个或多个 future 失败，则 `when_all_succeed()` 解析为失败的 future, 其中包含来自失败 `future` 之一的异常。如果给定的 `future` 不止一个失败，其中一个将被传递（未指定选择哪一个），其余的将被静默忽略。例如，

```cpp
using namespace seastar;
future<> f() {
    using namespace std::chrono_literals;
    return when_all_succeed(sleep(1s), make_ready_future<int>(2),
                    make_ready_future<double>(3.5)
            ).then([] (int i, double d) {
        std::cout << i << " " << d << "\n";
    });
}
```

Note how the integer and double values held by the futures are conveniently passed, individually (without a tuple) to the continuation. Since `sleep()` does not contain a value, it is waited for, but no third value is passed to the continuation. That also means that if we `when_all_succeed()` on several `future<>` (without a value), the result is also a `future<>`:

> 请注意，future 持有的整数和双精度值是如何方便地单独（没有使用元组）传递给 continuation 的。由于 `sleep()` 不包含值，因此没有值传递给 continuation。这也意味着在几个没有值的 `future<>` 上执行 `when_all_succeed()`, 结果也是一个 `future<>`:

```cpp
using namespace seastar;
future<> f() {
    using namespace std::chrono_literals;
    return when_all_succeed(sleep(1s), sleep(2s), sleep(3s));
}
```

This example simply waits for 3 seconds (the maximum of 1, 2 and 3 seconds).

> 此示例仅等待 3 秒（最大值为 1、2 和 3 秒）。

An example of `when_all_succeed()` with an exception:

> `when_all_succeed()` 处理异常的一个例子：

```cpp
using namespace seastar;
future<> f() {
    using namespace std::chrono_literals;
    return when_all_succeed(make_ready_future<int>(2),
                    make_exception_future<double>("oops")
            ).then([] (int i, double d) {
        std::cout << i << " " << d << "\n";
    }).handle_exception([] (std::exception_ptr e) {
        std::cout << "exception: " << e << "\n";
    });
}
```

In this example, one of the futures fails, so the result of `when_all_succeed` is a failed future, so the normal continuation is not run, and the `handle_exception()` continuation is done.

> 在这个例子中，有一个 future 失败了，所以 `when_all_succeed` 的结果是一个失败的 future，因此正常的 continuation 没有运行，而是运行 `handle_exception()` 的 `continuation` 。

TODO: also explain `when_all` and `when_all_succeed` for vectors.

> TODO: 解释 vector 的 `when_all` 和 `when_all_succeed`.

# Semaphores
Seastar's semaphores are the standard computer-science semaphores, adapted for futures. A semaphore is a counter into which you can deposit units or take them away. Taking units from the counter may wait if not enough units are available.

> Seastar 的信号量是标准的计算机科学信号量，适用于 `future`。信号量是一个计数器，你可以在其中存放或取走单元。如果没有足够的单元可用，从计数器取单元可能会等待。

## Limiting parallelism with semaphores
The most common use for a semaphore in Seastar is for limiting parallelism, i.e., limiting the number of instances of some code which can run in parallel. This can be important when each of the parallel invocations uses a limited resource (e.g., memory) so letting an unlimited number of them run in parallel can exhaust this resource.

> 在 Seastar 中，信号量最常见的用途是限制并行性，即限制可以并行运行的某些代码的实例数量。当每个并行调用使用有限的资源(例如，内存)时，这一点非常重要，因为让无限多个并行调用并行运行会耗尽此资源。

Consider a case where an external source of events (e.g., an incoming network request) causes an asynchronous function ```g()``` to be called. Imagine that we want to limit the number of concurrent ```g()``` operations to 100. I.e., If g() is started when 100 other invocations are still ongoing, we want it to delay its real work until one of the other invocations has completed. We can do this with a semaphore:

> 考虑一个外部事件(例如，传入的网络请求)导致调用异步函数 `g()` 的情况。假设我们希望将并发执行 `g()` 的数量限制为 100。也就是说，如果已经有 100 个 g() 调用在运行，再启动一个新的 g() 调用，我们希望它延迟它的实际工作，直到其中一个其他调用完成。那么可以通过一个信号量来做到这一点:

```cpp
seastar::future<> g() {
    static thread_local seastar::semaphore limit(100);
    return limit.wait(1).then([] {
        return slow(); // do the real work of g()
    }).finally([] {
        limit.signal(1);
    });
}
```

In this example, the semaphore starts with the counter at 100. The asynchronous operation `slow()` is only started when we can reduce the counter by one (`wait(1)`), and when `slow()` is done, either successfully or with exception, the counter is increased back by one (```signal(1)```). This way, when 100 operations have already started their work and have not yet finished, the 101st operation will wait, until one of the ongoing operations finishes and returns a unit to the semaphore. This ensures that at each time we have at most 100 concurrent `slow()` operations running in the above code.

> 在本例中，信号量计数器数量限制为100。异步操作 `slow()` 只有在成功获取到一个信号量（`limit.wait(1)`）之后才会启动，当 `slow()` 完成时，无论成功还是异常，计数器都会增加 1 (`limit.signal(1)`)。这样，当 100 个 `slow()` 操作已经开始工作并且还没有结束时，第 101 个操作将等待，直到其中一个正在进行的操作结束并向信号量计数器返回一个信号量。这确保了每次我们在上面的代码中最多有 100 个并发的 `slow()` 操作在运行。

Note how we used a ```static thread_local``` semaphore, so that all calls to ```g()``` from the same shard count towards the same limit; As usual, a Seastar application is sharded so this limit is separate per shard (CPU thread). This is usually fine, because sharded applications consider resources to be separate per shard.

> 注意我们如何使用 `static thread_local` 信号量，以便使用相同的信号量计数器对 `g()` 的所有调用达到相同的限制; 通常, Seastar 应用程序是分片的，因此每个分片(CPU线程)的限制是独立的。这通常是好的，因为分片应用程序认为每个分片的资源是独立的。

Luckily, the above code happens to be exception safe: `limit.wait(1)` can throw an exception when it runs out of memory (keeping a list of waiters), and in that case the semaphore counter is not decreased but the continuations below are not run so it is not increased either. `limit.wait(1)` can also return an exceptional future when the semaphore is *broken* (we'll discuss this later) but in that case the extra `signal()` call is ignored. Finally, `slow()` may also throw, or return an exceptional future, but the `finally()` ensures the semaphore is still increased.

> 幸运的是，上面的代码碰巧是异常安全的: `limit.wait(1)` 可以在内存耗尽时抛出异常(维护一个等待者列表)，在这种情况下，信号量计数器不会减少，但下面的 continuation 不会运行，所以它也不会增加。`limit.wait(1)` 也可以在信号量 *broken* 时返回异常的 future(稍后讨论)，但在这种情况下额外的 `signal()` 调用会被忽略。最后, `slow()` 也可能抛出或返回异常的 future，但 `finally()` 确保信号量仍然增加。

However, as the application code becomes more complex, it becomes harder to ensure that we never forget to call `signal()` after the operation is done, regardless of which code path or exceptions happen. As an example of what might go wrong, consider the following *buggy* code snippet, which differs subtly from the above one, and also appears, on first sight, to be correct:

> 然而，随着应用程序代码变得越来越复杂，很难确保在操作完成后不忘记调用 `signal()`, 无论发生哪个代码路径或异常。作为一个可能出错的例子，考虑下面的代码片段，它与上面的代码片段略有不同，乍一看也是正确的:

```cpp
seastar::future<> g() {
    static thread_local seastar::semaphore limit(100);
    return limit.wait(1).then([] {
        return slow().finally([] { limit.signal(1); });
    });
}
```

But this version is **not** exception safe: Consider what happens if `slow()` throws an exception before returning a future (this is different from `slow()` returning an exceptional future - we discussed this difference in the section about exception handling). In this case, we decreased the counter, but the `finally()` will never be reached, and the counter will never be increased back. There is a way to fix this code, by replacing the call to `slow()` with `seastar::futurize_invoke(slow)`. But the point we're trying to make here is not how to fix buggy code, but rather that by using the separate `semaphore::wait()` and `semaphore::signal()` functions, you can very easily get things wrong.

> 但是这个版本不是异常安全的: 考虑一下如果 `slow()` 在返回 future 之前抛出异常会发生什么情况(这与 `slow()` 返回异常 future 不同，我们在关于异常处理的部分讨论了这种差异)。在本例中，我们减少了计数器，但永远不会到达 `finally()`, 计数器也永远不会增加回来。有一个方法可以修复这个代码，用 `seastar::futurize_invoke(slow)` 替换对 `slow()` 的调用。但我们在这里试图说明的重点不是如何修复有 bug 的代码，而是通过使用单独的 `semaphore::wait()` 和 `semaphore::signal()` 函数，你很容易出错。

For exception safety, in C++ it is generally not recommended to have separate resource acquisition and release functions.  Instead, C++ offers safer mechanisms for acquiring a resource (in this case seamphore units) and later releasing it: lambda functions, and RAII ("resource acquisition is initialization"):

> 为了异常安全，在 C++ 中通常不建议使用独立的资源获取和释放函数。相反 C++ 提供了更安全的机制来获取资源(在本例中是 seamphore)并随后释放它: lambda 函数和 RAII(“资源获取是初始化”):

The lambda-based solution is a function ```seastar::with_semaphore()``` which is a shortcut for the code in the examples above:

> 基于 lambda 的解决方案是一个函数 `seastar::with_semaphore()`, 它是上面例子中代码的快捷方式:

```cpp
seastar::future<> g() {
    static thread_local seastar::semaphore limit(100);
    return seastar::with_semaphore(limit, 1, [] {
        return slow(); // do the real work of g()
    });
}
```

`with_semaphore()`, like the earlier code snippets, waits for the given number of units from the semaphore, then runs the given lambda, and when the future returned by the lambda is resolved, `with_semaphore()` returns back the units to the semaphore. `with_semaphore()` returns a future which only resolves after all these steps are done.

> 与前面的代码片段一样, `with_semaphore()` 等待特定数量的信号量，然后运行 lambda. 当 lambda 返回的 future 就绪时, `with_semaphore()` 将信号量返回给信号量计数器. `with_semaphore()` 返回一个只有在所有这些步骤完成后才会就绪的 future.

The function `seastar::get_units()` is more general. It provides an exception-safe alternative to `seastar::semaphore`'s separate `wait()` and `signal()` methods, based on C++'s RAII philosophy: The function returns an opaque units object, which while held, keeps the semaphore's counter decreased - and as soon as this object is destructed, the counter is increased back. With this interface you cannot forget to increase the counter, or increase it twice, or increase without decreasing: The counter will always be decreased once when the units object is created, and if that succeeded, increased when the object is destructed. When the units object is moved into a continuation, no matter how this continuation ends, when the continuation is destructed, the units object is destructed and the units are returned to the semaphore's counter. The above examples, written with `get_units()`, looks like this:

> 函数 `seastar::get_units()` 更为通用。它提供了一个异常安全的替代方法来替代 `seastar::semaphore` 的独立的 `wait()` 和 `signal()` 方法，基于 C++ 的RAII 哲学: 该函数返回一个不透明的单元对象，该对象被持有时，保持信号量的计数器减少, 一旦该对象被销毁，计数器就会增加回来。在这个界面中，你不能忘记增加计数器，或增加两次，或增加而不减少, 当对象创建时，计数器总是减少一次，如果成功，则在对象销毁时增加。当对象移动到 continuation 中时，无论这 continuation 如何结束，当 continuation 被析构时，对象将被析构，并将单元返回给信号量的计数器。上面的例子，用 `get_units()` 写的，看起来像这样:

```cpp
seastar::future<> g() {
    static thread_local semaphore limit(100);
    return seastar::get_units(limit, 1).then([] (auto units) {
        return slow().finally([units = std::move(units)] {});
    });
}
```

Note the somewhat convoluted way that `get_units()` needs to be used: The continuations must be nested because we need the `units` object to be moved to the last continuation. If `slow()` returns a future (and does not throw immediately),  the `finally()` continuation captures the `units` object until everything is done, but does not run any code.

> 注意使用 `get_units()` 的方式有些复杂: continuation 必须嵌套，因为我们需要将 `units` 对象移动到最后一个 continuation. 如果 `slow()` 返回一个 future(不抛异常), `finally()` continuation 会捕获 `units` 对象，直到一切都完成，但不运行任何代码。

Seastars programmers should generally avoid using the the `seamphore::wait()` and `semaphore::signal()` functions directly, and always prefer either `with_semaphore()` (when applicable) or `get_units()`.

> Seastars 程序员通常应该避免直接使用 `seamphore::wait()` 和 `semaphore::signal()` 函数，而总是选择 `with_semaphore()` (如果适用)或 `get_units()`


## Limiting resource use
Because semaphores support waiting for any number of units, not just 1, we can use them for more than simple limiting of the *number* of parallel invocation. For example, consider we have an asynchronous function ```using_lots_of_memory(size_t bytes)```, which uses ```bytes``` bytes of memory, and we want to ensure that not more than 1 MB of memory is used by all parallel invocations of this function --- and that additional calls are delayed until previous calls have finished. We can do this with a semaphore:

> 因为 semaphores 支持等待任意数量的单元，而不仅仅是等待 1 个单元，所以我们可以将它们用于多种用途，而不仅仅是简单地限制并行调用的数量。例如，考虑我们有一个异步函数 `using_lots_of_memory(size_t bytes)`, 它使用 `bytes` 字节的内存，我们希望确保该函数的所有并行调用使用的内存不超过 1mb, 并且额外的调用被延迟到之前的调用结束。我们可以通过信号量来做到这一点:

```cpp
seastar::future<> using_lots_of_memory(size_t bytes) {
    static thread_local seastar::semaphore limit(1000000); // limit to 1MB
    return seastar::with_semaphore(limit, bytes, [bytes] {
        // do something allocating 'bytes' bytes of memory
    });
}
```

Watch out that in the above example, a call to `using_lots_of_memory(2000000)` will return a future that never resolves, because the semaphore will never contain enough units to satisfy the semaphore wait. `using_lots_of_memory()` should probably check whether `bytes` is above the limit, and throw an exception in that case. Seastar doesn't do this for you.

> 注意，在上面的例子中，对 `using_lots_of_memory(2000000)` 的调用将返回一个永远不会就绪的 future, 因为信号量永远不会包含足够的单元来满足信号量等待。`using_lots_of_memory()` 可能应该检查 `bytes` 是否超过限制，并在这种情况下抛出异常。Seastar 不会为你做这些。


## Limiting parallelism of loops
Above, we looked at a function `g()` which gets called by some external event, and wanted to control its parallelism. In this section, we look at parallelism of loops, which also can be controlled with semaphores.

> 上面，我们看了一个函数 `g()`， 它被一些外部事件调用，并想要控制它的并行性。在本节中，我们将讨论循环的并行性，这也可以通过信号量来控制。

Consider the following simple loop:

```cpp
#include <seastar/core/sleep.hh>
seastar::future<> slow() {
    std::cerr << ".";
    return seastar::sleep(std::chrono::seconds(1));
}
seastar::future<> f() {
    return seastar::repeat([] {
        return slow().then([] { return seastar::stop_iteration::no; });
    });
}
```

This loop runs the ```slow()``` function (taking one second to complete) without any parallelism --- the next ```slow()``` call starts only when the previous one completed. But what if we do not need to serialize the calls to ```slow()```, and want to allow multiple instances of it to be ongoing concurrently?

> 这个循环运行 `slow()` 函数(需要一秒钟来完成)，没有任何并行性。下一个 `slow()` 调用只有在前一个调用完成时才会启动。但是，如果我们不需要序列化对 `slow()` 的调用，并且希望允许它的多个实例同时进行，该怎么办呢？

Naively, we could achieve more parallelism, by starting the next call to ```slow()``` right after the previous call --- ignoring the future returned by the previous call to ```slow()``` and not waiting for it to resolve:

> 我们可以在前一个调用之后开始下一个对 `slow()` 的调用来获得更多的并行性 --- 忽略前一个对 `slow()` 的调用所返回的 future，而不等待它就绪.

```cpp
seastar::future<> f() {
    return seastar::repeat([] {
        slow();
        return seastar::stop_iteration::no;
    });
}
```

But in this loop, there is no limit to the amount of parallelism --- millions of ```sleep()``` calls might be active in parallel, before the first one ever returned. Eventually, this loop may consume all available memory and crash.

> 但是在这个循环中，并行性的数量是没有限制的, 在第一个调用返回之前，数百万个 `sleep()` 可能是并行活动的。最终，这个循环可能消耗所有可用的内存并崩溃。

Using a semaphore allows us to run many instances of ```slow()``` in parallel, but limit the number of these parallel instances to, in the following example, 100:

> 使用信号量允许我们并行运行 `slow()` 的多个实例，但将这些并行实例的数量限制在以下示例中的100个:

```cpp
seastar::future<> f() {
    return seastar::do_with(seastar::semaphore(100), [] (auto& limit) {
        return seastar::repeat([&limit] {
            return limit.wait(1).then([&limit] {
                seastar::futurize_invoke(slow).finally([&limit] {
                    limit.signal(1); 
                });
                return seastar::stop_iteration::no;
            });
        });
    });
}
```

Note how this code differs from the code we saw above for limiting the number of parallel invocations of a function `g()`:
1. Here we cannot use a single `thread_local` semaphore. Each call to `f()` has its loop with parallelism of 100, so needs its own semaphore "`limit`", kept alive during the loop with `do_with()`.
2. Here we do not wait for `slow()` to complete before continuing the loop, i.e., we do not `return` the future chain starting at `futurize_invoke(slow)`. The loop continues to the next iteration when a semaphore unit becomes available, while (in our example) 99 other operations might be ongoing in the background and we do not wait for them.

> 注意这段代码与我们上面看到的限制函数 `g()` 并行调用次数的代码有何不同:
>
> 1. 在这里，我们不能使用单一的 `thread_local` 信号量。每个对 `f()` 的调用都有其并行度为 100 的循环，因此需要它自己的信号量 `limit`，在使用 `do_with()` 的循环期间保持活跃。
> 2. 这里我们不等待 `slow()` 完成后再继续循环，也就是说，我们不 `return` 从 `futurize_invoke(slow)` 开始的 future 链。当一个信号量单元可用时，循环继续到下一个迭代，而(在我们的例子中)其他99个操作可能正在后台进行，我们不等待它们。

In the examples in this section, we cannot use the `with_semaphore()` shortcut. `with_semaphore()` returns a future which only resolves after the lambda's returned future resolves. But in the above example, the loop needs to know when just the semaphore units are available, to start the next iteration --- and not wait for the previous iteration to complete. We could not achieve that with `with_semaphore()`. But the more general exception-safe idiom, `seastar::get_units()`, can be used in this case, and is recommended:

> 在本节的示例中，我们不能使用 `with_semaphore()` 快捷方式。`with_semaphore()` 返回一个 future, 该 future 只在 lambda 返回的 future 解析后才解析。但在上面的例子中，循环需要知道何时只有信号量单元可用，以便开始下一个迭代 -- 而不是等待前一个迭代完成。我们无法通过 `with_semaphore()` 实现这一点。但在这种情况下，可以使用更通用的异常安全习惯用法 `sestar::get_units()`，建议使用:


```cpp
seastar::future<> f() {
    return seastar::do_with(seastar::semaphore(100), [] (auto& limit) {
        return seastar::repeat([&limit] {
    	    return seastar::get_units(limit, 1).then([] (auto units) {
	            slow().finally([units = std::move(units)] {});
	            return seastar::stop_iteration::no;
	        });
        });
    });
}
```

The above examples are not realistic, because they have a never-ending loop and the future returned by `f()` will never resolve. In more realistic cases, the loop has an end, and at the end of the loop we need to wait for all the background operations which the loop started. We can do this by ```wait()```ing on the original count of the semaphore: When the full count is finally available, it means that *all* the operations have completed. For example, the following loop ends after 456 iterations:

> 上面的例子是不现实的，因为它们有一个永不结束的循环, `f()` 返回的 future 将永远无法解析。在更实际的情况下，循环有一个结束，在循环结束时，我们需要等待循环开始的所有后台操作。我们可以通过 `wait()` 获取信号量的原始计数来实现这一点: 当完整计数最终可用时，意味着所有操作已经完成。例如，下面的循环在456次迭代后结束:

```cpp
seastar::future<> f() {
    return seastar::do_with(seastar::semaphore(100), [] (auto& limit) {
        return seastar::do_for_each(boost::counting_iterator<int>(0),
                boost::counting_iterator<int>(456), [&limit] (int i) {
            return seastar::get_units(limit, 1).then([] (auto units) {
                slow().finally([units = std::move(units)] {});
	        });
        }).finally([&limit] {
            return limit.wait(100);
        });
    });
}
````

The last `finally` is what ensures that we wait for the last operations to complete: After the `repeat` loop ends (whether successfully or prematurely because of an exception in one of the iterations), we do a `wait(100)` to wait for the semaphore to reach its original value 100, meaning that all operations that we started have completed. Without this `finally`, the future returned by `f()` will resolve *before* all the iterations of the loop actually completed (the last 100 may still be running).

> 最后一个 `finally` 是确保我们等待最后一个操作完成的: 在 `repeat` 循环结束后(无论成功还是由于某个迭代中的异常而过早地结束)，我们执行 `wait(100)` 来等待信号量达到其初始值 100，这意味着我们开始的所有操作都已完成。如果没有这个 `finally`, `f()` 返回的 future 将在*实际完成所有循环迭代之前解析*(最后100次可能仍在运行)。

In the idiom we saw in the above example, the same semaphore is used both for limiting the number of background operations, and later to wait for all of them to complete. Sometimes, we want several different loops to use the same semaphore to limit their *total* parallelism. In that case we must use a separate mechanism for waiting for the completion of the background operations started by the loop. The most convenient way to wait for ongoing operations is using a gate, which we will describe in detail later. A typical example of a loop whose parallelism is limited by an external semaphore:

> 在我们在上面的例子中看到的习惯用法中，相同的信号量既用于限制后台操作的数量，又用于稍后等待所有这些操作完成。有时，我们希望几个不同的循环使用相同的信号量来限制它们的 “总” 并行度。在这种情况下，我们必须使用一个单独的机制来等待由循环启动的后台操作的完成。等待正在进行的操作的最方便的方法是使用 gate, 我们将在后面详细描述。一个典型的循环例子，其并行度受外部信号量的限制:

```cpp
thread_local seastar::semaphore limit(100);
seastar::future<> f() {
    return seastar::do_with(seastar::gate(), [] (auto& gate) {
        return seastar::do_for_each(boost::counting_iterator<int>(0),
                boost::counting_iterator<int>(456), [&gate] (int i) {
            return seastar::get_units(limit, 1).then([&gate] (auto units) {
                gate.enter();
                seastar::futurize_invoke(slow).finally([&gate, units = std::move(units)] {
                    gate.leave();
                });
	        });
        }).finally([&gate] {
            return gate.close();
        });
    });
}
```
In this code, we use the external semaphore `limit` to limit the number of concurrent operations, but additionally have a gate specific to this loop to help us wait for all ongoing operations to complete.

> 在这段代码中，我们使用外部信号量 `limit` 来限制并发操作的数量，但另外有一个针对这个循环的 gate 来帮助我们等待所有正在进行的操作完成。

TODO: also allow `get_units()` or something similar on a gate, and use that instead of the explicit gate.enter/gate.leave.

> TODO: 也允许 `get_units()` 或类似的东西在 gate 上，并使用它来代替显式的 gate.enter/gate.leave。

TODO: say something about semaphore fairness - if someone is waiting for a lot of units and later someone asks for 1 unit, will both wait or will the request for 1 unit be satisfied?

>TODO: 谈谈信号量的公平性 -- 如果有人在等待很多单元，然后有人请求1个单元，是两者都在等待，还是1个单元的请求得到满足？

TODO: say something about broken semaphores? (or in later section especially about breaking/closing/shutting down/etc?)

> TODO: 说说破碎信号量吧？(或者在后面的章节中，特别是关于打破/关闭/关闭/等等?)

TODO: Have a few paragraphs, or even a section, on additional uses of semaphores. One is for mutual exclusion using semaphore(1) - we need to explain why although why in Seastar we don't have multiple threads touching the same data, if code is composed of different continuations (i.e., a fiber) it can switch to a different fiber in the middle, so if data needs to be protected between two continuations, it needs a mutex. Another example is something akin to wait_all: we start with a semaphore(0), run a known number N of asynchronous functions with finally sem.signal(), and from all this return the future sem.wait(N). PERHAPS even have a separate section on mutual exclusion, where we begin with semaphore(1) but also mention shared_mutex

> TODO: 用几段甚至一节来介绍信号量的其他用法。一个是使用信号量的互斥 -- 我们需要解释为什么尽管在 Seastar 中我们没有多个线程接触相同的数据，但如果代码是由不同的 continuation(例如，一个 fiber)组成的，它可以切换到中间的不同的 fiber，所以如果数据需要在两个 continuation 之间被保护，它需要一个互斥。另一个例子是类似于 wait_all 的东西: 我们从一个 semaphore(0) 开始，最后用 sem.signal() 运行已知数量的 N 个异步函数，从所有这些返回 future 的 sem.wait(N). 甚至可能有一个关于互斥的单独章节，我们从 semaphore(1) 开始，但也提到了shared_mutex

# Pipes
Seastar's `pipe<T>` is a mechanism to transfer data between two fibers, one producing data, and the other consuming it. It has a fixed-size buffer to ensures a balanced execution of the two fibers, because the producer fiber blocks when it writes to a full pipe, until the consumer fiber gets to run and read from the pipe.

> Seastar 的 `pipe<T>` 是一种在两个 fibers 之间传输数据的机制，一个生产数据，另一个消耗数据。它有一个固定大小的缓冲区，以确保两种 fibers 的平衡执行，因为当生产者写入一个满的 pipe 时，它会阻塞，直到消费者开始运行并从 pipe 读取。

A `pipe<T>` resembles a Unix pipe, in that it has a read side, a write side, and a fixed-sized buffer between them, and supports either end to be closed independently (and EOF or broken pipe when using the other side). A `pipe<T>` object holds the reader and write sides of the pipe as two separate objects. These objects can be moved into two different fibers.  Importantly, if one of the pipe ends is destroyed (i.e., the continuations capturing it end), the other end of the pipe will stop blocking, so the other fiber will not hang.

> `pipe<T>` 类似于 Unix 管道，它有读端、写端，它们之间有一个固定大小的缓冲区，并支持任意一端独立关闭(使用另一端时支持 EOF 或破裂的管道)。`pipe<T>` 对象保存 reader，并将管道的两端作为两个单独的对象写入。这些物体可以被移动到两种不同的 fibers 中。重要的是，如果管道的一端被破坏了(即，捕获该一端的延续)，管道的另一端将停止阻塞，因此另一端的 fiber 将不会挂起。

The pipe's read and write interfaces are future-based blocking. I.e., the write() and read() methods return a future which is fulfilled when the operation is complete. The pipe is single-reader single-writer, meaning that until the future returned by read() is fulfilled, read() must not be called again (and same for write).

> pipe 的读和写接口是基于 future 的阻塞。也就是说，write() 和 read() 方法返回当操作完成时实现的 future。该 pipe 是单读单写的，这意味着在 read() 返回的 future 完成之前，不能再次调用 read (write 也是如此).

Note: The pipe reader and writer are movable, but *not* copyable. It is often convenient to wrap each end in a shared pointer, so it can be copied (e.g., used in an std::function which needs to be copyable) or easily captured into multiple continuations.

> 注意: pipe 的 reader 和 writer 是可移动的，但不可复制。可以封装在共享指针中，这样它就可以被复制(例如，在需要可复制的 std::function 中使用)，或者很容易被捕获到多个 continuation 中。

# Shutting down a service with a gate
Consider an application which has some long operation `slow()`, and many such operations may be started at any time. A number of `slow()` operations may even even be active in parallel.  Now, you want to shut down this service, but want to make sure that before that, all outstanding operations are completed. Moreover, you don't want to allow new `slow()` operations to start while the shut-down is in progress.

> 考虑一个应用程序，它有一些长时间的 `slow()` 操作，并且许多这样的操作可能在任何时候启动。许多 `slow()` 操作甚至可能是并行运行的。现在你想要关闭该服务，但希望在关闭该服务之前，所有未完成的操作都已完成。此外，不允许在关闭过程中启动新的 `slow()`操作。

This is the purpose of a `seastar::gate`. A gate `g` maintains an internal counter of operations in progress. We call `g.enter()` when entering an operation (i.e., before running `slow()`), and call `g.leave()` when leaving the operation (when a call to `slow()` completed). The method `g.close()` *closes the gate*, which means it forbids any further calls to `g.enter()` (such attempts will generate an exception); Moreover `g.close()` returns a future which resolves when all the existing operations have completed. In other words, when `g.close()` resolves, we know that no more invocations of `slow()` can be in progress - because the ones that already started have completed, and new ones could not have started.

> 这就是 `seastar::gate` 的目的。gate `g` 维护正在进行的操作的内部计数器。我们在进入操作时调用 `g.enter()` (例如，在运行 `slow()` 之前)，在离开操作时调用 `g.leave()` (当调用 `slow()` 完成时)。方法 `g.close()` *close the gate*，这意味着它禁止重新调用 `g.enter()` (这种尝试将生成异常)；此外，`g.close()` 返回当所有现有操作完成时就绪的 future。换句话说，当 `g.close()` 被解析时，我们知道 `slow()` 的调用不能再进行了，因为已经开始的调用已经完成了，新的调用不可能已经开始。

The construct
```cpp
seastar::with_gate(g, [] { return slow(); })
```
can be used as a shortcut to the idiom

> `with_gate` 可以作为上面描述的 `g.enter` 和 `g.leave` 的一种简单实现

```cpp
g.enter();
slow().finally([&g] { g.leave(); });
```

Here is a typical example of using a gate:

```cpp
#include <seastar/core/sleep.hh>
#include <seastar/core/gate.hh>
#include <boost/iterator/counting_iterator.hpp>

seastar::future<> slow(int i) {
    std::cerr << "starting " << i << "\n";
    return seastar::sleep(std::chrono::seconds(10)).then([i] {
        std::cerr << "done " << i << "\n";
    });
}
seastar::future<> f() {
    return seastar::do_with(seastar::gate(), [] (auto& g) {
        return seastar::do_for_each(boost::counting_iterator<int>(1),
                boost::counting_iterator<int>(6),
                [&g] (int i) {
            seastar::with_gate(g, [i] { return slow(i); });
            // wait one second before starting the next iteration
            return seastar::sleep(std::chrono::seconds(1));
		}).then([&g] {
            seastar::sleep(std::chrono::seconds(1)).then([&g] {
                // This will fail, because it will be after the close()
                seastar::with_gate(g, [] { return slow(6); });
            });
            return g.close();
        });
    });
}
```

In this example, we have a function `future<> slow()` taking 10 seconds to complete. We run it in a loop 5 times, waiting 1 second between calls, and surround each call with entering and leaving the gate (using `with_gate`). After the 5th call, while all calls are still ongoing (because each takes 10 seconds to complete), we close the gate and wait for it before exiting the program. We also test that new calls cannot begin after closing the gate, by trying to enter the gate again one second after closing it.

> 在这个例子中，我们有一个需要 10 秒才能完成的函数 `future<> slow()`。我们在循环中运行 5 次，在两次调用之间等待 1 秒，并在每个调用周围加上进出的 gate（使用 `with_gate`）。在第 5 次调用之后，虽然所有调用仍在进行中（因为每个调用需要 10 秒才能完成），但我们关闭 `gate` 并等待它退出程序。我们还通过在关闭 gate 后一秒钟尝试再次进入 gate 来测试无法在关闭 gate 后开始新调用。

The output of this program looks like this:

> 该程序的输出如下所示：

```
starting 1
starting 2
starting 3
starting 4
starting 5
WARNING: exceptional future ignored of type 'seastar::gate_closed_exception': gate closed
done 1
done 2
done 3
done 4
done 5
```

Here, the invocations of `slow()` were started at 1 second intervals. After the "`starting 5`" message, we closed the gate and another attempt to use it resulted in a `seastar::gate_closed_exception`, which we ignored and hence this message. At this point the application waits for the future returned by `g.close()`. This will happen once all the `slow()` invocations have completed: Immediately after printing "`done 5`", the test program stops.

> 在这里， `slow()` 的调用以 1 秒的间隔开始。在 `starting 5` 消息之后，我们关闭了 `gate`，并且再次尝试使用它导致了 `seastar::gate_closed_exception`，我们忽略了它，因此出现了这条消息。此时应用程序等待由 `g.close()` 返回的 `future`。 这将在所有 `slow()` 调用完成后发生：打印 "done 5" 之后，测试程序立即停止.

As explained so far, a gate can prevent new invocations of an operation, and wait for any in-progress operations to complete. However, these in-progress operations may take a very long time to complete. Often, a long operation would like to know that a shut-down has been requested, so it could stop its work prematurely. An operation can check whether its gate was closed by calling the gate's `check()` method: If the gate is already closed, the `check()` method throws an exception (the same `seastar::gate_closed_exception` that `enter()` would throw at that point). The intent is that the exception will cause the operation calling it to stop at this point.

> 如上所述，gate 可以阻止对操作的新调用，并等待任何正在进行的操作完成。然而，这些正在进行的操作可能需要很长时间才能完成。通常，长时间操作希望知道已经请求了关闭，因此它可以提前停止工作。一个操作可以通过调用 gate 的 `check()` 方法来检查它的 gate 是否关闭：如果 gate 已经关闭，`check()` 方法会抛出一个异常（与 `enter()` 一样抛出 `seastar::gate_closed_exception`）。其目的是，异常将导致调用它的操作在此时停止。

In the previous example code, we had an un-interruptible operation `slow()` which slept for 10 seconds. Let's replace it by a loop of 10 one-second sleeps, calling `g.check()` each second:

> 在前面的示例代码中，我们有一个不可中断的操作 `slow()`，它休眠了 10 秒。让我们用 10 个 1 秒休眠的循环来替换它，每秒调用 `g.check()`:

```cpp
seastar::future<> slow(int i, seastar::gate &g) {
    std::cerr << "starting " << i << "\n";
    return seastar::do_for_each(boost::counting_iterator<int>(0),
                                boost::counting_iterator<int>(10),
            [&g] (int) {
        g.check();
        return seastar::sleep(std::chrono::seconds(1));
    }).finally([i] {
        std::cerr << "done " << i << "\n";
    });
}
```

Now, just one second after gate is closed (after the "starting 5" message is printed), all the `slow()` operations notice the gate was closed, and stop. As expected, the exception stops the `do_for_each()` loop, and the `finally()` continuation is performed so we see the "done" messages for all five operations.

> 现在，在 gate 关闭后仅仅一秒钟(在 "starting 5" 消息打印出来之后)，所有的 `slow()` 操作都通知 gate 关闭并停止。正如预期的那样，异常将停止 ` do_for_each()` 循环，并执行 `finally()` continuation，因此我们将看到所有五个操作的 "done" 消息。


# Introducing shared-nothing programming

TODO: Explain in more detail Seastar's shared-nothing approach where the entire memory is divided up-front to cores, malloc/free and pointers only work on one core.
TODO: Introduce our shared_ptr (and lw_shared_ptr) and sstring and say the standard ones use locked instructions which are unnecessary when we assume these objects (like all others) are for a single thread. Our futures and continuations do the same.


# More about Seastar's event loop
TODO: Mention the event loop (scheduler). remind that continuations on the same thread do not run in parallel, so do not need locks, atomic variables, etc (different threads shouldn't access the same data - more on that below). continuations obviously must not use blocking operations, or they block the whole thread.

TODO: Talk about polling that we currently do, and how today even sleep() or waiting for incoming connections or whatever, takes 100% of all CPUs.

# Introducing Seastar's network stack

TODO: Mention the two modes of operation: Posix and native (i.e., take a L2 (Ethernet) interface (vhost or dpdk) and on top of it we built (in Seastar itself) an L3 interface (TCP/IP)).

> TODO: 说明两种操作模式：Posix 和 native (即采用L2(以太网)接口(vhost或dpdk)，并在其之上构建(在Seastar本身中)L3接口(TCP/IP))。

For optimal performance, Seastar's network stack is sharded just like Seastar applications are: each shard (thread) takes responsibility for a different subset of the connections. Each incoming connection is directed to one of the threads, and after a connection is established, it continues to be handled on the same thread.

> 为了获得最佳性能，Seastar 的网络堆栈像 Seastar 应用程序一样被分片：每个分片（线程）负责连接的不同子集。每个传入的连接都指向其中一个线程，在建立连接后，它会继续在同一个线程上处理。

In the examples we saw earlier, `main()` ran our function `f()` only once, on the first thread. Unless the server is run with the `"-c1"` option (one thread only), this will mean that any connection arriving to a different thread will not be handled. So in all the examples below, we will need to run the same service loop on all cores. We can easily do this with the `smp::submit_to` function:

> 在我们之前看到的示例中，`main()` 只在第一个线程上运行了一次我们的函数 `f()`。除非服务器使用 "`-c1`" 选项运行（仅一个线程），否则这将意味着任何到达不同线程的连接都不会被处理。因此，在下面的所有示例中，我们将需要在所有内核上运行相同的服务循环。我们可以使用 `smp::submit_to` 函数轻松做到这一点：

```cpp
seastar::future<> service_loop();

seastar::future<> f() {
    return seastar::parallel_for_each(boost::irange<unsigned>(0, seastar::smp::count),
            [] (unsigned c) {
        return seastar::smp::submit_to(c, service_loop);
    });
}
```

Here we ask each of Seastar cores (from 0 to `smp::count`-1) to run the same function `service_loop()`. Each of these invocations returns a future, and `f()` will return when all of them have returned (in the examples below, they will never return - we will discuss shutting down services in later sections).

> 在这里，我们要求每个 Seastar 内核（从 0 到 `smp::count`-1）运行相同的函数 `service_loop()`。每一个调用都会返回一个 `future`，`f()` 会在它们全部返回时返回（在下面的示例中，它们永远不会返回，我们将在后面的部分中讨论关闭服务）。

We begin with a simple example of a TCP network server written in Seastar. This server repeatedly accepts connections on TCP port 1234, and returns an empty response:

> 我们从一个用 Seastar 编写的 TCP 网络服务器的简单示例开始。此服务器反复接受 TCP 端口 1234 上的连接，并返回一个空响应：

```cpp
#include <seastar/core/seastar.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/future-util.hh>
#include <iostream>

seastar::future<> service_loop() {
    return seastar::do_with(seastar::listen(seastar::make_ipv4_address({1234})),
            [] (auto& listener) {
        return seastar::keep_doing([&listener] () {
            return listener.accept().then(
                [] (seastar::connected_socket s, seastar::socket_address a) {
                    std::cout << "Accepted connection from " << a << "\n";
                });
        });
    });
}
```

This code works as follows:
1. The ```listen()``` call creates a ```server_socket``` object, ```listener```, which listens on TCP port 1234 (on any network interface).
2. We use ```do_with()``` to ensure that the listener socket lives throughout the loop.
3. To handle one connection, we call ```listener```'s  ```accept()``` method. This method returns a ```future<connected_socket, socket_address>```, i.e., is eventually resolved with an incoming TCP connection from a client (```connected_socket```) and the client's IP address and port (```socket_address```).
4. To repeatedly accept new connections, we use the ```keep_doing()``` loop idiom. ```keep_doing()``` runs its lambda parameter over and over, starting the next iteration as soon as the future returned by the previous iteration completes. The iterations only stop if an exception is encountered. The future returned by ```keep_doing()``` itself completes only when the iteration stops (i.e., only on exception).

> 此代码的工作原理如下：
>
> 1. `listen()` 调用创建了一个 `server_socket` 对象 `listener`，它侦听 TCP 端口 1234（在任何网络接口上）。
> 2. 我们使用 `do_with()` 用来确保监听套接字在整个循环中都存在。
> 3. 为了处理一个连接，我们调用 `listener` 的 `accept()` 方法。该方法返回一个 `future<accept_result>`，即最终通过来自客户端的传入 TCP 连接 ( `accept_result.connection`) 以及客户端的 IP 地址和端口 (`accept_result.remote_address`) 进行解析。
> 4. 为了反复接受新的连接，我们使用 `keep_doing()` 。`keep_doing()` 一遍又一遍地运行它的 lambda 参数，一旦上一次迭代返回的 `future` 完成，就开始下一次迭代。只有遇到异常时迭代才会停止。仅当迭代停止时（即仅在异常情况下），`keep_doing()` 返回的 `future` 才会完成。

Output from this server looks like the following example:

> 此服务器的输出类似于以下示例：

```
$ ./a.out
Accepted connection from 127.0.0.1:47578
Accepted connection from 127.0.0.1:47582
...
```

If you run the above example server immediately after killing the previous server, it often fails to start again, complaining that:

> 如果你在杀死之前的服务器后立即运行上面的示例服务器，它经常无法重新启动，会返回下面的错误：

```
$ ./a.out
program failed with uncaught exception: bind: Address already in use
```

This happens because by default, Seastar refuses to reuse the local port if there are any vestiges of old connections using that port. In our silly server, because the server is the side which first closes the connection, each connection lingers for a while in the "```TIME_WAIT```" state after being closed, and these prevent ```listen()``` on the same port from succeeding. Luckily, we can give listen an option to work despite these remaining ```TIME_WAIT```. This option is analogous to ```socket(7)```'s ```SO_REUSEADDR``` option:

> 发生这种情况是因为默认情况下，如果使用该端口的旧连接有任何痕迹，Seastar 将拒绝重用本地端口。在我们这种愚蠢的服务器中，由于服务器是最先关闭连接的一方，每个连接在关闭后都会在 `TIME_WAIT` 状态下徘徊一段时间，这些都阻止了在同一个端口上 `listen()` 的成功。幸运的是，我们可以给 listen 指定一个选项来忽略这些存在着的 `TIME_WAIT`。这个选项类似于 `socket(7)` 的 `SO_REUSEADDR` 选项：

```cpp
    seastar::listen_options lo;
    lo.reuse_address = true;
    return seastar::do_with(seastar::listen(seastar::make_ipv4_address({1234}), lo),
```

Most servers will always turn on this ```reuse_address``` listen option. Stevens' book "Unix Network Programming" even says that "All TCP servers should specify this socket option to allow the server to be restarted". Therefore in the future Seastar should probably default to this option being on --- even if for historic reasons this is not the default in Linux's socket API.

> 大多数服务器将始终打开 `reuse_address` 监听选项。Stevens 的《Unix 网络编程》一书甚至说“所有 TCP 服务器都应指定此套接字选项以允许重新启动服务器”。因此，未来 Seastar 可能应该默认启用此选项，但是出于历史原因，这不是 Linux 套接字 API 中的默认设置。

Let's advance our example server by outputting some canned response to each connection, instead of closing each connection immediately with an empty reply.

> 让我们通过向每个连接输出一些预设响应来推进我们的示例服务器，而不是立即用空回复关闭每个连接。

```cpp
#include <seastar/core/seastar.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/future-util.hh>
#include <iostream>

const char* canned_response = "Seastar is the future!\n";

seastar::future<> service_loop() {
    seastar::listen_options lo;
    lo.reuse_address = true;
    return seastar::do_with(seastar::listen(seastar::make_ipv4_address({1234}), lo),
            [] (auto& listener) {
        return seastar::keep_doing([&listener] () {
            return listener.accept().then(
                [] (seastar::connected_socket s, seastar::socket_address a) {
                    auto out = s.output();
                    return seastar::do_with(std::move(s), std::move(out),
                        [] (auto& s, auto& out) {
                            return out.write(canned_response).then([&out] {
                                return out.close();
			    });
		    });
	        });
        });
    });
}
```

The new part of this code begins by taking the ```connected_socket```'s ```output()```, which returns an ```output_stream<char>``` object. On this output stream ```out``` we can write our response using the ```write()``` method. The simple-looking ```write()``` operation is in fact a complex asynchronous operation behind the scenes,  possibly causing multiple packets to be sent, retransmitted, etc., as needed. ```write()``` returns a future saying when it is ok to ```write()``` again to this output stream; This does not necessarily guarantee that the remote peer received all the data we sent it, but it guarantees that the output stream has enough buffer space (or in the TCP case, there is enough room in the TCP congestion window) to allow another write to begin.

> 这段代码的新部分以 `connected_socket` 的 `output()` 开始，它返回一个 `output_stream<char>` 对象。在这个输出流 `out` 上，我们可以使用 `write()` 方法编写我们的响应。看似简单的 `write()` 操作，其实是一个复杂的后台异步操作，可能会导致根据需要发送、重传等多个数据包。`write()` 返回一个 `future` 告诉我们什么时候可以再次 `write()` 到这个输出流；这并不一定保证远端能够接收到我们发送给它的所有数据，但它保证输出流有足够的缓冲区空间（或者在 TCP 的情况下，TCP 拥塞窗口中有足够的空间）允许另一个写入开始。

After ```write()```ing the response to ```out```, the example code calls ```out.close()``` and waits for the future it returns. This is necessary, because ```write()``` attempts to batch writes so might not have yet written anything to the TCP stack at this point, and only when close() concludes can we be sure that all the data we wrote to the output stream has actually reached the TCP stack --- and only at this point we may finally dispose of the ```out``` and ```s``` objects.

> 在向 `out` 写入响应之后，示例代码调用了 `out.close()` 并等待它返回的 `future`, 这是必要的，因为 `write()` 尝试批量写入，所以此时可能还没有向 TCP 协议写入任何内容，只有当 `close()` 结束时，我们才能确定我们写入输出流的所有数据实际上已经到达TCP协议栈，此时我们才能最终销毁 `out` 和 `s` 对象。

Indeed, this server returns the expected response:

> 事实上，这个服务器返回了预期的响应：

```
$ telnet localhost 1234
...
Seastar is the future!
Connection closed by foreign host.
```

In the above example we only saw writing to the socket. Real servers will also want to read from the socket. The ```connected_socket```'s ```input()``` method returns an ```input_stream<char>``` object which can be used to read from the socket. The simplest way to read from this stream is using the ```read()``` method which returns a future ```temporary_buffer<char>```, containing some more bytes read from the socket --- or an empty buffer when the remote end shut down the connection.

> 在上面的例子中，我们只看到了对套接字的写入。真正的服务器也需要从套接字中读取。`connected_socket` 的 `input()` 方法返回一个可用于从套接字读取的 `input_stream<char>` 对象。从此流中读取数据的最简单方法是使用 `read()` 方法，这个方法会返回一个 future `temporary_buffer<char>`，该方法包含从套接字读取的更多字节 —— 或远程端关闭连接时的空缓冲区。

```temporary_buffer<char>``` is a convenient and safe way to pass around byte buffers that are only needed temporarily (e.g., while processing a request). As soon as this object goes out of scope (by normal return, or exception), the memory it holds gets automatically freed. Ownership of buffer can also be transferred by ```std::move()```ing it. We'll discuss ```temporary_buffer``` in more details in a later section.

> `temporary_buffer<char>` 是一种用来传递仅临时需要的字节缓冲区（例如，在处理请求时）的方便且安全的方式。一旦该对象超出范围（通过正常返回或异常），它持有的内存就会自动释放。也可以通过 `std::move()` 来转移缓冲区的所有权。我们将在后面的部分中更详细地讨论 `temporary_buffer`。

Let's look at a simple example server involving both reads an writes. This is a simple echo server, as described in RFC 862: The server listens for connections from the client, and once a connection is established, any data received is simply sent back - until the client closes the connection.

> 让我们看一个涉及读取和写入的简单示例服务器。这是一个简单的回显服务器，如 RFC 862 中所述：服务器侦听来自客户端的连接，一旦建立连接，接收到的任何数据都会被简单地返回，直到客户端关闭连接。

```cpp
#include <seastar/core/seastar.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/future-util.hh>

seastar::future<> handle_connection(seastar::connected_socket s,
                                    seastar::socket_address a) {
    auto out = s.output();
    auto in = s.input();
    return do_with(std::move(s), std::move(out), std::move(in),
        [] (auto& s, auto& out, auto& in) {
            return seastar::repeat([&out, &in] {
                return in.read().then([&out] (auto buf) {
                    if (buf) {
                        return out.write(std::move(buf)).then([&out] {
                            return out.flush();
                        }).then([] {
                            return seastar::stop_iteration::no;
                        });
                    } else {
                        return seastar::make_ready_future<seastar::stop_iteration>(
                            seastar::stop_iteration::yes);
                    }
                });
            }).then([&out] {
                return out.close();
            });
        });
}

seastar::future<> service_loop() {
    seastar::listen_options lo;
    lo.reuse_address = true;
    return seastar::do_with(seastar::listen(seastar::make_ipv4_address({1234}), lo),
            [] (auto& listener) {
        return seastar::keep_doing([&listener] () {
            return listener.accept().then(
                [] (seastar::connected_socket s, seastar::socket_address a) {
                    // Note we ignore, not return, the future returned by
                    // handle_connection(), so we do not wait for one
                    // connection to be handled before accepting the next one.
                    handle_connection(std::move(s), std::move(a));
                });
        });
    });
}

```

The main function ```service_loop()``` loops accepting new connections, and for each connection calls ```handle_connection()``` to handle this connection. Our ```handle_connection()``` returns a future saying when handling this connection completed, but importantly, we do ***not*** wait for this future: Remember that ```keep_doing``` will only start the next iteration when the future returned by the previous iteration is resolved. Because we want to allow parallel ongoing connections, we don't want the next ```accept()``` to wait until the previously accepted connection was closed. So we call ```handle_connection()``` to start the handling of the connection, but return nothing from the continuation, which resolves that future immediately, so ```keep_doing``` will continue to the next ```accept()```.

> 主函数 `service_loop()` 循环接受新的连接，并为每个连接调用 `handle_connection()` 来处理该连接。`handle_connection()` 在该连接处理结束后返回一个 `future` ，但是我们不等待这个 `future` 就绪，注意 `keep_doing` 只有当前一个迭代返回的 `future` 被解决时才会开始下一个迭代。因为我们希望并行处理正在进行的连接，所以我们不希望下一个 `accept()` 等到之前接受的连接关闭，因此我们调用 `handle_connection()` 来开始处理连接，但没有从 `continuation` 中返回任何东西，这会立即解决这个 `future`，所以 `keep_doing` 将继续下一个 `accept()`.

This demonstrates how easy it is to run parallel _fibers_ (chains of continuations) in Seastar - When a continuation runs an asynchronous function but ignores the future it returns, the asynchronous operation continues in parallel, but never waited for.

> 这展示了在 Seastar 中并行运行 fiber（continuation 链）是多么容易：当 continuation 运行异步函数但忽略它返回的 future 时，异步操作将并行继续，但从不等待。

It is often a mistake to silently ignore an exception, so if the future we're ignoring might resolve with an except, it is recommended to handle this case, e.g. using a ```handle_exception()``` continuation. In our case, a failed connection is fine (e.g., the client might close its connection will we're sending it output), so we did not bother to handle the exception.

> 默默地忽略异常通常是错误的，所以如果我们忽略的 `future` 可能携带异常，建议处理这种情况，例如使用 `handle_exception()` continuation。在我们的例子中，一个失败的连接是没问题的（例如，客户端可能会关闭它的连接，我们会发送它输出），所以我们没有费心去处理这个异常。

The ```handle_connection()``` function itself is straightforward --- it repeatedly calls ```read()``` read on the input stream, to receive a ```temporary_buffer``` with some data, and then moves this temporary buffer into a ```write()``` call on the output stream. The buffer will eventually be freed, automatically, when the ```write()``` is done with it. When ```read()``` eventually returns an empty buffer signifying the end of input, we stop ```repeat```'s iteration by returning a ```stop_iteration::yes```.

> `handle_connection()` 函数本身很简单：它在输入流上反复调用 `read()` 以接收带有一些数据的 `temporary_buffer`，然后将此临时缓冲区 `move` 到对输出流的 `write()` 调用中。缓冲区最终将在 `write()` 完成后自动释放。当 `read()` 最终返回一个表示输入结束的空缓冲区时，我们通过返回 `stop_iteration::yes` 来停止 `repeat` 的迭代。

# Sharded services

In the previous section we saw that a Seastar application usually needs to run its code on all available CPU cores. We saw that the `seastar::smp::submit_to()` function allows the main function, which initially runs only on the first core, to start the server's code on all `seastar::smp::count` cores.

> 在上一节中，我们看到 Seastar 应用程序通常需要在所有可用的 CPU 上运行其代码。我们看到 `seastar::smp::submit_to()` 函数允许最初只在第一个核上运行的 main 函数在所有 `seastar::smp::count` 核上启动服务器的代码。

However, usually one needs not just to run code on each core, but also to have an object that contains the state of this code. Additionally, one may like to interact with those different objects, and also have a mechanism to stop the service running on the different cores.

> 但是，通常不仅需要在每个内核上运行代码，还需要有一个包含该代码状态的对象。此外，可能想要与那些不同的对象进行交互，并且还具有一种机制来停止在不同内核上运行的服务。

The `seastar::sharded<T>` template provides a structured way create such a _sharded service_. It creates a separate object of type `T` in each core, and provides mechanisms to interact with those copies, to start some code on each, and finally to cleanly stop the service.

> `seastar::sharded<T>` 模板提供了一种结构化的方式来创建这样的 `sharded service`。它在每个核心中创建一个单独的 `T` 类型的对象，并提供与这些副本交互的机制，在每个核心上启动一些代码，最后彻底停止服务。

To use `seastar::sharded`, first create a class for the object holding the state of the service on a single core. For example:

> 要使用 `seastar::sharded`，首先要为在单核上保存服务状态的对象创建一个类。例如：

```cpp
#include <seastar/core/future.hh>
#include <iostream>

class my_service {
public:
    std::string _str;
    my_service(const std::string& str) : _str(str) { }
    seastar::future<> run() {
        std::cerr << "running on " << seastar::engine().cpu_id() <<
            ", _str = " << _str << \n";
        return seastar::make_ready_future<>();
    }
    seastar::future<> stop() {
        return seastar::make_ready_future<>();
    }
};
```

The only mandatory method in this object is `stop()`, which will be called in each core when we want to stop the sharded service and want to wait until it stops on all cores.

> 该对象中唯一必须要实现的方法是 `stop()`，当我们想要停止分片服务并希望等到它在所有核心上停止时，它将在每个核心中调用。

Now let's see how to use it:

> 现在让我们看看如何使用它：

```cpp
#include <seastar/core/sharded.hh>

seastar::sharded<my_service> s;

seastar::future<> f() {
    return s.start(std::string("hello")).then([] {
        return s.invoke_on_all([] (my_service& local_service) {
            return local_service.run();
        });
    }).then([] {
        return s.stop();
    });
}
```

The `s.start()` starts the service by creating a `my_service` object on each of the cores. The arguments to `s.start()`, if any (in this example, `std::string("hello")`), are passed to `my_service`'s constructor.

> `s.start()` 通过在每个核心上创建一个 `my_service` 对象来启动服务。`s.start()` 的参数（如果有的话（在这个例子中，`std::string("hello")`））被传递给`my_service` 的构造函数。

But `s.start()` did not start running any code yet (besides the object's constructor). For that, we have the `s.invoke_on_all()` which runs the given lambda on all the cores - giving each lambda the local `my_service` object on that core. In this example, we have a `run()` method on each object, so we run that.

> 但 `s.start()` 还没有开始运行任何代码（除了对象的构造函数）。为此我们有 `s.invoke_on_all()` 函数，它在所有的核心上运行给定的 lambda，lambda 的参数是线程本地对象 `my_service`。在这个例子中，我们对每个对象都有一个 `run()` 方法，所以我们运行它。

Finally, at the end of the run we want to give the service on all cores a chance to shut down cleanly, so we call `s.stop()`. This will call the `stop()` method on each core's object, and wait for all of them to finish. Calling `s.stop()` before destroying `s` is mandatory - Seastar will warn you if you forget to do it.

> 最后，在运行结束时，我们想让所有核心上的服务有机会干净地关闭，所以我们调用 `s.stop()`。这将调用每个核心对象的 `stop` 方法，并等待它们全部完成。`s` 销毁前调用 `s.stop()` 是强制性的，如果你忘记这样做，Seastar 会警告你。

In addition to `invoke_on_all()` which runs the same code on all shards, another feature a sharded service often needs is for one shard to invoke code another specific shard. This is done by calling the sharded service's `invoke_on()` method. For example:

> 除了在所有分片上运行相同的代码的 `invoke_on_all()` 之外，分片服务通常需要的另一个功能是：在一个分片上调用另一个特定分片的代码。这是通过调用分片服务的 `invoke_on()` 方法来完成的。例如：

```cpp
seastar::sharded<my_service> s;
...
return s.invoke_on(0, [] (my_service& local_service) {
    std::cerr << "invoked on " << seastar::engine().cpu_id() <<
        ", _str = " << local_service._str << "\n";
});
```

This runs the lambda function on shard 0, with a reference to the local `my_service` object on that shard. 

> 这将在分片 0 上运行 lambda 函数，并引用该分片上的本地 `my_service` 对象。


# Shutting down cleanly

TODO: Handling interrupt, shutting down services, etc.

Move the seastar::gate section here.

# Command line options
## Standard Seastar command-line options
All Seastar applications accept a standard set of command-line arguments, such as those we've already seen above: The `-c` option for controlling the number of threads used, or  `-m` for determining the amount of memory given to the application.

TODO: list and explain more of these options.

Every Seastar application also accepts the `-h` (or `--help`) option, which lists and explains all the available options --- the standard Seastar ones, and the user-defined ones as explained below.
## User-defined command-line options
Seastar parses the command line options (`argv[]`) when it is passed to `app_template::run()`, looking for its own standard options. Therefore, it is not recommended that the application tries to parse `argv[]` on its own because the application might not understand some of the standard Seastar options and not be able to correctly skip them.

Rather, applications which want to have command-line options of their own should tell Seastar's command line parser of these additional application-specific options, and ask Seastar's command line parser to recognize them too. Seastar's command line parser is actually the Boost library's `boost::program_options`. An application adds its own option by using the `add_options()` and `add_positional_options()` methods on the `app_template` to define options, and later calling `configuration()` to retrieve the setting of these options. For example,

```cpp
#include <iostream>
#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
int main(int argc, char** argv) {
    seastar::app_template app;
    namespace bpo = boost::program_options;
    app.add_options()
        ("flag", "some optional flag")
        ("size,s", bpo::value<int>()->default_value(100), "size")
        ;
    app.add_positional_options({
       { "filename", bpo::value<std::vector<seastar::sstring>>()->default_value({}),
         "sstable files to verify", -1}
    });
    app.run(argc, argv, [&app] {
        auto& args = app.configuration();
        if (args.count("flag")) {
            std::cout << "Flag is on\n";
        }
        std::cout << "Size is " << args["size"].as<int>() << "\n";
        auto& filenames = args["filename"].as<std::vector<seastar::sstring>>();
        for (auto&& fn : filenames) {
            std::cout << fn << "\n";
        }
        return seastar::make_ready_future<>();
    });
    return 0;
}
```

In this example, we add via `add_options()` two application-specific options: `--flag` is an optional parameter which doesn't take any additional agruments, and `--size` (or `-s`) takes an integer value, which defaults (if this option is missing) to 100. Additionally, we ask via `add_positional_options()` that an unlimited number of arguments that do not begin with a "`-`" --- the so-called _positional_ arguments --- be collected to a vector of strings under the "filename" option. Some example outputs from this program:

```
$ ./a.out
Size is 100
$ ./a.out --flag
Flag is on
Size is 100
$ ./a.out --flag -s 3
Flag is on
Size is 3
$ ./a.out --size 3 hello hi
Size is 3
hello
hi
$ ./a.out --filename hello --size 3 hi
Size is 3
hello
hi
```

`boost::program_options` has more powerful features, such as required options, option checking and combining, various option types, and more. Please refer to Boost's documentation for more information.

# Debugging a Seastar program
## Debugging ignored exceptions
If a future resolves with an exception, and the application neglects to handle that exception or to explicitly ignore it, the application may have missed an important problem. This is likely to be an application bug.

Therefore, Seastar prints a warning message to the log if a future is destroyed when it stores an exception that hasn't been handled.

For example, consider this code:
```cpp
#include <seastar/core/future.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/app-template.hh>

class myexception {};

seastar::future<> g() {
    return seastar::make_exception_future<>(myexception());
}

seastar::future<> f() {
    g();
    return seastar::sleep(std::chrono::seconds(1));
}

int main(int argc, char** argv) {
    seastar::app_template app;
    app.run(argc, argv, f);
}
```

Here, the main function `f()` calls `g()`, but doesn't do anything with the future it returns. But this future resolves with an exception, and this exception is silently ignored. So Seastar prints this warning message about the ignored exception:
```
WARN  2020-03-31 11:08:09,208 [shard 0] seastar - Exceptional future ignored: myexception, backtrace:   /lib64/libasan.so.5+0x6ce7f
  0x1a64193
  0x1a6265f
  0xf326cc
  0xeaf1a0
  0xeaffe4
  0xead7be
  0xeb5917
  0xee2477
  0xec312e
  0xec8fcf
  0xeec765
  0xee1b29
  0xed9fab
  0xec27c8
  0xec867f
  0xf00acc
  0xef179d
  0xef1824
  0xef18b5
  0xee4827
  0xee470f
  0xf00f81
  0xebac29
  0xeb9095
  0xeb9174
  0xeb925a
  0xeb9964
  0xebef89
  0x10f74c3
  0x10fb439
  0x11005fd
  0xec4f08
  0xec2f43
  0xec3461
  0xeadabe
  /lib64/libc.so.6+0x271a2
  0xead52d
```

This message says that an exceptional future was ignored, and that the type of the exception was "`myexception`". The type of the exception is usually not enough to pinpoint where the problem happened, so the warning message also includes the backtrace - the call chain - leading to where the exceptional future was destroyed. The backtrace is given as a list of addresses, where code in other shared libraries is written as a shared library plus offset (when ASLR is enabled, the shared libraries are mapped in a different address each time).

Seastar includes a utility, `seastar-addr2line`, for translating these addresses into readable backtraces including exact method names, source files and line numbers. This utility needs the _unstripped_ executable. Typically, a stripped executable is used for production, but an unstripped copy is kept separately to be used in debugging - including `seastar-addr2line`.

To decode the backtrace, we run
```
seastar-addr2line -e a.out
```
And then paste the list of addresses in the warning message, and conclude with a `control-D` (it's also possible, if you want, to put the list of addresses in the `seastar-addr2line` command line). The result looks like this:

```
void seastar::backtrace<seastar::current_backtrace()::{lambda(seastar::frame)#1}>(seastar::current_backtrace()::{lambda(seastar::frame)#1}&&) at include/seastar/util/backtrace.hh:56
seastar::current_backtrace() at src/util/backtrace.cc:84
seastar::report_failed_future(std::__exception_ptr::exception_ptr const&) at src/core/future.cc:116
seastar::future_state_base::~future_state_base() at include/seastar/core/future.hh:335
seastar::future_state<>::~future_state() at include/seastar/core/future.hh:414
 (inlined by) seastar::future<>::~future() at include/seastar/core/future.hh:990
f() at test.cc:12
std::_Function_handler<seastar::future<> (), seastar::future<> (*)()>::_M_invoke(std::_Any_data const&) at /usr/include/c++/9/bits/std_function.h:286
std::function<seastar::future<> ()>::operator()() const at /usr/include/c++/9/bits/std_function.h:690
seastar::app_template::run(int, char**, std::function<seastar::future<> ()>&&)::{lambda()#1}::operator()() const at src/core/app-template.cc:131
std::_Function_handler<seastar::future<int> (), seastar::app_template::run(int, char**, std::function<seastar::future<> ()>&&)::{lambda()#1}>::_M_invoke(std::_Any_data const&) at /usr/include/c++/9/bits/std_function.h:286
std::function<seastar::future<int> ()>::operator()() const at /usr/include/c++/9/bits/std_function.h:690
seastar::future<int> seastar::futurize<seastar::future<int> >::invoke<std::function<seastar::future<int> ()>&>(std::function<seastar::future<int> ()>&) at include/seastar/core/future.hh:1670
auto seastar::futurize_invoke<std::function<seastar::future<int> ()>&>(std::function<seastar::future<int> ()>&) at include/seastar/core/future.hh:1754
seastar::app_template::run(int, char**, std::function<seastar::future<int> ()>&&)::{lambda()#1}::operator()() at src/core/app-template.cc:120 (discriminator 4)
std::_Function_handler<void (), seastar::app_template::run(int, char**, std::function<seastar::future<int> ()>&&)::{lambda()#1}>::_M_invoke(std::_Any_data const&) at /usr/include/c++/9/bits/std_function.h:300
std::function<void ()>::operator()() const at /usr/include/c++/9/bits/std_function.h:690
seastar::apply_helper<std::function<void ()>&, std::tuple<>&&, std::integer_sequence<unsigned long> >::apply(std::function<void ()>&, std::tuple<>&&) at include/seastar/core/apply.hh:36
auto seastar::apply<std::function<void ()>&>(std::function<void ()>&, std::tuple<>&&) at include/seastar/core/apply.hh:44
seastar::future<> seastar::futurize<void>::apply<std::function<void ()>&>(std::function<void ()>&, std::tuple<>&&) at include/seastar/core/future.hh:1634
auto seastar::futurize_apply<std::function<void ()>&>(std::function<void ()>&, std::tuple<>&&) at include/seastar/core/future.hh:1766
seastar::future<>::then<std::function<void ()>, seastar::future<> >(std::function<void ()>&&)::{lambda()#1}::operator()() at include/seastar/core/future.hh:1191
seastar::noncopyable_function<seastar::future<> ()>::direct_vtable_for<seastar::future<>::then<std::function<void ()>, seastar::future<> >(std::function<void ()>&&)::{lambda()#1}>::call(seastar::noncopyable_function<seastar::future<> ()> const*) at include/seastar/util/noncopyable_function.hh:101
seastar::noncopyable_function<seastar::future<> ()>::operator()() const at include/seastar/util/noncopyable_function.hh:184
seastar::apply_helper<seastar::noncopyable_function<seastar::future<> ()>, std::tuple<>&&, std::integer_sequence<unsigned long> >::apply(seastar::noncopyable_function<seastar::future<> ()>&&, std::tuple<>&&) at include/seastar/core/apply.hh:36
auto seastar::apply<seastar::noncopyable_function<seastar::future<> ()>>(seastar::noncopyable_function<seastar::future<> ()>&&, std::tuple<>&&) at include/seastar/core/apply.hh:44
seastar::future<> seastar::futurize<seastar::future<> >::apply<seastar::noncopyable_function<seastar::future<> ()>>(seastar::noncopyable_function<seastar::future<> ()>&&, std::tuple<>&&) at include/seastar/core/future.hh:1660
seastar::future<>::then_impl_nrvo<seastar::noncopyable_function<seastar::future<> ()>, seastar::future<> >(seastar::noncopyable_function<seastar::future<> ()>&&)::{lambda()#1}::operator()() const::{lambda(seastar::internal::promise_base_with_type<>&, seastar::future_state<>&&)#1}::operator()(seastar::internal::promise_base_with_type<>, seastar::future_state<>) at include/seastar/core/future.hh:1213
seastar::continuation<seastar::internal::promise_base_with_type<>, seastar::future<>::then_impl_nrvo<seastar::noncopyable_function<seastar::future<> ()>, seastar::future<> >(seastar::noncopyable_function<seastar::future<> ()>&&)::{lambda()#1}::operator()() const::{lambda(seastar::internal::promise_base_with_type<>&, seastar::future_state<>&&)#1}>::run_and_dispose() at include/seastar/core/future.hh:509
seastar::reactor::run_tasks(seastar::reactor::task_queue&) at src/core/reactor.cc:2124
seastar::reactor::run_some_tasks() at src/core/reactor.cc:2539 (discriminator 2)
seastar::reactor::run() at src/core/reactor.cc:2694
seastar::app_template::run_deprecated(int, char**, std::function<void ()>&&) at src/core/app-template.cc:199 (discriminator 1)
seastar::app_template::run(int, char**, std::function<seastar::future<int> ()>&&) at src/core/app-template.cc:115 (discriminator 2)
seastar::app_template::run(int, char**, std::function<seastar::future<> ()>&&) at src/core/app-template.cc:130 (discriminator 2)
main at test.cc:19 (discriminator 1)
__libc_start_main at /usr/src/debug/glibc-2.30-34-g994e529a37/csu/../csu/libc-start.c:308
_start at ??:?
```

Most of the lines at the bottom of this backtrace are not interesting, and just showing the internal details of how Seastar ended up running the main function `f()`. The only interesting part is the _first_ few lines:

```
seastar::report_failed_future(std::__exception_ptr::exception_ptr const&) at src/core/future.cc:116
seastar::future_state_base::~future_state_base() at include/seastar/core/future.hh:335
seastar::future_state<>::~future_state() at include/seastar/core/future.hh:414
 (inlined by) seastar::future<>::~future() at include/seastar/core/future.hh:990
f() at test.cc:12
```

Here we see that the warning message was printed by the `seastar::report_failed_future()` function which was called when destroying a future (`future<>::~future`) that had not been handled. The future's destructor was called in line 11 of our test code (`26.cc`), which is indeed the line where we called `g()` and ignored its result.  
This backtrace gives us an accurate understanding of where our code destroyed an exceptional future without handling it first, which is usually helpful in solving these kinds of bugs. Note that this technique does not tell us where the exception was first created, nor what code passed around the exceptional future before it was destroyed - we just learn where the future was destroyed. To learn where the exception was originally thrown, see the next section:

## Finding where an exception was thrown
Sometimes an application logs an exception, and we want to know where in the code the exception was originally thrown. Unlike languages like Java, C++ does not have a builtin method of attaching a backtrace to every exception. So Seastar provides functions which allow adding to an exception the backtrace recorded when throwing it.

For example, in the following code we throw and catch an `std::runtime_error` normally:

```cpp
#include <seastar/core/future.hh>
#include <seastar/util/log.hh>
#include <exception>
#include <iostream>

seastar::future<> g() {
    return seastar::make_exception_future<>(std::runtime_error("hello"));
}

seastar::future<> f() {
    return g().handle_exception([](std::exception_ptr e) {
        std::cerr << "Exception: " << e << "\n";
    });
}
```
The output is
```
Exception: std::runtime_error (hello)
```
From this output, we have no way of knowing that the exception was thrown in `g()`. We can solve this if we use `make_exception_future_with_backtrace` instead of `make_exception_future`:

```
#include <util/backtrace.hh>
seastar::future<> g() {
    return seastar::make_exception_future_with_backtrace<>(std::runtime_error("hello"));
}
```
Now the output looks like
```
Exception: seastar::internal::backtraced<std::runtime_error> (hello Backtrace:   0x678bd3
  0x677204
  0x67736b
  0x678cd5
  0x4f923c
  0x4f9c38
  0x4ff4d0
...
)
```
Which, as above, can be converted to a human-readable backtrace by using the `seastar-addr2line` script.

In addition to `seastar::make_exception_future_with_backtrace()`, Seastar also provides a function `throw_with_backtrace()`, to throw an exception instead of returning an exceptional future. For example:
```
    seastar::throw_with_backtrace<std::runtime_error>("hello");
```

In the current implementation, both `make_exception_future_with_backtrace` and `throw_with_backtrace` require that the original exception type (in the above example, `std::runtime_error`) is a subclass of the `std::exception` class. The original exception provides a `what()` string, and the wrapped exception adds the backtrace to this string, as demonstrated above. Moreover, the wrapped exception type is a _subclass_ of the original exception type, which allows `catch(...)` code to continue filtering by the exception original type - despite the addition of the backtrace.


## Debugging with gdb
handle SIGUSR1 pass noprint
handle SIGALRM pass noprint

# Promise objects

As we already defined above, An **asynchronous function**, also called a **promise**, is a function which returns a future and arranges for this future to be eventually resolved. As we already saw, an asynchronous function is usually written in terms of other asynchronous functions, for example we saw the function `slow()` which waits for the existing asynchronous function `sleep()` to complete, and then returns 3:

> 正如我们在上面已经定义的那样，异步函数(**asynchronous function**)，也称为 **promise**，是一个返回 future 并安排这个 future 最终被解决的函数。正如我们已经看到的，一个异步函数通常是根据其他异步函数来编写的，例如我们看到的异步函数 `slow()` 等待现有异步函数 `sleep()` 完成，然后返回 3：

```cpp
seastar::future<int> slow() {
    using namespace std::chrono_literals;
    return seastar::sleep(100ms).then([] { return 3; });
}
```

The most basic building block for writing promises is the **promise object**, an object of type `promise<T>`. A `promise<T>` has a method `future<T> get_future()` to returns a future, and a method `set_value(T)`, to resolve this future. An asynchronous function can create a promise object, return its future, and the `set_value` method to be eventually called - which will finally resolve the future it returned.

> 编写 promise 的最基本构建块是 promise 对象，它是一个 `promise<T>` 类型的对象。`promise<T>` 有一个返回 future 的方法 `future<T> get_future()` 和一个来解决这个 `future` 的方法 `set_value(T)`。一个异步函数可以创建一个 `promise` 对象，返回它的 `future`，以及最终通过调用 `set_value` 方法解决它返回的 `future`。

CONTINUE HERE. write an example, e.g., something which writes a message every second, and after 10 messages, completes the future.

# Memory allocation in Seastar
## Per-thread memory allocation
Seastar requires that applications be sharded, i.e., that code running on different threads operate on different objects in memory. We already saw in [Seastar memory] how Seastar takes over a given amount of memory (often, most of the machine's memory) and divides it equally between the different threads. Modern multi-socket machines have non-uniform memory access (NUMA), meaning that some parts of memory are closer to some of the cores, and Seastar takes this knowledge into account when dividing the memory between threads. Currently, the division of memory between threads is static, and equal - the threads are expected to experience roughly equal amount of load and require roughly equal amounts of memory.

> Seastar 要求对应用程序进行分片，即运行在不同线程上的代码操作内存中的不同对象。我们已经在 [Seastar memory] 中看到了 Seastar 如何接管给定数量的内存(通常是机器的大部分内存)，并将其平均分配给不同的线程。现代多插槽计算机具有非统一内存访问(NUMA)，这意味着内存的某些部分更接近某些内核，Seastar 在线程之间划分内存时，考虑到了这一点。
>
> 目前，线程之间的内存分配是静态的，并且是相等的，线程所承受的负载大致相等，需要的内存大致相等。

To achieve this per-thread allocation, Seastar redefines the C library functions `malloc()`, `free()`, and their numerous relatives --- `calloc()`, `realloc()`, `posix_memalign()`, `memalign()`, `malloc_usable_size()`, and `malloc_trim()`. It also redefines the C++ memory allocation functions, `operator new`, `operator delete`,  and all their variants (including array versions, the C++14 delete taking a size, and the C++17 variants taking required alignment).

> 为了实现这种按线程分配，Seastar 重新定义了 C 库函数 `malloc()`、`free()` 以及它们的众多相关函数: `calloc()`、`realloc()`、`posix_memalign()`、`memalign()`、`malloc_usable_size()` 和 `malloc_trim()`。它还重新定义了 c++ 的内存分配函数，`operator new`， `operator delete` 以及它们的所有变体(包括数组版本，c++14 的 delete 接受一个 size，c++17 的变体接受所需的对齐)。

It is important to remember that Seastar's different threads *can* see memory allocated by other threads, but they are nontheless strongly discouraged from actually doing this. Sharing data objects between threads on modern multi-core machines results in stiff performance penalties from locks, memory barriers, and cache-line bouncing. Rather, Seastar encourages applications to avoid sharing objects between threads when possible (by *sharding* --- each thread owns a subset of the objects), and when threads do need to interact they do so with explicit message passing, with `submit_to()`, as we shall see later.

> 重要的是要记住 Seastar 的不同线程可以看到其他线程分配的内存，但实际上不鼓励这样做。在现代多核机器的线程之间共享数据对象会导致锁、内存屏障和 cache-line bouncing 等严重的性能损失。相反，Seastar 鼓励应用程序尽可能避免在线程之间共享对象(通过分片 --- 每个线程拥有对象的一个子集)，当线程确实需要交互时，可以使用 `submit_to()` 方法，通过显式消息传递来实现，我们将在后面看到。

## Foreign pointers
An object allocated on one thread will be owned by this thread, and eventually should be freed by the same thread. Freeing memory on the *wrong* thread is strongly discouraged, but is currently supported (albeit slowly) to support library code beyond Seastar's control. For example, `std::exception_ptr` allocates memory; So if we invoke an asynchronous operation on a remote thread and this operation returns an exception, when we free the returned `std::exception_ptr` this will happen on the "wrong" core. So Seastar allows it, but inefficiently.

> 在一个线程上分配的对象将归该线程所有，最终应由同一线程释放。强烈建议不要在错误的线程上释放内存，但目前为了支持 Seastar 无法控制的库代码，这是支持的（尽管速度很慢）。比如 `std::exception_ptr` 分配内存。因此，如果我们在远程线程上调用异步操作并且该操作返回异常，则当我们释放返回的 `std::exception_ptr` 时，这将发生在 “错误” 的核心上。所以 Seastar 允许这样做，但效率低下。

In most cases objects should spend their entire life on a single thread and be used only by this thread. But in some cases we want to reassign ownership of an object which started its life on one thread, to a different thread. This can be done using a `seastar::foreign_ptr<>`. A pointer, or smart pointer, to an object is wrapped in a `seastar::foreign_ptr<P>`. This wrapper can then be moved into code running in a different thread (e.g., using `submit_to()`).

> 在大多数情况下，对象应该在一个线程上度过它们的整个生命周期，并且只被这个线程使用。但是在某些情况下，我们希望将一个对象的所有权从分配该对象的线程转移给另一个线程，这可以使用 `seastar::foreign_ptr<>` 来完成。指向对象的指针或智能指针被包装在 `seastar::foreign_ptr<P>` 中，然后可以将该包装器移动到在不同线程运行的代码中（例如，使用 `submit_to()`）。

The most common use-case is a `seastar::foreign_ptr<std::unique_ptr<T>>`. The thread receiving this `foreign_ptr` will get exclusive use of the object, and when it destroys this wrapper, it will go back to the original thread to destroy the object. Note that the object is not only freed on the original shard - it is also *destroyed* (i.e., its destructor is run) there. This is often important when the object's destructor needs to access other state which belongs to the original shard - e.g., unlink itself from a container.

> 最常见的用例是 `seastar::foreign_ptr<std::unique_ptr<T>>`。接收 `foreign_ptr` 的线程将独占这个对象，当它销毁这个包装器时，它会返回到原来的线程销毁该对象。请注意，该对象不仅在原始分片上被释放，它还在那里被销毁（即，它的析构函数运行）。当对象的析构函数需要访问属于原始分片的其他状态时，这通常很重要 - 例如，将自身与容器取消链接。

Although `foreign_ptr` ensures that the object's *destructor* automatically runs on the object's home thread, it does not absolve the user from worrying where to run the object's other methods. Some simple methods, e.g., methods which just read from the object's fields, can be run on the receiving thread. However, other methods may need to access other data owned by the object's home shard, or need to prevent concurrent operations. Even if we're sure that object is now used exclusively by the receiving thread, such methods must still be run, explicitly, on the home thread:

> 虽然 `foreign_ptr` 确保对象的析构函数自动在对象的主线程上运行，但它并不能免除用户担心在何处运行对象的其他方法的麻烦。一些简单的方法，例如，从对象的字段中读取的方法，可以在接收线程上运行。但是，其他方法可能需要访问对象的主分片所拥有的其他数据，或者需要防止并发操作。即使我们确定该对象现在仅由接收线程使用，这些方法仍必须在主线程上显式运行：

```
    // fp is some foreign_ptr<>
    return smp::submit_to(fp.get_owner_shard(), [p=fp.get()]
        { return p->some_method(); });
```
So `seastar::foreign_ptr<>` not only has functional benefits (namely, to run the destructor on the home shard), it also has *documentational* benefits - it warns the programmer to watch out every time the object is used, that this is a *foreign* pointer, and if we want to do anything non-trivial with the pointed object, we may need to do it on the home shard.

> 所以 `seastar::foreign_ptr<>` 不仅有功能上的好处（即在主分片上运行析构函数），它还有文档上的好处：它警告程序员每次使用对象时都要小心，这是一个外部指针，如果我们想要要对指向的对象做任何重要的事情，我们可能需要在主分片上做。

Above, we discussed the case of transferring ownership of an object to a another shard, via `seastar::foreign_ptr<std::unique_ptr<T>>`. However, sometimes the sender does not want to relinquish ownership of the object. Sometimes, it wants the remote thread to operate on its object and return with the object intact. Sometimes, it wants to send the same object to multiple shards. In such cases, `seastar::foreign_ptr<seastar::lw_shared_ptr<T>> is useful. The user needs to watch out, of course, not to operate on the same object from multiple threads concurrently. If this cannot be ensured by program logic alone, some methods of serialization must be used - such as running the operations on the home shard with `submit_to()` as described above.

> 上面，我们讨论了通过 `seastar::foreign_ptr<std::unique_ptr<T>>` 将对象的所有权转移到另一个分片的情况。但是，有时发送者不想放弃对象的所有权。有时，它希望远程线程对其对象进行操作，并返回完整的对象。有时，它想将同一个对象发送到多个分片。在这种情况下，可以使用`seastar::foreign_ptr<seastar::lw_shared_ptr<T>>`。使用者当然也要小心，不要从多个线程并行操作同一个对象。如果这不能通过程序逻辑来保证，必须使用一些串行化的方法： 比如在主分片使用上述的 `submit_to()` 来运行这些操作。

Normally, a `seastar::foreign_ptr` cannot not be copied - only moved. However, when it holds a smart pointer that can be copied (namely, a `shared_ptr`), one may want to make an additional copy of that pointer and create a second `foreign_ptr`. Doing this is inefficient and asynchronous (it requires communicating with the original owner of the object to create the copies), so a method `future<foreign_ptr> copy()` needs to be explicitly used instead of the normal copy constructor.

> 通常 `seastar::foreign_ptr` 不能被复制，只能 `move`。但是，当它拥有一个可以复制的智能指针（即`shared_ptr`）时，可能需要制作该指针的额外副本并创建第二个 `foreign_ptr`。这样做是低效且异步的（它需要与对象的原始所有者通信以创建副本），因此需要显式使用方法 `future<foreign_ptr> copy()` 而不是普通的复制构造函数。

# Seastar::thread
Seastar's programming model, using futures and continuations, is very powerful and efficient.  However, as we've already seen in examples above, it is also relatively verbose: Every time that we need to wait before proceeding with a computation, we need to write another continuation. We also need to worry about passing the data between the different continuations (using techniques like those described in the [Lifetime management] section). Simple flow-control constructs such as loops also become more involved using continuations. For example, consider this simple classical synchronous code:

> Seastar 使用了 future 和 continuation 的编程模型是非常强大和高效的。然而，正如我们在上面的例子中已经看到的那样，它也相对冗长：每次在进行计算之前我们需要等待，需要编写另一个 continuation。我们还需要考虑在不同的 continuation 之间传递数据(使用 [Lifetime management] 一节中描述的技术)。简单的流控制结构(如循环)也需要使用 continuation。例如，考虑以下简单的经典同步代码:

```cpp
    std::cout << "Hi.\n";
    for (int i = 1; i < 4; i++) {
        sleep(1);
        std::cout << i << "\n";
    }
```
In Seastar, using futures and continuations, we need to write something like this:

> 在 Seastar 中，使用 future 和 continuations 时，我们需要这样写:

```cpp
    std::cout << "Hi.\n";
    return seastar::do_for_each(boost::counting_iterator<int>(1),
        boost::counting_iterator<int>(4), [] (int i) {
        return seastar::sleep(std::chrono::seconds(1)).then([i] {
            std::cout << i << "\n";
        });
    });
```

But Seastar also allows, via `seastar::thread`, to write code which looks more like synchronous code. A `seastar::thread` provides an execution environment where blocking is tolerated; You can issue an asyncrhonous function, and wait for it in the same function, rather then establishing a callback to be called with `future<>::then()`:

> 但是 Seastar 也允许通过 `Seastar::thread` 编写看起来更像同步的代码。`seastar::thread` 提供了一个允许阻塞的执行环境，你可以发出一个异步函数，并在同一个函数中等待它，而不是建立一个要调用 `future<>::then()` 的回调:

```cpp
    seastar::thread th([] {
        std::cout << "Hi.\n";
        for (int i = 1; i < 4; i++) {
            seastar::sleep(std::chrono::seconds(1)).get();
            std::cout << i << "\n";
        }
    });
```
A `seastar::thread` is **not** a separate operating system thread. It still uses continuations, which are scheduled on Seastar's single thread (per core). It works as follows:

> `seastar::thread` 不是操作系统线程。它仍然使用在 Seastar 的单线程上运行的 continuation。其工作原理如下:

The `seastar::thread` allocates a 128KB stack, and runs the given function until the it *blocks* on the call to a future's `get()` method. Outside a `seastar::thread` context, `get()` may only be called on a future which is already available. But inside a thread, calling `get()` on a future which is not yet available stops running the thread function, and schedules a continuation for this future, which continues to run the thread's function (on the same saved stack) when the future becomes available.

> `seastar::thread` 分配一个 128KB 的堆栈，并运行给定的函数，直到它在 future 的 `get()` 方法的调用上阻塞。在 `seastar::thread` 上下文之外，`get()` 只能在已经就绪的 future 对象上调用。但在线程内部，调用一个还未就绪的 future 的 `get()` 方法将停止运行线程函数，并为这个 future 绑定一个 continuation，当 future 变为就绪时，它将继续运行线程的函数(在同一个已保存的堆栈上)。

Just like normal Seastar continuations, `seastar::thread`s always run on the same core they were launched on. They are also cooperative: they are never preempted except when `seastar::future::get()` blocks or on explict calls to `seastar::thread::yield()`.

> 就像普通的 Seastar continuation 一样, `seastar::thread` 总是在启动它们的同一核心上运行。它们也是协作性的: 它们永远不会被抢占，除非在 `seastar::future::get()` 阻塞或显式调用 `seastar::thread::yield()`.

It is worth reiterating that a `seastar::thread` is not a POSIX thread, and it can only block on Seastar futures, not on blocking system calls. The above example used `seastar::sleep()`, not the `sleep()` system call. The `seastar::thread`'s function can throw and catch exceptions normally. Remember that `get()` will throw an exception if the future resolves with an exception.

> 值得重申的是, `seastar::thread` 不是 POSIX 线程，它只能阻塞 Seastar futures, 而不能阻塞系统调用。上面的例子使用了 `seastar::sleep()`, 而不是 `sleep()` 系统调用。线程的函数可以正常地抛出和捕获异常。记住，如果 future 包含异常, `get()` 将抛出异常。

In addition to `seastar::future::get()`, we also have `seastar::future::wait()` to wait *without* fetching the future's result. This can sometimes be useful when you want to avoid throwing an exception when the future failed (as `get()` does). For example:

> 除了 `seastar::future::get()` 之外，我们还有 `seastar::future::wait()` 来等待而不获取 future 的结果。当你希望避免在 future 失败时抛出异常(如 `get()`)时，这有时会很有用。

```cpp
    future<char> getchar();
    int try_getchar() noexcept { // run this in seastar::thread context
        future fut = get_char();
        fut.wait();
        if (fut.failed()) {
            return -1;
        } else {
            // Here we already know that get() will return immediately,
            // and will not throw.
            return fut.get();
        }
    }
```

## Starting and ending a seastar::thread
After we created a `seastar::thread` object, we need wait until it ends, using its `join()` method. We also need to keep that object alive until `join()` completes. A complete example using `seastar::thread` will therefore look like this:

> 在我们创建了一个 `seastar::thread` 对象之后，我们需要使用它的 `join()` 方法等待它结束。在 `join()` 完成之前，我们还需要保持该对象处于活动状态。使用 `seastar::thread` 的完整示例如下:

```cpp
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
seastar::future<> f() {
    seastar::thread th([] {
        std::cout << "Hi.\n";
        for (int i = 1; i < 4; i++) {
            seastar::sleep(std::chrono::seconds(1)).get();
            std::cout << i << "\n";
        }
    });
    return do_with(std::move(th), [] (auto& th) {
        return th.join();
    });
}
```

The `seastar::async()` function provides a convenient shortcut for creating a `seastar::thread` and returning a future which resolves when the thread completes:

> `seastar::async()` 函数提供了一个方便快捷的方式来创建 `seastar::thread`，该函数在线程完成时返回一个已经就绪的 future:

```cpp
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
seastar::future<> f() {
    return seastar::async([] {
        std::cout << "Hi.\n";
        for (int i = 1; i < 4; i++) {
            seastar::sleep(std::chrono::seconds(1)).get();
            std::cout << i << "\n";
        }
    });
}
```

`seastar::async()`'s lambda may return a value, and `seastar::async()` returns it when it completes. For example:

> `seastar::async()` 的 lambda 可能会返回一个值，并在 `seastar::async()` 完成时返回它。例如：

```cpp
seastar::future<seastar::sstring> read_file(sstring file_name) {
    return seastar::async([file_name] () {  // lambda executed in a thread
        file f = seastar::open_file_dma(file_name).get0();  // get0() call "blocks"
        auto buf = f.dma_read(0, 512).get0();  // "block" again
        return seastar::sstring(buf.get(), buf.size());
    });
};
```

While `seastar::thread`s and `seastar::async()` make programming more convenient, they also add overhead beyond that of programming directly with continuations. Most notably, each `seastar::thread` requires additional memory for its stack. It is therefore not a good idea to use a `seastar::thread` to handle a highly concurrent operation. For example, if you need to handle 10,000 concurrent requests, do not use a `seastar::thread` to handle each --- use futures and continuations. But if you are writing code where you know that only a few instances will ever run concurrently, e.g., a background cleanup operation in your application, `seastar::thread` is a good match. `seastar::thread` is also great for code which doesn't care about performance --- such as test code.

> 虽然 `seastar::thread` 和 `seastar::async()` 使编程更加方便，但它们也增加了直接使用 continuations 进行编程的开销。最明显的是，每个 `seastar::thread` 需要额外的内存用于它的堆栈。因此，使用 `seastar::thread` 来处理高并发操作不是一个好主意。例如，如果你需要处理 10,000 个并发请求，不要使用 `seastar::thread` 来处理每个请求，而是使用 future 和 continuation。但如果你在写代码，你知道只有几个实例可以并发运行，例如，在你的应用程序的后台清理操作，`seastar::thread` 是一个很好的匹配。`seastar::thread` 对于不关心性能的代码也很好，比如测试代码。

# Isolation of application components
Seastar makes multi-tasking very easy - as easy as running an asynchronous function. It is therefore easy for a server to do many unrelated things in parallel. For example, a server might be in the process of answering 100 users' requests, and at the same time also be making progress on some long background operation.

> Seastar 使多任务处理就像运行异步函数一样简单。因此，服务器很容易并行地做许多不相关的事情。例如，服务器可能正在响应 100 个用户的请求，同时也在进行一些长时间的后台操作。

But in the above example, what percentage of the CPU and disk throughput will the background operation get? How long can one of the user's requests be delayed by the background operation? Without the mechanisms we describe in this section, these questions cannot be reliably answered:

* The background operation may be a very "considerate" single fiber, i.e., run a very short continuation and then schedule the next continuation to run later. At each point the scheduler sees 100 request-handling continuations and just one of the background continuations ready to run. The background task gets around 1% of the CPU time, and users' requests are hardly delayed.
* On the other hand, the background operation may spawn 1,000 fibers in parallel and have 1,000 ready-to-run continuations at each time. The background operation will get about 90% of the runtime, and the continuation handling a user's request may get stuck behind 1,000 of these background continuations, and experience huge latency.

> 但是在上面的例子中，后台操作将获得多少百分比的 CPU 和磁盘吞吐量？用户的一个请求可以被后台操作延迟多长时间？没有我们在本节中描述的机制，这些问题无法得到可靠的回答:
>
> * 后台操作可能是一个非常 “周到” 的单一 fiber，即运行一个非常短的 continuation, 然后安排下一个 continuation 稍后运行。在每个点上，调度器看到 100 个处理请求的 continuation, 并且只有一个准备好运行的后台 continuation。后台任务只占用 1% 左右的 CPU 时间，用户的请求几乎没有延迟。
> * 另一方面，后台操作可能并行生成 1,000 个 fiber，并且每次都有 1,000 个准备运行的 continuation. 后台操作将获得大约 90% 的运行时间，而处理用户请求的 continuation 可能会被安排在 1000 个这样的后台 continuation 之后，并经历巨大的延迟。

Complex Seastar applications often have different components which run in parallel and have different performance objectives. In the above example we saw two components - user requests and the background operation.  The first goal of the mechanisms we describe in this section is to _isolate_ the performance of each component from the others; In other words, the throughput and latency of one component should not depend on decisions that another component makes - e.g., how many continuations it runs in parallel. The second goal is to allow the application to _control_ this isolation, e.g., in the above example allow the application to explicitly control the amount of CPU the background operation recieves, so that it completes at a desired pace.

> 复杂的 Seastar 应用程序通常具有许多不同的并行运行的组件，这些组件具有不同的性能目标。在上面的例子中，我们看到了两个组件：用户请求和后台操作。我们在本节中描述的机制的第一个目标是将每个组件的性能与其他组件隔离开来，换句话说，一个组件的吞吐量和延迟不应该取决于另一个组件做出的决定，例如，它并行运行了多少个 continuation。第二个目标是允许应用程序控制这种隔离，例如，在上面的例子中，允许应用程序显式地控制后台操作接收的 CPU 数量，以便它以预期的速度完成。

In the above examples we used CPU time as the limited resource that the different components need to share effectively. As we show later, another important shared resource is disk I/O.

> 在上面的例子中，我们使用 CPU 时间作为不同组件需要共享的有限资源。我们稍后将介绍，另一个重要的共享资源是磁盘 I/O.

## Scheduling groups (CPU scheduler)
Consider the following asynchronous function `loop()`, which loops until some shared variable `stop` becomes true. It keeps a `counter` of the number of iterations until stopping, and returns this counter when finally stopping.

> 考虑下面的异步函数 `loop()`, 它循环直到某个共享变量 `stop` 变为 true。它保留迭代次数 `counter` 直到停止，并在最终停止时返回该计数器。

```cpp
seastar::future<long> loop(int parallelism, bool& stop) {
    return seastar::do_with(0L, [parallelism, &stop] (long& counter) {
        return seastar::parallel_for_each(boost::irange<unsigned>(0, parallelism),
            [&stop, &counter]  (unsigned c) {
                return seastar::do_until([&stop] { return stop; }, [&counter] {
                    ++counter;
                    return seastar::make_ready_future<>();
                });
            }).then([&counter] { return counter; });
    });
}
```
The `parallelism` parameter determines the parallelism of the silly counting operation: `parallelism=1` means we have just one loop incrementing the counter; `parallelism=10` means we start 10 loops in parallel all incrementing the same counter.

> `parallelism` 参数决定了计数操作的并行度: `parallelism=1` 意味着我们只有一个循环递增计数器; `parallelism=10` 意味着我们并行启动 10 个循环，所有循环都递增相同的计数器。

What happens if we start two `loop()` calls in parallel and let them run for 10 seconds?

> 如果我们并行启动两个 `loop()` 调用，并让它们运行 10 秒，会发生什么？

```c++
seastar::future<> f() {
    return seastar::do_with(false, [] (bool& stop) {
        seastar::sleep(std::chrono::seconds(10)).then([&stop] {
            stop = true;
        });
        return seastar::when_all_succeed(loop(1, stop), loop(1, stop)).then(
            [] (long n1, long n2) {
                std::cout << "Counters: " << n1 << ", " << n2 << "\n";
            });
    });
}
```
It turns out that if the two `loop()` calls had the same parallelism `1`, we get roughly the same amount of work from both of them:

> 事实证明，如果两个 `loop()` 调用具有相同的并行度 `1`, 它们的工作量大致相同:

```
Counters: 3'559'635'758, 3'254'521'376
```
But if for example we ran a `loop(1)` in parallel with a `loop(10)`, the result is that the `loop(10)` gets 10 times more work done:

> 但如果我们同时运行 `loop(1)` 和 `loop(10)`， 结果是 `loop(10)` 完成的工作量是 `loop(1)` 的 10 倍:

```
Counters: 629'482'397, 6'320'167'297
```

Why does the amount of work that loop(1) can do in ten seconds depends on the parallelism chosen by its competitor, and how can we solve this?

> 为什么 loop(1) 在 10 秒内所做的工作取决于竞争对手选择的并行度，我们如何解决这个问题？

The reason this happens is as follows: When a future resolves and a continuation was linked to it, this continuation becomes ready to run. By default, Seastar's scheduler keeps a single list of ready-to-run continuations (in each shard, of course), and runs the continuations at the same order they became ready to run. In the above example, `loop(1)` always has one ready-to-run continuation, but `loop(10)`, which runs 10 loops in parallel, always has ten ready-to-run continuations. So for every continuation of `loop(1)`, Seastar's default scheduler will run 10 continuations of `loop(10)`, which is why loop(10) gets 10 times more work done.

> 发生这种情况的原因如下: 当一个 future 就绪并且一个 continuation 链接到它时，这个 continuation 就可以运行了。默认情况下, Seastar 的调度器维护着一个准备运行的 continuation 列表(当然是在每个分片中)，并按照他们准备好的顺序运行这些 continuation。在上面的例子中，`loop(1)` 总是有一个准备运行的 continuation，但是并行运行 10 个循环的 `loop(10)` 总是有 10 个准备运行的 continuation。因此，对于 `loop(1)` 的每一个 continuation，Seastar 的默认调度器将运行 `loop(10)` 的 10 个 continuation，这就是为什么 `loop(10)` 可以多完成 10 倍的工作。

To solve this, Seastar allows an application to define separate components known as **scheduling groups**, which each has a separate list of ready-to-run continuations. Each scheduling group gets to run its own continuations on a desired percentage of the CPU time, but the number of runnable continuations in one scheduling group does not affect the amount of CPU that another scheduling group gets. Let's look at how this is done:

> 为了解决这个问题，Seastar 允许应用程序定义独立的组件，称为 **scheduling groups** ，每个调度组都有一个独立的准备运行的 continuation 列表。每个调度组可以在所需的 CPU 时间百分比上运行自己的 continuation，但是一个调度组中可运行 continuation 的数量不会影响另一个调度组获得的 CPU 数量。让我们看看这是如何做到的:

A scheduling group is defined by a value of type `scheduling_group`. This value is opaque, but internally it is a small integer (similar to a process ID in Linux). We use the `seastar::with_scheduling_group()` function to run code in the desired scheduling group:

> 调度组由类型为 `scheduling_group` 的值定义。这个值是不透明的，但在内部它是一个小整数(类似于Linux中的进程ID)。我们使用 ` seastar::with_scheduling_group()` 函数来运行所需调度组中的代码:

```cpp
seastar::future<long>
loop_in_sg(int parallelism, bool& stop, seastar::scheduling_group sg) {
    return seastar::with_scheduling_group(sg, [parallelism, &stop] {
        return loop(parallelism, stop);
    });
}
```

TODO: explain what `with_scheduling_group` group really does, how the group is "inherited" to the continuations started inside it.

> TODO: 解释 `with_scheduling_group` 组的真正作用，该组是如何 “继承” 到它内部开始的 continuation 的。

Now let's create two scheduling groups, and run `loop(1)` in the first scheduling group and `loop(10)` in the second scheduling group:

> 现在让我们创建两个调度组，并在第一个调度组中运行 `loop(1)`，在第二个调度组中运行 `loop(10)`:

```cpp
seastar::future<> f() {
    return seastar::when_all_succeed(
            seastar::create_scheduling_group("loop1", 100),
            seastar::create_scheduling_group("loop2", 100)).then(
        [] (seastar::scheduling_group sg1, seastar::scheduling_group sg2) {
        return seastar::do_with(false, [sg1, sg2] (bool& stop) {
            seastar::sleep(std::chrono::seconds(10)).then([&stop] {
                stop = true;
            });
            return seastar::when_all_succeed(loop_in_sg(1, stop, sg1), loop_in_sg(10, stop, sg2)).then(
                [] (long n1, long n2) {
                    std::cout << "Counters: " << n1 << ", " << n2 << "\n";
                });
        });
    });
}
```
Here we created two scheduling groups, `sg1` and `sg2`. Each scheduling group has an arbitrary name (which is used for diagnostic purposes only), and a number of *shares*, a number traditionally between 1 and 1000: If one scheduling group has twice the number of shares than a second scheduling group, it will get twice the amount of CPU time. In this example, we used the same number of shares (100) for both groups, so they should get equal CPU time.

> 这里我们创建了两个调度组: `sg1` 和 `sg2`。每个调度组都有一个任意的名称(仅用于诊断目的)和一些共享资源，这个数量通常在 1 到 1000 之间: 如果一个调度组的共享数量是第二个调度组的两倍，那么它将获得两倍的 CPU 时间。在本例中，我们为两组使用了相同数量的共享(100)，因此它们应该获得相同的 CPU 时间。

Unlike most objects in Seastar which are separate per shard, Seastar wants the identities and numbering of the scheduling groups to be the same on all shards, because it is important when invoking tasks on remote shards. For this reason, the function to create a scheduling group, `seastar::create_scheduling_group()`, is an asynchronous function returning a `future<scheduling_group>`.

> 不像 Seastar 中的大多数对象在每个分片上是独立的，Seastar 希望调度组的标识和编号在所有分片上是相同的，因为这在远程分片上调用任务时很重要。因此，创建调度组的函数 `seastar::create_scheduling_group()` 是一个返回 `future<scheduling_group>` 的异步函数。

Running the above example, with both scheduling group set up with the same number of shares (100), indeed results in both scheduling groups getting the same amount of CPU time:

> 运行上面的例子，用相同数量的共享(100)设置两个调度组，确实会导致两个调度组获得相同数量的 CPU 时间:

```
Counters: 3'353'900'256, 3'350'871'461
```

Note how now both loops got the same amount of work done - despite one loop having 10 times the parallelism of the second loop.

> 注意，现在两个循环完成的工作量是一样的，尽管其中一个循环的并行度是第二个循环的 10 倍。

If we change the definition of the second scheduling group to have 200 shares, twice the number of shares of the first scheduling group, we'll see the second scheduling group getting twice the amount of CPU time:

> 如果我们将第二个调度组的定义更改为 200 个共享，是第一个调度组的共享数量的两倍，我们将看到第二个调度组获得两倍的 CPU 时间量:

```
Counters: 2'273'783'385, 4'549'995'716
```
## Latency
TODO: Task quota, preempt, loops with built-in preemption check, etc.
## Disk I/O scheduler
TODO
## Network scheduler
TODO: Say that not yet available. Give example of potential problem - e.g., sharing a slow WAN link.
## Controllers
TODO: Talk about how to dynamically change the number of shares, and why.
## Multi-tenancy
TODO
