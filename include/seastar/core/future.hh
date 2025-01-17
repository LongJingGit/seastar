/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/core/apply.hh>
#include <seastar/core/task.hh>
#include <seastar/core/preempt.hh>
#include <seastar/core/thread_impl.hh>
#include <stdexcept>
#include <atomic>
#include <memory>
#include <type_traits>
#include <assert.h>
#include <cstdlib>
#include <seastar/core/function_traits.hh>
#include <seastar/util/alloc_failure_injector.hh>
#include <seastar/util/attribute-compat.hh>
#include <seastar/util/concepts.hh>
#include <seastar/util/noncopyable_function.hh>

#if __cplusplus > 201703L
#include <version>
#endif

namespace seastar {

/// \defgroup future-module Futures and Promises
///
/// \brief
/// Futures and promises are the basic tools for asynchronous
/// programming in seastar.  A future represents a result that
/// may not have been computed yet, for example a buffer that
/// is being read from the disk, or the result of a function
/// that is executed on another cpu.  A promise object allows
/// the future to be eventually resolved by assigning it a value.
///
/// \brief
/// Another way to look at futures and promises are as the reader
/// and writer sides, respectively, of a single-item, single use
/// queue.  You read from the future, and write to the promise,
/// and the system takes care that it works no matter what the
/// order of operations is.
///
/// \brief
/// The normal way of working with futures is to chain continuations
/// to them.  A continuation is a block of code (usually a lamdba)
/// that is called when the future is assigned a value (the future
/// is resolved); the continuation can then access the actual value.
///

/// \defgroup future-util Future Utilities
///
/// \brief
/// These utilities are provided to help perform operations on futures.


/// \addtogroup future-module
/// @{

template <class... T>
class promise;

template <class... T>
class future;

template <typename... T>
class shared_future;

struct future_state_base;

/// \brief Creates a \ref future in an available, value state.
///
/// Creates a \ref future object that is already resolved.  This
/// is useful when it is determined that no I/O needs to be performed
/// to perform a computation (for example, because the data is cached
/// in some buffer).
template <typename... T, typename... A>
future<T...> make_ready_future(A&&... value) noexcept;

/// \brief Creates a \ref future in an available, failed state.
///
/// Creates a \ref future object that is already resolved in a failed
/// state.  This is useful when no I/O needs to be performed to perform
/// a computation (for example, because the connection is closed and
/// we cannot read from it).
template <typename... T>
future<T...> make_exception_future(std::exception_ptr&& value) noexcept;

template <typename... T>
future<T...> make_exception_future(const std::exception_ptr& ex) noexcept {
    return make_exception_future<T...>(std::exception_ptr(ex));
}

template <typename... T>
future<T...> make_exception_future(std::exception_ptr& ex) noexcept {
    return make_exception_future<T...>(static_cast<const std::exception_ptr&>(ex));
}

/// \cond internal
void engine_exit(std::exception_ptr eptr = {});

void report_failed_future(const std::exception_ptr& ex) noexcept;

void report_failed_future(const future_state_base& state) noexcept;

/// \endcond

/// \brief Exception type for broken promises
///
/// When a promise is broken, i.e. a promise object with an attached
/// continuation is destroyed before setting any value or exception, an
/// exception of `broken_promise` type is propagated to that abandoned
/// continuation.
struct broken_promise : std::logic_error {
    broken_promise();
};

namespace internal {
template <class... T>
class promise_base_with_type;

template <typename... T>
future<T...> current_exception_as_future() noexcept;

extern template
future<> current_exception_as_future() noexcept;

// It doesn't seem to be possible to use std::tuple_element_t with an empty tuple. There is an static_assert in it that
// fails the build even if it is in the non enabled side of std::conditional.
template <typename... T>
struct get0_return_type {
    using type = void;
    static type get0(std::tuple<T...> v) { }
};

template <typename T0, typename... T>
struct get0_return_type<T0, T...> {
    using type = T0;
    static type get0(std::tuple<T0, T...> v) { return std::get<0>(std::move(v)); }
};

/// \brief Wrapper for keeping uninitialized values of non default constructible types.
///
/// This is similar to a std::optional<T>, but it doesn't know if it is holding a value or not, so the user is
/// responsible for calling constructors and destructors.
///
/// The advantage over just using a union directly is that this uses inheritance when possible and so benefits from the
/// empty base optimization.
template <typename T, bool is_trivial_class>
struct uninitialized_wrapper_base;

template <typename T>
struct uninitialized_wrapper_base<T, false> {
    union any {
        any() {}
        ~any() {}
        T value;
    } _v;

public:
    template<typename... U>
    void uninitialized_set(U&&... vs) {
        new (&_v.value) T(std::forward<U>(vs)...);
    }
    T& uninitialized_get() {
        return _v.value;
    }
    const T& uninitialized_get() const {
        return _v.value;
    }
};

template <typename T>
struct uninitialized_wrapper_base<T, true> : private T
{
    template<typename... U>
    void uninitialized_set(U&&... vs)
    {
        new (this) T(std::forward<U>(vs)...);       // 在 promise 执行 set_value 的时候会被调用
    }

    T& uninitialized_get()
    {
        return *this;
    }

    const T& uninitialized_get() const
    {
        return *this;
    }
};

template <typename T>
constexpr bool can_inherit =
#ifdef _LIBCPP_VERSION
// We expect std::tuple<> to be trivially constructible and
// destructible. That is not the case with libc++
// (https://bugs.llvm.org/show_bug.cgi?id=41714).  We could avoid this
// optimization when using libc++ and relax the asserts, but
// inspection suggests that std::tuple<> is trivial, it is just not
// marked as such.
        std::is_same<std::tuple<>, T>::value ||
#endif
        (std::is_trivially_destructible<T>::value && std::is_trivially_constructible<T>::value &&
                std::is_class<T>::value && !std::is_final<T>::value);

// The objective is to avoid extra space for empty types like std::tuple<>. We could use std::is_empty_v, but it is
// better to check that both the constructor and destructor can be skipped.
template <typename T>
struct uninitialized_wrapper
    : public uninitialized_wrapper_base<T, can_inherit<T>> {};

template <typename T>
struct is_trivially_move_constructible_and_destructible {
    static constexpr bool value = std::is_trivially_move_constructible<T>::value && std::is_trivially_destructible<T>::value;
};

template <bool... v>
struct all_true : std::false_type {};

template <>
struct all_true<> : std::true_type {};

template <bool... v>
struct all_true<true, v...> : public all_true<v...> {};
}

//
// A future/promise pair maintain one logical value (a future_state).
// There are up to three places that can store it, but only one is
// active at any time.
//
// - in the promise _local_state member variable
//
//   This is necessary because a promise is created first and there
//   would be nowhere else to put the value.
//
// - in the future _state variable
//
//   This is used anytime a future exists and then has not been called
//   yet. This guarantees a simple access to the value for any code
//   that already has a future.
//
// - in the task associated with the .then() clause (after .then() is called,
//   if a value was not set)
//
//
// The promise maintains a pointer to the state, which is modified as
// the state moves to a new location due to events (such as .then() or
// get_future being called) or due to the promise or future being
// moved around.
//

// non templated base class to reduce code duplication
struct future_state_base
{
    static_assert(sizeof(std::exception_ptr) == sizeof(void*), "exception_ptr not a pointer");

    enum class state : uintptr_t {
         invalid = 0,
         future = 1,            // future_state 的初始 state 为 future
         // the substate is intended to decouple the run-time prevention
         // for duplicative result extraction (calling e.g. then() twice
         // ends up in abandoned()) from the wrapped object's destruction
         // handling which is orchestrated by future_state. Instead of
         // creating a temporary future_state just for the sake of setting
         // the "invalid" in the source instance, result_unavailable can
         // be set to ensure future_state_base::available() returns false.
         result_unavailable = 2,
         result = 3,
         exception_min = 4,  // or anything greater
    };

    union any
    {
        any() noexcept { st = state::future; }
        any(state s) noexcept { st = s; }
        void set_exception(std::exception_ptr&& e) noexcept {
            new (&ex) std::exception_ptr(std::move(e));
            assert(st >= state::exception_min);
        }
        any(std::exception_ptr&& e) noexcept {
            set_exception(std::move(e));
        }
        bool valid() const noexcept { return st != state::invalid; }
        bool available() const noexcept { return st == state::result || st >= state::exception_min; }
        bool failed() const noexcept { return __builtin_expect(st >= state::exception_min, false); }
        void check_failure() noexcept {
            if (failed()) {
                report_failed_future(take_exception());
            }
        }
        ~any() noexcept {
            check_failure();
        }

        std::exception_ptr take_exception() noexcept {
            std::exception_ptr ret(std::move(ex));
            // Unfortunately in libstdc++ ~exception_ptr is defined out of line. We know that it does nothing for
            // moved out values, so we omit calling it. This is critical for the code quality produced for this
            // function. Without the out of line call, gcc can figure out that both sides of the if produce
            // identical code and merges them.if
            // We don't make any assumptions about other c++ libraries.
            // There is request with gcc to define it inline: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90295
#ifndef __GLIBCXX__
            ex.~exception_ptr();
#endif
            st = state::invalid;
            return ret;
        }
        void move_it(any&& x) noexcept {
            if (x.st < state::exception_min) {
                st = x.st;
                x.st = state::invalid;
            } else {
                new (&ex) std::exception_ptr(x.take_exception());
            }
        }
        any(any&& x) noexcept {
            move_it(std::move(x));
        }
        any& operator=(any&& x) noexcept {
            check_failure();
            // If this is a self move assignment, check_failure
            // guarantees that we don't have an exception and calling
            // move_it is safe.
            move_it(std::move(x));
            return *this;
        }
        bool has_result() const {
            return st == state::result || st == state::result_unavailable;
        }
        state st;
        std::exception_ptr ex;
    } _u;

