# WaveOpticsHairRendering

A customized PBRT-v4 project for wave-optics-based hair rendering using NS-FDTD simulation and precomputed BxDF tables.

This repository contains a customized version of [PBRT-v4](https://github.com/mmp/pbrt-v4) for wave-optics-based hair rendering, as part of my undergraduate thesis. The project implements a scattering model for hair fibers based on wave optics principles, simulated using the Non-Standard Finite-Difference Time-Domain (NS-FDTD) method. It also integrates precomputed BxDF tables for efficient rendering.

## Installation

1. Cloning the Repository

    ```bash
    git clone --recursive https://github.com/Remyuu/WaveOpticsHairRendering.git
    cd WaveOpticsHairRendering
    ```

2. Compiling The Renderer

    ```bash
    mkdir build
    cd build
    cmake ../
    make -j
    ```

    If you want to make the pbrt executable easily accessible from the terminal, create symbolic links (**Optional**):

    ```bash
    sudo ln -s /[your-pbrt-v4-PATH]/build/pbrt /usr/local/bin/pbrt4
    ```

## Common Build Issues

1. **Missing OpenGL**:
   If you encounter an error related to OpenGL, run the following to install the necessary libraries:

   ```bash
   sudo apt update
   sudo apt install -y \
       mesa-utils \
       libgl1-mesa-dev \
       libglu1-mesa-dev \
       freeglut3-dev \
       xorg-dev
   ```

2. **Missing `wayland-scanner`**:
   If you see an error related to `wayland-scanner`, install the necessary packages:

   ```bash
   sudo apt update
   sudo apt install -y \
       wayland-protocols \
       libwayland-dev
   ```

3. **Missing `xkbcommon`**:
   To resolve missing `xkbcommon` error, run:

   ```bash
   sudo apt update
   sudo apt install -y \
       libxkbcommon-dev
   ```

4. **Missing `pkg-config`**:
   To fix the missing `pkg-config` error, install the `pkg-config` tool:

   ```bash
   sudo apt-get install pkg-config
   ```

5. **Missing RandR headers**:
   If you encounter an error related to RandR, install the development package:

   ```bash
   sudo apt-get install xorg-dev libglu1-mesa-dev
   ```

6. **OpenGL not found**:
   In case OpenGL is not found, install the OpenGL development libraries:

   ```bash
   sudo apt-get install libgl1-mesa-dev
   ```

### Running the Renderer

To run the renderer with the custom hair material, use the following command:

```bash
./pbrt <path-to-scene-file>
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgements

- PBRT-v4 authors for their rendering framework.
- NS-FDTD developers for providing wave-optics simulation methods.

## References

1. https://github.com/KotaYokoyama/hair-rendering
2. https://github.com/mmp/pbrt-v4