<a href="https://www.packtpub.com/en-us/unlock"><img src="https://drive.google.com/uc?export=view&id=1lQCTQQ8iV5pGuPA1n5wuds-3pwJi0OD_"></a>
<h1 align="center">
Mastering Graphics Programming With Vulkan, Second Edition</h1>
<p align="center">This is the code repository for <a href ="https://www.packtpub.com/en-us/product/mastering-graphics-programming-with-vulkan-second-edition/9781806116379"> Mastering Graphics Programming With Vulkan, Second Edition</a>, published by Packt.
</p>

<h2 align="center">
Develop a modern rendering engine featuring GPU-driven rendering and ray tracing
</h2>
<p align="center">
Gabriel Sassone, Marco Castorina</p>

<p align="center">
   <a href="" alt="Discord" title="Learn more on the Discord server"><img width="32px" src="https://cliply.co/wp-content/uploads/2021/08/372108630_DISCORD_LOGO_400.gif"/></a>
  &#8287;&#8287;&#8287;&#8287;&#8287;
  <a href="https://packt.link/free-ebook/9781806116379"><img width="32px" alt="Free PDF" title="Free PDF" src="https://cdn-icons-png.flaticon.com/512/4726/4726010.png"/></a>
 &#8287;&#8287;&#8287;&#8287;&#8287;
  <a href="https://packt.link/gbp/9781806116379"><img width="32px" alt="Graphic Bundle" title="Graphic Bundle" src="https://cdn-icons-png.flaticon.com/512/2659/2659360.png"/></a>
  &#8287;&#8287;&#8287;&#8287;&#8287;
   <a href=""><img width="32px" alt="Amazon" title="Get your copy" src="https://cdn-icons-png.flaticon.com/512/15466/15466027.png"/></a>
  &#8287;&#8287;&#8287;&#8287;&#8287;
</p>
<details open> 
  <summary><h2>About the book</summary>
<a href="https://www.packtpub.com/en-us/product/mastering-graphics-programming-with-vulkan-second-edition/9781806116379">
<img src="https://content.packt.com/B34102/cover_image_small.jpg" alt="Mastering Graphics Programming With Vulkan, Second Edition" height="256px" align="right">
</a>

Building a modern rendering engine can feel overwhelming, especially with the rapid evolution of graphics APIs and techniques. Some developers struggle to bridge the gap between basic Vulkan tutorials and the advanced systems used in professional engines. This book tackles this challenge by guiding you through the design and implementation of a modern rendering engine using Vulkan’s latest features.
You’ll gain clarity and confidence working with the API thanks to a framework that strips away the boilerplate while preserving Vulkan’s concepts. As you progress, you’ll explore advanced Vulkan features like descriptor indexing, mesh shaders, and async compute for performance and flexibility. You’ll also use frame graphs to build a rendering architecture designed to scale and simplify resource management. Through clear explanations and hands-on examples, you’ll explore modern rendering techniques such as GPU-driven rendering, real-time ray tracing, and neural rendering approaches that are at the forefront of modern rendering techniques.
Written by two experienced graphics programmers, this book combines theory with implementation-focused guidance to help you gain production-ready skills. By the end, you’ll not only understand the Vulkan API at a deeper level but also have the knowledge to design and build your own modern renderer.</details>
<details open> 
  <summary><h2>Key Learnings</summary>
<ul>

<li>Integrate modern bindless techniques to reduce the complexity of descriptor set management</li>

<li>Design and implement a frame graph to simplify resource management and barrier placement</li>

<li>Implement a modern GPU driven rendering framework leveraging compute and mesh shaders</li>

<li>Integrate async compute to improve rendering efficiency</li>

<li>Implement modern rendering techniques, including TAA and volumetric fog</li>

<li>Design and implement a streaming system for acceleration structures</li>

<li>Integrate ray traced shadows and reflections</li>

<li>Implement a neural rendering algorithm</li>

</ul>

  </details>

<details open> 
  <summary><h2>Chapters</summary>
     <img src="https://cliply.co/wp-content/uploads/2020/02/372002150_DOCUMENTS_400px.gif" alt="Unity Cookbook, Fifth Edition" height="556px" align="right">
<ol>

  <li>Introducing the Raptor Engine</li>

  <li>Improving Pipelines and Descriptors Management</li>

  <li>Implementing a Frame Graph</li>

  <li>Unlocking Async Compute</li>

  <li>GPU-Driven Rendering</li>

  <li>Animating Meshlets</li>

  <li>Rendering Many Lights with Clustered Deferred Rendering</li>

  <li>Adding Shadows Using Mesh Shaders</li>

  <li>Adding Volumetric Fog</li>

  <li>Temporal Anti-Aliasing</li>

  <li>Getting Started with Ray Tracing</li>

  <li>Revisiting Shadows with Ray Tracing</li>

  <li>Implementing ReSTIR GI</li>

  <li>Adding Reflections with Ray Tracing</li>

  <li>Neural Rendering</li>

</ol>

</details>


<details open> 
  <summary><h2>Requirements for this book</summary>
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

  </details>
    


<details> 
  <summary><h2>Get to know Authors</h2></summary>

_Gabriel Sassone_ Gabriel Sassone is a rendering enthusiast currently working as a Principal Rendering Engineer at Multiplayer Group. Previously working for Avalanche Studios, where his first contact with Vulkan happened, where they developed the Vulkan layer for the proprietary Apex Engine and its Google Stadia Port. He previously worked at ReadyAtDawn, Codemasters, FrameStudios, and some non-gaming tech companies. His spare time is filled with music and rendering, gaming, and outdoor activities.

_Marco Castorina_ Marco Castorina first got familiar with Vulkan while working as a driver developer at Samsung. Later he developed a 2D and 3D renderer in Vulkan from scratch for a leading media-server company. He recently joined the games graphics performance team at AMD. In his spare time, he keeps up to date with the latest techniques in real-time graphics.



</details>
<details> 
  <summary><h2>Other Related Books</h2></summary>
<ul>

  <li><a href="https://www.packtpub.com/en-us/product/vulkan-3d-graphics-rendering-cookbook-second-edition/9781803248110">Vulkan 3D Graphics Rendering Cookbook, Second Edition</a></li>

  <li><a href="https://www.packtpub.com/en-us/product/the-modern-vulkan-cookbook-first-edition/9781803239989">The Modern Vulkan Cookbook, First Edition</a></li>
 
</ul>

</details>
