# wled-screen-streamer

Snelle native Linux/X11 screen-to-WLED-streamer in C++20. Het programma
captured steeds het nieuwste schermbeeld met X11 MIT-SHM, schaalt direct naar
een kleine RGB-matrix en verstuurt het resultaat als correct gefragmenteerde
WLED DDP-datagrammen via UDP.

Als alternatief kan een RTSP-netwerkcamera native via FFmpeg/libavformat en
libavcodec worden gedecodeerd. Er wordt geen `ffmpeg`-subprocess gestart.

De defaults zijn afgestemd op deze installatie:

- host `wled-matrix2.local`, UDP-poort 4048;
- matrix 64×64, RGB24 (12.288 bytes per frame);
- 60 fps, monitor 0, volledige capture;
- nearest-neighbour resize, supersample-AA factor 2;
- saturation 1,15, contrast 1,10, brightness 1,00 en gamma 1,00.

## Kenmerken

- MIT-SHM-capture; geen screenshots, Python of ffmpeg in het datapad.
- Native RTSP-decode met TCP/UDP-keuze, low-latencyopties en reconnect.
- Vooraf toegewezen capture-, RGB-, header- en socketbuffers.
- Eén Linux `sendmmsg()`-aanroep voor negen DDP-pakketten per frame.
- Nearest en bilinear resize.
- Box/area supersampling en separabele Gaussian anti-aliasing.
- Full, center-square, center-native en expliciete crop.
- Automatische crop-herberekening na XRandR-wijzigingen.
- Geen framewachtrij: achterstallige deadlines worden overgeslagen.
- Testpatronen, DDP-headerdump, benchmarks en nette signaalafhandeling.

## Vereisten en bouwen

Vereist Linux/X11 met MIT-SHM en XRandR, een C++20-compiler, CMake 3.20+,
Ninja, Xlib/Xext/Xrandr-developmentheaders en FFmpeg-developmentlibraries
(`libavformat`, `libavcodec`, `libavutil`, `libswscale`). Op CachyOS/Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake ninja libx11 libxext libxrandr ffmpeg
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

De releasebinary staat in `build/wled-screen-streamer`. De build gebruikt
`-O3 -march=native -mtune=native -flto` en is dus voor de lokale CPU
geoptimaliseerd. Optioneel installeren:

```sh
cmake --install build --prefix ~/.local
```

## Snel starten

```sh
# defaults: 64x64, 60 fps, full, nearest, supersample 2
./build/wled-screen-streamer

# vierkante crop zonder stretching of zwarte balken
./build/wled-screen-streamer --crop-mode center-square

# gecentreerde 128x128 broncrop, echte 2x2 area-downsample
./build/wled-screen-streamer --crop-mode center-native

# 120 fps met statistieken
./build/wled-screen-streamer --crop-mode center-square --fps 120 --benchmark

# zonder FPS-limiet
./build/wled-screen-streamer --fps 0 --benchmark

# native RTSP-camera via TCP
./build/wled-screen-streamer \
  --source rtsp --url rtsp://HOST:8554/reolink \
  --crop-mode center-square --fps 20
```

Ctrl+C sluit netjes af.

## Alle CLI-opties

| Optie | Betekenis | Default |
|---|---|---|
| `--host HOST` | WLED-hostnaam of IP-adres | `wled-matrix2.local` |
| `--source SOURCE` | `screen` of `rtsp` | `screen` |
| `--url URL` | Netwerkvideo-URL, verplicht voor RTSP | niet ingesteld |
| `--rtsp-transport MODE` | RTSP over `tcp` of `udp` | `tcp` |
| `--width N` | Outputbreedte | `64` |
| `--height N` | Outputhoogte | `64` |
| `--fps N` | Doel-FPS; `0` is uncapped | `60` |
| `--monitor N` | Actieve XRandR-monitorindex | `0` |
| `--crop X,Y,W,H` | Expliciete absolute schermcrop | niet ingesteld |
| `--crop-mode MODE` | `full`, `center-square` of `center-native` | `full` |
| `--crop-x N` | Horizontale center-square-offset in de monitor | automatisch |
| `--crop-y N` | Verticale center-square-offset in de monitor | automatisch |
| `--crop-size N` | Zijde van de vierkante crop | automatisch |
| `--filter FILTER` | `nearest` of `bilinear` | `nearest` |
| `--aa MODE` | `off`, `gaussian` of `supersample` | `supersample` |
| `--aa-strength FLOAT` | Gaussian sigma tussen 0,5 en 2,0 | `1.0` |
| `--supersample N` | Supersamplefactor tussen 1 en 8 | `2` |
| `--saturation FLOAT` | Kleurverzadigingsfactor, 0–4 | `1.15` |
| `--contrast FLOAT` | Contrastfactor rond 127,5, 0–4 | `1.10` |
| `--brightness FLOAT` | Helderheidsfactor, 0–4 | `1.00` |
| `--gamma FLOAT` | Streamergamma, 0,1–5 | `1.00` |
| `--no-color-correction` | Schakel de volledige kleurfase uit | uit |
| `--test-pattern PATTERN` | Pixelgenerator zonder screen capture | uit |
| `--ddp-debug` | Dump headers van alleen het eerste frame | uit |
| `--benchmark` | Periodieke datapadtimings | uit |
| `--help` | Ingebouwde hulp | — |

