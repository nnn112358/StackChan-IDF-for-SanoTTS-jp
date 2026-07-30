#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""ダンス blob (制御データ + 音声) を生成する PC オーサリング ツール。

blob レイアウト (little-endian, main/dance.hpp の DanceBlobHeader と一致):
  [Header 48B][DanceKeyframe(12B) × kf_count][audio audio_len B]

音声は現状 PCM16LE (audio_fmt=0)。AAC(1) は将来。座標は符号付き ±100%
(0=キャリブ中心, +100=軸max, -100=軸min)。移動時間は ms。

使い方:
  # テスト曲を生成して .bin を書く
  python3 build_dance.py --out /tmp/test_dance.bin
  # 生成してそのままデバイスへアップロード
  python3 build_dance.py --upload http://192.168.2.131 --password hogeFugapiyo
  # 自作の keyframes JSON + WAV(16bit mono) から作る
  python3 build_dance.py --keyframes kf.json --wav song.wav --out out.bin

keyframes JSON: [{"t":0,"move":300,"yaw":60,"pitch":30}, ...]
  t=開始からの ms, move=移動にかける ms, yaw/pitch=パーセンテージ(整数)。
"""
import argparse
import json
import math
import struct
import sys
import wave

MAGIC = 0x31434E44  # "DNC1"
RATE = 16000
HDR = "<IHHBBHIIII16sI"   # 48 bytes
KF = "<IHhhH"             # 12 bytes
assert struct.calcsize(HDR) == 48 and struct.calcsize(KF) == 12


def gen_test_song():
    """埋め込みサンプルとは別の、聞き分け可能なテスト曲 + 振り付けを生成。
    下降→上昇のフレーズ + 大きめの縦ノリ。~7 秒。"""
    # (freq Hz(0=休符), ms, yaw%, pitch%, move ms)
    steps = [
        (784, 300, 0, 80, 240), (699, 300, -70, 40, 240), (659, 300, 70, 40, 240),
        (587, 300, -70, 0, 240), (523, 450, 0, -30, 400), (0, 200, 0, 60, 200),
        (523, 300, 90, 30, 200), (587, 300, -90, 30, 200), (659, 300, 90, -30, 200),
        (699, 300, -90, -30, 200), (784, 500, 0, 100, 460), (0, 250, 0, 20, 220),
        (880, 300, 60, 50, 200), (784, 300, -60, 50, 200), (699, 300, 60, -40, 200),
        (659, 450, -60, -40, 400), (587, 300, 40, 20, 240), (523, 700, 0, 0, 640),
        (0, 300, 0, 50, 260), (659, 300, 100, 30, 180), (784, 300, -100, 30, 180),
        (988, 600, 0, 90, 540), (523, 800, 0, 0, 720),
    ]
    pcm = bytearray()
    kfs = []
    t = 0
    for (freq, ms, yaw, pitch, move) in steps:
        n = ms * RATE // 1000
        atk, rel = RATE * 5 // 1000, RATE * 12 // 1000
        for i in range(n):
            v = 0.0
            if freq > 0:
                ph = 2 * math.pi * freq * i / RATE
                v = math.sin(ph) + 0.30 * math.sin(3 * ph)
                env = 1.0
                if i < atk:
                    env = i / atk
                elif i > n - rel:
                    env = (n - i) / rel
                v *= env
            s = max(-32768, min(32767, int(v * 6500)))
            pcm += struct.pack("<h", s)
        kfs.append((t, move, yaw * 100, pitch * 100, 0))
        t += ms
    return kfs, bytes(pcm), t


def load_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2, "16-bit WAV required"
        assert w.getnchannels() == 1, "mono WAV required"
        rate = w.getframerate()
        return w.readframes(w.getnframes()), rate


def load_keyframes(path, total_ms):
    kfs = []
    for k in json.load(open(path)):
        kfs.append((int(k["t"]), int(k.get("move", 300)),
                    int(round(k["yaw"] * 100)), int(round(k["pitch"] * 100)), 0))
    return kfs


def build(kfs, pcm, total_ms, rate, name="test"):
    kf_count = len(kfs)
    audio_off = 48 + kf_count * 12
    hdr = struct.pack(HDR, MAGIC, 1, kf_count, 0, 1, 0, rate, audio_off, len(pcm),
                      total_ms, name.encode()[:15].ljust(16, b"\0"), 0)
    body = b"".join(struct.pack(KF, *kf) for kf in kfs)
    return hdr + body + pcm


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="出力 blob パス")
    ap.add_argument("--keyframes", help="keyframes JSON (省略時はテスト振り付け)")
    ap.add_argument("--wav", help="16bit mono WAV (省略時はテストメロディ生成)")
    ap.add_argument("--name", default="test")
    ap.add_argument("--upload", help="デバイス URL (例 http://192.168.2.131) — 生成して POST")
    ap.add_argument("--password", default="", help="Basic 認証パスワード")
    a = ap.parse_args()

    rate = RATE
    if a.wav:
        pcm, rate = load_wav(a.wav)
        gen_kfs, _, gen_total = gen_test_song()
        total = len(pcm) // 2 * 1000 // rate
    else:
        gen_kfs, pcm, total = gen_test_song()

    if a.keyframes:
        kfs = load_keyframes(a.keyframes, total)
    else:
        kfs = gen_kfs if not a.wav else gen_kfs

    blob = build(kfs, pcm, total, rate, a.name)
    print(f"blob: {len(blob)} bytes ({len(kfs)} keyframes, {len(pcm)} audio bytes, {total} ms, {rate} Hz)")

    if a.out:
        open(a.out, "wb").write(blob)
        print(f"wrote {a.out}")
    if a.upload:
        import base64
        import urllib.request
        req = urllib.request.Request(a.upload.rstrip("/") + "/api/dance/upload",
                                     data=blob, method="POST")
        if a.password:
            tok = base64.b64encode(f":{a.password}".encode()).decode()
            req.add_header("Authorization", "Basic " + tok)
        req.add_header("Content-Type", "application/octet-stream")
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                print(f"upload: HTTP {r.status}")
        except Exception as e:
            print(f"upload failed: {e}", file=sys.stderr)
            sys.exit(1)
    if not a.out and not a.upload:
        print("(no --out / --upload; nothing written)")


if __name__ == "__main__":
    main()
