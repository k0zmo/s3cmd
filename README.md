# s3cmd for Total Commander

`s3cmd` is a WFX file-system plugin that puts Amazon S3 in Total Commander.
It uses standard AWS profiles, so it does not keep a separate copy of your credentials.

The plugin has a simple path: AWS profile, S3 bucket, then object(s).

## Install

The release package contains `s3cmd.wfx64`, `pluginst.inf`, and this README.

1. Open the release ZIP in Total Commander.
2. Accept the plugin installation prompt from `pluginst.inf`.

You can also install the plugin manually:

1. Extract the release ZIP.
2. Open Configuration > Options > Plugins in Total Commander.
3. Select File system plugins (.WFX).
4. Select Configure.
5. Add `s3cmd.wfx64`.

## Configure AWS access

The plugin uses the standard AWS SDK credential chain and AWS shared profile files.
Create profiles before you open the plugin. You can use the AWS CLI or edit the standard
AWS profile files manually.

For access keys, configure a named profile:

```powershell
aws configure --profile work
```

For AWS IAM Identity Center (SSO), run this command:

```powershell
aws configure sso --profile work
```

The profile must reference an `[sso-session]` section.

When an SSO session is missing or expired, the plugin opens the AWS PKCE browser login flow,
receives the callback on `127.0.0.1`, and stores the token in the standard AWS SSO cache.
If PKCE fails, the plugin offers device-code login as a fallback. The AWS CLI is not required.

## Use

1. Open Network Neighborhood (`\\`) in Total Commander.
2. Open Amazon S3.
3. Open an AWS profile.
4. Open a bucket.
5. Use the usual Total Commander copy, move, rename, create-directory, and delete commands.

Plugin paths have this form:

```text
\\profile\\bucket\\object
```

If AWS denies `ListBuckets`, the profile view shows `_F7=register bucket.txt`.
Press F7 and then enter an existing bucket name. The plugin finds its region or asks you to enter it.

Bucket registration is stored locally. F7 does not create an S3 bucket and removing a registered bucket doesn't delete it from AWS.

The plugin exposes a `Region` content field for bucket entries. If you need region data, add this field to a custom-column view.

## Features

- Lists the default profile and named profiles from the AWS configuration and credentials files.
- Uses the standard AWS credential chain.
- Supports AWS CLI profiles and IAM Identity Center sessions.
- Lists all accessible buckets with paginated `ListBuckets` requests.
- Uses registered buckets as a fallback when an account cannot call `ListBuckets`.
- Finds and caches each bucket region for region-correct S3 requests.
- Lists objects and directory prefixes with pagination, file sizes, and modification times.
- Downloads objects through a temporary file, then replaces the destination after a successful transfer.
- Uploads objects with overwrite protection.
- Reports upload and download progress and supports cancellation from Total Commander.
- Advertises background upload and download support to compatible hosts.
- Supports move operations between S3 and the local file system. The source is removed only after a successful transfer.
- Copies, renames, and moves objects inside S3 with server-side `CopyObject` requests.
- Copies objects across buckets when the target profile can read the source bucket.
- Moves objects across profiles when the source profile can also delete the source object.
- Deletes objects and directory markers.
- Creates S3 directory markers for directories created with F7.
- Removes a directory marker only after the directory contents are removed.
- Registers and unregisters existing buckets when automatic bucket discovery is unavailable.
- Exposes the bucket region as a `Region` content field.
- Reuses S3 clients and supports concurrent background transfers during one plugin session.

## Configuration file

The plugin stores its own configuration in `%APPDATA%\s3cmd\s3cmd.toml`
This file is created when the plugin stores a registered bucket. Otherwise, it might not exist.

Set `AwsLogLevel` under `[settings]` to control which AWS SDK messages are sent to the debugger.
The level defaults to `Info`.

```toml
[settings]
# Off, Fatal, Error, Warn, Info, Debug, or Trace
AwsLogLevel = "Debug"
```

## Limitations

- Transfer resume and multipart transfer are not supported.
- A single upload cannot exceed 5 GiB.
- S3 copy, rename, and move operations cannot exceed 5 GiB.
- S3 copy, rename, and move progress changes directly from 0% to 100%.
- Directory rename and directory move are not supported.
- The plugin does not create or delete S3 buckets. It only registers existing buckets when discovery is unavailable.

## Build

The project requires Windows, CMake 3.24 or newer, Ninja, and a Visual Studio C++ toolchain.
The checked-in presets select the compiler, platform, and build type.
You can add a package manager with a local preset. For vcpkg, you can also pass its toolchain directly.

For reusable combinations, create `CMakeUserPresets.json` in the project root. For example:

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "msvc-release-vcpkg",
      "inherits": ["msvc-release", ":vcpkg"]
    },
    {
      "name": "msvc-debug-vcpkg",
      "inherits": ["msvc-debug", ":vcpkg"]
    },
    {
      "name": "msvc-release-conan",
      "inherits": ["msvc-release", ":conan"]
    },
    {
      "name": "msvc-debug-conan",
      "inherits": ["msvc-debug", ":conan"]
    }
  ]
}
```

You can combine another compiler preset with either package-manager preset in the same way.

### Build with vcpkg

You do not need a combined preset for vcpkg. Set `VCPKG_ROOT` in a Visual Studio developer shell:

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
cmake --preset msvc-release --toolchain "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build/msvc-release --target package
```

If you prefer a shorter, reusable configure command, use `msvc-release-vcpkg` from the earlier example.

### Build with Conan

Install Conan 2.0.5 or newer. Create its default profile once. Then run the local preset:

```powershell
python -m pip install "conan>=2.0.5"
conan profile detect --force
cmake --preset msvc-release-conan
cmake --build build/msvc-release-conan --target package
```

The CMake Conan provider installs the dependencies from `conanfile.txt` and builds missing packages.

### Run the unit tests

This example passes the vcpkg toolchain directly to the checked-in debug preset:

```powershell
cmake --preset msvc-debug --toolchain "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build/msvc-debug
ctest --test-dir build/msvc-debug -L unit --output-on-failure
```