`--crop-x/y/size` vereisen `--crop-mode center-square`. Waarde `-1` betekent
automatisch; kleinere negatieve waarden zijn ongeldig.

## Videobronnen

### X11-scherm

```sh
./build/wled-screen-streamer --source screen
```

`screen` is de default en gebruikt de bestaande X11 MIT-SHM-backend. Alle
monitor-, XRandR- en cropfunctionaliteit blijft beschikbaar.

### RTSP-netwerkcamera

```sh
./build/wled-screen-streamer \
  --source rtsp \
  --url rtsp://HOST:8554/reolink \
  --rtsp-transport tcp \
  --crop-mode center-square \
  --fps 20
```

RTSP wordt in-process geopend met libavformat, gedecodeerd met libavcodec en
naar RGB24 geconverteerd met libswscale. Er wordt nooit een externe
`ffmpeg`-binary gestart. TCP is robuuster bij pakketverlies; UDP kan op een
betrouwbaar lokaal netwerk een lagere latency bieden:

```sh
./build/wled-screen-streamer --source rtsp --url rtsp://CAMERA/stream --rtsp-transport udp
```

De decoder draait in één achtergrondthread. Deze publiceert één gedeelde
`latest frame`-buffer; een nieuw frame vervangt het vorige direct. Er bestaat
geen framequeue in de streamer. De DDP-thread verwerkt uitsluitend een nieuwe
sequence en stuurt hetzelfde cameraframe nooit opnieuw. Daardoor interpoleert
`--fps 60` een camera van 20 fps niet kunstmatig naar 60 fps. `--fps` is voor
RTSP een maximale verzendfrequentie.

Voor minimale latency gebruikt RTSP `nobuffer`, low-delay, nul extra max-delay,
een decoderthread en afbreekbare I/O-timeouts. Als openen, lezen of decoderen
mislukt wordt de sessie gesloten en na één seconde opnieuw opgebouwd. Ctrl+C
onderbreekt ook een vastgelopen netwerk-read.

Crop, resize, AA, kleurcorrectie en DDP zijn na decode dezelfde pipeline als
voor screen capture. Bij een cameraresolutiewijziging wordt de effectieve crop
opnieuw berekend en gemeld. Voorbeeld:

```text
Capture: 1920x1080 (RTSP)
Crop: center-square 1080x1080+420+0
Output: 64x64
```

`--source test` bestaat bewust niet. Gebruik de bestaande `--test-pattern`-
optie; een testpatroon omzeilt zowel screen- als RTSP-capture.

## Capture en crops

### Full

`--crop-mode full` gebruikt de hele geselecteerde monitor. Wanneer bron en
output een andere beeldverhouding hebben, wordt het beeld uitgerekt.

```sh
./build/wled-screen-streamer --monitor 0 --crop-mode full
```

### Center-square

`--crop-mode center-square` neemt vóór de resize het grootste gecentreerde
vierkant. Zo wordt een vierkante matrix zonder stretching en zwarte balken
gevuld. Voor 1920×1080:

```text
size = min(1920, 1080) = 1080
x    = (1920 - 1080) / 2 = 420
y    = (1080 - 1080) / 2 = 0
crop = 1080x1080+420+0
```

Startup toont de effectieve geometrie:

```text
Capture: 1920x1080
Crop: center-square 1080x1080+420+0
Output: 64x64
```

