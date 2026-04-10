from flask import Flask, request, jsonify
from datetime import datetime
from pathlib import Path
import wave

"""
=========================================================
ESP32 Audio Upload Server (Flask)
=========================================================

Description:
This script implements a lightweight Flask server that receives
raw audio data (PCM) from an ESP32 over HTTP and saves it as
a .wav file on the host machine.

Purpose:
- Capture audio recorded on the ESP32 for debugging and validation
- Verify what audio is actually being used for TinyML inference
- Store audio samples for offline analysis (e.g., playback, MFCC comparison)

How it works:
- ESP32 sends raw PCM audio via HTTP POST to /upload
- Metadata (sample rate, channels, etc.) is sent in headers
- Server reconstructs and saves the audio as a standard WAV file

Output:
- Saved .wav files in the "recordings/" directory
- JSON response with metadata and validation info
"""

# =========================
# Flask app initialization
# =========================
app = Flask(__name__)

# Directory where recorded audio files will be stored
OUTPUT_DIR = Path("recordings")
OUTPUT_DIR.mkdir(exist_ok=True)  # Create folder if it doesn't exist


# =========================
# Audio upload endpoint
# =========================
@app.route("/upload", methods=["POST"])
def upload():
    # Raw binary audio data sent from ESP32 (no encoding, just PCM)
    raw_audio = request.data

    # Validate that audio data was received
    if not raw_audio:
        return jsonify({"ok": False, "error": "No audio data received"}), 400

    # =========================
    # Read metadata from headers
    # =========================
    # These headers should be sent by the ESP32
    sample_rate = int(request.headers.get("X-Sample-Rate", "16000"))
    bits_per_sample = int(request.headers.get("X-Bits-Per-Sample", "16"))
    channels = int(request.headers.get("X-Channels", "1"))
    sample_count = int(request.headers.get("X-Sample-Count", "0"))

    # Only support 16-bit PCM audio (standard for most embedded audio pipelines)
    if bits_per_sample != 16:
        return jsonify({"ok": False, "error": "Only 16-bit PCM is supported"}), 400

    # =========================
    # Generate unique filename
    # =========================
    # Format: esp32_capture_YYYYMMDD_HHMMSS_microseconds.wav
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    wav_path = OUTPUT_DIR / f"esp32_capture_{timestamp}.wav"

    # =========================
    # Save raw audio as WAV file
    # =========================
    with wave.open(str(wav_path), "wb") as wf:
        wf.setnchannels(channels)                  # Mono (1) or Stereo (2)
        wf.setsampwidth(bits_per_sample // 8)      # Convert bits → bytes (16 bits = 2 bytes)
        wf.setframerate(sample_rate)               # Sample rate (e.g., 16000 Hz)
        wf.writeframes(raw_audio)                  # Write raw PCM data

    # =========================
    # Validate sample count
    # =========================
    # Calculate actual number of samples received
    actual_samples = len(raw_audio) // (bits_per_sample // 8)

    # =========================
    # JSON response
    # =========================
    return jsonify({
        "ok": True,
        "file": str(wav_path),                 # Saved file path
        "bytes_received": len(raw_audio),      # Total bytes received
        "sample_rate": sample_rate,
        "channels": channels,
        "bits_per_sample": bits_per_sample,
        "sample_count_header": sample_count,   # Expected (from ESP32)
        "sample_count_actual": actual_samples  # Calculated (server-side)
    }), 200


# =========================
# Run Flask server
# =========================
if __name__ == "__main__":
    # Accessible from local network (important for ESP32 → PC communication)
    app.run(host="0.0.0.0", port=5000, debug=True)