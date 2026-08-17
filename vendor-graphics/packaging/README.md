# Graphics component packaging

This directory defines the shared runtime contract for uDroid and standalone
Termux users. It is a development checkpoint, not a downloadable release yet.

## Published assets

One release version publishes these independent assets:

```text
udroid-graphics-VERSION-linux-aarch64-glibc.tar.xz
udroid-graphics-VERSION-linux-aarch64-glibc.tar.xz.sha256
udroid-graphics-release-index.json
```

`runtime_contract.py` is the release-time inventory shared by the manifest
builder and verifier. `manifest.json` describes compatibility and every payload
entry. `SHA256SUMS` covers `manifest.json` and every regular payload file. The
release asset digest protects the complete archive while `verify-runtime.py`
catches extraction damage, unexpected files, unsafe links, wrong modes, mixed
inventories and local tampering. It also parses `libhybris-common.so.1`
directly, without running it, and rejects a non-AArch64 object, a missing or
out-of-range isolated `PT_TLS` reserve, or any embedded RPATH/RUNPATH.

`build-manifest.py` finalizes a clean staging tree. In addition to generating
those inventories, it rejects absolute or non-`$ORIGIN` ELF search paths,
absolute Vulkan manifest library paths, escaping links, and source/build
artifacts. For example:

```sh
install -Dm755 ../runtime/bin/udroid-gpu-run "$stage/bin/udroid-gpu-run"
install -Dm755 ../runtime/bin/udroid-gpu-probe "$stage/bin/udroid-gpu-probe"
install -Dm755 ../tools/run-wsi-qualification.sh \
  "$stage/libexec/run-wsi-qualification.sh"
```

The matched Mesa, bridge, WSI layer, and compiled probe outputs are staged by
their clean AArch64 build jobs. The finalizer will reject the tree unless all
runtime-contract paths are present and executable tools have the correct mode.

```sh
./build-manifest.py "$stage" \
  --version 0.1.0-dev.1 \
  --source mesa="$mesa_revision" \
  --source libhybris="$libhybris_revision" \
  --source android_headers="$android_headers_revision" \
  --source sysvk="$sysvk_revision" \
  --source ginkage="$ginkage_revision" \
  --source vulkan_headers="$vulkan_headers_revision" \
  --source wsi_headers="$wsi_headers_revision" \
  --source vulkan_loader="$vulkan_loader_revision" \
  --patch zink-khr-vertex-attribute-divisor \
  --patch libhybris-isolated-static-tls
./verify-runtime.py "$stage"
./package-runtime.py "$stage" \
  "udroid-graphics-0.1.0-dev.1-linux-aarch64-glibc.tar.xz" \
  --source-date-epoch "$SOURCE_DATE_EPOCH" \
  --metadata-output package-linux-aarch64-glibc.json
./build-release-index.py package-linux-aarch64-glibc.json \
  --base-url "https://github.com/ORG/REPO/releases/download/TAG" \
  --recommended 0.1.0-dev.1 \
  --source-date-epoch "$SOURCE_DATE_EPOCH" \
  --output udroid-graphics-release-index.json
```

The first archive uses XZ because uDroid's bundled, PRoot-safe extractor can
stream it without adding another native decompressor. The archive is
deterministic for a fixed staging tree and `SOURCE_DATE_EPOCH`, contains one
versioned top-level directory, and is accompanied by an asset checksum. A
future format change must be declared by the release index rather than inferred
from the Android ABI.

The release index contains an explicit recommended version for each target,
the immutable asset URL, archive format, byte size, archive SHA-256 and
manifest SHA-256. Clients select that exact record; they do not scrape release
names, compare opaque versions, or guess which archive belongs to a device.

The release index is the only mutable network input. Both clients cache a
previously accepted index and pin the chosen asset SHA-256. The app downloads
the runtime only when a user enables or updates experimental graphics. The APK
contains the X11 integration; matched guest libraries and probes stay in the
versioned component.

## Termux checkpoint

Install the frontend and verifier during development:

```sh
install -Dm755 udroid-gpu "$PREFIX/bin/udroid-gpu"
install -Dm755 verify-runtime.py "$PREFIX/libexec/udroid-gpu/verify-runtime.py"
install -Dm644 runtime_contract.py "$PREFIX/libexec/udroid-gpu/runtime_contract.py"
```

Then install an already extracted component and launch a PRoot Distro guest:

```sh
udroid-gpu install-dir ./udroid-graphics-0.1.0-dev.1-linux-aarch64-glibc
udroid-gpu doctor
udroid-gpu proot-login ubuntu --profile vendor-vulkan:ginkage-ahb
```

Current `proot-distro` already binds the readable Android system trees, `/dev`,
`/proc` and `/sys`. The frontend deliberately does not duplicate those mounts;
it adds only the component and X11 socket. For an existing custom PRoot
launcher, bind the concrete component directory to `/opt/udroid/graphics`,
bind the existing `/system`, `/system_ext`, `/vendor`, `/odm`, `/product`,
`/apex`, `/dev`, `/proc`, and `/sys` paths, and execute:

```sh
/opt/udroid/graphics/bin/udroid-gpu-run \
  --profile vendor-vulkan:ginkage-ahb -- COMMAND
```

The utility never edits `.bashrc`, `/etc/environment`, Mesa alternatives or a
rootfs. The `system` profile executes the command without graphics overrides.
Vendor profiles resolve one concrete Vulkan HAL, preload the patched
`libhybris-common.so.1` followed by the AHardwareBuffer symbol wrapper, and ask
the embedded Android linker to initialize that same HAL before application
`main()`. Existing user preloads follow those two component libraries without
duplicates. Ambiguous devices must provide the HAL selected by qualification
through `UDROID_VULKAN_HAL`; the launcher never guesses between multiple
modules.

## uDroid integration boundary

The app implements the same operations natively:

1. read the signed release index and select an ABI/libc-compatible asset;
2. download into app-private staging and verify the asset digest;
3. extract with path and link traversal checks;
4. apply the manifest verifier and atomically activate the concrete version;
5. store renderer and WSI selection per installed rootfs;
6. bind that concrete directory and required existing Android paths when its
   supervisor starts a terminal, Linux app or desktop;
7. run the packaged probe ladder and journal the resolved route;
8. approve `Automatic` only for the same device fingerprint, component hash,
   and WSI profile that produced a complete passing report.

The Android implementation belongs on a `feat/graphics-runtime` branch created
from app `main`. It should reuse the existing versioned media/audio component
lifecycle but remain independent from FMA settings and transport.

## Still required before the first asset

- reproduce Mesa, libhybris, sysvk and Ginkage in a clean AArch64 glibc build;
- give every ELF a relative `$ORIGIN` RUNPATH and every Vulkan JSON a relative
  library path;
- generate the manifest, checksums, notices, source lock and SBOM in CI;
- validate the archive on Debian/Ubuntu plus embedded Lorie and Termux:X11;
- publish a pinned release index consumed by both clients.