    future_state_base() noexcept { }
    future_state_base(state st) noexcept : _u(st) { }
    future_state_base(std::exception_ptr&& ex) noexcept : _u(std::move(ex)) { }
    future_state_base(future_state_base&& x) noexcept : _u(std::move(x._u)) { }

    // We never need to destruct this polymorphicly, so we can make it
    // protected instead of virtual.
protected:
    ~future_state_base() noexcept = default;

public:

    bool valid() const noexcept { return _u.valid(); }
    bool available() const noexcept { return _u.available(); }
    bool failed() const noexcept { return _u.failed(); }

    void set_to_broken_promise() noexcept;

    void ignore() noexcept;

    void set_exception(std::exception_ptr&& ex) noexcept {
        assert(_u.st == state::future);
        _u.set_exception(std::move(ex));
    }
    future_state_base& operator=(future_state_base&& x) noexcept = default;
    void set_exception(future_state_base&& state) noexcept {
        assert(_u.st == state::future);
        *this = std::move(state);
    }
    std::exception_ptr get_exception() && noexcept {
        assert(_u.st >= state::exception_min);
        // Move ex out so future::~future() knows we've handled it
        return _u.take_exception();
    }
    const std::exception_ptr& get_exception() const& noexcept {
        assert(_u.st >= state::exception_min);
        return _u.ex;
    }

    static future_state_base current_exception() noexcept;

    template <typename... U>
    friend future<U...> internal::current_exception_as_future() noexcept;
};

struct ready_future_marker {};
struct exception_future_marker {};
struct future_for_get_promise_marker {};

/// \cond internal
template <typename... T>
struct future_state :  public future_state_base, private internal::uninitialized_wrapper<std::tuple<T...>>
{
    static constexpr bool copy_noexcept = std::is_nothrow_copy_constructible<std::tuple<T...>>::value;
    static constexpr bool has_trivial_move_and_destroy =
        internal::all_true<internal::is_trivially_move_constructible_and_destructible<T>::value...>::value;
    static_assert(std::is_nothrow_move_constructible<std::tuple<T...>>::value,
                  "Types must be no-throw move constructible");
    static_assert(std::is_nothrow_destructible<std::tuple<T...>>::value,
                  "Types must be no-throw destructible");

    future_state() noexcept {}

    void move_it(future_state&& x) noexcept
    {
        if (has_trivial_move_and_destroy)
        {
#pragma GCC diagnostic push
// Unfortunately gcc 8 warns about the memcpy of uninitialized
// memory. We can drop this when we drop support for gcc 8.
#pragma GCC diagnostic ignored "-Wuninitialized"
            memmove(reinterpret_cast<char*>(&this->uninitialized_get()),
                   &x.uninitialized_get(),
                   internal::used_size<std::tuple<T...>>::value);
#pragma GCC diagnostic pop
        }
        else if (_u.has_result())
        {
            this->uninitialized_set(std::move(x.uninitialized_get()));
            x.uninitialized_get().~tuple();
        }
    }

    [[gnu::always_inline]]
    future_state(future_state&& x) noexcept
        : future_state_base(std::move(x))
    {
        move_it(std::move(x));
    }

    void clear() noexcept
    {
        if (_u.has_result())
        {
            this->uninitialized_get().~tuple();
        }
    }

    __attribute__((always_inline))
    ~future_state() noexcept
    {
        clear();
    }

    future_state& operator=(future_state&& x) noexcept
    {
        clear();
        future_state_base::operator=(std::move(x));
        // If &x == this, _u.st is now state::invalid and so it is
        // safe to call move_it.
        move_it(std::move(x));
        return *this;
    }

    template <typename... A>
    future_state(ready_future_marker, A&&... a) noexcept
        : future_state_base(state::result)      // 执行基类的构造函数，将 future 的 _state::_u 设置为 state::result
    {
        try
        {
            // 将 future 的 _state::_v 设置为 a.
            // 注意: 这一步很关键, 这里的 a 实际上是作为多个异步任务之间的参数在进行传递，比如在链式调用 .then(func_1).then(func_2) 中, 如果想要将 func_1 的返回值作为 func_2 的入参, 那么参数传递将是通过这里的 a 进行的.
            this->uninitialized_set(std::forward<A>(a)...);
        }
        catch (...)
        {
            new (this) future_state(exception_future_marker(), std::current_exception());
        }
    }

    template <typename... A>
    void set(A&&... a)
    {
        assert(_u.st == state::future);
        new (this) future_state(ready_future_marker(), std::forward<A>(a)...);      // 将 _u.st 设置为 state::result, 将 uninitialized_wrapper_base::_v 设置为 3
    }

    future_state(exception_future_marker m, std::exception_ptr&& ex) noexcept : future_state_base(std::move(ex)) { }
    future_state(exception_future_marker m, future_state_base&& state) noexcept : future_state_base(std::move(state)) { }

    std::tuple<T...>&& get_value() && noexcept
    {
        assert(_u.st == state::result);
        return std::move(this->uninitialized_get());
    }

    std::tuple<T...>&& take_value() && noexcept
    {
        assert(_u.st == state::result);
        _u.st = state::result_unavailable;
        return std::move(this->uninitialized_get());
    }

    template<typename U = std::tuple<T...>>
    const std::enable_if_t<std::is_copy_constructible<U>::value, U>& get_value() const& noexcept(copy_noexcept)
    {
        assert(_u.st == state::result);
        return this->uninitialized_get();
    }

    std::tuple<T...>&& take() &&
    {
        assert(available());
        if (_u.st >= state::exception_min)
        {
            // Move ex out so future::~future() knows we've handled it
            std::rethrow_exception(std::move(*this).get_exception());
        }
        _u.st = state::result_unavailable;
        return std::move(this->uninitialized_get());
    }

    std::tuple<T...>&& get() &&
    {
        assert(available());
        if (_u.st >= state::exception_min)
        {
            // Move ex out so future::~future() knows we've handled it
            std::rethrow_exception(std::move(*this).get_exception());
        }
        return std::move(this->uninitialized_get());
    }

    const std::tuple<T...>& get() const&
    {
        assert(available());
        if (_u.st >= state::exception_min)
        {
            std::rethrow_exception(_u.ex);
        }
        return this->uninitialized_get();
    }

    using get0_return_type = typename internal::get0_return_type<T...>::type;
    static get0_return_type get0(std::tuple<T...>&& x)
    {
        return internal::get0_return_type<T...>::get0(std::move(x));
    }
};

template <typename... T>
class continuation_base : public task
{
protected:
    // 调用无参构造函数时, _state 被初始化成了 state::future
    // 如果 promise_base 中的 _state 指向 continuation_base::_state, 则在 promise 执行 set_value 时, 会将这里的 _state::_u 设置为 result, 并且设置 _state::_v 的值
    // _state::_u 用来判断下一个异步任务是否可以执行; _state::_v 用来在两个异步任务之间传参
    future_state<T...> _state;
    using future_type = future<T...>;
    using promise_type = promise<T...>;

public:
    continuation_base() = default;

    explicit continuation_base(future_state<T...>&& state)
        : _state(std::move(state))
    {}

    void set_state(future_state<T...>&& state)
    {
        _state = std::move(state);
    }

    friend class internal::promise_base_with_type<T...>;
    friend class promise<T...>;
    friend class future<T...>;
};

template <typename Promise, typename... T>
class continuation_base_with_promise : public continuation_base<T...>
{
protected:
    continuation_base_with_promise(Promise&& pr, future_state<T...>&& state)
        : continuation_base<T...>(std::move(state))
        , _pr(std::move(pr))
    {}

    continuation_base_with_promise(Promise&& pr)
        : _pr(std::move(pr))
    {}

    Promise _pr;
};

template <typename Promise, typename Func, typename... T>
struct continuation final : continuation_base_with_promise<Promise, T...>
{
    continuation(Promise&& pr, Func&& func, future_state<T...>&& state)
        : continuation_base_with_promise<Promise, T...>(std::move(pr), std::move(state))
        , _func(std::move(func))
    {}

    continuation(Promise&& pr, Func&& func)
        : continuation_base_with_promise<Promise, T...>(std::move(pr))
        , _func(std::move(func))
    {}

    virtual void run_and_dispose() noexcept override        // 在 reactor 的事件循环中，会调用该接口执行一个任务
    {
        // 这里的 _func 是 then_impl_nrvo --> schedule 中的 lambda; _state 在 promise 执行 set_value 的时候已经被赋值
        _func(this->_pr, std::move(this->_state));      // 真正执行 task 的函数. 注意，这里的参数 this->_pr 是新创建的 promise
        delete this;
    }

