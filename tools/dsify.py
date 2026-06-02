#!/usr/bin/env python3
# dsify.py -- convert MP3/WAV/OGG to .dsnd
# Requires: pydub, numpy
# Usage: python dsify.py input.mp3 output.dsnd [--rate 0|1|2|3] [--loop] [--16bit]

import sys, struct, numpy as np, argparse
from pydub import AudioSegment

RATES = [32768, 16384, 8192, 5512]
DSND_MAGIC = 0x444E5344

def dsify(src, dst, rateDiv=1, loop=False, bit16=False):
    audio = AudioSegment.from_file(src).set_channels(1)
    audio = audio.set_frame_rate(RATES[rateDiv])
    samples = np.array(audio.get_array_of_samples())
    if bit16:
        pcm = samples.astype(np.int16).tobytes()
        flags = 0x04
    else:
        # PCM8: scale -32768..32767 to 0..255 (unsigned for NDS)
        # NDS PCM8 is signed? Actually libnds says:
        # "8-bit signed PCM (values -128 to 127)"
        # Wait, the prompt says ((samples >> 8) + 128).astype(np.uint8).tobytes()
        # which is 0..255.
        pcm = ((samples >> 8) + 128).astype(np.uint8).tobytes()
        flags = 0x00
    if loop: flags |= 0x02
    hdr = struct.pack("<IBBHI", DSND_MAGIC, rateDiv, flags, 0, len(pcm))
    with open(dst, "wb") as f: f.write(hdr + pcm)
    print(f"Written {len(pcm)} bytes @ {RATES[rateDiv]}Hz flags={flags:#x}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Convert audio to .dsnd")
    parser.add_argument("input", help="Input audio file")
    parser.add_argument("output", help="Output .dsnd file")
    parser.add_argument("--rate", type=int, choices=[0,1,2,3], default=1, help="Rate divisor (0=32kHz, 1=16kHz, 2=8kHz, 3=5kHz)")
    parser.add_argument("--loop", action="store_true", help="Set loop flag")
    parser.add_argument("--16bit", action="store_true", dest="bit16", help="Use 16-bit PCM")
    
    args = parser.parse_args()
    dsify(args.input, args.output, args.rate, args.loop, args.bit16)
