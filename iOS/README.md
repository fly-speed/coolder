# iOS support

使用 Foundation Objective-C binding 生成静态 XCFramework，最低 iOS 13.0。
需要 macOS、Xcode/iOS SDK、CMake 和 Python 3。

```sh
python3 iOS/build_xcframework.py --output build/ios
```

产物 `build/ios/LibAI.xcframework` 包含真机 arm64 和模拟器 arm64/x86_64，
支持 `--deployment-target 13.0` 设置最低系统版本。已存在产物不会被脚本自动删除。
将 XCFramework 添加至 Xcode target，静态库选择 Do Not Embed，链接 Foundation/libc++，
启用 ARC；Swift target 使用 `import LibAI`，Objective-C 使用 `@import LibAI`。
模块头为 LAIAgent.h。纯 Objective-C target 的 Other Linker Flags 添加 `-lc++`。

接口示例见 `../libai-objc/README.md`。在后台线程调用同步 run，在 UI 线程派发观察者更新；
网络与设备工具由应用提供，应用负责 ATS、隐私权限和网络取消。

已验证设备与双架构模拟器 archive 编译、XCFramework 打包、macOS Foundation 回调测试；
尚未在 iOS 设备或模拟器运行测试。
