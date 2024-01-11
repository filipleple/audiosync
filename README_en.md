# Automatic Synchronization of Sound with 360 Camera

**VST3Plugin** is a plugin written in C++ using the Juce framework. It allows the user to decode time codes, find their difference, and send this value through MIDI CC 14 bit.

## Installation

Make sure the following programs are installed on your computer:

- [Juce](https://juce.com/download/) - a C++ framework for creating audio applications and plugins.
- [Visual Studio](https://visualstudio.microsoft.com) - a standalone IDE that works on Windows, macOS, and Linux.
- [Git client](https://git-scm.com/) - Git is a free and open-source distributed version control system.

Then
- Clone the repository:
```bash 
git clone https://git.pg.edu.pl/p1334942/automatic-synchronization-of-sound-with-360-camera
```

- Open the file *Tsimafei 187719.jucer* in **Projucer**.

- From **Projucer**, open the project in **Visual Studio**.
- Compile and run the project, all executable files will be in the *builds* folder.

## Usage

Add the plugin to REAPER, run it on the channel where the time codes are played. The decoded codes will be displayed in the HH:MM:SS:FF format. In the middle of the interface the time difference will appear as a number of frames and milliseconds. On the lower panel, there is an option to select the refresh rate for the time code. Additionally, the plugin includes a slider for manual delay correction.

# Authors and Acknowledgements

- Tsimafei Dalhou
- Dr. Bartłomiej Mróz
- Dr. Piotr Odya

# Project Status
The project is completed. The prototype of the final solution has been presented.