Handmatige overrides zijn relatief aan de geselecteerde monitor. Weggelaten
coördinaten worden met de gekozen grootte automatisch gecentreerd:

```sh
./build/wled-screen-streamer \
  --crop-mode center-square \
  --crop-x 400 --crop-y 0 --crop-size 1080
```

### Center-native

`--crop-mode center-native` neemt uit het midden van de geselecteerde monitor
een gebied op outputresolutie, vermenigvuldigd met de supersamplefactor wanneer
supersample-AA actief is. Met de defaults (64×64, factor 2) resulteert dit in:

```text
Capture: 1920x1080
Crop: center-native 128x128+896+476
Output: 64x64
AA: supersample factor=2
```

De 128×128-crop wordt met een echte 2×2 box average naar 64×64 teruggebracht.
Met `--supersample 3` wordt de crop 192×192 op `+864+444` en gebruikt iedere
outputpixel het gemiddelde van zijn corresponderende 3×3 brongebied.

```sh
./build/wled-screen-streamer --crop-mode center-native
```

Met `--aa off` of `--aa gaussian` is de center-native crop exact 64×64 en is er
geen geometrische schaalstap. Bij afwijkende `--width` en `--height` wordt de
native crop overeenkomstig groot. De effectieve crop inclusief supersampling
moet binnen de geselecteerde monitor passen.
`--crop-x`, `--crop-y` en `--crop-size` horen uitsluitend bij center-square.

### Expliciete crop

`--crop X,Y,W,H` gebruikt absolute X11-schermcoördinaten en heeft voorrang op
monitor- en crop-modeberekeningen:

```sh
./build/wled-screen-streamer --crop 100,50,800,800
```

Iedere crop moet volledig binnen het X11-scherm vallen.

### Wijzigende monitor of resolutie

De streamer luistert naar XRandR screen-, CRTC- en outputevents. Daarna worden
monitor en crop opnieuw berekend. Alleen als de geometrie verandert wordt de
XShm-buffer herbouwd; er wordt niet ieder frame gepolld.

## Resizefilters

- `nearest` is de snelste optie en behoudt harde pixels en lage latency.
- `bilinear` mengt vier bronpixels per outputpixel voor een zachter beeld en
  kost meer rekentijd.

```sh
./build/wled-screen-streamer --filter nearest
./build/wled-screen-streamer --filter bilinear
```

## Anti-aliasing en moiréreductie

AA wordt alleen op live screen capture toegepast. Testpatronen blijven bewust
exact, zodat kleuren en DDP-payloads byte voor byte controleerbaar zijn.

### Off

```sh
./build/wled-screen-streamer --aa off
```

De capture wordt direct met het gekozen resizefilter naar de output gebracht.
Dit is het goedkoopst, maar fijne patronen kunnen op een 64×64-matrix aliasing
of moiré veroorzaken.

### Supersample

```sh
./build/wled-screen-streamer --aa supersample --supersample 2
```

Dit is de default. Eerst wordt in een vooraf toegewezen werkbuffer met `N` keer
de outputbreedte en -hoogte gerenderd. Daarna wordt iedere outputpixel berekend
als het afgeronde gemiddelde van exact zijn `N×N` RGB-bronpixels. Dit is een
echte area/box-downsample; er worden geen pixels overgeslagen en er zijn geen
allocaties per frame.

Bij center-native bepaalt de factor ook de capturecrop:

| Factor | Capturecrop | Box per outputpixel |
|---:|---:|---:|
| 1 | 64×64 | 1×1 |
| 2 | 128×128 | 2×2 |
| 3 | 192×192 | 3×3 |

Factoren 1 tot en met 8 zijn toegestaan. De interne werkafmetingen mogen niet
groter worden dan 4096×4096.

### Gaussian

```sh
./build/wled-screen-streamer --aa gaussian --aa-strength 1.0
```

Gaussian gebruikt een lichte separabele low-pass op de 64×64 RGB-output. De
sterkte is sigma en ligt tussen 0,5 en 2,0. De kernel wordt bij startup tot
drie sigma opgebouwd en genormaliseerd. Horizontale tussenresultaten gebruiken
een vooraf toegewezen floatbuffer; randen worden geklemd. Richtlijnen:

- `0.5`: zeer licht, behoudt veel detail;
- `1.0`: gebalanceerde defaultsterkte;
- `2.0`: duidelijk zachter, sterkere moiréreductie.

