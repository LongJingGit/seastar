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

