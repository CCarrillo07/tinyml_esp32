# TinyML Speech Command Recognition on ESP32

This project implements an end-to-end TinyML pipeline for real-time speech command recognition using an ESP32 + INMP441 microphone, powered by a 2D CNN trained on MFCC features.

---

## Overview

The system:
- Captures audio from a microphone  
- Extracts MFCC features in real time  
- Performs on-device inference using a TensorFlow Lite model  

### Supported Commands

- yes  
- no  
- on  
- off  
- up  
- down  
- left  
- right  
- stop  
- go  

---

## Training

The model was trained using the **Google Speech Commands dataset (v0.02)** with a subset of 10 commands.

### Key Design Decision

The MFCC pipeline is identical in Python and ESP32, ensuring:
- Matching training/inference distributions  
- Consistent on-device predictions  


### Audio Processing (MFCC)

#### *Parameters*

| Parameter   | Value     |
|------------|----------|
| Sample Rate | 16000 Hz |
| Frame Size  | 512      |
| Hop Length  | 160      |
| MFCC Coeffs | 13       |
| Mel Filters | 26       |
| Max Frames  | 100      |

#### *Pipeline*

- Hann window  
- FFT → Power Spectrum  
- Mel Filterbank  
- Log10 scaling  
- DCT → MFCC  

#### *Notes:*
- Audio is normalized  
- Converted into fixed-length MFCC sequences (padding/truncation)  

### Model Details

- Architecture: 2D CNN  
- Input Shape: (100, 13, 1)  
- Framework: TensorFlow / Keras  
- Output: 10-class softmax  

```python
model = tf.keras.Sequential([
    tf.keras.layers.Input(shape=(MAX_FRAMES, MFCC_COUNT, 1)),
    tf.keras.layers.Conv2D(8, (3,3), activation="relu"),
    tf.keras.layers.MaxPool2D((2,2)),
    tf.keras.layers.Conv2D(16, (3,3), activation="relu"),
    tf.keras.layers.MaxPool2D((2,2)),
    tf.keras.layers.Flatten(),
    tf.keras.layers.Dense(32, activation="relu"),
    tf.keras.layers.Dense(NUM_CLASSES, activation="softmax")
])
```
### Model Conversion

- Converted to **TensorFlow Lite (float)**
- No quantization applied

---

## ESP32 Implementation

- **Microcontroller:** ESP32  
- **Microphone:** INMP441 (I2S)  
- **Inference:** TensorFlow Lite Micro (`speech_commands_mfcc.tflite` → `model.h` → copied into `model.cc`)  

### On-Device Pipeline

1. Capture audio via I2S  
2. Extract MFCC features  
3. Run CNN inference  

### Configurable Modes (`app_config.h`)

- **Inference + Display** → Output on GC9A01  
- **Inference + WiFi** → Send audio to local server  

> Due to resource constraints, display and WiFi are not used simultaneously.
>
> ---

## Tools

This project includes auxiliary tools used for model conversion, debugging, and data validation.

### Files Overview

- **`speech_commands_mfcc.tflite`**  
  TensorFlow Lite model generated from the trained Keras model.

- **`print_ops.py`**  
  Utility script to inspect TensorFlow Lite model operations and verify compatibility with TensorFlow Lite Micro.

- **`model.h`**  
  C array representation of the `.tflite` model, generated using:

  ```bash
  xxd -i speech_commands_mfcc.tflite > model.h
  ```

- **`server.py`**  
  Local Flask-based server used to receive audio data from the ESP32.  
  Useful for debugging and validating captured audio.
