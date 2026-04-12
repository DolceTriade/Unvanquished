{pkgs}: {
  commonNativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    (python3.withPackages (
      ps:
        with ps; [
          jinja2
          pyyaml
        ]
    ))
  ];

  commonBuildInputs = with pkgs; [
    curl
    freetype
    gmp
    glew
    libGL
    libjpeg
    libogg
    libpng
    libvorbis
    libwebp
    lua5_4
    ncurses
    nettle
    openal
    opusfile
    sdl3
    zlib
  ];
}