    Func _func;
};

namespace internal {

template <typename... T>
future<T...> make_exception_future(future_state_base&& state) noexcept;

template <typename... T, typename U>
void set_callback(future<T...>& fut, U* callback) noexcept;

class future_base;

class promise_base
{
protected:
    enum class urgent { no, yes };
    future_base* _future = nullptr;

    // This points to the future_state that is currently being
    // used. See comment above the future_state struct definition for
    // details.
    // 调用调用 promise_base(future_state_base* state) 构造函数，则 _state 指向的是派生类 promise::_local_state.
    // 如果调用的是 promise_base(future_base* future, future_state_base* state) 构造函数，_state 会指向 future 的 _state.
    // 如果是将 func 封装成异步任务然后需要由 promise 执行 set_value 才可以将 task 递交给 reactor 去执行, 会在 schedule(Pr&& pr, Func&& func) 方法中, 将 _state 指向 continuation_base::_state, continuation_base::_state 被初始化成了 state::future, 当 promise 执行 set_value 时, _state 会被设置为 state::result
    future_state_base* _state;

    task* _task = nullptr;  // 需要执行异步任务时，会将任务封装到 task 中，然后将 _task 指向封装好的 task, 在 make_ready 方法中会将该 task 递交给 reactor

    promise_base(const promise_base&) = delete;
    promise_base(future_state_base* state) noexcept : _state(state) {}
    promise_base(future_base* future, future_state_base* state) noexcept;
    void move_it(promise_base&& x) noexcept;
    promise_base(promise_base&& x) noexcept;

    void clear() noexcept;

    // We never need to destruct this polymorphicly, so we can make it
    // protected instead of virtual
    ~promise_base() noexcept
    {
        clear();
    }

    void operator=(const promise_base&) = delete;
    promise_base& operator=(promise_base&& x) noexcept;

    template<urgent Urgent>
    void make_ready() noexcept;

    template<typename T>
    void set_exception_impl(T&& val) noexcept
    {
        if (_state)
        {
            _state->set_exception(std::move(val));
            make_ready<urgent::no>();
        }
        else
        {
            // We get here if promise::get_future is called and the
            // returned future is destroyed without creating a
            // continuation.
            // In older versions of seastar we would store a local
            // copy of ex and warn in the promise destructor.
            // Since there isn't any way for the user to clear
            // the exception, we issue the warning from here.
            report_failed_future(val);
        }
    }

    void set_exception(future_state_base&& state) noexcept
    {
        set_exception_impl(std::move(state));
    }

    void set_exception(std::exception_ptr&& ex) noexcept
    {
        set_exception_impl(std::move(ex));
    }

    void set_exception(const std::exception_ptr& ex) noexcept
    {
        set_exception(std::exception_ptr(ex));
    }

    template<typename Exception>
    std::enable_if_t<!std::is_same<std::remove_reference_t<Exception>, std::exception_ptr>::value, void> set_exception(Exception&& e) noexcept
    {
        set_exception(make_exception_ptr(std::forward<Exception>(e)));
    }

    friend class future_base;
    template <typename... U> friend class seastar::future;

private:
    void set_to_current_exception() noexcept;
};

/// \brief A promise with type but no local data.
///
/// This is a promise without any local data. We use this for when the
/// future is created first, so we know the promise always has an
/// external place to point to. We cannot just use promise_base
/// because we need to know the type that is being stored.
template <typename... T>
class promise_base_with_type : protected internal::promise_base
{
protected:
    future_state<T...>* get_state()
    {
        return static_cast<future_state<T...>*>(_state);
    }

    static constexpr bool copy_noexcept = future_state<T...>::copy_noexcept;

public:
    promise_base_with_type(future_state_base* state) noexcept
        : promise_base(state)
    {}

    // 通过 future 做参数来构造 promise 实例. 将 future 的 _state 和 promise 绑定到一起
    promise_base_with_type(future<T...>* future) noexcept
        : promise_base(future, &future->_state)
    {}

    promise_base_with_type(promise_base_with_type&& x) noexcept = default;
    promise_base_with_type(const promise_base_with_type&) = delete;
    promise_base_with_type& operator=(promise_base_with_type&& x) noexcept = default;
    void operator=(const promise_base_with_type&) = delete;

    void set_urgent_state(future_state<T...>&& state) noexcept
    {
        auto* ptr = get_state();        // 这里获取了 promise_base::_state 的指针，实际上这里的 promise_base::_state 已经指向了当前 promise 上绑定的 continuation 的 _state, 所以下面对 promise_base::_state 的修改实际上都是在修改 continuation::_state
        // The state can be null if the corresponding future has been
        // destroyed without producing a continuation.
        if (ptr)
        {
            // FIXME: This is a fairly expensive assert. It would be a
            // good candidate for being disabled in release builds if
            // we had such an assert.
            assert(ptr->_u.st == future_state_base::state::future);
            new (ptr) future_state<T...>(std::move(state));     // 实际上修改的是 continuation::_state 的值
            make_ready<urgent::yes>();
        }
    }

    template <typename... A>
    void set_value(A&&... a)
    {
        if (auto *s = get_state())      // 此时 promise 的 _state 已经指向了 then(func) 中的 continuation_base::state. 具体参见 void schedule(Pr&& pr, Func&& func)
        {
            s->set(std::forward<A>(a)...); // 调用 future_state::set. 将 state::_u 设置为 state::result, 将 state::_v 设置为 a. (实际上设置的是 continuation_base 的 _state)
            make_ready<urgent::no>();      // 将 _task 添加到 reactor 的任务队列中
        }
    }

#if defined(SEASTAR_COROUTINES_TS) || defined(__cpp_lib_coroutine)
    void set_coroutine(future_state<T...>& state, task& coroutine) noexcept {
        _state = &state;
        _task = &coroutine;
    }
#endif

private:
    // 调用该接口的 promise 是旧的 promise, 参数 pr 指向的是新创建的 promise, 将 func 和 新的 promise 封装到 continuation 中, 然后将该 continuation 挂在旧的 promise 的 _task 中, 在后面旧的 promise 调用 set_value 时，会将该 task 添加到 reactor 的任务队列中
    template <typename Pr, typename Func>
    void schedule(Pr&& pr, Func&& func) noexcept
    {
        // continuation 中的 _state 被初始化成了 state::future. (在执行旧的 promise 的 set_value 的时候会将 state 设置为 result, 并且设置 state::_v)
        auto tws = new continuation<Pr, Func, T...>(std::move(pr), std::move(func));
        _state = &tws->_state;      // 注意，旧的 promise 的 _state 指向了 continuation_base 中的 _state
        _task = tws;    // 将 continuation 绑定到 promise 的 _task 中, 当 promise 执行 set_value 时会将该 task 添加到 reactor 的任务队列中
    }

    void schedule(continuation_base<T...>* callback) noexcept
    {
        _state = &callback->_state;
        _task = callback;
    }

    template <typename... U>
    friend class seastar::future;

    friend struct seastar::future_state<T...>;
};
}
/// \endcond

/// \brief promise - allows a future value to be made available at a later time.
///
/// \tparam T A list of types to be carried as the result of the associated future.
///           A list with two or more types is deprecated; use
///           \c promise<std::tuple<T...>> instead.
template <typename... T>
class promise : private internal::promise_base_with_type<T...>
{
    future_state<T...> _local_state;    // 初始 state 为 state::future

public:
    /// \brief Constructs an empty \c promise.
    ///
    /// Creates promise with no associated future yet (see get_future()).
    promise() noexcept
        : internal::promise_base_with_type<T...>(&_local_state) {}      // 将 _local_state 作为构造函数入参, 调用基类构造函数

    /// \brief Moves a \c promise object.
    void move_it(promise&& x) noexcept;

    promise(promise&& x) noexcept
        : internal::promise_base_with_type<T...>(std::move(x))
    {
        move_it(std::move(x));
    }

    promise(const promise&) = delete;
    void operator=(const promise&) = delete;

    promise& operator=(promise&& x) noexcept
    {
        internal::promise_base_with_type<T...>::operator=(std::move(x));
        // If this is a self-move, _state is now nullptr and it is
        // safe to call move_it.
        move_it(std::move(x));
        return *this;
    }

    /// \brief Gets the promise's associated future.
    ///
    /// The future and promise will be remember each other, even if either or
    /// both are moved.  When \c set_value() or \c set_exception() are called
    /// on the promise, the future will be become ready, and if a continuation
    /// was attached to the future, it will run.
    future<T...> get_future() noexcept;

    /// \brief Sets the promises value
    ///
    /// Forwards the arguments and makes them available to the associated
    /// future.  May be called either before or after \c get_future().
    ///
    /// The arguments can have either the types the promise is
    /// templated with, or a corresponding std::tuple. That is, given
    /// a promise<int, double>, both calls are valid:
    ///
    /// pr.set_value(42, 43.0);
    /// pr.set_value(std::tuple<int, double>(42, 43.0))
    template <typename... A>
    void set_value(A&&... a)
    {
        internal::promise_base_with_type<T...>::set_value(std::forward<A>(a)...);
    }

    /// \brief Marks the promise as failed
    ///
    /// Forwards the exception argument to the future and makes it
    /// available.  May be called either before or after \c get_future().
    void set_exception(std::exception_ptr&& ex) noexcept
    {
        internal::promise_base::set_exception(std::move(ex));
    }

