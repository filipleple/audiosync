# Automatyczna synchronizacja dźwięku z kamerą 360

**Wtyczka VST3** napisana w C++ z wykorzystaniem frameworka JUCE. Automatycznie synchronizuje
nagrania z wielu kamer 360° poprzez dekodowanie kodów czasowych SMPTE LTC z ścieżek audio,
obliczanie opóźnień między ścieżkami i ich stosowanie w czasie rzeczywistym. Gdy jakość LTC
spada lub sygnał zostaje przerwany, wtyczka przełącza się płynnie na algorytm dopasowania
audio (Normalized Cross-Correlation na krzywych nowości energetycznej).

## Architektura

Wtyczka działa w dwóch rolach, które należy skonfigurować w sesji DAW:

- **Master** — umieszczony na ścieżce referencyjnej z LTC. Dekoduje LTC, wyznacza krzywą
  nowości audio i zapisuje oba wyniki do wspólnej pamięci. Nie opóźnia własnego wyjścia.
- **Slave** — jedna instancja na każdą dodatkową ścieżkę. Odczytuje dane mastera z pamięci
  współdzielonej, dekoduje własny LTC, oblicza opóźnienie między ścieżkami i stosuje je za
  pomocą silnika opóźnienia. Obsługiwanych jest do 8 instancji slave na grupę.

Obie role współdzielą nazwę grupy (ustawianą w interfejsie wtyczki), która identyfikuje
region pamięci współdzielonej używany do komunikacji.

## Instalacja

Upewnij się, że zainstalowane są następujące programy:

- [JUCE](https://juce.com/download/) — framework C++ do tworzenia aplikacji audio.
- [CMake](https://cmake.org/) 3.22+ i kompilator C++17 (kompilacja Linux/macOS).
- [Visual Studio 2022](https://visualstudio.microsoft.com) (kompilacja Windows).
- [Git](https://git-scm.com/) — system kontroli wersji.

Sklonuj repozytorium:

```bash
git clone https://git.pg.edu.pl/p1334942/automatic-synchronization-of-sound-with-360-camera
```

### Kompilacja (Linux / macOS)

```bash
mkdir -p build && cd build
cmake ..
make
```

Wynik: `build/TsimafeiPlugin_artefacts/Release/VST3/`

### Kompilacja (Windows)

Otwórz `Tsimafei 187719.jucer` w **Projucer**, wyeksportuj do Visual Studio 2022, a następnie
skompiluj z IDE lub:

```bash
msbuild "Builds/VisualStudio2022/Tsimafei 187719.sln"
```

## Użycie

1. W DAW (zalecany REAPER) wczytaj VST3 na ścieżkę referencyjną z LTC. Ustaw rolę **Master**
   i wybierz nazwę grupy.
2. Wczytaj drugą instancję na każdą kolejną ścieżkę. Ustaw rolę **Slave** i użyj tej samej
   nazwy grupy.
3. Naciśnij Play. Master wyświetla zdekodowany kod czasowy w formacie HH:MM:SS:FF. Każdy
   slave wyświetla obliczone opóźnienie w milisekundach i klatkach oraz stosuje je automatycznie.
4. Na każdej instancji dostępny jest suwak ręcznej korekty.
5. Gdy jakość LTC spada (Q_LTC < 0,5), wtyczka przełącza się na śledzenie opóźnienia na
   podstawie audio. Karta diagnostyczna pokazuje aktywne źródło (LTC / AUD / NONE).

## Autorzy i podziękowania

- Tsimafei Dalhou
- dr inż. Bartłomiej Mróz
- dr inż. Piotr Odya

## Status projektu

Aktywny rozwój. Dekodowanie LTC, komunikacja master-slave przez pamięć współdzieloną oraz
tryb awaryjny oparty na audio są funkcjonalne. Znane otwarte zagadnienia: dryft podczas
przejścia z LTC na mowę (`current_problem.md`).
