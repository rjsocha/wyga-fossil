# syntax=docker/dockerfile:1
#
# Pre-baked builder image for static wyga-fossil binaries (timer-fork edition).
# Build the image ONCE:
#
#     docker build -f Dockerfile.builder -t wyga-fossil-builder .
#
# Then build the binary from the current checkout WITHOUT re-pulling deps:
#
#     docker run --rm \
#         --user "$(id -u):$(id -g)" \
#         -v "$PWD:/src" -w /src \
#         wyga-fossil-builder
#
# The --user flag makes the resulting ./wyga-fossil binary owned by you instead
# of by root.  The default CMD runs `./configure --static && make`.
# Override at will:
#
#     docker run --rm --user "$(id -u):$(id -g)" \
#         -v "$PWD:/src" -w /src wyga-fossil-builder \
#         sh -c 'make clean && ./configure --static && make -j$(nproc)'
#
# Output binary lands at ./wyga-fossil in the host (because /src is a bind mount).
#
# Pinning a specific Alpine release keeps the image stable; bump the tag
# when you want fresh upstream tools.
FROM alpine:3.20

RUN apk add --no-cache \
        gcc make musl-dev linux-headers \
        openssl-dev openssl-libs-static \
        zlib-dev zlib-static \
        binutils upx file

WORKDIR /src

# Default behaviour: configure + parallel build, then strip debug/symbols
# and squeeze with UPX.  The strip drops the binary from ~32 MB to ~5 MB;
# UPX compresses further to roughly 1.5 MB at the cost of a small startup
# decompression step (negligible for wyga-fossil's CLI invocation).  Override
# at run time if you want to skip either:
#
#     docker run ... wyga-fossil-builder \
#         sh -c './configure --static && make -j$(nproc)'
CMD ["sh", "-c", "\
    ./configure --static && \
    make -j$(nproc) && \
    strip --strip-all wyga-fossil && \
    upx --best --lzma wyga-fossil && \
    file wyga-fossil && \
    ls -la wyga-fossil"]
