# Automatyczna synchronizacja dźwięku z kamera 360 

**VST3Plugin** to plugin napisany w C++ z wykorzystaniem Juce. Umożliwia użytkownikowi dekodowanie kodów czasowych, znalezienie ich różnicy oraz wysłanie tej wartości poprzez MIDI CC 14 bit.

## Instalacja

Upewnij się, że na komputerze zainstalowane są następujące programy:

- [Juce](https://juce.com/download/) - framework C++ do tworzenia aplikacji audio i wtyczek.
- [Visual Studio](https://visualstudio.microsoft.com/ru/) - samodzielne IDE działające na Windows, macOS i Linux.
- [Git client](https://git-scm.com/) - Git to darmowy i otwarty rozproszony system kontroli wersji.

Następnie
- Klonuj repozytorium:
```bash 
git clone https://git.pg.edu.pl/p1334942/automatyczna-synchronizacja-dzwieku-z-kamera-360
```

- Otwórz plik *Tsimafei 187719.jucer* w **Projucer**.

- Z **Projucer** otwórz projekt w **Visual Studio**.
- Skompiluj i uruchom projekt, wszystkie pliki wykonywalne będą znajdować się w folderze *builds*.

## Użycie


Dodaj wtyczkę do REAPER,  uruchom ją na kanale, gdzie odtwarzane są kody czasowe. Dekodowane kody wyświetlą się w formacie HH:MM:SS:FF. Na środku interfejsu pojawi się różnica czasowa w postaci liczby klatek i milisekund. Na dolnym panelu dostępna jest opcja wyboru częstotliwości odświeżania dla kodu czasowego. Dodatkowo, wtyczka zawiera suwak umożliwiający ręczną korektę opóźnienia.

# Autorzy i podziękowania

- Tsimafei Dalhou
- dr inż. Bartłomiej Mróz
- dr inż. Piotr Odya

# Status projektu
Projekt zakończony. Zaprezentowano prototyp finalnego rozwiązania.
