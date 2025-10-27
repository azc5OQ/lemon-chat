Steps for Rebuilding on Windows
1. Required Tools

05/24/2025  06:26 PM    <DIR>          .
05/24/2025  06:26 PM    <DIR>          ..
05/24/2025  08:07 AM       939,466,752 clang+llvm-20.1.5-x86_64-pc-windows-msvc.tar.xz
05/24/2025  09:07 AM        46,473,960 cmake-3.31.7-windows-x86_64.zip
05/24/2025  07:07 AM           275,425 ninja-win.zip
07/27/2024  04:53 AM        76,798,128 w64devkit-1.23.0.zip


You may also need to download Visual Studio Community and the C++ toolset


2. Setup Directories

Create a directory , for example C:/Software

Extract the following tools to C:/Software:
Ninja
CMake
LLVM
w64devkit

But ninja also goes to other location, more on that later

3. Update System PATH

Add the following directories to your system’s PATH environment variable:

C:/Software
C:/Software/cmake-4.0.2-windows-x86_64/bin
C:/Software/w64devkit/bin
C:/Software/clang+llvm-20.1.5-x86_64-pc-windows-msvc/bin

To do this:

Go to Edit the system environment variables.

Select Environment Variables.

Edit the PATH variable and add the directories listed above.

4. Note on Ninja
The Ninja program must be located in two directories:
C:/Software (which will be in the PATH).
C:/Software/w64devkit/bin, since CMake will look for Ninja here.


5. Build Process

Once the tools are set up:

Running the .bat script will automatically handle the build process.
No downloading of additional internet dependencies is needed, source code of dependencies is already here
result of built will be .exe file in directory built result

6. Code Formatting

Everyone has a different coding style, so there is a .bat script in main folder that uses clang-format