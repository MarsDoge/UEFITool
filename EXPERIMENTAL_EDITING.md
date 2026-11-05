# Experimental NE fixed-layout editing

This fork adds a deliberately limited editing path to UEFITool NE. It does **not** complete the general-purpose NE builder or restore all old_engine editing operations. Upstream baseline: `de73942df908b9466c055e15108ac4e4c0b1688c`.

## Usage

1. Open an image and select an eligible physical FFS file directly in a supported firmware volume.
2. Choose **Replace as-is** (complete FFS file) or **Replace body** (without the 24-byte FFS header).
3. Read the experimental warning. The replacement must have exactly the original size and retain the parsed layout. Complete-file replacements must also preserve GUID, type, attributes, state and size, and supply valid checksums.
4. Save to a **new filename**. Existing destinations and the original are refused, including destinations created by another process during saving. Output is staged in a uniquely created temporary file in the destination directory, flushed and closed, then published with an atomic no-replace operation: POSIX hard link on Unix, or a Windows move without replacement. Unsupported platforms/filesystems fail closed; there is no overwrite or copy fallback. Only the owned temporary file is cleaned up, never a competing destination. Published output is read back for byte comparison; a verification error leaves the destination untouched. This is not a power-loss durability guarantee.

The engine updates FFS integrity checksums, reparses the candidate, and checks the whole parsed tree's layout before accepting an edit. Opening another image or closing with unsaved edits requires confirmation. Insert, remove and general rebuild remain disabled.

## Limits

- No resizing, insertion, deletion, relocation, recompression or direct section editing.
- No extended FFS file headers or extended data alignment. Only supported revision-2 standard FFS2/FFS3 volumes and canonical DATA_VALID files are eligible; this is not full FFSv3 write support.
- Capsules, nested volumes, compressed/GUID-defined containers, recognized firmware protection metadata, PE certificate directories and unsupported FV extensions are rejected. Standard name-only FV extensions are accepted.
- Parser diagnostics associated with the selected file or its volume prevent editing.
- Security metadata detection is not exhaustive. Structural validity, unchanged layout and successful saving do **not** establish authenticity, hardware compatibility or safe flashability.
- No hardware flashing is performed. Keep an independent original backup and recovery method. Do not treat this experimental build as a production BIOS editor.

Larger-file replacement and FV reconstruction remain future work, not delivered features.

## Build and tests

Install the normal Qt development dependencies for UEFITool. With Qt discoverable by CMake:

```sh
cmake -S UEFITool -B build-edit -DCMAKE_BUILD_TYPE=Debug -DUEFITOOL_EDIT_TESTS=ON
cmake --build build-edit -j4
ctest --test-dir build-edit --output-on-failure
```

The optional test executable exercises generated synthetic FV/FFS fixtures and actual Qt replacement/save dialogs offscreen. These fixtures are not vendor firmware samples. A small harness uses the same editing engine as the GUI:

```sh
build-edit/uefi_fixed_edit list IMAGE
build-edit/uefi_fixed_edit body IMAGE OFFSET REPLACEMENT NEW_OUTPUT
build-edit/uefi_fixed_edit file IMAGE OFFSET REPLACEMENT NEW_OUTPUT
```

The qmake GUI build remains supported using `UEFITool/uefitool.pro`.

## Verification performed for this experimental slice

- CMake and qmake builds completed; the regression test executable passed.
- Real Ubuntu OVMF_CODE_4M.fd: a same-size UI-name edit in the uncompressed SecMain file changed `SecMain` to `SecEdit`. Exactly three image bytes changed; image size remained unchanged.
- Original and edited OVMF were separately booted with fresh OVMF_VARS_4M.fd copies under QEMU q35/TCG. Both reached the UEFI Interactive Shell prompt. The processes were deliberately terminated after the observation period; this is a boot smoke test, not an OS or physical-platform test.
- Edited OVMF SHA-256: `e06ae5df0c760014216edb553d406ce5e02d63b4a7ee53490a1473969f6b977a`.

That single real-image result does not establish compatibility with arbitrary firmware or executable replacements.

## 中文摘要

这是 NE 的固定布局实验编辑版，不是完整 builder：支持符合条件的 FFS 文件等长替换（完整文件或正文），重新计算校验、解析验证后另存到新文件。不支持变长、插入、删除、重排和重新压缩。

已做合成镜像回归测试、Qt 对话框测试，以及真实 OVMF 的 UI 名称修改与 QEMU 启动对照；原版和修改版均进入 Shell。未做物理主板刷写，不能据此承诺任意 BIOS 修改后可启动。
