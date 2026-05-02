#!/bin/bash
# embed_ffmpeg.sh — FFmpeg dylib を app bundle に埋め込む
# Usage: embed_ffmpeg.sh <ffmpeg_lib_dir> <frameworks_dir> <binary>
#
# otool -L で binary にリンクされた FFmpeg dylib (libav*, libsw*) を検出し、
# Frameworks/ にコピーして @executable_path/../Frameworks/ に書き換える。
# インクリメンタルビルドでも常に dylib を再コピーし、id を更新する。

set -e

FFMPEG_LIB_DIR="$1"
FRAMEWORKS_DIR="$2"
BINARY="$3"

if [[ -z "$FFMPEG_LIB_DIR" || -z "$FRAMEWORKS_DIR" || -z "$BINARY" ]]; then
    echo "Usage: embed_ffmpeg.sh <ffmpeg_lib_dir> <frameworks_dir> <binary>" >&2
    exit 1
fi

mkdir -p "$FRAMEWORKS_DIR"
# 前回ビルドの残骸を除去してから再配置
rm -f "$FRAMEWORKS_DIR"/lib*.dylib

# binary にリンクされている dylib のうち libav* / libsw* を処理
# （絶対パスの場合も @executable_path 形式に変わっている場合も対応）
while IFS= read -r INSTALL_NAME; do
    FILENAME=$(basename "$INSTALL_NAME")
    SOURCE="$FFMPEG_LIB_DIR/$FILENAME"

    if [[ ! -f "$SOURCE" ]]; then
        echo "[embed_ffmpeg] WARNING: Source not found: $SOURCE" >&2
        continue
    fi

    echo "[embed_ffmpeg] Copying $FILENAME"
    cp "$SOURCE" "$FRAMEWORKS_DIR/$FILENAME"

    # バイナリの参照が絶対パスのままなら @executable_path に書き換え
    if [[ "$INSTALL_NAME" == "$FFMPEG_LIB_DIR"* ]]; then
        echo "[embed_ffmpeg] Fixing binary ref: $INSTALL_NAME"
        /usr/bin/install_name_tool -change \
            "$INSTALL_NAME" \
            "@executable_path/../Frameworks/$FILENAME" \
            "$BINARY"
    fi

    # Frameworks/ 内の dylib の id を更新
    /usr/bin/install_name_tool -id \
        "@executable_path/../Frameworks/$FILENAME" \
        "$FRAMEWORKS_DIR/$FILENAME"

done < <(otool -L "$BINARY" | awk '{print $1}' | grep -E 'lib(av|sw)')

echo "[embed_ffmpeg] Done."
