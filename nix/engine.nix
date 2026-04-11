{
  self,
  pkgs,
}: let
  lib = pkgs.lib;
  common = import ./common.nix {
    inherit pkgs;
  };
  naclRuntime = import ./nacl-runtime.nix {
    inherit pkgs;
  };

  version =
    if self ? shortRev
    then self.shortRev
    else "dirty";

  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type: let
      baseName = builtins.baseNameOf path;
      relPath = lib.removePrefix (toString ../. + "/") (toString path);
    in
      !(baseName
        == ".git"
        || baseName == "build"
        || baseName == "CMakeFiles"
        || baseName == "CMakeCache.txt"
        || lib.hasPrefix "result" baseName
        || relPath == "flake.lock");
  };

  mkDaemonPackage = {
    pname,
    executable,
    buildClient ? false,
    buildServer ? false,
    buildTtyClient ? false,
  }:
    pkgs.stdenv.mkDerivation {
      inherit pname version src;

      nativeBuildInputs = common.commonNativeBuildInputs;
      buildInputs = common.commonBuildInputs;
      passthru = {
        inherit (naclRuntime) package supported;
      };

      cmakeFlags = [
        "-G"
        "Ninja"
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
        "-DBUILD_CLIENT=${
          if buildClient
          then "ON"
          else "OFF"
        }"
        "-DBUILD_SERVER=${
          if buildServer
          then "ON"
          else "OFF"
        }"
        "-DBUILD_TTY_CLIENT=${
          if buildTtyClient
          then "ON"
          else "OFF"
        }"
        "-DBUILD_CGAME=OFF"
        "-DBUILD_SGAME=OFF"
        "-DBUILD_GAME_NACL=OFF"
        "-DUSE_BREAKPAD=ON"
        "-DUSE_CPP23=ON"
        "-DUSE_EXTERNAL_DEPS=OFF"
        "-DUSE_EXTERNAL_DEPS_LIBS=OFF"
        "-DUSE_PRECOMPILED_HEADER=ON"
      ];

      installPhase = ''
        runHook preInstall
        mkdir -p "$out/bin"
        cp "${executable}" "$out/bin/${executable}"
        ${naclRuntime.installSnippet}
        runHook postInstall
      '';

      meta = with lib; {
        description = "Unvanquished ${pname} target";
        license = licenses.bsd2;
        mainProgram = executable;
        platforms = platforms.linux;
      };
    };

  client = mkDaemonPackage {
    pname = "unvanquished-client";
    executable = "daemon";
    buildClient = true;
  };

  server = mkDaemonPackage {
    pname = "unvanquished-server";
    executable = "daemonded";
    buildServer = true;
  };

  ttyClient = mkDaemonPackage {
    pname = "unvanquished-tty-client";
    executable = "daemon-tty";
    buildTtyClient = true;
  };
in {
  inherit client server;

  tty-client = ttyClient;
  default = client;
}