    void set_exception(const std::exception_ptr& ex) noexcept
    {
        internal::promise_base::set_exception(ex);
    }

    /// \brief Marks the promise as failed
    ///
    /// Forwards the exception argument to the future and makes it
    /// available.  May be called either before or after \c get_future().
    template<typename Exception>
    std::enable_if_t<!std::is_same<std::remove_reference_t<Exception>, std::exception_ptr>::value, void> set_exception(Exception&& e) noexcept
    {
        internal::promise_base::set_exception(std::forward<Exception>(e));
    }

    using internal::promise_base_with_type<T...>::set_urgent_state;

    template <typename... U>
    friend class future;
};

/// \brief Specialization of \c promise<void>
///
/// This is an alias for \c promise<>, for generic programming purposes.
/// For example, You may have a \c promise<T> where \c T can legally be
/// \c void.
template<>
class promise<void> : public promise<> {};

/// @}

/// \addtogroup future-util
/// @{


/// \brief Check whether a type is a future
///
/// This is a type trait evaluating to \c true if the given type is a
/// future.
///
template <typename... T> struct is_future : std::false_type {};

/// \cond internal
/// \addtogroup future-util
template <typename... T> struct is_future<future<T...>> : std::true_type {};

/// \endcond


/// \brief Converts a type to a future type, if it isn't already.
///
/// \return Result in member type 'type'.
template <typename T>
struct futurize;

SEASTAR_CONCEPT(

template <typename T>
concept Future = is_future<T>::value;

template <typename Func, typename... T>
concept CanInvoke = requires (Func f, T... args) {
    f(std::forward<T>(args)...);
};

// Deprecated alias
template <typename Func, typename... T>
concept CanApply = CanInvoke<Func, T...>;

template <typename Func, typename Return, typename... T>
concept InvokeReturns = requires (Func f, T... args) {
    { f(std::forward<T>(args)...) } -> std::same_as<Return>;
};

// Deprecated alias
template <typename Func, typename Return, typename... T>
concept ApplyReturns = InvokeReturns<Func, Return, T...>;

template <typename Func, typename... T>
concept InvokeReturnsAnyFuture = requires (Func f, T... args) {
    requires is_future<decltype(f(std::forward<T>(args)...))>::value;
};

// Deprecated alias
template <typename Func, typename... T>
concept ApplyReturnsAnyFuture = InvokeReturnsAnyFuture<Func, T...>;

)

template <typename T>
struct futurize {
    /// If \c T is a future, \c T; otherwise \c future<T>
    using type = future<T>;
    /// The promise type associated with \c type.
    using promise_type = promise<T>;
    /// The value tuple type associated with \c type
    using value_type = std::tuple<T>;

    /// Apply a function to an argument list (expressed as a tuple)
    /// and return the result, as a future (if it wasn't already).
    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept;

    /// Invoke a function to an argument list
    /// and return the result, as a future (if it wasn't already).
    template<typename Func, typename... FuncArgs>
    static inline type invoke(Func&& func, FuncArgs&&... args) noexcept;

    /// Deprecated alias of invoke
    template<typename Func, typename... FuncArgs>
    [[deprecated("Use invoke for varargs")]]
    static inline type apply(Func&& func, FuncArgs&&... args) noexcept;

    /// Convert a value or a future to a future
    static inline type convert(T&& value) { return make_ready_future<T>(std::move(value)); }
    static inline type convert(type&& value) { return std::move(value); }

    /// Convert the tuple representation into a future
    static type from_tuple(value_type&& value);
    /// Convert the tuple representation into a future
    static type from_tuple(const value_type& value);

    /// Makes an exceptional future of type \ref type.
    template <typename Arg>
    static type make_exception_future(Arg&& arg) noexcept;

private:
    /// Forwards the result of, or exception thrown by, func() to the
    /// promise. This avoids creating a future if func() doesn't
    /// return one.
    template<typename Func>
    SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func> )
    static void satisfy_with_result_of(internal::promise_base_with_type<T>&&, Func&& func);

    template <typename... U>
    friend class future;
};

/// \cond internal
template <>
struct futurize<void> {
    using type = future<>;
    using promise_type = promise<>;
    using value_type = std::tuple<>;

    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept;

    template<typename Func, typename... FuncArgs>
    static inline type invoke(Func&& func, FuncArgs&&... args) noexcept;

    template<typename Func, typename... FuncArgs>
    [[deprecated("Use invoke for varargs")]]
    static inline type apply(Func&& func, FuncArgs&&... args) noexcept;

    static inline type from_tuple(value_type&& value);
    static inline type from_tuple(const value_type& value);

    template <typename Arg>
    static type make_exception_future(Arg&& arg) noexcept;

private:
    template<typename Func>
    SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func> )
    static void satisfy_with_result_of(internal::promise_base_with_type<>&&, Func&& func);

    template <typename... U>
    friend class future;
};

template <typename... Args>
struct futurize<future<Args...>> {
    using type = future<Args...>;
    using promise_type = promise<Args...>;
    using value_type = std::tuple<Args...>;

    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept;

    template<typename Func, typename... FuncArgs>
    static inline type invoke(Func&& func, FuncArgs&&... args) noexcept;

    template<typename Func, typename... FuncArgs>
    [[deprecated("Use invoke for varargs")]]
    static inline type apply(Func&& func, FuncArgs&&... args) noexcept;

    static inline type from_tuple(value_type&& value);
    static inline type from_tuple(const value_type& value);

    static inline type convert(Args&&... values) { return make_ready_future<Args...>(std::move(values)...); }
    static inline type convert(type&& value) { return std::move(value); }

    template <typename Arg>
    static type make_exception_future(Arg&& arg) noexcept;

private:
    template<typename Func>
    SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func> )
    static void satisfy_with_result_of(internal::promise_base_with_type<Args...>&&, Func&& func);

    template <typename... U>
    friend class future;
};
/// \endcond

// Converts a type to a future type, if it isn't already.
template <typename T>
using futurize_t = typename futurize<T>::type;

/// @}

template<typename Func, typename... Args>
auto futurize_invoke(Func&& func, Args&&... args) noexcept;

template<typename Func, typename... Args>
auto futurize_apply(Func&& func, std::tuple<Args...>&& args) noexcept;

/// \addtogroup future-module
/// @{
namespace internal {
class future_base
{
protected:
    promise_base* _promise;     // 创建 future 对象的 promise

    future_base() noexcept
        : _promise(nullptr)
    {}

    future_base(promise_base* promise, future_state_base* state) noexcept
        : _promise(promise)
    {
        _promise->_future = this;
        _promise->_state = state;
    }

    void move_it(future_base&& x, future_state_base* state) noexcept
    {
        _promise = x._promise;
        if (auto* p = _promise)
        {
            x.detach_promise();
            p->_future = this;
            p->_state = state;
        }
    }

    future_base(future_base&& x, future_state_base* state) noexcept
    {
        move_it(std::move(x), state);
    }

    void clear() noexcept
    {
        if (_promise)
        {
            detach_promise();
        }
    }

    ~future_base() noexcept
    {
        clear();
    }

    promise_base* detach_promise() noexcept
    {
        _promise->_state = nullptr;
        _promise->_future = nullptr;
        return std::exchange(_promise, nullptr);        // 使用 nullptr 替换 _promise , 并返回 _promise 的旧值
    }

    friend class promise_base;
};

template <bool IsVariadic>
struct warn_variadic_future
{
    // Non-varidic case, do nothing
    void check_deprecation() {}
};


// Note: placing the deprecated attribute on the class specialization has no effect.
template <>
struct warn_variadic_future<true> {
    // Variadic case, has deprecation attribute
    [[deprecated("Variadic future<> with more than one template parmeter is deprecated, replace with future<std::tuple<...>>")]]
    void check_deprecation() {}
};

}

/// \brief A representation of a possibly not-yet-computed value.
///
/// A \c future represents a value that has not yet been computed
/// (an asynchronous computation).  It can be in one of several
/// states:
///    - unavailable: the computation has not been completed yet
///    - value: the computation has been completed successfully and a
///      value is available.
///    - failed: the computation completed with an exception.
///
/// methods in \c future allow querying the state and, most importantly,
/// scheduling a \c continuation to be executed when the future becomes
/// available.  Only one such continuation may be scheduled.
///
/// A \ref future should not be discarded before it is waited upon and
/// its result is extracted. Discarding a \ref future means that the
/// computed value becomes inaccessible, but more importantly, any
/// exceptions raised from the computation will disappear unchecked as
/// well. Another very important consequence is potentially unbounded
/// resource consumption due to the launcher of the deserted
/// continuation not being able track the amount of in-progress
/// continuations, nor their individual resource consumption.
/// To prevent accidental discarding of futures, \ref future is
/// declared `[[nodiscard]]` if the compiler supports it. Also, when a
/// discarded \ref future resolves with an error a warning is logged
/// (at runtime).
/// That said there can be legitimate cases where a \ref future is
/// discarded. The most prominent example is launching a new
/// [fiber](\ref fiber-module), or in other words, moving a continuation
/// chain to the background (off the current [fiber](\ref fiber-module)).
/// Even if a \ref future is discarded purposefully, it is still strongly
/// advisable to wait on it indirectly (via a \ref gate or
/// \ref semaphore), control their concurrency, their resource consumption
/// and handle any errors raised from them.
///
/// \tparam T A list of types to be carried as the result of the future,
///           similar to \c std::tuple<T...>. An empty list (\c future<>)
///           means that there is no result, and an available future only
///           contains a success/failure indication (and in the case of a
///           failure, an exception).
///           A list with two or more types is deprecated; use
///           \c future<std::tuple<T...>> instead.
template <typename... T>
class SEASTAR_NODISCARD future : private internal::future_base, internal::warn_variadic_future<(sizeof...(T) > 1)>
{
    future_state<T...> _state;      // _state 的初始值为 state::future
    static constexpr bool copy_noexcept = future_state<T...>::copy_noexcept;

private:
    // This constructor creates a future that is not ready but has no
    // associated promise yet. The use case is to have a less flexible
    // but more efficient future/promise pair where we know that
    // promise::set_value cannot possibly be called without a matching
    // future and so that promise doesn't need to store a
    // future_state.
    // 如果调用的是该构造函数, _state 的初始值为 state::future, 并且因为调用的是基类 future_base 的无参构造函数，所以 future_base::_promise 会被初始化为 nullptr
    future(future_for_get_promise_marker m) noexcept {}

