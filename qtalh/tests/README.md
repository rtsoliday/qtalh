# Audio fixture

`alarm.ogg` is a generated 0.2-second, 880 Hz sine wave encoded as Ogg Vorbis.
The UI test decodes and plays it twice with output muted, on both Qt 5 and Qt 6.
It does not require a system sound theme. The FFmpeg command-line tool is only
needed to regenerate the fixture.
On macOS the offscreen UI suite runs the audio case in a Cocoa child process,
which supplies the native event loop needed by the media backends.

To regenerate it with FFmpeg and its libvorbis encoder:

```sh
ffmpeg -f lavfi -i sine=frequency=880:duration=0.2:sample_rate=44100 \
  -c:a libvorbis -q:a 2 alarm.ogg
```
