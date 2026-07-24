# Linux 4K Intro Bootstrap

> Historical planning note. The project has evolved into a general tiny-demo
> studio with music authoring as a first-class feature. See `README.md` for the
> current architecture and build instructions.

## Goal

Build a modern demoscene-style 4K intro on Fedora.

The project should prioritize:

-   Live shader development
-   Procedural graphics
-   Procedural music
-   Tiny executable size
-   Clean code suitable for experimentation

The first objective is **not** fitting into 4096 bytes. The first
objective is producing a working intro. Size optimization comes later.

------------------------------------------------------------------------

# Technology Stack

## Visual Development

Use **glslViewer** during development.

Reasons:

-   Live shader reload
-   Fast iteration
-   Native Linux
-   Minimal setup

Install:

``` bash
sudo dnf install glslviewer
```

------------------------------------------------------------------------

## Final Intro Framework

Use **shrinky-intro**.

Repository:

https://github.com/xyproto/shrinky-intro

Clone:

``` bash
git clone https://github.com/xyproto/shrinky-intro.git
cd shrinky-intro
```

------------------------------------------------------------------------

## Graphics

Primary language:

-   GLSL Fragment Shader

Host:

-   C

Rendering:

-   Fullscreen triangle
-   Fragment shader
-   Time uniform
-   Resolution uniform

------------------------------------------------------------------------

## Audio

Phase 1:

No audio.

Phase 2:

Small procedural synthesizer written in C.

Eventually generate:

-   kick
-   bass
-   lead
-   pad
-   noise

Everything generated at runtime.

No samples.

------------------------------------------------------------------------

## Development Order

### Phase 1

Create shader playground.

Implement:

-   plasma
-   tunnel
-   starfield
-   raymarched sphere
-   color palettes

------------------------------------------------------------------------

### Phase 2

Scene system.

Create timeline.

Switch effects using time.

------------------------------------------------------------------------

### Phase 3

Camera animation.

Implement:

-   fly-through
-   rotations
-   zoom
-   beat timing

------------------------------------------------------------------------

### Phase 4

Procedural audio.

Synchronize visuals to music.

------------------------------------------------------------------------

### Phase 5

Integrate into shrinky-intro.

Replace development framework.

------------------------------------------------------------------------

### Phase 6

Optimize.

Reduce executable size.

------------------------------------------------------------------------

# Suggested Repository Layout

``` text
demo/
├── shaders/
│   ├── plasma.frag
│   ├── tunnel.frag
│   └── common.glsl
├── src/
│   ├── main.c
│   ├── synth.c
│   ├── timeline.c
│   └── math.c
├── tools/
├── assets/
└── README.md
```

------------------------------------------------------------------------

# Initial Milestones

1.  Render fullscreen shader.
2.  Animate with time.
3.  Implement plasma.
4.  Implement tunnel.
5.  Implement raymarching.
6.  Build scene timeline.
7.  Add procedural synth.
8.  Synchronize visuals and music.
9.  Port to shrinky-intro.
10. Optimize toward a real 4K intro.

------------------------------------------------------------------------

# Future Effects

-   Plasma
-   Fire
-   Tunnel
-   Starfield
-   Metaballs
-   Raymarching
-   Fractals
-   Signed Distance Fields
-   Soft shadows
-   Ambient occlusion
-   Bloom
-   Chromatic aberration
-   CRT simulation
-   Particle systems
-   Procedural textures
-   Kaleidoscope
-   Infinite city
-   Voxel terrain

------------------------------------------------------------------------

# Prompt for Codex

You are helping build a Linux-native demoscene framework.

Rules:

-   C23
-   CMake
-   OpenGL 4.5
-   GLSL
-   Clean architecture
-   Small executable
-   No unnecessary dependencies
-   Prefer compile-time configuration
-   Keep shader code modular
-   Document every subsystem
-   Build incrementally
-   Every commit should leave the project runnable

Start by creating a minimal application that opens a window, renders a
fullscreen triangle, loads a fragment shader from disk with live reload,
and exposes time and resolution uniforms.
