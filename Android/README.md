# Android support

可直接运行 `./build.sh --output build`；支持原 Python 构建器的所有参数，
用 `./build.sh --help` 查看。可从任意工作目录调用脚本，默认产物位于本目录的 `build/` 下。

使用 Java/JNI 绑定打包 AAR，minSdk 21。默认包含 arm64-v8a 和 x86_64；
可通过 `--abis` 增加 armeabi-v7a/x86。构建不依赖 Gradle 下载，只需 Python 3、CMake、NDK 和 javac。

```sh
python3 Android/build_aar.py --ndk "$ANDROID_NDK_HOME" --java-home "$JAVA_HOME" --output build/android
```

产物 `build/android/libai.aar` 包含 classes.jar、Manifest、各 ABI 的 libai_jni.so 和 R8 规则。
C++ 标准库静态链接进 JNI，无需另行打包 libc++_shared.so。
将 AAR 放入应用 `libs` 并在 Gradle 添加：

```kotlin
dependencies { implementation(files("libs/libai.aar")) }
```

使用 `io.coolder.libai.Agent`，完整接口及示例见 `../libai-java/README.md`。
在后台线程运行 agent，用主线程派发 streaming UI 更新。Activity 销毁时先 cancel，等待 run
退出再 close。模型回调可以使用应用自己的网络客户端；网络需要应用声明 INTERNET 权限。
需要相机、文件等能力时由应用申请平台权限并注册工具 handler。

已验证 NDK 21.4 的 arm64-v8a/x86_64 交叉编译和 AAR 内容；设备运行仍需应用侧集成测试。

## 清理构建产物

运行 `./clean.sh` 清理默认构建输出；自定义输出目录使用
`./clean.sh /absolute/output/path` 或 `./clean.sh --output /absolute/output/path`。
只删除本绑定生成的目录和文件，保留输出目录中的其他文件；重复执行也可使用。
默认清理 `build/android/`；相对路径与 `build.sh --output` 一样，相对于本绑定目录。

共享 CodingService API 也随绑定打包，可查询 available。当前移动端包提供 portable Agent；
完整编码 backend 依赖桌面 ACL，移动构建不启用该组件，CodingService 调用会明确报告不可用。
可通过已有 Model/Tool 回调接入平台原生模型和文件能力。
