# Fuzzing toolchain for tools/fuzz_freertos.py: clang 18 with libFuzzer,
# AddressSanitizer and UBSan (not available in zig cc on Windows).
FROM ubuntu:24.04
RUN apt-get update \
    && apt-get install -y --no-install-recommends clang-18 libclang-rt-18-dev \
    && rm -rf /var/lib/apt/lists/*
