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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/scollectd.hh>
#include <seastar/core/metrics_api.hh>
#include <boost/program_options.hpp>
#include <seastar/core/print.hh>
#include <seastar/util/log.hh>
#include <seastar/util/log-cli.hh>
#include <boost/program_options.hpp>
#include <boost/make_shared.hpp>
#include <fstream>
#include <cstdlib>

namespace seastar {

namespace bpo = boost::program_options;

static
reactor_config
reactor_config_from_app_config(app_template::config cfg) {
    reactor_config ret;
    ret.auto_handle_sigint_sigterm = cfg.auto_handle_sigint_sigterm;
    ret.task_quota = cfg.default_task_quota;
    return ret;
}

app_template::app_template(app_template::config cfg)
    : _cfg(std::move(cfg))
    , _opts(_cfg.name + " options")
    , _conf_reader(get_default_configuration_reader()) {
        _opts.add_options()
                ("help,h", "show help message")
                ;

        smp::register_network_stacks();
        _opts_conf_file.add(reactor::get_options_description(reactor_config_from_app_config(_cfg)));
        _opts_conf_file.add(seastar::metrics::get_options_description());
        _opts_conf_file.add(smp::get_options_description());
        _opts_conf_file.add(scollectd::get_options_description());
        _opts_conf_file.add(log_cli::get_options_description());

        _opts.add(_opts_conf_file);
}

app_template::configuration_reader app_template::get_default_configuration_reader() {
    return [this] (bpo::variables_map& configuration) {
        auto home = std::getenv("HOME");
        if (home) {
            std::ifstream ifs(std::string(home) + "/.config/seastar/seastar.conf");
            if (ifs) {
                bpo::store(bpo::parse_config_file(ifs, _opts_conf_file), configuration);
            }
            std::ifstream ifs_io(std::string(home) + "/.config/seastar/io.conf");
            if (ifs_io) {
                bpo::store(bpo::parse_config_file(ifs_io, _opts_conf_file), configuration);
            }
        }
    };
}

void app_template::set_configuration_reader(configuration_reader conf_reader) {
    _conf_reader = conf_reader;
}

boost::program_options::options_description& app_template::get_options_description() {
    return _opts;
}

boost::program_options::options_description& app_template::get_conf_file_options_description() {
    return _opts_conf_file;
}

boost::program_options::options_description_easy_init
app_template::add_options() {
    return _opts.add_options();
}

void
app_template::add_positional_options(std::initializer_list<positional_option> options) {
    for (auto&& o : options) {
        _opts.add(boost::make_shared<bpo::option_description>(o.name, o.value_semantic, o.help));
        _pos_opts.add(o.name, o.max_count);
    }
}


bpo::variables_map&
app_template::configuration() {
    return *_configuration;
}

int
app_template::run(int ac, char ** av, std::function<future<int> ()>&& func) {
    return run_deprecated(ac, av, [func = std::move(func)] () mutable {
        std::cout << __FILE__ << ":" << __LINE__<< std::endl;
        auto func_done = make_lw_shared<promise<>>();
        engine().at_exit([func_done] { return func_done->get_future(); });
        // No need to wait for this future.
        // func's returned exit_code is communicated via engine().exit()
        (void)futurize_invoke(func).finally([func_done] {
            func_done->set_value();
        }).then([] (int exit_code) {
            return engine().exit(exit_code);
        }).or_terminate();
    });
}

int
app_template::run(int ac, char ** av, std::function<future<> ()>&& func) {
    return run(ac, av, [func = std::move(func)] {
        std::cout << __FILE__ << ":" << __LINE__<< std::endl;
        return func().then([] () {
            return 0;
        });
    });
}

int
app_template::run_deprecated(int ac, char ** av, std::function<void ()>&& func)
{
#ifdef SEASTAR_DEBUG
    fmt::print("WARNING: debug mode. Not for benchmarking or production\n");
#endif
    bpo::variables_map configuration;
    try
    {
        bpo::store(bpo::command_line_parser(ac, av)
                    .options(_opts)
                    .positional(_pos_opts)
                    .run()
            , configuration);
        _conf_reader(configuration);        // 解析配置
    }
    catch (bpo::error& e)
    {
        fmt::print("error: {}\n\nTry --help.\n", e.what());
        return 2;
    }

    if (configuration.count("help"))
    {
        if (!_cfg.description.empty())
        {
            std::cout << _cfg.description << "\n";
        }

        std::cout << _opts << "\n";
        return 1;
    }

    if (configuration["help-loggers"].as<bool>())
    {
        log_cli::print_available_loggers(std::cout);
        return 1;
    }

    bpo::notify(configuration);

    // Needs to be before `smp::configure()`.
    try
    {
        apply_logging_settings(log_cli::extract_settings(configuration));
    }
    catch (const std::runtime_error& exn)
    {
        std::cout << "logging configuration error: " << exn.what() << '\n';
        return 1;
    }

    configuration.emplace("argv0", boost::program_options::variable_value(std::string(av[0]), false));

    try
    {
        smp::configure(configuration, reactor_config_from_app_config(_cfg));        // 创建并运行线程
    }
    catch (...)
    {
        std::cerr << "Could not initialize seastar: " << std::current_exception() << std::endl;
        return 1;
    }

    _configuration = {std::move(configuration)};

    /**
     * 关于以下 engine().when_started().then(func1).then(func2).then(func3).then_wrapeed() 链式调用的解释:
     * 1. engine(): 返回的是 reactor 实例.
     *   1.1 每个线程都有一个 reactor 实例
     *   1.2 每个 reactor 实例对应着一个 promise => _start_promise
     *   1.3 每个 promise 对应着一个 future, 通过 get_future() 接口获取
     *
     * 2. when_started(): 返回的是 reactor 实例下 _start_promise 的 future
     *
     * 3. then(func1):
     *   3.1 将 func1 封装到 continuation 中，然后将 continuation 绑定到 _start_promise 的 task 上
     *   3.2 创建新的 promise 和新的 future, then(func1) 接口返回后，会用新创建的 future 调用下一个 then(func2)
     *   3.3 当在 engine().run() 中执行 _start_promise 的 set_value 时, task 会被添加到 reactor 的任务队列并执行；在执行具体的 task 时，
     * 会在 satisfy_with_result_of() 中执行 (3.2) 新创建的 promise 的 set_value. (然后会将新创建的 promise 上绑定的 task 添加到 reactor 的任务队列中)
     *
     * 4. then(func2)
     *   4.1 调用该 then 接口的是 (3.2) 创建的新的 future, 该 then 调用仍然是先将 func2 封装到 continuation 中，然后将其绑定到新的 promise 的 task 上
     *   4.2 创建新的 promise 和新的 future, 然后进行下一次的 then 调用
     *   4.3 当 3.3 中执行新的 promise.set_value() 之后, 本次调用的 task 又会被添加到 reactor 的任务队列, 当在 reactor 中执行 task 时，
     * 会调用本次新创建的 promise 的 set_value (下一次 then 调用中的任务又会被添加到任务队列然后被执行)
     *
     * 5. then(func3) 执行过程和上面相同
     */

    // No need to wait for this future.
    // func is waited on via engine().run()
    (void)engine()
    .when_started()
    .then([this] {
        return seastar::metrics::configure(this->configuration()).then([this] {
            // set scollectd use the metrics configuration, so the later
            // need to be set first
            scollectd::configure( this->configuration());
        });
    })
    .then(std::move(func))
    .then_wrapped([] (auto&& f) {       // .then_wrapped: 检查输入的 future 是否包含异常或者值
        try
        {
            f.get();
        }
        catch (std::exception& ex)
        {
            std::cout << "program failed with uncaught exception: " << ex.what() << "\n";
            engine().exit(1);
        }
    });

    auto exit_code = engine().run();   // 主线程进入 while 循环, 会触发 _start_promise 的 set_value 调用
    smp::cleanup();
    return exit_code;
}

}
