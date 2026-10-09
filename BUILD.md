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


## C++ 代码结构检查

`libai` 和 `coolder` 使用 Linux 内核风格的 8 列 Tab 缩进；每行最多 4 级结构缩进，换行参数使用空格续行对齐。函数（包括 lambda）最多 200 行，源码和头文件均最多 1000 行，行数包含注释和空行。超限时按职责提取函数或翻译单元。

```sh
python3 -m venv /tmp/coolder-style-venv
/tmp/coolder-style-venv/bin/pip install -r coolder/tools/style-requirements.txt
find libai coolder -type f \( -name '*.cpp' -o -name '*.h' \) -not -path '*/build/*' -print0 | xargs -0 /tmp/coolder-style-venv/bin/clang-format --dry-run --Werror
/tmp/coolder-style-venv/bin/python coolder/tools/check_cpp_structure.py
/tmp/coolder-style-venv/bin/python coolder/tools/check_header_comments.py
```

结构检查使用 C++ 语法树计算函数范围，不代替编译。拆分 `.cpp` 后，CMake 会重新发现模块源码；同时需要维护对应的 `.vcxproj` 和 `.vcxproj.filters`。Windows 源码覆盖检查可用 `python3 coolder/tests/vs_project_test.py`，完整检查要求已初始化 ACL 子模块。

头文件中的类、结构体、成员变量及函数声明需提供英文注释，说明用途以及必要的单位、所有权或调用约束。同一声明包含多个成员时，逐个注明其含义。源码注释重点说明事务边界、并发协调、恢复条件和资源生命周期。注释检查覆盖声明及内联定义；注释准确性仍需结合实现审查。
