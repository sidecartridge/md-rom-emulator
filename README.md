# SidecarTridge Multi-device ROM Emulator app

This is a microfirmware app for the SidecarTridge Multi-device that emulates the cartridge ROM of a Atari ST/STE/Mega ST/Mega STE computer. 




## Setting up the development environment

### Requirements

#### If you only want to use SidecarTridge Multi-device

- A SidecarTridge Multi-device board. You can build your own or purchase one from the [SidecarTridge website](https://sidecartridge.com).

- A Raspberry Pi Pico W or WH with the debug connector.

- A Raspberry Pi Pico Debug Probe.

- An Atari ST / STE / Mega computer. Forget about emulators, this project needs real retro hardware. You can find Atari ST computers in eBay for less than 100€.

#### If you want to contribute to the project

- If you are a developer, you'll need a Raspberry Pi Pico Debug Probe.

- The [atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker) is pivotal. Familiarize yourself with its installation and usage.

- A `git` client, command line or GUI, your pick.

- A Makefile attuned with GNU Make.

- Visual Studio Code is highly recommended if you want to debug the code. You can find the configuration files in the [.vscode](.vscode) folder. 

- An Atari ST/STE/MegaST/MegaSTE computer. You can also use an emulator such as Hatari or STEEM for testing purposes, but you cannot really test any real functionality of the app without a real computer.

### Setup a Raspberry Pi Pico development environment from scratch

If you are not familiar with the development on the Raspberry Pi Pico or RP2040, we recommend you to follow the [Getting Started with Raspberry Pi Pico](https://datasheets.raspberrypi.org/pico/getting-started-with-pico.pdf) guide. This guide will help you to setup the Raspberry Pi Pico development environment from scratch.

We also think it's very important to setup the picoprobe debugging hardware. The picoprobe is a hardware debugger that will allow you to debug the code running on the Raspberry Pi Pico. You can find more information in this two excellent tutorials from Shawn Hymel:
- [Raspberry Pi Pico and RP2040 - C/C++ Part 1: Blink and VS Code](https://www.digikey.es/en/maker/projects/raspberry-pi-pico-and-rp2040-cc-part-1-blink-and-vs-code/7102fb8bca95452e9df6150f39ae8422)
- [Raspberry Pi Pico and RP2040 - C/C++ Part 2 Debugging with VS Code](https://www.digikey.es/en/maker/projects/raspberry-pi-pico-and-rp2040-cc-part-2-debugging-with-vs-code/470abc7efb07432b82c95f6f67f184c0)

To support the debugging the SidecarTridge has four pins that are connected to the picoprobe hardware debugger. These pins are:
- UART TX: This pin is used to send the debug information from the RP2040 to the picoprobe hardware debugger.
- UART RX: This pin is used to send the debug information from the picoprobe hardware debugger to the RP2040.
- GND: Two ground pins. One MUST connect to the GND of the Raspberry Pi Pico W (the middle connector between DEBUG and SWCLK and SWDIO) and the other MUST connect to the GND of the picoprobe hardware debugger. Don't let this pins floating, otherwise the debugging will not work.

Also a good tutorial about setting up a debugging environment with the picoprobe can be found in the [Raspberry Pi Debug Probe](https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html) tutorial.

Trying to develop software for this microcontroller without the right environment is frustrating and a waste of time. So please, take your time to setup the development environment properly. It will save you a lot of time in the future.

#### Configure environment variables

The following environment variables are required to be set:

- `PICO_SDK_PATH`: The path to the Raspberry Pi Pico SDK.
- `PICO_EXTRAS_PATH`: The path to the Raspberry Pi Pico Extras SDK.
- `FATFS_SDK_PATH`: The path to the FatFS SDK.

This repository contains subrepos pointing to these SDKs in root folder.

All the compile, debug and build scripts use these environment variables to locate the SDKs. So it's very important to set them properly. An option would be to set them in your `.bashrc` file if you are using Bash, or in your `.zshrc` file if you are using ZSH. 

#### Configure Visual Studio Code

To configure Visual Studio Code to work with the Raspberry Pi Pico, please follow the [Raspberry Pi Pico and RP2040 - C/C++ Part 2 Debugging with VS Code](https://www.digikey.es/en/maker/projects/raspberry-pi-pico-and-rp2040-cc-part-2-debugging-with-vs-code/470abc7efb07432b82c95f6f67f184c0) tutorial.

The `.vscode` folder contains the configuration files for Visual Studio Code. **Please modify them as follows**:

- `launch.json`: Modify the `gdbPath` property to point to the `arm-none-eabi-gdb` file in your computer.
- `launch.json`: Modify the `searchDir` property to point to the `/tcl` folder inside the `openocd` project in your computer.
- `settings.json`: Modify the `cortex-debug.gdbPath` property to point to the `arm-none-eabi-gdb` file in your computer.

These variables allow the debugger to **automatically find GDB and OpenOCD** without modifying the `launch.json` file every time you switch environments.

**Linux/macOS**
```sh
export ARM_GDB_PATH=/path/to/your/arm-toolchain
export PICO_OPENOCD_PATH=/path/to/openocd/tcl
```

**Windows (PowerShell)**
```powershell
$env:ARM_GDB_PATH="C:\Path\to\your\arm-toolchain"
$env:PICO_OPENOCD_PATH="C:\Path\to\openocd\tcl"
```

**Windows (CMD)**
```cmd
set ARM_GDB_PATH=C:\Path\to\your\arm-toolchain
set PICO_OPENOCD_PATH=C:\Path\to\openocd\tcl
```

#### Install Visual Studio Code extensions

- [C/C++ Extension Pack](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools-extension-pack)
- [CMake](https://marketplace.visualstudio.com/items?itemName=twxs.cmake)
- [CMake Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools)
- [Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug)


## Building the microfirmware app

### From the command line

To build a production ready microfirmware app, follow these steps:

1. Clone this repository:

2. Navigate to the cloned repository:

```
cd MYREPO
```

3. Trigger the `build.sh` script to build the firmware:

To build a release version of the app, run the following command:
```sh
./build.sh pico_w release <YOUR UUID4 HERE>
```

To build a debug version of the app, run the following command:
```sh
./build.sh pico_w debug <YOUR UUID4 HERE>
```

All applications need a unique UUID4. You can generate one using the `uuidgen` command in Linux or macOS, or by using an online UUID generator. If you are developing a new app, you can use the UUID4 for development `44444444-4444-4444-8444-444444444444`.

4. The `dist` folder now houses the files `UUID4.uf2`, the `UUID4.json` file with the app description and the `rp.uf2.md5sum` with the MD5 hash of the binary file generated by the build process. 

5. Copy the `UUID4.uf2` and `UUID4.json` file to the micro SD card `/apps` folder. If you want to host your app in a remote repository, you will have to update or add the content of the `UUID4.json` file to the `apps.json` remote file. 

## Debugging in Visual Studio Code

### Prerequisites

Before trying to deploy and debug your first microfirmware app, you must deploy in your Pico W the *Booster* app. This app is responsible for downloading the microfirmware app from the micro SD card and flashing it to the Pico W. The *Booster* app is located in the [repository](https://github.com/sidecartridge/rp2-booster-bootloader). Use the debug version of the *Booster* app to deploy it in your Pico W. The debug version contains the `DEV APP` version with the UUID4 `44444444-4444-4444-8444-444444444444`. This UUID4 is used by the *Booster* app to download the microfirmware app from the micro SD card.

On the web interface of the *Booster* app, select the `DEV APP` application and launch it. This will crete the basic structure for the microfirmware settings.  Without this step, the microfirmware app will not work properly.

### Configuring Visual Studio Code

After modifying paths in the `.vscode/launch.json` file, you can build the project directly from Visual Studio Code.

Review the installation prerequisites in the previous section. 


### Launching the app

When debugging the app it will be automatically placed at the top memory of the flash memory. This is the place where default apps for the RP2040 microcontroller are placed.

## Microfirmware app components  

The Microfirmware app is designed to run as a stand alone rp2040/rp235x plus the computer side firmware that communicates with it. The key components include:  

- **Microfirmware app Core**  
  The heart of the system, responsible for handling the micro SD card, communicating with the remote computer and providing the web interface if needed.  

- **Global Settings**  
  Stored in flash memory, these settings apply to the entire device and its apps. The microfirmware app does only read the settings, it does not modify them.

- **Local Settings**  
  Each app has its own isolated configuration stored in flash memory, ensuring app-specific settings remain independent. This section is read and written by the app.

- **Computer firmware**  
  The Booster app uses the remote computer as the terminal and implements the logic to communicate with the microfirmware app. Since it runs in ROM, it can be challenging to debug and develop it. Try to put all the debugging code in the microfirmware app and use the computer as a terminal. 

## Memory Map  

The Pico W and Pico 2W feature a **2MB flash memory**, structured as follows:  

```plaintext
+-----------------------------------------------------+
|                                                     |
|              MICROFIRMWARE APP (1152K)              |  0x10000000
|          Active microfirmware flash space           |
|                                                     |
+-----------------------------------------------------+
...
+-----------------------------------------------------+
|                                                     |
|                  CONFIG FLASH (120K)                |  0x101E0000
|             Microfirmwares configurations           |
|                                                     |
+-----------------------------------------------------+
|                                                     |
|              GLOBAL LOOKUP TABLE (4K)               |  0x101FE000
|  Microfirmwares mapping to configuration sectors    |
|                                                     |
+-----------------------------------------------------+
|                                                     |
|              GLOBAL SETTINGS (4K)                   |  0x101FF000
|           Device-wide configurations                |
|                                                     |
+-----------------------------------------------------+
```


## Microfirmware app libraries and components  

### External Libraries  

These libraries are in different repositories and are included as submodules:  

- Pico-SDK: The official Raspberry Pi Pico SDK, providing essential libraries and tools for development.
- Pico-Extras: A collection of additional libraries and utilities for the Pico platform, enhancing functionality and ease of use.
- [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico): A library for interfacing with micro SD cards using the SPI protocol, enabling file system access and management.

### Internal Libraries  

These libraries are part of the project and are included in the repository:  

- httpc: A lightweight HTTP client library for making requests to web servers, used for fetching updates and resources.
- settings: The `rp-settings` library.
- u8g2: A graphics library for rendering text and images on various display types, used for the external display support in the Booster Core.

## Project structure

### Two different architectures

The project is structure in two very different folders with code:

- `/rp`: where the code deployed in the RP2040 is located. This code is compiled and linked with the RP2040 SDK.
- `/target`: where the code for the target computer is located. The spirit of this folder is contain the code that will be compiled for the remote computer target. In this project the code is for `atarist`, so a folder `/target/atarist` is created. The code in this folder is compiled with the `atarist-toolkit-docker` project. This project is a docker image that contains all the tools needed to compile the code for the Atari ST/STE/MegaST/MegaSTE computers.

The binary file generated by the `build.sh` file in the `/target/atarist` folder must create a C array that should be included in the `/rp/src/include` folder with the filename `target_firmware.h`. The binary content of this array should be copied to the `ROM_IN_RAM` section of the RAM. 

### Building the `/target/atarist` binary


