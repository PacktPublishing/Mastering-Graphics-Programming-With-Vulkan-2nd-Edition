# Mastering Graphics Programming with Vulkan 2nd Edition

<a href="https://www.packtpub.com/product/mastering-graphics-programming-with-vulkan/9781803244792?utm_source=github&utm_medium=repository&utm_campaign=9781803244792"><img src="https://static.packt-cdn.com/products/9781803244792/cover/smaller" alt="" height="256px" align="right"></a>

This is the code repository for [Mastering Graphics Programming with Vulkan](https://www.packtpub.com/product/mastering-graphics-programming-with-vulkan/9781803244792?utm_source=github&utm_medium=repository&utm_campaign=9781803244792), published by Packt.

**Develop a modern rendering engine from first principles to state-of-the-art techniques**

## What is this book about?
Vulkan is now an established and flexible multi-platform graphics API. It has been adopted in many industries, including game development, medical imaging, movie productions, and media playback. Learning Vulkan is a foundational step to understanding how a modern graphics API works, both on desktop and mobile.

This book covers the following exciting features:
* Understand resources management and modern bindless techniques
* Get comfortable with how a frame graph works and know its  advantages
* Explore how to render efficiently with many light sources
* Discover how to integrate variable rate shading
* Understand the benefits and limitations of temporal anti-aliasing
* Get to grips with how GPU-driven rendering works
* Explore and leverage ray tracing to improve render quality

If you feel this book is for you, get your [copy](https://www.amazon.com/dp/1803244798) today!

<a href="https://www.packtpub.com/?utm_source=github&utm_medium=banner&utm_campaign=GitHubBanner"><img src="https://raw.githubusercontent.com/PacktPublishing/GitHub/master/GitHub.png"
alt="https://www.packtpub.com/" border="5" /></a>

## Getting started

The project uses CMake on all supported platforms. On Windows, CMake generates a Visual Studio solution containing the chapter examples.

### Windows requirements
Install the following tools:

* **Visual Studio 2022 or 2026**, with the **Desktop development with C++** workload, including the MSVC compiler and Windows SDK.
* **CMake**, available through your `PATH`: version **3.21 or later** for Visual Studio 2022, or **4.2 or later** for Visual Studio 2026.
* **Vulkan SDK 1.4.328.1**, the version used to test the project, including the Slang and SPIRV-Cross development libraries.
* **Git**, available through your `PATH`.
* **Python 3**, available as `python` through your `PATH`.

Ensure that the `VULKAN_SDK` environment variable points to your SDK installation. You will also need a GPU and driver supporting the Vulkan features used by the chapter you want to run.

After installation, open a new terminal and verify that the tools are available:

```bat
cmake --version
git --version
python --version
```

### Downloading the source code and sample assets

Clone the repository, including its submodules:

```bat
git clone --recurse-submodules https://github.com/PacktPublishing/Mastering-Graphics-Programming-With-Vulkan-2nd-Edition.git
cd Mastering-Graphics-Programming-With-Vulkan-2nd-Edition
```

If you already cloned the repository without its submodules, initialize them with:

```bat
git submodule update --init --recursive
```

Download the sample glTF assets:

```bat
python bootstrap.py
```

The models are downloaded to `deps/src/glTF-Sample-Models`. This step requires an internet connection and may take some time.

Run all remaining commands from the repository root.

### Generating the Visual Studio solution

From the repository root, run the command matching your installed version of Visual Studio.

**Visual Studio 2022:**

```bat
cmake -S . -B project -G "Visual Studio 17 2022" -A x64
```

**Visual Studio 2026:**

```bat
cmake -S . -B project -G "Visual Studio 18 2026" -A x64
```

CMake creates the `project` directory and generates the Visual Studio solution. Open it using:

```bat
cmake --open project
```

If you switch between Visual Studio versions, remove the generated `project` directory before running the new configuration command.


Alternatively, open `project/RaptorEngine2.sln` directly in Visual Studio.

Select the desired configuration, such as **Release**, and build the chapter you want to run. The chapter targets are named `Chapter01`, `Chapter02`, and so on.

### Building from the command line

You can also compile a chapter without opening Visual Studio:

```bat
cmake --build project --config Release --target Chapter01 --parallel
```

Replace `Chapter01` with another chapter target as needed. Omit `--target Chapter01` to build all chapters.

Executables are written to the `bin` directory, with the configuration included in the filename. For example:

```text
bin/Chapter01_Release.exe
```

### Running the first example

From the repository root, run Chapter 1 with the downloaded Sponza model:

```bat
.\bin\Chapter01_Release.exe "deps\src\glTF-Sample-Models\2.0\Sponza\glTF\Sponza.gltf"
```

To run it from Visual Studio, set **Chapter01** as the startup project and enter the full path to `Sponza.gltf` under **Project Properties → Configuration Properties → Debugging → Command Arguments**.

### Troubleshooting

* **`cmake`, `git`, or `python` is not recognized:** ensure the tool is installed and its executable directory is included in `PATH`, then open a new terminal.
* **Vulkan cannot be found:** check your SDK installation and the `VULKAN_SDK` environment variable.
* **The bootstrap submodule is missing:** run `git submodule update --init --recursive`.
* **A model cannot be found:** ensure `python bootstrap.py` completed successfully and check the path passed to the executable.
* **CMake reports a generator mismatch:** remove the generated `project` directory and run the configuration command again.


### Building on Linux

You will need:

* A C++ compiler with C++20 support.
* CMake 3.21 or later.
* A build tool such as Make or Ninja.
* Git and Python 3.
* The SDL3 development libraries.
* The Vulkan SDK, including the Slang and SPIRV-Cross development libraries used by the project.

Follow the source download instructions above. On systems where Python is available as `python3`, download the sample assets using:

```bash
python3 bootstrap.py
```

If you installed the Vulkan SDK from the Linux archive, load its environment settings before configuring, building, or running the examples:

```bash
source /path/to/vulkan-sdk/setup-env.sh
```

Replace `/path/to/vulkan-sdk` with the directory containing the SDK's `setup-env.sh` script.

From the repository root, configure a Release build:

```bash
cmake -S . -B project -DCMAKE_BUILD_TYPE=Release
```

Build the first chapter:

```bash
cmake --build project --target Chapter01 --parallel
```

Replace `Chapter01` with another chapter target as needed. Omit `--target Chapter01` to build all chapters.

Run the first example with the downloaded Sponza model:

```bash
./bin/Chapter01_Release "deps/src/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf"
```

CMake creates the `project` build directory automatically, and executables are written to `bin`.

To build with debugging information and without the Release optimizations, configure with `-DCMAKE_BUILD_TYPE=Debug`. The resulting executable will be named `Chapter01_Debug`.


**Following is what you need for this book:**
This book is for professional graphics and game developers who want to gain in-depth knowledge about how to write a modern and performant rendering engine in Vulkan. Familiarity with basic concepts of graphics programming (i.e. matrices, vectors, etc.) and fundamental knowledge of Vulkan are required.

With the following software and hardware list you can run all code files present in the book (Chapter 1-15).
### Software and Hardware List
| Chapter | Software required | OS required |
| -------- | ------------------------------------ | ----------------------------------- |
| 1-15 | Vulkan 1.4+ | Windows or Linux |

We also provide a PDF file that has color images of the screenshots/diagrams used in this book. [Click here to download it](https://packt.link/ht2jV).

### Related products
* 3D Graphics Rendering Cookbook [[Packt]](https://www.packtpub.com/product/3d-graphics-rendering-cookbook/9781838986193?utm_source=github&utm_medium=repository&utm_campaign=9781838986193) [[Amazon]](https://www.amazon.com/dp/1838986197)

* Vulkan Cookbook [[Packt]](https://www.packtpub.com/product/vulkan-cookbook/9781786468154?utm_source=github&utm_medium=repository&utm_campaign=9781786468154) [[Amazon]](https://www.amazon.com/dp/1786468158)

## Errata
 * Page 6 (Almost at the end of the page):  **$ cmake --build build --target chapter1 -- -j 4** _should be_ **$ cmake --build build --target Chapter1 -- -j 4**

## Get to Know the Authors
**Marco Castorina** first got familiar with Vulkan while working as a driver developer at Samsung. Later he developed a 2D and 3D renderer in Vulkan from scratch for a leading media-server company. He recently joined the games graphics performance team at AMD. In his spare time, he keeps up to date with the latest techniques in real-time graphics. He also likes cooking and playing
guitar.

**Gabriel Sassone** is a rendering enthusiast currently working as a Principal Rendering Engineer at Multiplayer Group. Previously working for Avalanche Studios, where his first contact with Vulkan happened, where they developed the Vulkan layer for the proprietary Apex Engine and its Google Stadia Port. He previously worked at ReadyAtDawn, Codemasters, FrameStudios, and some non-gaming tech companies. His spare time is filled with music and rendering, gaming, and outdoor activities.

### Download a free PDF

 <i>If you have already purchased a print or Kindle version of this book, you can get a DRM-free PDF version at no cost.<br>Simply click on the link to claim your free PDF.</i>
<p align="center"> <a href="https://packt.link/free-ebook/9781803244792">https://packt.link/free-ebook/9781803244792 </a> </p>
