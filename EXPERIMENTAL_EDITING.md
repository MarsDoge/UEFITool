# Experimental NE recursive Replace

This fork implements a **scoped recursive replacement engine on the NE tree**. It does not invoke the old parser, enable the unfinished general NE builder, or restore every old_engine editing operation. The prior fixed-layout implementation remains as an independent regression harness; the GUI now uses `RecursiveReplace`.

## Usage and semantics

1. Select an FFS file or section, including a node inside a supported compressed or nested FV. Unsupported selections have a reason in the status bar and action tooltip.
2. **Replace as-is** accepts a complete serialized FFS file or section. Sizes may grow or shrink. The supplied size field must match the bytes. File type and non-size attributes, or section type and processing GUID, must remain the same. A file GUID can change; dependency references are not rewritten. FFS state is normalized to the destination's live erase polarity and checksums are recalculated.
3. **Replace body** retains the header and recalculates sizes/checksums. As in old_engine, the body of a *locally compressed section* means **decoded data**. Use NE's **Extract uncompressed** for that input, not its serialized **Extract body**. For ordinary sections and FFS files it means the serialized body without the header. A file merely located inside a compressed FV is not itself a compressed section.
4. Read the warning and apply. The edit is built in a temporary byte array and the entire candidate is reparsed before changing the GUI image. Failed edits leave the image and dirty state unchanged.
5. **Save edited image as…** requires a new filename. The original and existing destinations, including symlinks and competing destinations created during saving, are never overwritten.

Save As retains the existing atomic implementation: stage a unique file in the destination directory, flush/close, publish via POSIX hard link or Windows no-replace move, then read back and compare bytes. There is no overwrite/copy fallback. Only the owned temporary name is cleaned up. A verification error never deletes a competing destination. This is not a power-loss durability guarantee.

## Rebuild contract

- Rebuild only the selected node and its ancestors. Untouched siblings are copied byte-for-byte, including unsupported contents. Opaque/nonempty PAD files are pinned rather than discarded.
- Use NE row paths, not `base()` offsets, to identify decoded nodes. Descendant bases in decompressed streams are not physical flash addresses.
- Pack FFS files on 8-byte boundaries, honor standard and extended data alignment, and generate valid PAD FFS for additional alignment. Pack sections on 4-byte boundaries.
- Support standard revision-2 FFS2/FFS3 volumes, canonical DATA_VALID files and name-only FV extended headers. FFS3 large-file and extended-section headers are supported and promoted when necessary. FFS2 24-bit overflow fails explicitly.
- Rebuild standard uncompressed, EFI 1.1, Tiano and LZMA section streams. Recognize Tiano, standard/HP/MS LZMA and CRC32 guided GUIDs. Recompute CRC32; preserve the parsed LZMA dictionary. Every encoded stream is decoded again and compared byte-for-byte before accepting it.
- Support nested FV-image sections and nested volumes through supported ancestor chains. **Both physical and nested FV extents remain fixed**; edits must fit existing volume capacity. Physical images never grow. Nested-FV block-map growth is not implemented.
- Validate FV headers/block maps, original affected FFS headers/checksums, rebuilt extents, reparsed changed nodes and untouched significant siblings. Original root warnings such as missing VTF do not automatically forbid edits. New structural diagnostics reject the transaction.
- An unchanged replacement returns the original bytes without repacking/recompression, including nested compressed targets.

## Explicit gaps

- No insert, remove, direct volume replacement, arbitrary file/section type conversion, capsule reconstruction or general rebuild command.
- No executable relocation/rebasing or VTF patching. Fixed-address, SEC/PEI/VTF targets and relocation of such siblings are rejected. Unknown platform absolute-address dependencies remain outside the validator.
- No rebuilding through LZMAF86, Intel-legacy LZMA, Brotli, gzip/zlib or unknown/signed GUID processing ancestors. Those siblings can remain untouched; there is no silent old-parser fallback.
- No proprietary FV extension updates, Apple CRC/UsedSpace regeneration, FFSv1 tails, deleted/transitional FFS states or nonstandard section-alignment quirks.
- Rebuild buffers and input images/replacements are limited to 128 MiB. Rebuilding is synchronous; large compression operations can pause the GUI.
- Type/header restrictions mean this is **not full old_engine parity**. Recompression can produce different bytes even when decoded siblings are unchanged.