    // 如果调用的是该构造函数，则 future 中的 _state, future_base->_promise->_state 都会被设置为 promise 的 _local_state
    // 这种方式下，如果 promise 执行 set_value, 就可以通过判断 future 中的 _state 是否就绪，然后决定是否执行任务
    future(promise<T...>* pr) noexcept
        : future_base(pr, &_state)      // 这里传入的是 _state 的指针
        , _state(std::move(pr->_local_state))       // 将 _state 设置为 promise 的 _local_state
    {}

    template <typename... A>
    future(ready_future_marker m, A&&... a) noexcept
        : _state(m, std::forward<A>(a)...)
    {}

    future(exception_future_marker m, std::exception_ptr&& ex) noexcept
        : _state(m, std::move(ex))
    {}

    future(exception_future_marker m, future_state_base&& state) noexcept
        : _state(m, std::move(state))
    {}

    [[gnu::always_inline]]
    explicit future(future_state<T...>&& state) noexcept
            : _state(std::move(state))
    {}

    internal::promise_base_with_type<T...> get_promise() noexcept
    {
        assert(!_promise);      // 注意: 调用该方法时, 基类 future_base 中的 _promise 成员变量在构造函数中被初始化成了 nullptr
        return internal::promise_base_with_type<T...>(this);
    }

    // 返回的是旧的 promise, 该 promise 的 _state 和 _future 变量都被置为空了
    internal::promise_base_with_type<T...>* detach_promise()
    {
        return static_cast<internal::promise_base_with_type<T...>*>(future_base::detach_promise());
    }

    template <typename Pr, typename Func>
    void schedule(Pr&& pr, Func&& func) noexcept
    {
        // 如果 future 是否已经就绪或者 future 没有设置 promise(说明不需要等待 promise 执行 set_value, 比如_network_stack_ready), 表明该任务可以立即执行,将该任务封装到 task 中，然后递交给 reactor 去执行
        if (_state.available() || !_promise)
        {
            if (__builtin_expect(!_state.available() && !_promise, false))
            {
                _state.set_to_broken_promise();
            }
            // 将任务封装成 continuation, 加入到任务队列中
            // 调用的 schedule 方法的定义在 reactor.cc:schedule(task* t) 中
            ::seastar::schedule(new continuation<Pr, Func, T...>(std::move(pr), std::move(func), std::move(_state)));
        }
        else
        {
            // 在此之前, future 和 promise 是相互绑定的, future 中的 _state 和 future_base::_promise->_state 是绑定的

            // 需要等待 promise 完成(即需要等待 promise 执行 set_value 之后才可以将 task 递交给 reactor)
            // 将任务封装成 continuation 挂到 promise 下的 _task, 等到 promise 的 set_value 方法被调用后，再将任务加入到任务队列中
            assert(_promise);
            // 调用 schedule 方法的是非空的 future->_promise, 但是 future->_promise->future 和 future->_promise->_state 为 nullptr
            detach_promise()->schedule(std::move(pr), std::move(func));
            _state._u.st = future_state_base::state::invalid;       // 将旧的 future 中的 state 设置为 invalid

            /**
             * 至此，旧的 future 和旧的 promise 就已经解除绑定了. future->_promise 和 future->_promise->_future 都被置为 nullptr, future->_promise->_state 也被置为 nullptr.
             * 旧的 promise 的 _state 指针会指向为 continuation 的 _state, 在旧的 promise 执行 set_value 时，会将 _state 设置为 state::result, 然后将这里封装的 promise->_task 递交给 reactor 去执行.
             * 旧的 future 的 _state 不再使用.
             */
        }
    }

    [[gnu::always_inline]]
    future_state<T...>&& get_available_state_ref() noexcept
    {
        if (_promise)
        {
            detach_promise();
        }
        return std::move(_state);
    }

    [[gnu::noinline]]
    future<T...> rethrow_with_nested() noexcept
    {
        if (!failed())
        {
            return internal::current_exception_as_future<T...>();
        }
        else
        {
            //
            // Encapsulate the current exception into the
            // std::nested_exception because the current libstdc++
            // implementation has a bug requiring the value of a
            // std::throw_with_nested() parameter to be of a polymorphic
            // type.
            //
            std::nested_exception f_ex;
            try
            {
                get();
            }
            catch (...)
            {
                try
                {
                    std::throw_with_nested(f_ex);
                }
                catch (...)
                {
                    return internal::current_exception_as_future<T...>();
                }
            }
            __builtin_unreachable();
        }
    }

    template<typename... U>
    friend class shared_future;

public:
    /// \brief The data type carried by the future.
    using value_type = std::tuple<T...>;
    /// \brief The data type carried by the future.
    using promise_type = promise<T...>;

    /// \brief Moves the future into a new object.
    [[gnu::always_inline]]
    future(future&& x) noexcept
        : future_base(std::move(x), &_state)
        , _state(std::move(x._state))
    {}

    future(const future&) = delete;

    ~future()
    {
        this->check_deprecation();
    }

    future& operator=(future&& x) noexcept
    {
        clear();
        move_it(std::move(x), &_state);
        _state = std::move(x._state);
        return *this;
    }

    void operator=(const future&) = delete;
    /// \brief gets the value returned by the computation
    ///
    /// Requires that the future be available.  If the value
    /// was computed successfully, it is returned (as an
    /// \c std::tuple).  Otherwise, an exception is thrown.
    ///
    /// If get() is called in a \ref seastar::thread context,
    /// then it need not be available; instead, the thread will
    /// be paused until the future becomes available.
    [[gnu::always_inline]]
    std::tuple<T...>&& get()
    {
        if (!_state.available())
        {
            do_wait();
        }
        return get_available_state_ref().take();
    }

    [[gnu::always_inline]]
    std::exception_ptr get_exception() noexcept
    {
        return get_available_state_ref().get_exception();
    }

    /// Gets the value returned by the computation.
    ///
    /// Similar to \ref get(), but instead of returning a
    /// tuple, returns the first value of the tuple.  This is
    /// useful for the common case of a \c future<T> with exactly
    /// one type parameter.
    ///
    /// Equivalent to: \c std::get<0>(f.get()).
    typename future_state<T...>::get0_return_type get0()
    {
        return future_state<T...>::get0(get());
    }

    /// Wait for the future to be available (in a seastar::thread)
    ///
    /// When called from a seastar::thread, this function blocks the
    /// thread until the future is availble. Other threads and
    /// continuations continue to execute; only the thread is blocked.
    void wait() noexcept
    {
        if (!_state.available()) {
            do_wait();
        }
    }

private:
    class thread_wake_task final : public continuation_base<T...>
    {
        thread_context* _thread;
        future* _waiting_for;

    public:
        thread_wake_task(thread_context* thread, future* waiting_for)
            : _thread(thread)
            , _waiting_for(waiting_for)
        {}

        virtual void run_and_dispose() noexcept override
        {
            _waiting_for->_state = std::move(this->_state);
            thread_impl::switch_in(_thread);
            // no need to delete, since this is always allocated on
            // _thread's stack.
        }
    };

    void do_wait() noexcept
    {
        if (__builtin_expect(!_promise, false))
        {
            _state.set_to_broken_promise();
            return;
        }

        auto thread = thread_impl::get();
        assert(thread);
        thread_wake_task wake_task{thread, this};
        detach_promise()->schedule(static_cast<continuation_base<T...>*>(&wake_task));
        thread_impl::switch_out(thread);
    }

public:
    /// \brief Checks whether the future is available.
    ///
    /// \return \c true if the future has a value, or has failed.
    [[gnu::always_inline]]
    bool available() const noexcept
    {
        return _state.available();
    }

    /// \brief Checks whether the future has failed.
    ///
    /// \return \c true if the future is availble and has failed.
    [[gnu::always_inline]]
    bool failed() const noexcept
    {
        return _state.failed();
    }

