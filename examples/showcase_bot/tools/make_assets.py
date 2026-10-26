#!/usr/bin/env python3
"""Recreate tiny, original media fixtures. Requires Pillow, ffmpeg, espeak-ng."""
from pathlib import Path
import json
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont

ASSETS = Path(__file__).resolve().parents[1] / "assets"
ASSETS.mkdir(exist_ok=True)


def font(size):
    try:
        path = subprocess.check_output(["fc-match", "-f", "%{file}", "sans-serif"], text=True)
        return ImageFont.truetype(path, size)
    except (OSError, subprocess.SubprocessError):
        return ImageFont.load_default(size=size)


def card(name, accent, square=False):
    width, height = (512, 512) if square else (768, 512)
    image = Image.new("RGB", (width, height), "#101726")
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((28, 28, width-28, height-28), 28, fill="#182337", outline=accent, width=3)
    draw.ellipse((width-175, 58, width-65, 168), fill=accent)
    draw.text((58, 185), "tgpp", fill="white", font=font(90))
    draw.text((62, 305), "C++17 / Telegram", fill=accent, font=font(28))
    draw.text((62, height-95), "MESSAGE SHOWCASE", fill="#b9c6de", font=font(22))
    image.save(ASSETS / name)


def ffmpeg(*arguments):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *map(str, arguments)], check=True)


def main():
    card("photo.png", "#70efc1")
    card("photo_two.png", "#8aafff")
    card("square.png", "#ffbb78", True)
    sticker = Image.new("RGBA", (512, 512))
    draw = ImageDraw.Draw(sticker)
    draw.rounded_rectangle((32, 80, 480, 432), 96, fill="#70efc1", outline="white", width=12)
    draw.text((74, 187), "tg++", fill="#101726", font=font(104))
    sticker.save(ASSETS / "sticker.webp", lossless=True)
    with tempfile.TemporaryDirectory() as directory:
        speech = Path(directory) / "speech.wav"
        subprocess.run(["espeak-ng", "-s", "145", "-w", str(speech),
                        "Hello from T G P P. This voice message was uploaded by your generated C plus plus Telegram client."], check=True)
        ffmpeg("-i", speech, "-map_metadata", "-1", "-c:a", "libopus", "-b:a", "32k", "-ac", "1", ASSETS / "voice.ogg")
        ffmpeg("-i", speech, "-map_metadata", "-1", "-c:a", "libmp3lame", "-b:a", "64k", ASSETS / "audio.mp3")
        ffmpeg("-loop", "1", "-i", ASSETS / "photo.png", "-i", speech, "-t", "3", "-vf",
               "scale=480:320,fade=t=in:st=0:d=0.4", "-r", "24", "-pix_fmt", "yuv420p",
               "-c:v", "libx264", "-preset", "veryfast", "-c:a", "aac", "-movflags", "+faststart", ASSETS / "video.mp4")
    ffmpeg("-f", "lavfi", "-i", "testsrc2=size=320x320:rate=24", "-t", "3", "-an",
           "-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart", ASSETS / "animation.mp4")
    ffmpeg("-loop", "1", "-i", ASSETS / "square.png", "-t", "3", "-vf", "scale=320:320,fade=t=in:st=0:d=0.5",
           "-r", "24", "-an", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart", ASSETS / "video_note.mp4")
    (ASSETS / "sample.json").write_text(json.dumps({"library": "tgpp", "language": "C++17", "purpose": "Telegram document upload example", "original_fixture": True}, indent=2) + "\n")
    print("Generated original showcase fixtures:", sum(p.stat().st_size for p in ASSETS.iterdir() if p.is_file()), "bytes")


if __name__ == "__main__":
    main()
