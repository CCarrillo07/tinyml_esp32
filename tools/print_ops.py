import tensorflow as tf
from pathlib import Path

"""
=========================================================
TFLite Model Operator Inspection Script
=========================================================

Description:
This script loads a TensorFlow Lite (.tflite) model and
extracts the list of operators (ops) used inside it.

Purpose:
- Identify which operations are required by the model
- Verify compatibility with TensorFlow Lite Micro (e.g., ESP32)
- Help debug deployment issues in TinyML projects

Usage:
Place the .tflite model file in the same directory as this script,
then run the script to print all unique operators used.

Output:
A sorted list of operator names (e.g., CONV_2D, FULLY_CONNECTED)
"""

# =========================
# Model path (same directory as script)
# =========================
# __file__ → current script location
# .parent → directory containing this script
MODEL_PATH = Path(__file__).parent / "speech_commands_mfcc.tflite"

# Validate that the model file exists
if not MODEL_PATH.exists():
    raise FileNotFoundError(f"Model file not found: {MODEL_PATH}")

# =========================
# Load TFLite model into memory
# =========================
with open(MODEL_PATH, "rb") as f:
    model_buf = f.read()

# =========================
# Initialize TFLite interpreter
# =========================
interpreter = tf.lite.Interpreter(model_content=model_buf)

# Allocate memory for tensors (required before inspection/inference)
interpreter.allocate_tensors()

# =========================
# Extract operators used in the model
# =========================
# NOTE: _get_ops_details() is a private API, but commonly used for debugging
ops_set = set()

for op in interpreter._get_ops_details():
    ops_set.add(op["op_name"])

# =========================
# Print results
# =========================
print("Ops used by the model:")

for op_name in sorted(ops_set):
    print(op_name)