`--aa-strength` wordt alleen door Gaussian gebruikt. `--supersample` wordt alleen
door supersample-AA gebruikt.

## Kleurcorrectie

Kleurcorrectie draait uitsluitend op live screen capture, na resize en AA en
direct vóór DDP-packetizing. De standaardinstellingen zijn:

```text
saturation = 1.15
contrast   = 1.10
brightness = 1.00
gamma      = 1.00
```

De bewerkingsvolgorde is vast:

1. contrast rond middenniveau 127,5;
2. saturation ten opzichte van Rec.709-luminantie;
3. brightness;
4. optionele gamma (`pow(channel, 1/gamma)`);
5. clamp naar 0–255.

Contrast wordt bij startup als een 256-entry Q12-tabel berekend. Saturation en
brightness gebruiken vaste-punt-integerberekeningen. Ook de gamma-LUT heeft 256
entries en wordt slechts één keer opgebouwd. Er zijn geen frame-allocaties.

Gamma staat bewust standaard op `1.00`: dat is een exacte identity en voorkomt
dat gamma zowel in de streamer als door WLEDs realtime-gammaverwerking wordt
toegepast. Kies alleen een andere waarde wanneer WLED niet al gamma toepast.

Alle correctie uitschakelen:

```sh
./build/wled-screen-streamer --no-color-correction
```

Startup toont de effectieve toestand:

```text
Color correction: saturation=1.15 contrast=1.10 brightness=1.00 gamma=1.00
```

of:

```text
Color correction: disabled
```

Aanbevolen TV-modus:

```sh
./build/wled-screen-streamer \
  --crop-mode center-square \
  --aa supersample \
  --supersample 2 \
  --fps 60 \
  --saturation 1.15 \
  --contrast 1.10
```

## DDP-wireformat

64×64 RGB24 is exact `64 × 64 × 3 = 12.288` databytes. De packetizer gebruikt
WLEDs fragmentgrootte van 1440 databytes. Elke UDP-datagram bevat een header van
10 bytes, direct gevolgd door uitsluitend de juiste RGB-byte-range. Offset en
length zijn big-endian byteaantallen, geen pixelaantallen.

| Pakket | Flags | Offset | Data | Datagram |
|---:|---:|---:|---:|---:|
| 0 | `0x40` | 0 | 1440 | 1450 |
| 1 | `0x40` | 1440 | 1440 | 1450 |
| 2 | `0x40` | 2880 | 1440 | 1450 |
| 3 | `0x40` | 4320 | 1440 | 1450 |
| 4 | `0x40` | 5760 | 1440 | 1450 |
| 5 | `0x40` | 7200 | 1440 | 1450 |
| 6 | `0x40` | 8640 | 1440 | 1450 |
| 7 | `0x40` | 10080 | 1440 | 1450 |
| 8 | `0x41` | 11520 | 768 | 778 |

Headerindeling:

| Bytes | Inhoud |
|---|---|
| 0 | DDP versie 1 (`0x40`), plus PUSH op alleen pakket 8 (`0x41`) |
| 1 | Sequence in de lage nibble, 1–15 met wraparound |
| 2 | Datatype `1`: RGB24 |
| 3 | Destination `1`: display |
| 4–7 | 32-bit offset, big-endian |
| 8–9 | 16-bit payloadlengte, big-endian |

Het laatste pakket bevat exact 768 payloadbytes, zonder padding of headerbytes
in de RGB-payload. Alle pakketten gaan via één `sendmmsg()` naar de kernel.

### DDP-headerdump

```sh
./build/wled-screen-streamer --test-pattern solid-blue --ddp-debug
```

Dit print bij het eerste frame pakketnummer, flags, sequence, datatype,
destination, offset en length. De stream loopt daarna normaal door.

## Testpatronen

Testpatronen omzeilen capture, resize, AA en kleurcorrectie, zodat DDP,
kleurvolgorde en fysieke pixelmapping onafhankelijk getest kunnen worden. Zelfs
expliciete extreme kleurparameters veranderen een testpatroon niet.

