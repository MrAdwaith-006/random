# Audio routing boundaries

```
Playback:   selected source endpoint (WASAPI loopback) -> DSP -> selected physical output
Microphone: physical mic or Voicemod virtual microphone -> DSP -> virtual microphone endpoint
```

The playback foundation now uses a WASAPI loopback capture session, a bounded real-time FIFO, protected DSP, and a WASAPI render session. The capture and render selections must never use the same endpoint in a loopback path, preventing feedback. The current milestone requires both devices to provide compatible 32-bit floating-point formats; format conversion is the next audio-engine addition.

The processor will expand from the current protected preamp into high-pass filtering, EQ, compressor, look-ahead limiter, and output metering. Digital processing can prevent clipping; it cannot exceed the physical limits of the selected headset or speakers.
