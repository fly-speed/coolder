# 第三方依赖

- `acl/`：官方 Git 子模块，保留主仓库锁定的提交。
- `openssl-1.1.1q.tar.gz`：从原 aicool 项目复制的 OpenSSL 源码包。
- `openssl/include`、`openssl/lib`：从原项目复制的本机头文件和静态库；构建时从源码重新生成。这些生成文件不提交 Git。
- `zlib-1.2.11/`：从原项目复制的 zlib 源码，包含 VS 2022 工程和上游版权说明。
- `build-openssl.bat`：沿用原项目的 Windows OpenSSL 构建脚本。

依赖版本沿用原项目。ACL 不用旧目录副本覆盖。coolder 未使用的 SQLite、libarchive 等依赖不复制。

## macOS / Linux

在仓库根目录执行：

```sh
git submodule update --init --recursive
make -C third-party
cmake -S coolder -B coolder/build -DCMAKE_BUILD_TYPE=Release -DBUILD_ACL=OFF
cmake --build coolder/build --parallel 4
```

Makefile 参考原项目构建规则：先构建 OpenSSL，再以 `OPENSSL_STATIC=yes` 编译 ACL；zlib 独立编译。macOS 生成 arm64 + x86_64 通用静态库，最低系统版本默认 13.0；Linux 编译当前架构。需要 C/C++ 编译器、make、tar、完整 Perl 环境；macOS 还需要 Xcode Command Line Tools 和 lipo。

可单独运行 `make -C third-party openssl-lib`、`acl-lib` 或 `zlib-lib`。`OPENSSL_JOBS=4` 控制 OpenSSL/zlib 的并行度，`OPENSSL_PERL=/path/to/perl` 可指定 Perl。zlib 在 `.build/zlib` 编译并安装到 `zlib-1.2.11/install`，不改写源码中的 VS 头文件。libai 的 CMake 优先链接该目录下的静态 zlib，未构建时仍使用系统 zlib。

`make -C third-party clean` 清理本地依赖构建产物，包括复制来的 OpenSSL 头文件和库；保留源码包、zlib 源码和 ACL 子模块。再次运行 make 可重建。更换编译器、平台或部署目标时先 clean。

## Windows / VS 2022

安装 MSVC v143、Windows SDK 和 Windows 原生 Perl，在 x64 Native Tools Command Prompt 中执行：

```bat
cd third-party
build-openssl.bat
cd ..
msbuild coolder\coolder.sln /m /p:Configuration=Release /p:Platform=x64
```

批处理沿用原项目的 x64 Release 配置，同时生成 OpenSSL 静态库和动态库；会重建整个 `openssl/` 目录。VS 解决方案负责构建 ACL 和 zlib。macOS 的 `.a` 文件不能用于 MSVC。其他平台或 Debug 配置需要相应架构和运行库的 OpenSSL，参见 `coolder/README.md` 的本地属性设置。

## 本地兼容修正

zlib 1.2.11 的 `zutil.h` 将新版 macOS SDK 误判为经典 Mac OS，导致 fdopen 宏与系统声明冲突；本地修正仅让现代 Darwin 跳过经典 Mac OS 分支。

## 版本控制

提交源码包、zlib 源码、Makefile、脚本和说明；不提交本机编译产物。OpenSSL 的许可证在源码包内，解压后位于 `openssl/src/LICENSE`；zlib 版权与许可见 `zlib-1.2.11/README`，ACL 许可保留在子模块中。
