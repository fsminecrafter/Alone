# Alone

An isometric 3D survival game set in **Alingsås, Sweden**, inspired by
*Project Zomboid*. Everyone is gone. Hunt, eat, search, loot, fight, survive -
and listen to your body, because it will tell you *how* you feel but never
*why*.

![Alingsås at noon](docs/screenshots/game.jpg)

| | |
|---|---|
| ![Vimpeln, the shopping centre by Gerdsken](docs/screenshots/vimpeln.jpg) | ![The food court inside Vimpeln](docs/screenshots/vimpeln_inside.jpg) |
| ![Kungsgatan and Gallerian Storken](docs/screenshots/kungsgatan.jpg) | ![Hjortmarka: forest from an orienteering map](docs/screenshots/hjortmarka.jpg) |
| ![Houses on a hillside: the ground has its real heights](docs/screenshots/hills.jpg) | ![A downpour on the square: streaks, splashes and puddles](docs/screenshots/rain.jpg) |
| ![A camp: tent, storm kitchen and a plank cabin](docs/screenshots/camp.jpg) | ![Treating wounds, one body part at a time](docs/screenshots/health.jpg) |
| ![Symptoms with a tooltip](docs/screenshots/symptoms.jpg) | ![Map of Alingsås](docs/screenshots/map.jpg) |
| ![Alingsås station at night](docs/screenshots/station_night.jpg) | ![You only know the time once you find a watch](docs/screenshots/watch.jpg) |
| ![Inventory and a searched container](docs/screenshots/inventory.jpg) | ![Cause of death is revealed only when you die](docs/screenshots/death.jpg) |
| ![Meeting Elin, a nurse a few streets away](docs/screenshots/people.jpg) | ![Shouting across the yard at Jonas in the police station](docs/screenshots/parley.jpg) |
| ![The journal: who you know of and what they need](docs/screenshots/journal.jpg) | ![Trading: friends give better deals](docs/screenshots/trade.jpg) |
| ![On a bike in the Vimpeln car park](docs/screenshots/vimpeln_carpark.jpg) | ![A fuel station: fill a gas can at the pumps](docs/screenshots/fuel_station.jpg) |
| ![The build menu](docs/screenshots/build.jpg) | ![Aiming with Ctrl: a reticle on the ground](docs/screenshots/aim.jpg) |

**The player** is one smooth body mesh skinned to a bone rig, with procedural
animations that react to how you are: a hurt leg makes you limp, a hurt arm is
held close, pain bends you over, cold makes you hug yourself and shiver, nausea
puts a hand on your stomach, and when you're out of breath you rest your hands
on your knees. You carry what you hold at the ready, pedal a bike, and sit in a
car seat. The body and hair are your own: a man's or a woman's build, and short,
long, ponytail, bun or shaved hair, in the clothes and colours you picked
([MP4](docs/media/character_animations.mp4)):

![Character animations](docs/media/character_animations.gif)

> Early prototype (v0.3). The game was originally an NDS project; it has been
> rewritten from scratch in C++17 for PC (Linux and Windows), with a browser version on the side.

## What's in it

**A real place.** Alingsås is built from OpenStreetMap data: more than 5,500
real buildings with their real size, use and (where mapped) colour, every named
street, footpath and forest track, the railway and the station, parks, car
parks, squares, piers, single trees and street lamps. Lake Mjörn, Gerdsken and
Säveån winding through Nolhaga; Christinae kyrka; the hospital; the E20.

* **Vimpeln**, the big shopping centre by Gerdsken, has its glass entrance
  hall with the orange sign, a car park full of abandoned cars, and inside:
  a supermarket, electronics, home goods, a pharmacy, a tool and hardware
  store, clothes and shoes, sport and outdoor gear (tents, storm kitchens),
  and a food court where you can find leftover burgers, cold fries and kebab.
* **Gallerian Storken** is the small gallery on Kungsgatan, with its café,
  clothes and books. The old town around it is wooden houses in pastel
  colours with tile roofs, shop windows and striped awnings.
* The waterfront flats at Gerdska Strand have their balconies.
* **Inside, like Project Zomboid:** the building you're in loses its roof and
  upper floors, and the walls between you and the camera are cut down to a low
  stub so you can see the rooms - the walls behind you stay up. A building that
  hides you from outside is cut open along your line of sight. Anything else in
  the way shows a faint outline of you.
* **Homes have real floor plans:** the front door opens into a hall that leads
  to a corridor; the living room and kitchen are at the front, bedrooms and the
  bathroom open off the corridor. Small cottages have a kitchen-living room with
  the bedroom and bathroom off it.
