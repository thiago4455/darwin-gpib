<p align="center">
  <img src="https://github.com/thiago4455/darwin-gpib/blob/master/.github/Icon-256.png?raw=true" height="128">
  <h1 align="center">Open GPIB for macOS</h1>
  <p align="center">The Darwin GPIB package</p>
</p>

Open GPIB for macOS is an open-source implementation of a GPIB (IEEE-488.2) driver for macOS, built using Apple’s modern DriverKit framework. 
The project includes a C library interface compatible with the Linux-GPIB project headers, allowing existing applications and libraries (such as PyVISA with pyvisa-py) to use GPIB on macOS without modification. 

This driver leverages Apple’s system extension model, and is packaged with a macOS-native GUI for managing devices, refreshing connections, and handling permissions. 
As NI's GPIB support ended with macOS 12, this project aims to restore USB GPIB functionality on Apple Silicon and modern macOS systems.

<img width="1012" alt="github-banner" src="https://github.com/thiago4455/darwin-gpib/blob/master/.github/screenshot.png?raw=true&">

> [!NOTE]
> There are no releases in this repository while I await for Apple approval of the DriverKit entitlement.


> [!IMPORTANT]
> Of the 3 included drivers, only KUSB-488B is tested against real instruments.


## Building

The project builds with Xcode. Open `darwin-gpib.xcodeproj` and build the `Open GPIB` scheme.

That produces the app with the driver extension, the `gpibd` broker and `libgpib` all bundled inside it. Run the app, click **Install driver**, approve the extension in System Settings, and plug in the adapter.


### KUSB-488B
> [!IMPORTANT]
> The KUSB-488B firmware is not included and should be extracted from the Windows drivers. You generate it yourself from a copy of the KI-488 driver package.
>
> You need two files from it: `kusbgpib_fwdlx64.sys` and `USB-GPIB.RBF`. Run `KI-488.exe /a /v"TARGETDIR=C:\KI-488"` to unpack it , then extract the driver package without installing it:
>
> ```
> cd "C:\KI-488\Disk1\NI Command Compatible Driver"
> setup.exe /a /s /v"/qn TARGETDIR=C:\ki488_admin"
> ```
>
> Both files are in `C:\ki488_admin\Keithley\KIGPIB\X64\NT10\`. Then:
> ```
> tools/extract_kusb_firmware.py \
>     --driver kusbgpib_fwdlx64.sys \
>     --rbf    USB-GPIB.RBF \
>     --out    driver/devices/kusb_488b/kusb_488b_firmware_data.h
> ```
>
> Do this **before** building. The generated header is ignored by git and is the only piece not in the repository.
> 
> Without that header the build still succeeds. The adapter is recognised, but never leaves loader mode.