    /// \brief Schedule a block of code to run when the future is ready.
    ///
    /// Schedules a function (often a lambda) to run when the future becomes
    /// available.  The function is called with the result of this future's
    /// computation as parameters.  The return value of the function becomes
    /// the return value of then(), itself as a future; this allows then()
    /// calls to be chained.
    ///
    /// If the future failed, the function is not called, and the exception
    /// is propagated into the return value of then().
    ///
    /// \param func - function to be called when the future becomes available,
    ///               unless it has failed.
    /// \return a \c future representing the return value of \c func, applied
    ///         to the eventual value of this future.
    template <typename Func, typename Result = futurize_t<std::result_of_t<Func(T&&...)>>>
    SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func, T...> )
    Result
    then(Func&& func) noexcept
    {
#ifndef SEASTAR_TYPE_ERASE_MORE
        return then_impl(std::move(func));
#else
        return then_impl(noncopyable_function<Result (T&&...)>([func = std::forward<Func>(func)] (T&&... args) mutable {
            return futurize_apply(func, std::forward_as_tuple(std::move(args)...));     // 注意，在这个调用里面，会单独执行 func, 然后创建一个已经就绪的 future 并返回
        }));
#endif
    }

private:
    // Keep this simple so that Named Return Value Optimization is used.
    template <typename Func, typename Result>
    Result then_impl_nrvo(Func&& func) noexcept
    {
        using futurator = futurize<std::result_of_t<Func(T&&...)>>;
        typename futurator::type fut(future_for_get_promise_marker{});      // 创建新的 future 对象

        // If there is a std::bad_alloc in schedule() there is nothing that can be done about it, we cannot break future
        // chain by returning ready future while 'this' future is not ready. The noexcept will call std::terminate if
        // that happens.
        [&] () noexcept {
            using pr_type = decltype(fut.get_promise());
            memory::disable_failure_guard dfg;

            /**
             * fut.get_promise() 会使用新创建的 future 然后构造新的 promise 对象，并且将新的 promise 对象中的 _state 指向 future 的 _state
             *
             * 这个 lambda 表达式会在调用该 then_impl_nrvo 接口的 future 对应的 promise 执行 set_value 的时候才会被调用，具体是在 continuation 的 run_and_dispose() 方法中. 其中,
             * 1. 入参 pr 是新创建的 promise,
             * 2. 入参 state 是 continuation_base 中的 state. schedule 中会将 lambda 封装成 task, 并且会将该 state 被初始化成 state::future. (在旧的 promise 执行 set_value 时, 会将 state::_u 设置为 result, 并且会设置 state::_v 的值)
             */
            schedule(fut.get_promise(), [func = std::forward<Func>(func)] (pr_type& pr, future_state<T...>&& state) mutable {
                if (state.failed())
                {
                    pr.set_exception(static_cast<future_state_base&&>(std::move(state)));
                }
                else
                {
                    try
                    {
                        /**
                         * NOTE: 在 then_impl_nrvo 接口中创建了新的 future 和 promise, 所以为了区分，把调用该接口的 future 和 promise 统一称为旧的 future 和 promise.
                         *
                         * 当旧的 future 调用 then_impl_nrvo 的时候，在 schedule 中会将新创建的 promise 连同 func 封装到 task 中, 然后将 task 绑定到旧的 promise 的 _task 上,
                         * 当旧的 promise 执行 set_value() 时, promise 上的 _task 会被添加到 reactor 的任务队列中并执行,
                         * 当执行到 satisfy_with_result_of 的时候，传入的参数是新的 promise, 会执行新创建的 promise 的 set_value(),
                         * 然后等待该新创建的 promise 的值的也就是新创建的 future, 也就是这里 then 调用返回的 future, 也就是说, then().then() 可以继续执行了.
                         */
                        // satisfy_with_result_of 调用的是下面这个接口:
                        // void futurize<future<Args...>>::satisfy_with_result_of(internal::promise_base_with_type<Args...>&& pr, Func&& func)
                        // 这里的 state 已经被设置成了 state::result
                        futurator::satisfy_with_result_of(std::move(pr), [&func, &state] {
                            // 注意，这里调用 func 时传入的参数是 state::_v, 该变量是在旧的 promise 执行 set_value 的时候被创建的
                            // 注意: state::_v 的值主要是用来在多个异步任务之间进行参数传递的. 比如 then(func_1).then(func_2) 中, func_1 的返回值作为 func_2 参数
                            return ::seastar::apply(std::forward<Func>(func), std::move(state).get_value());
                        });
                    }
                    catch (...)
                    {
                        pr.set_to_current_exception();
                    }
                }
            });
        } ();

        return fut;     // 返回新的 future 对象
    }

    template <typename Func, typename Result = futurize_t<std::result_of_t<Func(T&&...)>>>
    Result then_impl(Func&& func) noexcept
    {
        using futurator = futurize<std::result_of_t<Func(T&&...)>>;
        /**
         * avaiable(): future 的值已经获取到了
         * need_preempt(): 当前是否有任务需要立即执行. 如果有其他任务需要立即执行，将 func 放入到任务队列, 否则直接执行 func. DEBUG 模式下一直返回 true.
         *
         * 只有在 promise 执行了 set_value 之后, future 才可以获取到值
         * 如果进程刚启动，执行了 engine().run() 进入 while 循环的时候才会执行 _start_promise.set_value, 之后 future 才可以获取到值
         */
        if (available() && !need_preempt())
        {
            if (failed())
            {
                return futurator::make_exception_future(static_cast<future_state_base&&>(get_available_state_ref()));
            }
            else
            {
                return futurator::apply(std::forward<Func>(func), get_available_state_ref().take_value());      // 立即执行 func
            }
        }

        /**
         * 如果上面 future 的值没有获取到，说明当前无法立即执行该任务，需要将该任务作为一个异步任务交给 reactor 去执行，所以执行以下步骤:
         *
         * 1. 创建一个新的 future 对象和新的 promise 对象
         * 2. 将任务封装成 continuation 然后绑定到旧的 promise 下的 _task
         * 3. 当旧的 promise 执行 set_value 时, _task 被加入到 reactor 的工作队列中
         * 4. 返回新的 future 对象，后续的 then 方法将在新返回的 future 对象上被调用
         */
        return then_impl_nrvo<Func, Result>(std::forward<Func>(func));
    }

public:
    /// \brief Schedule a block of code to run when the future is ready, allowing
    ///        for exception handling.
    ///
    /// Schedules a function (often a lambda) to run when the future becomes
    /// available.  The function is called with the this future as a parameter;
    /// it will be in an available state.  The return value of the function becomes
    /// the return value of then_wrapped(), itself as a future; this allows
    /// then_wrapped() calls to be chained.
    ///
    /// Unlike then(), the function will be called for both value and exceptional
    /// futures.
    ///
    /// \param func - function to be called when the future becomes available,
    /// \return a \c future representing the return value of \c func, applied
    ///         to the eventual value of this future.
    template <typename Func, typename FuncResult = std::result_of_t<Func(future)>>
    SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func, future> )
    futurize_t<FuncResult>
    then_wrapped(Func&& func) & noexcept {
        return then_wrapped_maybe_erase<false, FuncResult>(std::forward<Func>(func));
    }

    template <typename Func, typename FuncResult = std::result_of_t<Func(future&&)>>
    SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func, future&&> )
    futurize_t<FuncResult>
    then_wrapped(Func&& func) && noexcept {
        return then_wrapped_maybe_erase<true, FuncResult>(std::forward<Func>(func));
    }

private:

    template <bool AsSelf, typename FuncResult, typename Func>
    futurize_t<FuncResult>
    then_wrapped_maybe_erase(Func&& func) noexcept
    {
#ifndef SEASTAR_TYPE_ERASE_MORE
        return then_wrapped_common<AsSelf, FuncResult>(std::forward<Func>(func));
#else
        using futurator = futurize<FuncResult>;
        using WrapFuncResult = typename futurator::type;
        return then_wrapped_common<AsSelf, WrapFuncResult>(noncopyable_function<WrapFuncResult (future&&)>([func = std::forward<Func>(func)] (future&& f) mutable {
            return futurator::invoke(std::forward<Func>(func), std::move(f));
        }));
#endif
    }

    // Keep this simple so that Named Return Value Optimization is used.
    template <typename FuncResult, typename Func>
    futurize_t<FuncResult>
    then_wrapped_nrvo(Func&& func) noexcept
    {
        using futurator = futurize<FuncResult>;
        typename futurator::type fut(future_for_get_promise_marker{});
        // If there is a std::bad_alloc in schedule() there is nothing that can be done about it, we cannot break future
        // chain by returning ready future while 'this' future is not ready. The noexcept will call std::terminate if
        // that happens.
        [&] () noexcept {
            using pr_type = decltype(fut.get_promise());
            memory::disable_failure_guard dfg;
            schedule(fut.get_promise(), [func = std::forward<Func>(func)] (pr_type& pr, future_state<T...>&& state) mutable {
                try {
                    futurator::satisfy_with_result_of(std::move(pr), [&func, &state] {
                        return func(future(std::move(state)));
                    });
                } catch (...) {
                    pr.set_to_current_exception();
                }
            });
        } ();
        return fut;
    }


    template <bool AsSelf, typename FuncResult, typename Func>
    futurize_t<FuncResult>
    then_wrapped_common(Func&& func) noexcept
    {
        using futurator = futurize<FuncResult>;
        if (available() && !need_preempt())
        {
            // TODO: after dropping C++14 support use `if constexpr ()` instead.
            if (AsSelf)
            {
                if (_promise)
                {
                    detach_promise();
                }
                return futurator::invoke(std::forward<Func>(func), std::move(*this));
            }
            else
            {
                return futurator::invoke(std::forward<Func>(func), future(get_available_state_ref()));
            }
        }
        return then_wrapped_nrvo<FuncResult, Func>(std::forward<Func>(func));
    }

    void forward_to(internal::promise_base_with_type<T...>&& pr) noexcept
    {
        if (_state.available())
        {
            pr.set_urgent_state(std::move(_state));     // 将当前 future::_state 的值移动给了 promise_base::_state.(包含 _state::_v 和 _state::_u)
        }
        else
        {
            *detach_promise() = std::move(pr);      // 将 promise 绑定到 future 上
        }
    }

