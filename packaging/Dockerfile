FROM debian:trixie-slim
ARG TARGETARCH
LABEL org.opencontainers.image.source="https://github.com/danosito/qemu-system-subleq" \
      org.opencontainers.image.licenses="GPL-2.0-or-later" \
      org.opencontainers.image.description="SUBLEQ system emulator (QEMU fork); no guest media"
RUN apt-get update && apt-get install -y --no-install-recommends \
    libglib2.0-0t64 libpixman-1-0 libslirp0 zlib1g libatomic1 \
    && rm -rf /var/lib/apt/lists/*
COPY binaries/${TARGETARCH}/qemu-system-subleq /usr/local/bin/qemu-system-subleq
COPY licenses/ /usr/share/doc/qemu-system-subleq/
RUN chmod 755 /usr/local/bin/qemu-system-subleq && \
    /usr/local/bin/qemu-system-subleq --help | grep -F 'https://github.com/danosito/qemu-system-subleq' && \
    /usr/local/bin/qemu-system-subleq -machine help | grep -E 'esi.*(default|SUBLEQ)'
ENTRYPOINT ["/usr/local/bin/qemu-system-subleq"]
