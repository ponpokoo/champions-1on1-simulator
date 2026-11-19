#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if command -v python3 >/dev/null 2>&1; then
  PY_CMD="python3"
elif command -v python >/dev/null 2>&1; then
  PY_CMD="python"
else
  echo "[Error] Python 3 が見つかりませんでした。Python 3 をインストールしてから再実行してください。" >&2
  exit 1
fi

if ! command -v gcc >/dev/null 2>&1; then
  echo "[Error] gcc が見つかりませんでした。" >&2
  echo "  macOS: xcode-select --install" >&2
  echo "  Ubuntu/Debian: sudo apt install build-essential" >&2
  echo "※結果分析のサンプル閲覧だけなら viewer/index.html をブラウザで直接開けます。" >&2
  exit 1
fi

echo "Champions 1on1 Studio を起動しています... (終了: Ctrl+C)"
exec "$PY_CMD" viewer/server.py "$@"