## Security and hardware boundary

Recognized protection metadata is a **warning**, not a blanket prohibition. The pre-apply warning explains that protected hashes, signatures and executable certificates can become invalid. The specific NE protected-range hash-mismatch diagnostic is allowed and remains visible after the edit; structural/checksum errors are not waived. No signatures are recreated, no authentication bypass is supplied, and no bootability/authenticity claim is made. Unknown signatures and platform dependencies cannot be exhaustively detected.

**Reparse success is not a boot test.** Keep a separate original backup and board recovery method. No hardware flashing is performed.

## Build and tests

With the usual Qt development dependencies discoverable:

```sh
cmake -S UEFITool -B build-edit -DCMAKE_BUILD_TYPE=Debug -DUEFITOOL_EDIT_TESTS=ON
cmake --build build-edit -j4
ctest --test-dir build-edit --output-on-failure
```

`fixedlayout_tests` preserves the prior engine and atomic-save race regressions. `recursive_tests` adds synthetic growth/shrink, file and section as-is/body, multiple compression depths, CRC-guided bodies, nested FV reconstruction, physical and nested overflow, opaque sibling preservation, alignment padding, fixed-address refusal, 24-bit/FFS3 promotion, no-op equality and real Qt menu/dialog/Save As workflows. Synthetic fixtures are not vendor firmware.

The recursive CLI shares the GUI engine:

```sh
build-edit/uefi_recursive_edit list IMAGE
build-edit/uefi_recursive_edit body IMAGE 0/9/0/0/1/0/2 BODY NEW_OUTPUT
build-edit/uefi_recursive_edit asis IMAGE ROW/PATH COMPLETE_NODE NEW_OUTPUT
```

Use a path from `list` for that exact input image; the example is not a universal location. The legacy `uefi_fixed_edit` CLI retains its old offset/same-size behavior for regression comparison only. The qmake project includes the new source as well.

Optional local acceptance with a read-only firmware fixture:

```sh
mkdir -p build-edit/acceptance-new
QT_QPA_PLATFORM=offscreen build-edit/recursive_tests /path/to/LS3A60007A.fd build-edit/acceptance-new
```

The optional acceptance currently selects `DebugPortProtocolDxe`, adds a valid RAW section to grow its FFS, saves/reparses, exercises full FFS replacement through GUI menus, then grows its UI section through the GUI and saves/reparses again. It also exercises shrinking and verifies the original SHA-256 is unchanged. Output names must not already exist. Never commit the fixture or generated firmware.

## Local validation of this slice

- CMake Qt5 and qmake GUI builds completed; both CTest suites passed. The final recursive run including the read-only real fixture passed 490 checks, including strict EFI/Tiano codec-preservation regressions.
- The real 8 MiB fixture's DXE chain was inspected: physical FFS3 FV → FV-image FFS → LZMA guided section (16 MiB dictionary) → extended FV-image section → nested FFS2 FV. It is not a direct physical FFS replacement.
- Real nested DXE FFS growth/body and as-is replacement, UI-section growth, LZMA recompression, atomic Save As, full reparse, unchanged-sibling verification and original-file hash preservation passed. The GUI tests invoke the actual menu slots, file dialogs and warning confirmations using Qt's offscreen platform.
- A separate recursive OVMF smoke test grew the Shell UI section inside its compressed nested FV, rebuilt/recompressed the image and reached the UEFI Shell prompt under QEMU q35/TCG. This name-change test is not evidence for arbitrary executable replacement or physical-board compatibility. No physical board boot was attempted.

## 中文摘要

本版已从“等长物理 FFS 替换”扩展为基于 NE 树的递归 Replace：支持 FFS/Section 的完整替换和正文替换、变长、支持算法的重新压缩及嵌套 FV。压缩 section 的 Replace body 按旧引擎语义接收解压正文；NE 中应使用 Extract uncompressed 获取。物理和嵌套 FV 暂不扩容，空间不足或需要未实现的重定位时明确拒绝，不会切换到旧 parser。

已通过合成回归、真实镜像中嵌套 DXE 文件及 UI section 的 Qt 菜单替换/另存/重解析测试，原镜像哈希不变。保护元数据以失效警告呈现，不承诺签名有效或修改后可启动；未刷写、未验证真实主板启动。
