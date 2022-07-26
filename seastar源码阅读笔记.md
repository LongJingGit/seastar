## 源码分析

```sh

// 创建 listen socket, 并执行 bind, listen 等操作

_tcp_listeners.push_back(seastar::listen(make_ipv4_address(addr), lo));

--->

server_socket listen(socket_address sa, listen_options opts)            // reactor.cc:4301

--->

reactor::listen(socket_address sa, listen_options opt)                  // reactor.cc:1455

--->

posix_network_stack::listen(socket_address sa, listen_options opt)      // posix-stack.cc:623

--->

reactor::posix_listen(socket_address sa, listen_options opts)           // reactor.cc:1354

--->

server_socket(std::make_unique<posix_server_socket_impl>())             // posix-stack.c:636

--->

explicit posix_server_socket_impl()                                     // posix-stack.cc:225

--->

server_socket::server_socket(std::unique_ptr<net::server_socket_impl> ssi)          // stack.cc:159


########################################################################################################################


// 将 listen socket 的 POLLINT 事件封装成异步任务, 添加到 iocbs 中

do_accepts(_tcp_listeners);                     

--->

(void)listeners[which].accept()                                                 // tcp_sctp_server_demo.cc:84

--->

future<accept_result> server_socket::accept()                                   // stack.cc:168

--->

posix_server_socket_impl::accept()                                              // posix-stack.cc:446

--->

future<std::tuple<pollable_fd, socket_address>> accept()                        // pollable_fd.hh:155

--->

future<std::tuple<pollable_fd, socket_address>> pollable_fd_state::accept()     // reactor.cc:441

--->

reactor_backend_aio::accept(pollable_fd_state& listenfd)                        // reactor_backend.cc:526

--->

reactor::do_accept(pollable_fd_state& listenfd)                                 // reactor.cc:229

--->

future<> reactor::readable_or_writeable(pollable_fd_state& fd)                  // reactor.cc:982

--->

future<> reactor_backend_aio::readable_or_writeable(pollable_fd_state& fd)      // reactor_backend.cc:515

--->

// 将监听 socket 的 POLLIN 事件封装成异步 io, 然后将该异步任务添加到 iocbs 中
future<> reactor_backend_aio::poll(pollable_fd_state& fd, int events)           // reactor_backend.cc:485       

--->

void aio_general_context::queue(linux_abi::iocb* iocb)                          // reactor_backend.cc:250


// 返回的是未就绪的 future 

########################################################################################################################


// kernel_submit_work_pollfn::poll               将异步任务递交给内核

poller kernel_submit_work_poller(std::make_unique<kernel_submit_work_pollfn>(*this));       // reactor.cc:2755

--->

check_for_work                                                  // reactor.cc:2848
                                                                
--->                                                                
                                                                
reactor::poll_once()                                            // reactor.cc:2975

--->

_r._backend->kernel_submit_work()                               // reactor.cc:2263

--->

bool reactor_backend_aio::kernel_submit_work()                  // reactor_backend.cc:433

--->

size_t aio_general_context::flush()                             // reactor_backend.cc:254
aio_storage_context::submit_work()                              // reactor_backend.cc:147

--->

int io_submit(aio_context_t io_context, long nr, iocb** iocbs)          // linux-aio.cc: 69


########################################################################################################################


// reap_kernel_completions_pollfn::poll         检查内核是否完成了异步 io

// 如果内核完成了异步任务, 则执行 promise.set_value, 然后 future 变成就绪态, future.then 调用中的任务可以执行

poller final_real_kernel_completions_poller(std::make_unique<reap_kernel_completions_pollfn>(*this));       // reactor.cc:2756

--->

check_for_work                                                  // reactor.cc:2848

--->

reactor::poll_once()                                            // reactor.cc:2975

--->

_r.reap_kernel_completions()                                    // reactor.cc:2360

--->

reactor::reap_kernel_completions()                              // reactor.cc:1524

--->

bool reactor_backend_aio::reap_kernel_completions()             // reactor_backend.cc:427

--->

bool reactor_backend_aio::await_events(int timeout, const sigset_t* active_sigmask)     // reactor_backend.cc:370

--->

desc->complete_with(event.res);             // reactor_backend.cc:396


########################################################################################################################
########################################################################################################################


// 从连接 socket 读取数据

future<> process()                          // tcp_sctp_server_demo.cc:119

--->

future<> read()                             // tcp_sctp_server_demo.cc:124

-->

input_stream<CharType>::read_exactly(size_t n)              // iostream-impl.hh:169

--->

_fd.get()                                                   // iostream-impl.hh:186

--->

future<temporary_buffer<char>> get()                        // iostream.hh:64

--->

posix_data_source_impl::get()                               // posix-stack.cc:561

--->

future<temporary_buffer<char>> read_some(internal::buffer_allocator* ba)            // pollable_fd.hh:125

--->

future<temporary_buffer<char>> pollable_fd_state::read_some()                       // reactor.cc:380

--->

reactor_backend_aio::read_some()                                                    // reactor_backend.cc:549

--->

reactor::do_read_some(pollable_fd_state& fd, internal::buffer_allocator* ba)        // reactor.cc:275

--->

future<> pollable_fd_state::readable()                      // reactor.cc:419

--->

future<> reactor::readable(pollable_fd_state& fd)           // reactor.cc:974

-->

// 将连接 socket 的 POLLIN 事件封装成异步 io, 然后将该异步任务添加到 iocbs 中
future<> reactor_backend_aio::poll(pollable_fd_state& fd, int events)      // reactor_backend.cc:485       

--->

kernel_submit_work_pollfn::poll         将异步 io 递交给内核

reap_kernel_completions_pollfn::poll    检查内核是否已经完成了异步 io

--->

/* 假如内核已经完成了异步 io */

boost::optional<size_t> read(void* buffer, size_t len)              // posix.hh:216         从连接 socket 读取数据


########################################################################################################################
########################################################################################################################












// 构造异步 io

future<size_t> posix_file_impl::write_dma()                 // file.cc:327

--->

future<size_t> reactor::submit_io_write()                   // reactor.cc:1547

--->

future<size_t> io_queue::queue_request()                    // io_queue.cc:248

--->

void fair_queue::queue()                                    // fair_queue.cc:147

--->

push_priority_class(pc);                                                            // fair_queue.cc:151
pc->_queue.push_back(priority_class::request{std::move(func), std::move(desc)});    // fair_queue.cc:153


########################################################################################################################


// io_queue_submission_pollfn::poll             将异步 io 保存到 _pending_io 中

h = pop_priority_class();                                   // fair_queue.cc:168
auto req = std::move(h->_queue.front());                    // fair_queue.cc:171

--->

req.func()                                                  // fair_queue.cc:199. 这里的 func 实际上就是 pc->_queue.push_back 时的 func

--->

reactor::submit_io(kernel_completion* desc, io_request req)         // reactor.cc:1507


########################################################################################################################


// kernel_submit_work_pollfn::poll           将异步io递交给内核

// reap_kernel_completions_pollfn::poll      检查内核是否完成了异步io, 并执行 promise.set_value, 然后 future 变成就绪态, future.then 调用中的任务可以执行


########################################################################################################################

```

