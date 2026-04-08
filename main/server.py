from flask import Flask, request, jsonify
from datetime import datetime
from pathlib import Path
import wave

app = Flask(__name__)

OUTPUT_DIR = Path("recordings")
OUTPUT_DIR.mkdir(exist_ok=True)

@app.route("/upload", methods=["POST"])
def upload():
    raw_audio = request.data

    if not raw_audio:
        return jsonify({"ok": False, "error": "No audio data received"}), 400

    sample_rate = int(request.headers.get("X-Sample-Rate", "16000"))
    bits_per_sample = int(request.headers.get("X-Bits-Per-Sample", "16"))
    channels = int(request.headers.get("X-Channels", "1"))
    sample_count = int(request.headers.get("X-Sample-Count", "0"))

    if bits_per_sample != 16:
        return jsonify({"ok": False, "error": "Only 16-bit PCM is supported"}), 400

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    wav_path = OUTPUT_DIR / f"esp32_capture_{timestamp}.wav"

    with wave.open(str(wav_path), "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(bits_per_sample // 8)
        wf.setframerate(sample_rate)
        wf.writeframes(raw_audio)

    actual_samples = len(raw_audio) // (bits_per_sample // 8)

    return jsonify({
        "ok": True,
        "file": str(wav_path),
        "bytes_received": len(raw_audio),
        "sample_rate": sample_rate,
        "channels": channels,
        "bits_per_sample": bits_per_sample,
        "sample_count_header": sample_count,
        "sample_count_actual": actual_samples
    }), 200

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)