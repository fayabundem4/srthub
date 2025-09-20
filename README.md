# srthub

srthub is a simple open source SRT hub for relaying MPEG-TS streams to multiple clients. I created this program because I couldn't find any other open source, minimal SRT hubs that were easy to use and understand.

## Features
- Minimal and straightforward SRT hub implementation
- Relays MPEG-TS packets from a single source to multiple clients
- Easy to build and run

## Usage

```
./srthub <source_port> <client_port>
```

- `source_port`: Port to listen for the incoming SRT stream (from ffmpeg or other source)
- `client_port`: Port to accept client connections (e.g., mpv, ffplay)

### Example

1. Start the hub:
   ```bash
   ./srthub 10000 9000
   ```

2. Send a stream to the hub using ffmpeg:
   ```bash
   ffmpeg -re -stream_loop -1 -i INPUTVIDEO.mp4 -c:v libx265 -preset veryfast -b:v 3000k -bufsize 6000k -maxrate 3000k -c:a aac -b:a 128k -f mpegts "srt://127.0.0.1:10000?mode=caller&latency=1000"
   ```

3. Connect a client (e.g., mpv) to receive the stream:
   ```bash
   mpv "srt://127.0.0.1:9000?mode=caller&latency=1000"
   ```

You can connect multiple clients to the client port. Each will receive the relayed stream.

## Building

Make sure you have the SRT library installed:

Ubuntu/Debian: `sudo apt install libsrt-dev`

Fedora: `sudo dnf install srt-devel`

Then run:

```
make
```

## License

BSD-3-Clause, See [LICENSE](LICENSE) file
