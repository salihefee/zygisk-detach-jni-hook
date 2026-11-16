# zygisk-detach

Detaches installed apps from Play Store against its aggressive updating policy which ignores the auto-update option user sets.

### How
Hooks PackageManager Binder requests with Zygisk.

This branch uses Zygisk's `BinderProxy.transactNative` JNI hook instead of a
libbinder PLT hook. On the tested Android 16 / ReZygisk setup, the PLT API reported
success while leaving the transaction relocation unchanged. The JNI hook keeps
the existing selected-package enabled-state behavior; unselected apps and other
PackageManager methods are left alone. Parcel data is accessed through its Java
API and parsed with bounds checks rather than through a private C++ layout.

### Building this branch

Set `ANDROID_NDK_HOME` to an installed Linux Android NDK and run
`bash zygisk/build.sh`. Native libraries for all four ABIs are written to `out/`.
The C++ runtime is linked statically and unresolved symbols fail the build.

The parser checks can be run without Android:

```sh
mkdir -p out
c++ -std=c++17 -fsanitize=address,undefined checks/detach_request.cpp -o out/parcel-check
out/parcel-check
```

### Usage
* Enable zygisk in Magisk **or flash a Zygisk module if you are on KernelSU**
* Flash zygisk-detach module
* Reboot
* Run the cli: `detach` (or `su -c /data/adb/modules/zygisk-detach/detach` if you are not using termux)  
	or use [zygisk-detach-app](https://github.com/j-hc/zygisk-detach-app) if you dont wanna use the terminal  
	or use the WebUI if you are on KernelSU
* Select apps you wish to detach. Changes are applied immediately, no need to reboot.

### Note
Another way to automatically detach any apps upon flashing the module is to put a `detach.txt` with the package names inside the module zip and the apps will be detached without using the cli once module is flashed.  
An example of the detach.txt that should go in the zygisk-detach zip:
```
com.app1
org.app2
```
