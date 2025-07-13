<p align="center">
  <img src="https://github.com/thiago4455/darwin-gpib/blob/master/.github/Icon-256.png?raw=true" height="128">
  <h1 align="center">Open GPIB for macOS</h1>
  <p align="center">The Darwin GPIB package</p>
</p>

Open GPIB for macOS is an open-source implementation of a GPIB (IEEE-488.2) driver for macOS, built using Apple’s modern DriverKit framework. 
The project includes a C library interface compatible with both NI GPIB API and the Linux-GPIB project, allowing existing applications and libraries (such as PyVISA with pyvisa-py) to use GPIB on macOS without modification. 

This driver is fully sandboxed, leveraging Apple’s system extension model, and is packaged with a macOS-native GUI for managing devices, refreshing connections, and handling permissions. 
As NI's GPIB support ended with macOS 12, this project aims to restore USB GPIB functionality on Apple Silicon and modern macOS systems.

<img width="1012" alt="github-banner" src="https://github.com/thiago4455/darwin-gpib/blob/master/.github/screenshot.png?raw=true&">

> [!NOTE]
> The drivers module is not included in this repository while I await for Apple approval of the DriverKit entitlement. The project is not buidable without a valid Developer ID, so signed releases are planned. 


> [!IMPORTANT]
> Darwin GPIB drivers are still under development. Until an automatic test suite is made with real equipment, the use in production is disencouraged.
