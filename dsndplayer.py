#!/usr/bin/env python3
"""
DSND / DSNDS Tkinter player

What it does:
- Browses folders from a chosen root directory
- Opens .dsnd and .dsnds files
- Decodes the project's DSND container:
    magic[4]  = b"DSND"
    rateDiv   = u8
    flags     = u8   (bit0 stereo, bit2 16-bit)
    loopStart  = u16
    sampleCount= u32
- Converts the raw PCM payload into a temporary WAV
- Plays it with ffplay (available on most Linux installs)

Controls:
- Double-click a file, or select and press Play
- Loop toggle restarts playback automatically
- Volume is applied by rescaling the PCM before playback

Dependencies:
- Python 3.10+
- tkinter (standard)
- ffplay (from ffmpeg)

This is intentionally self-contained and does not depend on pygame / pyaudio.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import tempfile
import threading
import time
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

DSND_MAGIC = b"DSND"
DSND_FLAG_STEREO = 1 << 0
DSND_FLAG_16BIT = 1 << 2
DSND_FLAG_ADPCM  = 1 << 3

PLAYER_COMMANDS = ["ffplay", "avplay"]

RATE_DIV_TO_HZ = {
    0: 32768,
    1: 16384,
    2: 8192,
    3: 5512,
}


@dataclass
class DsndTrack:
    path: Path
    rate_hz: int
    stereo: bool
    is_16bit: bool
    loop_start_frames: int
    sample_count: int
    pcm: bytes

    @property
    def frames(self) -> int:
        return self.sample_count // (2 if self.stereo else 1)


def _read_dsnd(path: Path) -> DsndTrack:
    raw = path.read_bytes()
    if len(raw) < 12:
        raise ValueError("File too small to be a DSND file.")

    magic, rate_div, flags, loop_start, sample_count = struct.unpack_from("<4sBBHL", raw, 0)
    if magic != DSND_MAGIC:
        raise ValueError("Bad magic. Not a DSND file.")
    if rate_div not in RATE_DIV_TO_HZ:
        raise ValueError(f"Unsupported rateDiv {rate_div}.")
    rate_hz = RATE_DIV_TO_HZ[rate_div]
    stereo = bool(flags & DSND_FLAG_STEREO)
    is_16bit = bool(flags & DSND_FLAG_16BIT)
    is_adpcm  = bool(flags & DSND_FLAG_ADPCM)

    payload = raw[12:]
    if not payload:
        raise ValueError("No payload data in file.")

    # ADPCM handling: the DSND ADPCM payload uses a 4-byte preamble followed
    # by 4-bit samples (nibbles). The byte-count is therefore 4 + ceil(nibbles/2).
    if is_adpcm:
        # For now we only detect ADPCM and report it; decoding not implemented.
        data_bytes = 4 + (sample_count + 1) // 2
        if len(payload) < data_bytes:
            raise ValueError(
                f"ADPCM payload is truncated: expected at least {data_bytes} bytes, got {len(payload)}."
            )
        raise ValueError("IMA-ADPCM payload detected — decoder not implemented in this player.")

    # Basic integrity checks for PCM formats
    pcm = payload
    bytes_per_sample = 2 if is_16bit else 1
    if stereo:
        expected_samples = sample_count
        if expected_samples % 2 != 0:
            raise ValueError("Stereo sampleCount is not even.")
        expected_bytes = expected_samples * bytes_per_sample
    else:
        expected_bytes = sample_count * bytes_per_sample

    if len(pcm) < expected_bytes:
        raise ValueError(
            f"PCM payload is truncated: expected at least {expected_bytes} bytes, got {len(pcm)}."
        )

    # Trim to the declared size; extra bytes are ignored.
    pcm = pcm[:expected_bytes]

    return DsndTrack(
        path=path,
        rate_hz=rate_hz,
        stereo=stereo,
        is_16bit=is_16bit,
        loop_start_frames=int(loop_start // (2 if stereo else 1)),
        sample_count=int(sample_count),
        pcm=pcm,
    )


def _scale_pcm(track: DsndTrack, volume: int) -> bytes:
    """Scale PCM to the requested volume (0..100)."""
    volume = max(0, min(100, int(volume)))
    if volume == 100:
        return track.pcm
    if volume == 0:
        if track.is_16bit:
            return b"\x00\x00" * (len(track.pcm) // 2)
        return b"\x80" * len(track.pcm)  # silence for unsigned 8-bit

    scale = volume / 100.0

    if track.is_16bit:
        count = len(track.pcm) // 2
        samples = struct.unpack("<" + "h" * count, track.pcm)
        out = []
        for s in samples:
            v = int(s * scale)
            if v < -32768:
                v = -32768
            elif v > 32767:
                v = 32767
            out.append(v)
        return struct.pack("<" + "h" * len(out), *out)

    # 8-bit unsigned PCM
    out = bytearray(len(track.pcm))
    for i, b in enumerate(track.pcm):
        signed = b - 128
        scaled = int(signed * scale)
        if scaled < -128:
            scaled = -128
        elif scaled > 127:
            scaled = 127
        out[i] = (scaled + 128) & 0xFF
    return bytes(out)


def _write_wav(track: DsndTrack, pcm: bytes, wav_path: Path) -> None:
    channels = 2 if track.stereo else 1
    sampwidth = 2 if track.is_16bit else 1
    with wave.open(str(wav_path), "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sampwidth)
        wf.setframerate(track.rate_hz)
        wf.writeframes(pcm)


class PlayerBackend:
    def __init__(self) -> None:
        self._proc: Optional[subprocess.Popen] = None
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._loop = False
        self._lock = threading.Lock()
        self.player_cmd = self._find_player_command()

    def _find_player_command(self) -> Optional[str]:
        for candidate in PLAYER_COMMANDS:
            path = shutil.which(candidate)
            if path:
                return path
        return None

    @property
    def available(self) -> bool:
        return self.player_cmd is not None

    @property
    def playing(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def stop(self):
        with self._lock:
            self._loop = False
            self._stop_event.set()

            proc = self._proc
            self._proc = None

        if proc:
            try:
                proc.kill()
                proc.wait(timeout=2)
            except Exception:
                pass

    def play(self, track: DsndTrack, volume: int, loop: bool) -> None:
        self.stop()
        if not self.player_cmd:
            raise RuntimeError(
                "No compatible audio player found. Install ffplay or avplay to play DSND files."
            )
        self._stop_event = threading.Event()
        self._loop = bool(loop)

        thread = threading.Thread(
            target=self._run,
            args=(track, int(volume), self._stop_event, bool(loop)),
            daemon=True,
        )
        self._thread = thread
        thread.start()

    def _run(self, track: DsndTrack, volume: int, stop_event: threading.Event, loop: bool) -> None:
        try:
            scaled = _scale_pcm(track, volume)

            with tempfile.TemporaryDirectory(prefix="dsnd_player_") as tmpdir:
                tmpdir_path = Path(tmpdir)
                wav_path = tmpdir_path / (track.path.stem + ".wav")
                _write_wav(track, scaled, wav_path)

                while not stop_event.is_set():
                    cmd = [
                        self.player_cmd,
                        "-nodisp",
                        "-autoexit",
                        "-loglevel",
                        "error",
                        str(wav_path),
                    ]
                    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    with self._lock:
                        self._proc = proc

                    while True:
                        if stop_event.is_set():
                            try:
                                proc.terminate()
                            except Exception:
                                pass
                            break
                        ret = proc.poll()
                        if ret is not None:
                            break
                        time.sleep(0.05)

                    if stop_event.is_set():
                        break
                    if not loop:
                        break
        finally:
            with self._lock:
                self._proc = None


class DsndBrowser(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("DSND Player")
        self.geometry("860x560")
        self.minsize(760, 480)

        self.backend = PlayerBackend()
        self.root_dir = tk.StringVar(value=str(Path.cwd()))
        self.status = tk.StringVar(value="Ready.")
        self.info = tk.StringVar(value="No file loaded.")
        if not self.backend.available:
            self.status.set("No audio player found. Install ffplay or avplay.")
        self.loop_var = tk.BooleanVar(value=False)
        self.volume_var = tk.IntVar(value=100)
        self.search_var = tk.StringVar(value="")
        self.protocol("WM_DELETE_WINDOW", self.on_close)

        self._entries: list[Path] = []
        self._selected_track: Optional[DsndTrack] = None
        self._current_dir: Optional[Path] = None

        self._build_ui()
        self._bind_keys()
        self.refresh_dir()

        self.after(250, self._ui_tick)

    def on_close(self):
        self.backend.stop()

        if self.backend._thread:
            self.backend._thread.join(timeout=2)

        self.destroy()

    def _build_ui(self) -> None:
        top = ttk.Frame(self, padding=10)
        top.pack(fill="x")

        ttk.Label(top, text="Root folder:").pack(side="left")
        self.root_entry = ttk.Entry(top, textvariable=self.root_dir)
        self.root_entry.pack(side="left", fill="x", expand=True, padx=(6, 6))
        ttk.Button(top, text="Browse", command=self.choose_root).pack(side="left")
        ttk.Button(top, text="Refresh", command=self.refresh_dir).pack(side="left", padx=(6, 0))

        mid = ttk.Frame(self, padding=(10, 0, 10, 10))
        mid.pack(fill="x")
        ttk.Label(mid, text="Filter:").pack(side="left")
        filt = ttk.Entry(mid, textvariable=self.search_var)
        filt.pack(side="left", fill="x", expand=True, padx=(6, 8))
        filt.bind("<KeyRelease>", lambda e: self.refresh_dir())
        ttk.Checkbutton(mid, text="Loop", variable=self.loop_var).pack(side="left", padx=(0, 8))
        ttk.Label(mid, text="Volume").pack(side="left")
        self.volume_var = tk.IntVar(value=100)

        vol = ttk.Scale(
            mid,
            from_=0,
            to=100,
            orient="horizontal",
            variable=self.volume_var,
            command=self._on_volume_changed,
        )

        vol.pack(side="left", fill="x", expand=True, padx=(6, 8))
        self.vol_label = ttk.Label(mid, text="100%")
        self.vol_label.pack(side="left")

        main = ttk.Frame(self, padding=(10, 0, 10, 10))
        main.pack(fill="both", expand=True)

        left = ttk.Frame(main)
        left.pack(side="left", fill="both", expand=True)

        self.listbox = tk.Listbox(left, activestyle="dotbox", font=("TkDefaultFont", 11))
        self.listbox.pack(side="left", fill="both", expand=True)
        self.listbox.bind("<Double-Button-1>", lambda e: self.open_selected())
        self.listbox.bind("<<ListboxSelect>>", lambda e: self._update_selection())

        scroll = ttk.Scrollbar(left, orient="vertical", command=self.listbox.yview)
        scroll.pack(side="left", fill="y")
        self.listbox.configure(yscrollcommand=scroll.set)

        right = ttk.Frame(main, width=260)
        right.pack(side="left", fill="y", padx=(10, 0))

        info_box = ttk.LabelFrame(right, text="File info", padding=10)
        info_box.pack(fill="x")
        ttk.Label(info_box, textvariable=self.info, wraplength=230, justify="left").pack(fill="x")

        controls = ttk.LabelFrame(right, text="Playback", padding=10)
        controls.pack(fill="x", pady=(10, 0))
        ttk.Button(controls, text="Play / Open", command=self.open_selected).pack(fill="x")
        ttk.Button(controls, text="Stop", command=self.stop).pack(fill="x", pady=(6, 0))
        ttk.Button(controls, text="Choose File…", command=self.choose_file).pack(fill="x", pady=(6, 0))
        ttk.Button(controls, text="Up folder", command=self.go_up).pack(fill="x", pady=(6, 0))

        status_box = ttk.LabelFrame(right, text="Status", padding=10)
        status_box.pack(fill="x", pady=(10, 0))
        ttk.Label(status_box, textvariable=self.status, wraplength=230, justify="left").pack(fill="x")

        bottom = ttk.Frame(self, padding=(10, 0, 10, 10))
        bottom.pack(fill="x")
        ttk.Label(bottom, text="Enter = play   Backspace = up   Space = stop   Esc = quit").pack(side="left")

    def _bind_keys(self) -> None:
        self.bind("<Return>", lambda e: self.open_selected())
        self.bind("<BackSpace>", lambda e: self.go_up())
        self.bind("<space>", lambda e: self.stop())
        self.bind("<Escape>", lambda e: self.on_close())
        self.bind("<Up>", lambda e: self._move_sel(-1))
        self.bind("<Down>", lambda e: self._move_sel(1))
        self.bind("<Left>", lambda e: self._adjust_volume(-5))
        self.bind("<Right>", lambda e: self._adjust_volume(5))

    def choose_root(self) -> None:
        chosen = filedialog.askdirectory(initialdir=self.root_dir.get() or str(Path.cwd()))
        if chosen:
            self.root_dir.set(chosen)
            self.refresh_dir()

    def choose_file(self) -> None:
        filetypes = [("DSND Files", "*.dsnd *.dsnds"), ("All Files", "*")]
        chosen = filedialog.askopenfilename(
            title="Open DSND File",
            initialdir=self.root_dir.get() or str(Path.cwd()),
            filetypes=filetypes,
        )
        if not chosen:
            return
        self.root_dir.set(str(Path(chosen).parent))
        self.refresh_dir()
        self.listbox.selection_clear(0, tk.END)
        for idx, entry in enumerate(self._entries):
            if entry == Path(chosen):
                self.listbox.selection_set(idx)
                self.listbox.see(idx)
                break
        self.open_selected()

    def _adjust_volume(self, delta: int) -> None:
        new_vol = max(0, min(100, self.volume_var.get() + delta))
        self.volume_var.set(new_vol)
        self.vol_label.config(text=f"{new_vol}%")
        if self._selected_track and self.backend.playing:
            self.open_selected()

    def _on_volume_changed(self, value):
        vol = int(float(value))
        self.volume_var.set(vol)

        if hasattr(self, "vol_label"):
            self.vol_label.config(text=f"{vol}%")

    def _move_sel(self, delta: int) -> None:
        if not self._entries:
            return
        idx = self.listbox.curselection()
        current = idx[0] if idx else 0
        new = max(0, min(len(self._entries) - 1, current + delta))
        self.listbox.selection_clear(0, tk.END)
        self.listbox.selection_set(new)
        self.listbox.see(new)
        self._update_selection()

    def _selected_path(self) -> Optional[Path]:
        idx = self.listbox.curselection()
        if not idx or idx[0] >= len(self._entries):
            return None
        return self._entries[idx[0]]

    def _update_selection(self) -> None:
        path = self._selected_path()
        if not path:
            self.info.set("No file selected.")
            return
        if path.is_dir():
            self.info.set(f"Directory\n{path}")
            return
        try:
            track = _read_dsnd(path)
            self._selected_track = track
            self.info.set(
                f"File: {path.name}\n"
                f"Path: {path}\n"
                f"Rate: {track.rate_hz} Hz\n"
                f"Stereo: {'yes' if track.stereo else 'no'}\n"
                f"16-bit: {'yes' if track.is_16bit else 'no'}\n"
                f"Frames: {track.frames}\n"
                f"Loop start: {track.loop_start_frames}"
            )
        except Exception as exc:
            self._selected_track = None
            self.info.set(f"File: {path.name}\nNot a valid DSND: {exc}")

    def refresh_dir(self) -> None:
        base = Path(self.root_dir.get()).expanduser()
        if not base.exists():
            messagebox.showerror("DSND Player", f"Folder does not exist:\n{base}")
            return
        self._current_dir = base

        needle = self.search_var.get().strip().lower()
        entries = []
        try:
            for child in sorted(base.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower())):
                if child.is_dir():
                    entries.append(child)
                else:
                    name = child.name.lower()
                    if name.endswith(".dsnd") or name.endswith(".dsnds"):
                        if not needle or needle in name:
                            entries.append(child)
        except Exception as exc:
            messagebox.showerror("DSND Player", f"Failed to list folder:\n{exc}")
            return

        self._entries = entries
        self.listbox.delete(0, tk.END)
        for p in entries:
            tag = "[DIR]" if p.is_dir() else "[SND]"
            self.listbox.insert(tk.END, f"{tag} {p.name}")

        if entries:
            self.listbox.selection_set(0)
            self.listbox.see(0)
        self._update_selection()
        self.status.set(f"{len(entries)} item(s) in {base}")

    def open_selected(self) -> None:
        path = self._selected_path()
        if not path:
            return
        if path.is_dir():
            self.root_dir.set(str(path))
            self.refresh_dir()
            return

        try:
            track = _read_dsnd(path)
        except Exception as exc:
            messagebox.showerror("DSND Player", f"Failed to load file:\n{path}\n\n{exc}")
            return

        self._selected_track = track
        self.status.set(f"Playing: {path.name}")
        self.loop_var.set(bool(self.loop_var.get()))
        self.backend.play(track, self.volume_var.get(), self.loop_var.get())

    def stop(self) -> None:
        self.backend.stop()
        self.status.set("Stopped.")

    def go_up(self) -> None:
        if self.backend.playing:
            self.stop()
            return
        cur = Path(self.root_dir.get()).expanduser()
        parent = cur.parent if cur.parent != cur else cur
        self.root_dir.set(str(parent))
        self.refresh_dir()

    def _ui_tick(self) -> None:
        if self.backend.playing:
            self.status.set(f"Playing: {self._selected_track.path.name if self._selected_track else 'track'}")
        self.after(250, self._ui_tick)


def main() -> None:
    app = DsndBrowser()
    app.mainloop()


if __name__ == "__main__":
    main()