| Naam | Inhoud |
|---|---|
| `rood` / `red` | Alle pixels RGB(255,0,0) |
| `groen` / `green` | Alle pixels RGB(0,255,0) |
| `blauw` / `blue` | Alle pixels RGB(0,0,255) |
| `solid-blue` | Expliciete regressietest: 4096× RGB(0,0,255) |
| `checkerboard` | Zwart-witte blokken van 8×8 pixels |
| `lijn` / `line` | Bewegende witte verticale lijn |

```sh
./build/wled-screen-streamer --test-pattern rood --fps 30
./build/wled-screen-streamer --test-pattern checkerboard
./build/wled-screen-streamer --test-pattern lijn --fps 60
./build/wled-screen-streamer --test-pattern solid-blue --ddp-debug
```

De solid-blue-wiretest is lokaal byte voor byte gereconstrueerd: alle 12.288
bytes waren aanwezig en vormden exact 4096 triplets `(0,0,255)`. De echte matrix
gaf het resultaat egaal blauw weer.

## FPS, latency en benchmark

Bij een positieve `--fps` gebruikt de streamer monotone deadlines. Wanneer de
verwerking achterloopt worden verlopen perioden als `skipped` geteld; oude
beelden worden nooit in een wachtrij gezet. De volgende iteratie captured het
nieuwste beeld. `--fps 0` schakelt pacing uit.

`--benchmark` rapporteert per seconde en bij afsluiten:

- werkelijke FPS en frames;
- overgeslagen deadlines;
- gemiddelde capture-, resize/AA-, kleurcorrectie-, send- en totale frametijd;
- proces-CPU als percentage van één core.

CPU-werk van de afzonderlijke Xorg-server valt buiten de proces-CPU-meting. UDP
bevestigt niet dat WLED ieder uncapped frame werkelijk weergeeft.

### Gemeten resultaten

Intel N100, 1920×1080 X11-bron, 64×64 RGB24, nearest, AA off en lokale WLED
(de oorspronkelijke baseline vóór AA):

| Doel | Werkelijk | Capture | Resize | Send | Totaal | Skipped | CPU |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 30 | 30,21 | 6,26 ms | 0,10 ms | 0,05 ms | 6,42 ms | 0 | 0,59% |
| 60 | 60,16 | 5,11 ms | 0,11 ms | 0,06 ms | 5,28 ms | 0 | 1,16% |
| 75 | 75,14 | 4,85 ms | 0,12 ms | 0,07 ms | 5,05 ms | 0 | 1,58% |
| 90 | 90,21 | 4,61 ms | 0,12 ms | 0,09 ms | 4,82 ms | 0 | 1,99% |
| 120 | 120,10 | 3,06 ms | 0,09 ms | 0,05 ms | 3,20 ms | 0 | 2,18% |
| uncapped | 429,97 | 2,16 ms | 0,12 ms | 0,05 ms | 2,33 ms | 0 | 7,39% |

Een 30-fps center-square-test verlaagde capture van circa 6,57 ms voor
1920×1080 full naar 4,84 ms voor 1080×1080. Resultaten variëren met compositor,
desktopactiviteit, CPU-frequentie en netwerkbelasting.

Center-native AA is daarna opnieuw op 60 fps getest:

| AA-modus | Crop | Werkelijk | Capture | Resize + AA | Send | Totaal | Skipped |
|---|---:|---:|---:|---:|---:|---:|---:|
| supersample 2 | 128×128 | 60,33 | 4,33 ms | 0,27 ms | 0,06 ms | 4,66 ms | 0 |
| supersample 3 | 192×192 | 60,33 | 3,92 ms | 0,70 ms | 0,08 ms | 4,71 ms | 0 |
| Gaussian 0,5 | 64×64 | 60,33 | 4,97 ms | 0,35 ms | 0,07 ms | 5,40 ms | 0 |
| Gaussian 2,0 | 64×64 | 60,37 | 3,80 ms | 0,63 ms | 0,05 ms | 4,48 ms | 0 |
| off | 64×64 | 60,38 | 3,57 ms | 0,06 ms | 0,06 ms | 3,69 ms | 0 |

Alle gevraagde AA-modi behielden daarmee probleemloos 60 fps zonder skips.

De finale A/B-test van center-native supersample 2 met dezelfde 60-fps-pipeline:

| Kleurcorrectie | Werkelijk | Capture | Resize/AA | Color | Send | Totaal | Skipped | CPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| defaults aan | 60,13 | 5,45 ms | 0,30 ms | 0,08 ms | 0,07 ms | 5,90 ms | 0 | 2,63% |
| volledig uit | 60,16 | 5,29 ms | 0,29 ms | 0,00 ms | 0,06 ms | 5,63 ms | 0 | 2,21% |