* **Forests from orienteering maps.** Around Hjortmarka and Kvarnsjön the
  forest comes from local orienteering maps: open forest, dense young
  forest, near-impassable thickets, bogs and the small paths orienteers use.
  Thickets and bogs are slow going. Elsewhere forests are as thick as the
  real ones around town.

**Hills.** The ground has its real heights, from the Copernicus elevation
model: the valley Säveån runs through, Mjörn at 57 m above sea level, the town
around 65-70 m, Hjortmarka's ridges over 100 m. Buildings stand level on
slopes with a foundation on the downhill side. Walking uphill is slow and
leaves you out of breath (and burns more), steep downhills are slow too - and
running down a steep slope, especially when it's wet, dark or you're tired,
can make you trip, fall and scrape your hands or knees. The map shows the hills
with shading and 5 m contour lines.

One tile is one metre; the map covers about 9.4 x 5.3 km. Interiors, furniture
and loot are generated deterministically on top of the real footprints, and
areas outside the mapped part are filled in procedurally.

**Light and weather.** Sun and moon cast real shadows; at night street lamps,
lit windows and flickering TVs make warm pools of light (while the power lasts -
it goes out within the first two weeks). A flashlight cone, rain in streaks
that slant with the wind and splash on the ground, puddles that fill up and
mirror the sky (never indoors), and snow cover in winter.

