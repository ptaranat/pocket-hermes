sources := "app/main.c app/voice.c app/relay.c app/hub.c kit/pk.c"
flags := "-std=gnu11 -O2 -Wall -Wextra -Ikit -Ithird_party"

default: desktop

desktop:
    mkdir -p build
    cc {{flags}} -Wno-unused-parameter $(sdl2-config --cflags) {{sources}} -o build/pocket-hermes $(sdl2-config --libs) -lm -lpthread

shots:
    just desktop
    mkdir -p build/shots
    for s in talk listening working reply chats chat inbox approval agent look; do build/pocket-hermes --shot $s build/shots/$s.png; done

run *args:
    just desktop
    POCKET_HERMES_CONFIG=desktop.config build/pocket-hermes {{args}}

zig := "uvx --from ziglang python -m ziglang"
target := "aarch64-linux-gnu.2.33"

arm:
    mkdir -p build/arm
    for f in app/main app/voice app/relay app/hub kit/pk; do {{zig}} cc -target {{target}} {{flags}} -O2 -w $(sdl2-config --cflags) -c $f.c -o build/arm/$(basename $f).o; done
    nm -u build/arm/main.o | awk '/SDL_/ {print $NF}' | sort -u | sed 's/.*/void &(void) {}/' > build/arm/sdl_stub.c
    {{zig}} cc -target {{target}} -shared -Wl,-soname,libSDL2-2.0.so.0 build/arm/sdl_stub.c -o build/arm/libSDL2-2.0.so.0
    {{zig}} cc -target {{target}} build/arm/main.o build/arm/voice.o build/arm/relay.o build/arm/hub.o build/arm/pk.o build/arm/libSDL2-2.0.so.0 -lm -lpthread -o build/arm/pocket-hermes

voice-smoke wav seconds="6":
    mkdir -p build/data
    cc {{flags}} -Iapp test/voice_smoke.c app/voice.c app/relay.c -o build/voice_smoke -lm -lpthread
    build/voice_smoke desktop.config {{wav}} {{seconds}}

deploy host="pocket-brick":
    just arm
    ssh root@{{host}} 'cat > /mnt/UDISK/pocket-hermes/pocket-hermes.new' < build/arm/pocket-hermes
    ssh root@{{host}} 'cd /mnt/UDISK/pocket-hermes && chmod 755 pocket-hermes.new && mv -f pocket-hermes.new pocket-hermes'