public:
    /// \brief Satisfy some \ref promise object with this future as a result.
    ///
    /// Arranges so that when this future is resolve, it will be used to
    /// satisfy an unrelated promise.  This is similar to scheduling a
    /// continuation that moves the result of this future into the promise
    /// (using promise::set_value() or promise::set_exception(), except
    /// that it is more efficient.
    ///
    /// \param pr a promise that will be fulfilled with the results of this
    /// future.
    void forward_to(promise<T...>&& pr) noexcept
    {
        if (_state.available())
        {
            pr.set_urgent_state(std::move(_state));
        }
        else if (&pr._local_state != pr._state)
        {
            // The only case when _state points to _local_state is
            // when get_future was never called. Given that pr will
            // soon be destroyed, we know get_future will never be
            // called and we can just ignore this request.
            *detach_promise() = std::move(pr);
        }
    }



    /**
     * Finally continuation for statements that require waiting for the result.
     * I.e. you need to "finally" call a function that returns a possibly
     * unavailable future. The returned future will be "waited for", any
     * exception generated will be propagated, but the return value is ignored.
     * I.e. the original return value (the future upon which you are making this
     * call) will be preserved.
     *
     * If the original return value or the callback return value is an
     * exceptional future it will be propagated.
     *
     * If both of them are exceptional - the std::nested_exception exception
     * with the callback exception on top and the original future exception
     * nested will be propagated.
     */
    template <typename Func>
    SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func> )
    future<T...> finally(Func&& func) noexcept
    {
        return then_wrapped(finally_body<Func, is_future<std::result_of_t<Func()>>::value>(std::forward<Func>(func)));
    }

    template <typename Func, bool FuncReturnsFuture>
    struct finally_body;

    template <typename Func>
    struct finally_body<Func, true>
    {
        Func _func;

        finally_body(Func&& func) : _func(std::forward<Func>(func))
        { }

        future<T...> operator()(future<T...>&& result) noexcept {
            return futurize_invoke(_func).then_wrapped([result = std::move(result)](auto&& f_res) mutable {
                if (!f_res.failed()) {
                    return std::move(result);
                } else {
                    try {
                        f_res.get();
                    } catch (...) {
                        return result.rethrow_with_nested();
                    }
                    __builtin_unreachable();
                }
            });
        }
    };

    template <typename Func>
    struct finally_body<Func, false>
    {
        Func _func;

        finally_body(Func&& func) : _func(std::forward<Func>(func))
        { }

        future<T...> operator()(future<T...>&& result) noexcept {
            try {
                _func();
                return std::move(result);
            } catch (...) {
                return result.rethrow_with_nested();
            }
        };
    };

    /// \brief Terminate the program if this future fails.
    ///
    /// Terminates the entire program is this future resolves
    /// to an exception.  Use with caution.
    future<> or_terminate() noexcept
    {
        return then_wrapped([] (auto&& f)
        {
            try {
                f.get();
            } catch (...) {
                engine_exit(std::current_exception());
            }
        });
    }

    /// \brief Discards the value carried by this future.
    ///
    /// Converts the future into a no-value \c future<>, by
    /// ignoring any result.  Exceptions are propagated unchanged.
    future<> discard_result() noexcept {
        return then([] (T&&...) {});
    }

    /// \brief Handle the exception carried by this future.
    ///
    /// When the future resolves, if it resolves with an exception,
    /// handle_exception(func) replaces the exception with the value
    /// returned by func. The exception is passed (as a std::exception_ptr)
    /// as a parameter to func; func may return the replacement value
    /// immediately (T or std::tuple<T...>) or in the future (future<T...>)
    /// and is even allowed to return (or throw) its own exception.
    ///
    /// The idiom fut.discard_result().handle_exception(...) can be used
    /// to handle an exception (if there is one) without caring about the
    /// successful value; Because handle_exception() is used here on a
    /// future<>, the handler function does not need to return anything.
    template <typename Func>
    /* Broken?
    SEASTAR_CONCEPT( requires ::seastar::InvokeReturns<Func, future<T...>, std::exception_ptr>
                    || (sizeof...(T) == 0 && ::seastar::InvokeReturns<Func, void, std::exception_ptr>)
                    || (sizeof...(T) == 1 && ::seastar::InvokeReturns<Func, T..., std::exception_ptr>)
    ) */
    future<T...> handle_exception(Func&& func) noexcept {
        return then_wrapped([func = std::forward<Func>(func)]
                             (auto&& fut) mutable -> future<T...> {
            if (!fut.failed()) {
                return make_ready_future<T...>(fut.get());
            } else {
                return futurize_invoke(func, fut.get_exception());
            }
        });
    }

    /// \brief Handle the exception of a certain type carried by this future.
    ///
    /// When the future resolves, if it resolves with an exception of a type that
    /// provided callback receives as a parameter, handle_exception(func) replaces
    /// the exception with the value returned by func. The exception is passed (by
    /// reference) as a parameter to func; func may return the replacement value
    /// immediately (T or std::tuple<T...>) or in the future (future<T...>)
    /// and is even allowed to return (or throw) its own exception.
    /// If exception, that future holds, does not match func parameter type
    /// it is propagated as is.
    template <typename Func>
    future<T...> handle_exception_type(Func&& func) noexcept {
        using trait = function_traits<Func>;
        static_assert(trait::arity == 1, "func can take only one parameter");
        using ex_type = typename trait::template arg<0>::type;
        return then_wrapped([func = std::forward<Func>(func)]
                             (auto&& fut) mutable -> future<T...> {
            try {
                return make_ready_future<T...>(fut.get());
            } catch(ex_type& ex) {
                return futurize_invoke(func, ex);
            }
        });
    }

    /// \brief Ignore any result hold by this future
    ///
    /// Ignore any result (value or exception) hold by this future.
    /// Use with caution since usually ignoring exception is not what
    /// you want
    void ignore_ready_future() noexcept {
        _state.ignore();
    }

#if defined(SEASTAR_COROUTINES_TS) || defined(__cpp_lib_coroutine)
    void set_coroutine(task& coroutine) noexcept {
        assert(!_state.available());
        assert(_promise);
        detach_promise()->set_coroutine(_state, coroutine);
    }
#endif
private:
    void set_callback(continuation_base<T...>* callback) noexcept {
        if (_state.available()) {
            callback->set_state(get_available_state_ref());
            ::seastar::schedule(callback);
        } else {
            assert(_promise);
            detach_promise()->schedule(callback);
        }

    }

    /// \cond internal
    template <typename... U>
    friend class future;
    template <typename... U>
    friend class promise;
    template <typename U>
    friend struct futurize;
    template <typename... U>
    friend class internal::promise_base_with_type;
    template <typename... U, typename... A>
    friend future<U...> make_ready_future(A&&... value) noexcept;
    template <typename... U>
    friend future<U...> make_exception_future(std::exception_ptr&& ex) noexcept;
    template <typename... U, typename Exception>
    friend future<U...> make_exception_future(Exception&& ex) noexcept;
    template <typename... U>
    friend future<U...> internal::make_exception_future(future_state_base&& state) noexcept;
    template <typename... U, typename V>
    friend void internal::set_callback(future<U...>&, V*) noexcept;
    /// \endcond
};

inline internal::promise_base::promise_base(future_base* future, future_state_base* state) noexcept
    : _future(future), _state(state)
{
    _future->_promise = this;
}

template <typename... T>
inline
future<T...>
promise<T...>::get_future() noexcept
{
    assert(!this->_future && this->_state && !this->_task);    // 这里的 _state 是基类 promise_base 的成员变量, _state 在执行 reactor 类中的 _start_promise 的无参构造函数的时候会将基类 promise_base 的指针指向派生类 promise 的 _local_state 变量
    return future<T...>(this);          // 用 promise 构造 future 实例. 通过这种方法，将 promise 和 future 相互绑定
}

template <typename... T>
inline
void promise<T...>::move_it(promise&& x) noexcept
{
    if (this->_state == &x._local_state)
    {
        this->_state = &_local_state;
        new (&_local_state) future_state<T...>(std::move(x._local_state));
    }
}

template <typename... T, typename... A>
inline
future<T...> make_ready_future(A&&... value) noexcept
{
    return future<T...>(ready_future_marker(), std::forward<A>(value)...);
}

template <typename... T>
inline
future<T...> make_exception_future(std::exception_ptr&& ex) noexcept
{
    return future<T...>(exception_future_marker(), std::move(ex));
}

template <typename... T>
inline
future<T...> internal::make_exception_future(future_state_base&& state) noexcept
{
    return future<T...>(exception_future_marker(), std::move(state));
}

