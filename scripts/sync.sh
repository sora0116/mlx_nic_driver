#!/usr/bin/env sh
set -eu

REMOTE_HOST="${REMOTE_HOST:-sdn-svr6}"
REMOTE_DIR="${REMOTE_DIR:-~/work/takagi/nicd}"

rsync -az --delete \
  --exclude '.git/' \
  --exclude '.mlxnicd-state/' \
  --exclude '*.o' \
  --exclude 'mlxnicd' \
  ./ "${REMOTE_HOST}:${REMOTE_DIR}/"