**Runs on modest hardware.** Only what the camera (or the sun) can see is drawn,
nearest first; big buildings and forests are cut into cells so hidden parts are
skipped; the sun's shadow map is cached and only redrawn, a little per frame,
when you've walked a fair way or the sun has moved. Settings > Graphics has the
render resolution (Auto lowers it while the frame rate is low and raises it when
there's room), shadows and lights; the browser version starts with lighter
settings. On laptops with an NVIDIA card the game asks to run on it. The mouse
pointer is the system's own, so it stays smooth even when the game isn't, and
short clicks between frames aren't lost.

**Know only what you can know.** There's no clock on screen until you find a
wristwatch or an alarm clock - before that you only see roughly what part of
the day it is from the sun. The temperature is "Chilly" or "Freezing" until you
carry a thermometer.

**A body that works like a body.** The survival model tracks real quantities:
stomach contents, blood sugar, liver and muscle glycogen, body fat, lean mass,
free amino acids, the liver's urea capacity, ammonia, ketosis adaptation,
vitamin C stores, hydration, blood volume, core temperature (with clothing
insulation, wetness, wind, shivering and sweating), sleep pressure, wounds,
infections and toxins. Some consequences:

* **Rabbit starvation is real.** Hare and roe deer are very lean. Live on them
  alone and you exceed what your liver can process: you stay hungry however
  much you eat, then come nausea, diarrhoea, weakness - and death within a
  week or two. Add fat (butter, lard, boar back fat, deer tallow) and you're
  fine.
* Lean people burn muscle sooner when starving; fat can only be released so fast.
* No vitamin C for two months and your gums start to bleed (scurvy). Rose hips,
  lingonberries, raw liver, potatoes and tablets help.
* Untreated lake water can give you Giardia days later. Raw hare can carry
  tularemia ("harpest"), undercooked wild boar trichinosis, spoiled food food
  poisoning.
* Some white mushrooms are field mushrooms. Some are death caps: you feel sick
  the next day, then better... then your liver fails. A mushroom field guide
  in your pack tells them apart.
* Wet clothes in the wind kill. So does an untreated deep wound.

**Symptoms, not diagnoses.** The HUD shows how you feel - *Feeling queasy*,
*Queasy*, *Nauseous*, *Retching*; *Chilly*, *Cold*, *Freezing*, *Numb with cold*;
*Craving*, *Bleeding gums*, *Yellowish skin*... Hover over a symptom to read
what it feels like. The cause is only revealed after death. All symptom text
lives in `data/text/symptoms.ini`.

**Wounds, one body part at a time.** The health screen (H) shows your body;
hover over a part to look at it, right-click to treat it: disinfect it, put on
a bandaid (only good for small cuts), a bandage or a rag, or take a dressing
off. Dressings soak up blood and what the wound weeps, and get dirty from
where you walk and what you do - fast on mud and forest floor, slowly indoors
on a wooden floor, faster in the rain. A clean dressing protects a wound; a
filthy one infects it. Rip clothes, towels and sheets into rags (jeans and
jackets need a knife or scissors), wash dirty rags and bandages in water.
Bandaids, bandages and disinfectant are in bathrooms, pharmacies and
hospitals; their dirty versions turn up in rubbish bins. Blood drips from
wounds nothing covers, and the rain washes it away.

**Camping and carpentry.** Pitch a tent (a 1x2 bivvy or a 2x2 dome) to get out
of the wind and rain and sleep warmer; a sleeping bag and a mat help too. Cook
on a **storm kitchen**, a spirit stove that burns red alcohol - and never top
it up while it's burning, or the bottle goes up in your hand. Chop a tree for a
log, saw it into planks, find nails, and build (B): walls, doors, windows,
floors, roofs, storage crates, rain collectors, beds and fences. A carpentry
book teaches you doors, windows and roofs. Fish from the shore with a rod.

**Other people.** Nine survivors live at real places in town, and they don't
all want the same thing. Elin, a nurse a few streets from where you wake up,
is glad to see you; Gunnar the old radio amateur in Nolby and Britt-Marie the
hunter up by Enehagen keep you at arm's length until you've earned it; Jonas
has barricaded himself into the police station and shoots anyone who comes
close; and two raiders work the shops around Vimpeln while a loner with a bow
hunts the Noltorp woods. Talk to people (right-click or F), ask what they
need, trade (they value food, medicine, ammo and petrol), and ask about
others: people pass on who they've heard of and roughly where, and warn you
when they've seen raiders nearby - those rumours go on your map as a ring
with a question mark. Those who trust you may come with you - companions follow you,
fight what attacks you and ride along in your car. Help someone and they
teach you things (hotwiring, mushrooms, carpentry). The journal (J) keeps
track of who you know and what they need, the map marks them, and the goal
under the clock says what to do next. People are defined in
`data/survivors/survivors.ini`.

**Making your character.** A new game starts in the house you wake up in,
with you standing in it: pick a name, man, woman or other, skin, hair colour
and hairstyle (short, long, ponytail, bun, shaved), and what you're wearing -
shirt, sweater, jacket, trousers, shoes and a hat, each in its own colour.
People call you by your name. The last character you made is remembered.

**Days and nights.** People live in their houses: they walk from room to
room (opening and closing the doors behind them), mutter to themselves, go
to bed at their own bedtime and get up in the morning. Sneak in at night and
you'll find them asleep; walk past the bed and they may wake up - and they
have something to say about being woken. Companions doze off next to you
while you sleep.

**Friendship.** Everyone you meet is somewhere on a ladder: *Enemy, Stranger,
Acquaintance, Friend, Best friend, Family* or *Lover*. Talk every day, ask
about their life (each person has a story, told a piece at a time as they
trust you), tell jokes, bring gifts and help them. Friends give better
deals and open up. The last steps are two different roads, and you have to
pick one: look out for each other ("whatever happens, I've got your back")
and someone becomes *Family*; flirt with them and an adult may become your
*Lover*. Each pulls the other down - flirt with someone you've made family and
it gets awkward, and a lover who hears "you're like family to me" is hurt. The
talk window shows how far along each road you are. Family and lovers
come with you even if they'd usually stay home, and keep things aside for
you. Hurt someone and it's over. Hundreds of lines of dialogue, from small
talk about the weather to what they say when you turn up bleeding, live in
`data/text/dialogue.ini` and each person's own lines in `survivors.ini`.

**Talking your way out.** You can call out to people who won't let you near.
Jonas in the police station will hear you out from across the yard: tell him
you're not here to fight, strike a deal (he names the price), bring what he
needs, and he may let you in. Raiders give you a few seconds when you start
talking: hand over enough of your valuables and the whole gang leaves you alone
for a day, or face them down - with a loaded gun in your hands they might back
off; with your fists, probably not.

**Bikes and cars.** Bikes stand by the odd front door (and outside the bike
shops): get on and ride, quietly and without fuel. Cars are harder: one in
five doors is unlocked (or has the key left in it), and only one in five of
those has the key in the ignition - otherwise sit in it out of the rain,
smash a window, or hotwire it once someone has shown you how. You drive like
a car: W is the throttle, S brakes and then reverses, A/D turn the wheel
(which only turns you while you're rolling). Every car has
0-40% of a tank. A gas can holds any liquid but is meant for petrol: fill it
at the pumps of the real fuel stations (while there's power; afterwards a
hose down the filler pipe works), or siphon any car with a hose. Crash and
you'll get hurt; run things down and they get hurt.

**Water:** when you're thirsty you take a sip from an open bottle of clean
water by yourself (never lake water, never anything still sealed). Turn it
off under Settings > Game.

**Things to do:** search kitchens, wardrobes, sheds, shops, the pharmacy and the
hospital - a search opens the container so you can look through it and take
what you want; eat (all, half, a quarter) and drink; open cans (or pry them
open with a knife and maybe cut yourself); fill bottles at lakes, toilet tanks
and rain barrels; boil or purify water; pick lingonberries, blueberries and
rose hips; forage for mushrooms; gather sticks, chop trees, build and light
campfires, cook; set snares for hares; hunt hare, roe deer, wild boar and moose
with melee weapons, a shotgun or a moose rifle; butcher carcasses; take
painkillers, antibiotics and vitamins; wear clothing for warmth and rain;
sleep (beds are warmer); read the map.

**Saving:** the game autosaves every few minutes and when you quit; continue
from the main menu. Death is permanent - the save goes with you.

**Sound:** the soundtrack (`data/assets/audio/`) plays in the background with
pauses in between. When something hunts you, the music cross-fades to the
danger tracks in `data/assets/audio/danger/` (prompts for making them are in
[docs/music_prompts.md](docs/music_prompts.md)). Rain, wind, campfires,
footsteps, gunshots, doors and the rest are synthesised on the fly.

**Items:** over 200 items, defined in plain text in `data/items/*.ini`: a
Swedish pantry (pasta, crispbread, pea soup, fish roe spread, lingonberry jam,
pick-and-mix, princess cake), fridge and freezer food that spoils, drinks,
clothes, tools, camping gear, building materials, weapons from a Mora knife to a
moose rifle, medicine, books and the odds and ends of people's lives. Food has
real nutrition values per 100 g (from food composition tables); store food has a
nutrition label you can read, wild food doesn't.

## Building

The game uses [raylib](https://www.raylib.com) 5.5, which CMake downloads
automatically. You need CMake 3.16+ and a C++17 compiler.

### Linux

```sh
sudo apt install build-essential cmake git \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libasound2-dev
./scripts/build-linux.sh        # builds, runs the tests
./build/alone
```

### Windows

With Visual Studio 2019 or newer (Desktop development with C++) and CMake:

```bat
scripts\build-windows.bat
build\Release\alone.exe
```

Or cross-compile from Linux with MinGW-w64 (`sudo apt install mingw-w64`):

```sh
./scripts/build-windows-cross.sh   # -> build-windows/alone.exe + data/
```

The `data/` folder must sit next to the executable (the build copies it).

### Web (browser)

A WebAssembly build runs in any browser with WebGL 2 (Chrome, Edge, Firefox,
Safari 15+). It is the same game, built with [Emscripten](https://emscripten.org):

```sh
source /path/to/emsdk/emsdk_env.sh
./scripts/build-web.sh --serve      # then open http://localhost:8080/alone.html
```

The output is `build-web/alone.html`, `.js`, `.wasm` and `.data` (the packed
`data/` folder, about 28 MB); put the four files on any static web host.
Saves and settings are kept in the browser's storage (IndexedDB). The browser
only allows sound after your first click or key press.

### Options

* `-DALONE_BUILD_CLIENT=OFF` builds only the backend library and tests (no graphics).
* `alone --debug` enables the F3 debug overlay with the real body values.
* `alone --seed N` picks a world seed; `--data DIR` points to another data folder.
* `alone --screenshots` runs an automated tour and saves screenshots (used as a smoke test in CI).
  `ALONE_SHOTS=storken,forest` limits it to some stops.
* `alone --anim-showcase DIR` renders the character animations to PNG frames
  (`ffmpeg -framerate 30 -i DIR/frame_%04d.png out.mp4`).
* `alone --bench` prints frame timings at four places (the start house, inside
  Vimpeln, Kungsgatan, the Hjortmarka forest). `ALONE_BENCH_FINISH=1` waits for
  the GPU after each phase so the times are the GPU's; `ALONE_BENCH_SHOTS=DIR`
  saves a picture of each place.
* `ALONE_NO_GPU_SWITCH=1` stops the game from asking for the NVIDIA GPU on Linux.

## Controls

| Key | Action |
|---|---|
| W A S D | Walk (Shift run, C crouch); driving: W gas, S brake/reverse, A/D steer |
| Ctrl (hold) | Aim: face the cursor and walk while aiming, steadier shots |
| Right click | Interact with what's under the cursor |
| F | Use what's in front of you: the door you face, a cupboard, a person, a car (else what's under the cursor) |
| Left click / Space | Attack with what you hold |
| Tab / I | Inventory - double-click moves one, shift-click a stack, drag clothes onto the figure |
| J | Journal: the people you know of, and what they need |
| H | Health: look at each body part, right-click to treat it |
| B | Build: walls, doors, floors, roofs, crates... |
| M | Map |
| Z / R | Sleep / rest for an hour |
| L | Flashlight |
| Q / E, wheel | Rotate / zoom camera |
| 1 / 2 / 3 | Time x1 / x5 / x20 |
| Esc | Pause (Settings: audio, graphics, length of a day) |

A day lasts 24 real minutes by default (changeable in Settings).

## Project layout

```
src/core/       logging, config files, noise, random numbers, paths
src/survival/   Body (the physiology model), Symptoms
src/items/      item definitions, instances, inventory, loot tables
src/world/      map data, terrain zones, buildings & interiors, chunks, clock & weather
src/game/       Game (simulation, actions, combat), wildlife AI
src/client/     raylib renderer, UI, app (menus, HUD, inventory, map)
tests/          unit tests for the backend (diet scenarios, world, gameplay)
data/           everything moddable: items, loot, animals, symptoms, map, shaders, fonts, music
tools/mapgen/   digitize.py: map image -> terrain + roads; osm_import.py: OpenStreetMap -> buildings, streets, land use;
                omap_import.py: orienteering maps -> forest detail, paths; dem_import.py: elevation model -> heights
tools/worlddump world generation preview tool
```

The backend (`alone_core`) has no graphics dependency, so the whole simulation
can be tested headless (`ctest`).

### The map pipeline

`tools/mapgen/digitize.py <map image> <out dir>` classifies every pixel of a
map image into land cover (water, forest, woodland, park, residential, field,
meadow, commercial, hospital), removes labels and icons, recovers the river
under the municipal border line, and vectorises the road network. The output
(`terrain.pgm`, `roads.txt`) plus hand-placed `features.txt` (railway, centre
streets, plazas, landmarks, district names) and `map.ini` make up a map. A
procedural Sweden can later generate the same files.

`tools/mapgen/osm_import.py <map dir> <file.osm[.gz]>...` adds the detail from
OpenStreetMap exports (the Alingsås ones are in `tools/mapgen/osm/alingsas/`).
It aligns them to the map with the named points in `anchors.txt`, then writes
a 2 m land-use grid (`terrain2m.pgm`), named streets and paths in metres
(`roads_m.txt`) and every building, tree and street lamp
(`osm_features.txt`). Building use comes from OSM tags and the shops and
amenities inside each building.

`tools/mapgen/omap_import.py <map dir> <map.pdf>... [--scale 10000 --angle 3]`
reads orienteering maps (OCAD PDFs). It finds where each one lies by matching its
black building shapes against the OpenStreetMap buildings (searching scale,
rotation and offset), turns the map colours into land cover in the 2 m grid
(open forest, dense forest, thicket, marsh, rock, open land), and joins the
dashed path symbols from the PDF's vector data into paths that OpenStreetMap
doesn't have. Run it after `osm_import.py`. The orienteering maps themselves are
not in the repository.

`tools/mapgen/dem_import.py <map dir> <dem.tif> [--preview relief.png]` turns
an elevation model into `elevation.pgm`, heights in centimetres on an 8 m grid
(the Alingsås one is the Copernicus GLO-30 DEM from OpenTopography, in
`tools/mapgen/dem/`). It places the model with the latitude/longitude transform
`osm_import.py` writes into `map.ini`. Copernicus measures the top of whatever
is there, so the tool lowers forests by how far the radar saw into the canopy,
cuts out big buildings and fills them in from the ground around them, and gives
lakes and rivers a flat surface with banks above it. At this latitude the model
is made at 1.5 arcseconds east-west; files resampled to 1" repeat every third
column, and the tool rebuilds the real columns so slopes don't come out striped.
Run it last.

## Roadmap

* Procedural Swedish countryside beyond the map edge (Västra Bodarna and the
  rest of Mjörn's shore; its orienteering map is waiting)
* Upper floors, basements, more building types (farms, summer cottages)
* Carpentry skill levels, more to build (stairs, shelves, drying racks)
* Trapping more animals, preserving food (smoking, drying, salting)

## License

GPL-3.0 (see `LICENSE`). Fonts: DejaVu (see `data/fonts/LICENSE-DejaVu.txt`).
raylib is zlib-licensed. Map data © OpenStreetMap contributors, available
under the Open Database License (ODbL). Forest detail around Hjortmarka and
Kvarnsjön is derived from local orienteering maps. Elevation produced using
Copernicus WorldDEM-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space
GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all
rights reserved.