template <typename... T>
future<T...> internal::current_exception_as_future() noexcept
{
    return internal::make_exception_future<T...>(future_state_base::current_exception());
}

void log_exception_trace() noexcept;

/// \brief Creates a \ref future in an available, failed state.
///
/// Creates a \ref future object that is already resolved in a failed
/// state.  This no I/O needs to be performed to perform a computation
/// (for example, because the connection is closed and we cannot read
/// from it).
template <typename... T, typename Exception>
inline
future<T...> make_exception_future(Exception&& ex) noexcept {
    log_exception_trace();
    return make_exception_future<T...>(std::make_exception_ptr(std::forward<Exception>(ex)));
}

/// @}

/// \cond internal

template<typename T>
template<typename Func, typename... FuncArgs>
typename futurize<T>::type futurize<T>::apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept
{
    try
    {
        return convert(::seastar::apply(std::forward<Func>(func), std::move(args)));
    }
    catch (...)
    {
        return internal::current_exception_as_future<T>();
    }
}

template<typename T>
template<typename Func>
SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func> )
void futurize<T>::satisfy_with_result_of(internal::promise_base_with_type<T>&& pr, Func&& func)
{
    pr.set_value(func());
}

template<typename T>
template<typename Func, typename... FuncArgs>
typename futurize<T>::type futurize<T>::invoke(Func&& func, FuncArgs&&... args) noexcept
{
    try
    {
        return convert(func(std::forward<FuncArgs>(args)...));
    }
    catch (...)
    {
        return internal::current_exception_as_future<T>();
    }
}

template<typename T>
template<typename Func, typename... FuncArgs>
typename futurize<T>::type futurize<T>::apply(Func&& func, FuncArgs&&... args) noexcept
{
    return invoke(std::forward<Func>(func), std::forward<FuncArgs>(args)...);
}

template<typename Func, typename... FuncArgs>
typename futurize<void>::type futurize<void>::apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept
{
    try
    {
        ::seastar::apply(std::forward<Func>(func), std::move(args));
        return make_ready_future<>();       // 返回就绪的 future
    }
    catch (...)
    {
        return internal::current_exception_as_future<>();
    }
}

template<typename Func>
SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func> )
    void futurize<void>::satisfy_with_result_of(internal::promise_base_with_type<>&& pr, Func&& func)
{
    func();
    pr.set_value();
}

template<typename Func, typename... FuncArgs>
typename futurize<void>::type futurize<void>::invoke(Func&& func, FuncArgs&&... args) noexcept
{
    try {
        func(std::forward<FuncArgs>(args)...);
        return make_ready_future<>();
    } catch (...) {
        return internal::current_exception_as_future<>();
    }
}

template<typename Func, typename... FuncArgs>
typename futurize<void>::type futurize<void>::apply(Func&& func, FuncArgs&&... args) noexcept
{
    return invoke(std::forward<Func>(func), std::forward<FuncArgs>(args)...);
}

template<typename... Args>
template<typename Func, typename... FuncArgs>
typename futurize<future<Args...>>::type futurize<future<Args...>>::apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept {
    try {
        return ::seastar::apply(std::forward<Func>(func), std::move(args));
    } catch (...) {
        return internal::current_exception_as_future<Args...>();
    }
}

template<typename... Args>
template<typename Func>
SEASTAR_CONCEPT( requires ::seastar::CanInvoke<Func> )
void futurize<future<Args...>>::satisfy_with_result_of(internal::promise_base_with_type<Args...>&& pr, Func&& func)
{
#if 0
    auto func_2 = [this] {
        return seastar::metrics::configure(this->configuration()).then([this] {
            // set scollectd use the metrics configuration, so the later
            // need to be set first
            scollectd::configure( this->configuration());
        });
    };

    auto func_1 = noncopyable_function<Result (T&&...)>([func_2 = std::forward<Func>(func_2)] (T&&... args) mutable
    {
        // 单独执行 func_2(), 然后创建一个就绪的 future 并返回
        // func_2(); return ready_future();
        return futurize_apply(func_2, std::forward_as_tuple(std::move(args)...));
    };

    auto func = [&func, &state] { return ::seastar::apply(std::forward<Func>(func_1), std::move(state).get_value());    // 实际上执行的是 return func_1()

    // func() 返回的 func_1() 返回的新创建的已经就绪的 future, 通过调用 forward_to(), 将 promise_base 的 _state 设置为 state::result
    // func().forward_to(std::move(pr));
#endif

    /**
     * 以下链式调用的执行过程分析:
     * 1. func() 内部是多个 lambda 表达式的递归调用，内部调用结果如上.
     * 2. func() 返回的 future 是在 func_1() 调用中已经就绪的 future. 注意: 这个 future::_state 的 _u 和 _v 参数都已经被赋值了
     * 3. 使用 func() 返回的已经就绪的 future 来调用 forward_to, forward_to 内部会执行两个步骤:
     *   3.1 用 future->_state 给 pr->_state 赋值. 注意: 这时 promsie 的 _state 指向的是 continuation_base::_state. 也就是说, 实际上 continuation_base 的 _state 被设置成了 future::_state(包含 _state::_u 和 _state::_v). 然后这里的 future 就没有其他作用了.
     *   3.2 在 make_ready 接口中将 promise 上绑定的 _task 递交给 reactor 去执行.
     *
     * 这里 promise 中的 _task 是在什么时候被赋值的?
     *   作为 satisfy_with_result_of 参数的 pr 在创建之初就已经和新创建的 future 绑定在一起了，在第一次 then 调用之后会返回新创建的 future, 然后在第二次 then 调用中将对应的任务封装成 task, 然后绑定到 future->_promise->_task 上
     */
    func().forward_to(std::move(pr));
}

template<typename... Args>
template<typename Func, typename... FuncArgs>
typename futurize<future<Args...>>::type futurize<future<Args...>>::invoke(Func&& func, FuncArgs&&... args) noexcept {
    try {
        return func(std::forward<FuncArgs>(args)...);
    } catch (...) {
        return internal::current_exception_as_future<Args...>();
    }
}

template<typename... Args>
template<typename Func, typename... FuncArgs>
typename futurize<future<Args...>>::type futurize<future<Args...>>::apply(Func&& func, FuncArgs&&... args) noexcept {
    return invoke(std::forward<Func>(func), std::forward<FuncArgs>(args)...);
}

template <typename T>
template <typename Arg>
inline
future<T>
futurize<T>::make_exception_future(Arg&& arg) noexcept {
    using ::seastar::make_exception_future;
    using ::seastar::internal::make_exception_future;
    return make_exception_future<T>(std::forward<Arg>(arg));
}

template <typename... T>
template <typename Arg>
inline
future<T...>
futurize<future<T...>>::make_exception_future(Arg&& arg) noexcept {
    using ::seastar::make_exception_future;
    using ::seastar::internal::make_exception_future;
    return make_exception_future<T...>(std::forward<Arg>(arg));
}

template <typename Arg>
inline
future<>
futurize<void>::make_exception_future(Arg&& arg) noexcept {
    using ::seastar::make_exception_future;
    using ::seastar::internal::make_exception_future;
    return make_exception_future<>(std::forward<Arg>(arg));
}

template <typename T>
inline
future<T>
futurize<T>::from_tuple(std::tuple<T>&& value) {
    return make_ready_future<T>(std::move(value));
}

template <typename T>
inline
future<T>
futurize<T>::from_tuple(const std::tuple<T>& value) {
    return make_ready_future<T>(value);
}

inline
future<>
futurize<void>::from_tuple(std::tuple<>&& value) {
    return make_ready_future<>();
}

inline
future<>
futurize<void>::from_tuple(const std::tuple<>& value) {
    return make_ready_future<>();
}

template <typename... Args>
inline
future<Args...>
futurize<future<Args...>>::from_tuple(std::tuple<Args...>&& value) {
    return make_ready_future<Args...>(std::move(value));
}

template <typename... Args>
inline
future<Args...>
futurize<future<Args...>>::from_tuple(const std::tuple<Args...>& value) {
    return make_ready_future<Args...>(value);
}

template<typename Func, typename... Args>
auto futurize_invoke(Func&& func, Args&&... args) noexcept
{
    using futurator = futurize<std::result_of_t<Func(Args&&...)>>;
    return futurator::invoke(std::forward<Func>(func), std::forward<Args>(args)...);
}

template<typename Func, typename... Args>
[[deprecated("Use futurize_invoke for varargs")]]
auto futurize_apply(Func&& func, Args&&... args) noexcept {
    return futurize_invoke(std::forward<Func>(func), std::forward<Args>(args)...);
}

template<typename Func, typename... Args>
auto futurize_apply(Func&& func, std::tuple<Args...>&& args) noexcept
{
    using futurator = futurize<std::result_of_t<Func(Args&&...)>>;
    // typename futurize<void>::type futurize<void>::apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept
    return futurator::apply(std::forward<Func>(func), std::move(args));
}

namespace internal {

template <typename... T, typename U>
inline
void set_callback(future<T...>& fut, U* callback) noexcept {
    // It would be better to use continuation_base<T...> for U, but
    // then a derived class of continuation_base<T...> won't be matched
    return fut.set_callback(callback);
}

}


/// \endcond

}
