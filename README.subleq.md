# QEMU SUBLEQ system emulator

This browsable source tree is a fork of upstream [QEMU](https://github.com/qemu/qemu) at commit `f8ed81651e61d9c2166df6121ce2af0f44f06b3e`, with the SUBLEQ CPU, ESI machine and associated device changes. The initial import came from the audited archive `qemu-source.tar.zst` (SHA-256 `c86ed6b686013d11e2037e88a6483792724aae685765ed223dc53b3951a136fa`); subsequent commits add checked SUBLEQ TCG translation optimizations directly to this public tree. No guest algorithm, private syscall, BF interpreter or challenge code is part of the emulator. Upstream QEMU licenses, notices, and vendored subprojects' notices are retained (`LICENSE`, `COPYING`, `COPYING.LIB`, `subprojects/*/COPYING*`, etc.). The subprojects `berkeley-softfloat-3`, `berkeley-testfloat-3`, and `keycodemapdb` are included as actual source directories; they are deliberately committed despite upstream's `.gitignore` rules. There is no guest ROM, kernel, disk, account, or private test media in this repository or image.

[Machine documentation](docs/system/target-subleq.rst) · [Release binaries](https://github.com/danosito/qemu-system-subleq/releases) · [Container](https://github.com/danosito/qemu-system-subleq/pkgs/container/qemu-system-subleq). Guest kernel and tooling are separate projects: [subleq-kernel](https://github.com/danosito/subleq-kernel) and [subleq-tools](https://github.com/danosito/subleq-tools). No guest code or disk is copied into this QEMU distribution.

## Build on Debian Trixie

```sh
apt-get update
apt-get install -y build-essential python3 python3-venv ninja-build meson pkg-config libglib2.0-dev libpixman-1-dev libslirp-dev libfdt-dev zlib1g-dev
mkdir build && cd build
../configure --target-list=subleq-softmmu --without-default-features --enable-tcg --enable-slirp --enable-fdt=system --disable-docs --disable-download
ninja -j2 qemu-system-subleq
./qemu-system-subleq --help
./qemu-system-subleq -machine help
```

The checked optimized RAM/ROM translation is opt-in and remains a single-vCPU feature. For the same machine flags on a suitable Linux guest, use `-accel tcg,thread=single -smp 1 -global subleq-cpu.x-compact-tb=on -global subleq-cpu.x-ordered-read-pair=on -global subleq-cpu.x-retranslation-helper=on -global subleq-cpu.x-direct-c-link=on`; the ordinary machine configuration stays the default. This is a speed option, not a permission to bypass SoftMMU address checks, dirty-code invalidation or device bounds. x86-64 and AArch64 native code generation and wall-clock speeds differ; no common latency guarantee follows from one host's results. The build mode is deliberately limited to `subleq-softmmu`; this is not a claim that every upstream QEMU target or every vendored optional feature builds offline. Both linux/amd64 and linux/arm64 are built natively on GitHub runners from the public source tree. Release binaries are dynamically linked, with per-file SHA-256 checksums. `packaging/Dockerfile` makes a Debian Trixie slim runtime image with only the executable and its runtime libraries; `/usr/local/bin/qemu-system-subleq` is the entrypoint. To extend it for a service, use `FROM ghcr.io/danosito/qemu-system-subleq@sha256:<manifest-digest>` and install extra packages as root. Run `docker run --rm ghcr.io/danosito/qemu-system-subleq:latest --help` to check the target. No firmware is shipped; supply a guest image separately to actually boot.