## seastar Ubuntu20.04 源码编译

```sh
sudo ./install-dependencies.sh
```

```sh
CXX=g++ ./cooking.sh -i c-ares -i fmt # 默认是 debug 版本，如果想要编译 release, 可以用 -t Release 指定
```

如果执行这一步显示 CXX环境变量没有设置，可以通过 `export CXX=/usr/bin/g++` 设置环境变量

注意，执行这一步可能出现如下错误：

```sh
FAILED: _cooking/ingredient/fmt/stamp/ingredient_fmt-download
cd /home/jinglong/workspace/seastar/build/_cooking/ingredient/fmt && /usr/local/bin/cmake -P /home/jinglong/workspace/seastar/build/_cooking/ingredient/fmt/stamp/download-ingredient_fmt.cmake && /usr/local/bin/cmake -P /home/jinglong/workspace/seastar/build/_cooking/ingredient/fmt/stamp/verify-ingredient_fmt.cmake && /usr/local/bin/cmake -P /home/jinglong/workspace/seastar/build/_cooking/ingredient/fmt/stamp/extract-ingredient_fmt.cmake && /usr/local/bin/cmake -E touch /home/jinglong/workspace/seastar/build/_cooking/ingredient/fmt/stamp/ingredient_fmt-download
-- Downloading...
   dst='/home/jinglong/workspace/seastar/build/_cooking/ingredient/fmt/src/5.2.1.tar.gz'
   timeout='none'
   inactivity timeout='none'
-- Using src='https://github.com/fmtlib/fmt/archive/5.2.1.tar.gz'
CMake Error at stamp/download-ingredient_fmt.cmake:170 (message):
  Each download failed!

    error: downloading 'https://github.com/fmtlib/fmt/archive/5.2.1.tar.gz' failed
          status_code: 56
          status_string: "Failure when receiving data from the peer"
          log:
          --- LOG BEGIN ---
            Trying 20.205.243.166:443...

  Connected to github.com (20.205.243.166) port 443 (#0)

  ALPN, offering h2

  ALPN, offering http/1.1

  successfully set certificate verify locations:

```

可能是由于网络导致的下载程序失败，建议关闭代理后重试

```sh
ninja -C build
```

如果使用上面步骤编译完成之后，以后可以使用 cmake&make 进行编译:

```sh
mkdir _build & cd _build
cmake ..
make -j2
```

