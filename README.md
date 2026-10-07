# coolder

浏览器端 AI 编程智能体，应用源码位于 `coolder/`，共享 AI 库位于 `libai/`。

## 获取源码与 ACL

ACL 以 Git 子模块保存在 `third-party/acl`，来源为 https://github.com/acl-dev/acl。主仓库记录确定的 ACL 提交版本。

首次克隆：

```sh
git clone --recurse-submodules https://github.com/fly-speed/coolder.git
cd coolder
```

已克隆的仓库（或切换版本后）：

```sh
git submodule update --init --recursive
```

GitHub 下载的源码 ZIP 不包含子模块内容，请使用上述 Git 克隆方式。

## 构建

安装 CMake、C/C++ 编译工具链和 OpenSSL 开发库后，在仓库根目录执行：

```sh
cmake -S coolder -B coolder/build -DCMAKE_BUILD_TYPE=Release -DBUILD_ACL=ON
cmake --build coolder/build --parallel 4
./coolder/build/coolder
```

`BUILD_ACL=ON` 会从子模块源码编译 ACL，无需事先生成 ACL 库。非标准位置的 OpenSSL 可通过 `-DOPENSSL_ROOT_DIR=/path/to/openssl` 指定。浏览器访问 http://127.0.0.1:18095。

也可以使用仓库自带的 OpenSSL 和 zlib，统一编译依赖：

```sh
make -C third-party
cmake -S coolder -B coolder/build -DCMAKE_BUILD_TYPE=Release -DBUILD_ACL=OFF
cmake --build coolder/build --parallel 4
```

Windows 的原生 VS 工程位于 `coolder/coolder.sln`，引用 `third-party/acl` 和 `third-party/zlib-1.2.11` 下的工程。OpenSSL 可使用 `third-party/build-openssl.bat` 构建（x64 Release）。详细步骤见 [第三方依赖说明](third-party/README.md) 和 [应用说明](coolder/README.md)。