De kleurfase zelf kostte gemiddeld 0,08 ms per frame. De variatie in totale tijd
komt grotendeels uit de X11-capture; beide runs hielden 60 fps zonder skips.

## Gevalideerde installatie

- `wled-matrix2.local` resolveerde correct via mDNS;
- ping: 0% pakketverlies;
- WLED 16.0.1 op ESP32-S3/Adafruit MatrixPortal;
- 64×64, 4096 RGB-pixels;
- WLED-status tijdens streaming: `live=true`, `lm=DDP`;
- solid-blue fysiek egaal blauw.

De RTSP-uitbreiding is gebouwd tegen libavformat 63.1.100, libavcodec 63.1.100,
libavutil 61.1.100 en libswscale 10.1.100. Een lokale H.264-bron van 320×180 op
20 fps werd native gedecodeerd; center-square werd correct herberekend als
`180x180+70+0` en door de bestaande AA-, kleur- en DDP-pipeline gestuurd. Zowel
TCP als UDP zijn geopend via de gekozen transportoptie. Een verbroken bron en
een geweigerde verbinding activeerden de reconnectlus. Na toevoeging van RTSP
behaalde de ongewijzigde X11-backend bij een regressietest 60,24 fps, nul skips
en 4,32 ms totale frametijd.

DNS/mDNS-resolutie gebeurt één keer bij startup, nooit in de frame-loop. Een
numeriek adres kan sneller starten:

```sh
./build/wled-screen-streamer --host WLED_IP
```

## systemd user-service

De unit gebruikt `DISPLAY=:0`, 60 fps en nearest. Installeren:

```sh
mkdir -p ~/.config/systemd/user
cp systemd/wled-screen-streamer.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now wled-screen-streamer.service
```

Beheer:

```sh
systemctl --user status wled-screen-streamer.service
journalctl --user -u wled-screen-streamer.service -f
systemctl --user disable --now wled-screen-streamer.service
```

De unit verwacht de binary onder
`~/Development/wled-screen-streamer/build/wled-screen-streamer`. Pas `ExecStart`
aan voor een ander pad of opties, bijvoorbeeld:

```ini
ExecStart=%h/Development/wled-screen-streamer/build/wled-screen-streamer --fps 120 --filter nearest --crop-mode center-square
```

Importeer zo nodig de X11-omgeving vanuit de actieve grafische sessie:

```sh
systemctl --user import-environment DISPLAY XAUTHORITY
systemctl --user restart wled-screen-streamer.service
```

## Troubleshooting

### X11-display opent niet

```sh
echo "$DISPLAY"
echo "$XAUTHORITY"
xrandr --listmonitors
```

De service gebruikt `DISPLAY=:0`; pas dit aan als de sessie elders draait.

### MIT-SHM ontbreekt

```sh
xdpyinfo | grep MIT-SHM
```

Er is bewust geen trage screenshotfallback.

### Host resolveert niet

```sh
getent ahostsv4 wled-matrix2.local
ping -c 3 wled-matrix2.local
```

Controleer Avahi/mDNS of gebruik `--host` met het IP-adres.

### WLED ontvangt niets

Controleer host, firewall en UDP-poort 4048. WLED toont een ontvangen stream in
`/json/info` als `live=true` en `lm=DDP`:

```sh
curl -s http://wled-matrix2.local/json/info
./build/wled-screen-streamer --test-pattern solid-blue --ddp-debug
```

### Verkeerde kleuren, banden of zwarte pixels

Controleer met `solid-blue` de headerwaarden tegen de DDP-tabel. Controleer ook
WLEDs matrixafmetingen, RGB-volgorde en fysieke 2D-mapping.

### Crop buiten het scherm

`--crop-x/y` zijn monitor-relatief, maar de effectieve crop moet volledig binnen
het totale X11-scherm liggen. Verlaag offset of grootte, of laat waarden weg om
automatisch te centreren.

## Projectstructuur

```text
CMakeLists.txt                         buildconfiguratie
src/main.cpp                          streamerimplementatie
systemd/wled-screen-streamer.service  systemd user-unit
README.md                              documentatie
build/wled-screen-streamer             lokale releasebinary
```
