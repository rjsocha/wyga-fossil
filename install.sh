#!/bin/sh
# install.sh - build wyga-fossil and copy it to /usr/local/bin/wyga-fossil.
#
# /usr/local/bin is root-owned; the second docker run executes as root
# inside the container and bind-mounts /usr/local/bin, so no sudo on the
# host is required.  The first run uses --user "$(id -u):$(id -g)" so the
# build artefact in the source tree stays user-owned.
#
# Requires the builder image:
#   docker build -f Dockerfile.builder -t wyga-fossil-builder .

set -e
cd "$(dirname "$0")"

IMAGE=wyga-fossil-builder
DEST=${DEST:-/usr/local/bin}

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "error: docker image '$IMAGE' not found." >&2
    echo "build it first: docker build -f Dockerfile.builder -t $IMAGE ." >&2
    exit 1
fi

if [ ! -d "$DEST" ]; then
    echo "error: install destination $DEST does not exist." >&2
    exit 1
fi

echo "==> Building wyga-fossil ..."
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD:/src" -w /src "$IMAGE"

echo "==> Installing to $DEST/wyga-fossil ..."
docker run --rm \
    -v "$PWD:/src" -v "$DEST:/install" -w /src \
    --entrypoint sh "$IMAGE" \
    -c 'install -m 0755 /src/wyga-fossil /install/wyga-fossil'

echo "==> Done:"
ls -la "$DEST/wyga-fossil"
"$DEST/wyga-fossil" version
