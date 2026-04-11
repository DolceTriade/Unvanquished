{pkgs}: let
  lib = pkgs.lib;
in
  if pkgs.stdenv.hostPlatform.system != "x86_64-linux"
  then {
    supported = false;
    package = null;
    installSnippet = "";
  }
  else let
    runtimeFiles = [
      "nacl_loader"
      "nacl_helper_bootstrap"
      "irt_core-amd64.nexe"
    ];

    archive = pkgs.fetchurl {
      url = "https://dl.unvanquished.net/deps/linux-amd64-default_11.tar.xz";
      hash = "sha256-lw6ZfM9QohVf5oo1+avyxcWWfLVoakhVuvDMqJI10ow=";
    };

    package = pkgs.stdenvNoCC.mkDerivation {
      pname = "unvanquished-nacl-runtime";
      version = "11";

      src = archive;
      dontBuild = true;
      nativeBuildInputs = [
        pkgs.patchelf
      ];

      unpackPhase = ''
        runHook preUnpack
        tar -xJf "$src"
        runHook postUnpack
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p "$out"
        cp linux-amd64-default_11/nacl_loader "$out/"
        cp linux-amd64-default_11/nacl_helper_bootstrap "$out/"
        cp linux-amd64-default_11/irt_core-amd64.nexe "$out/"

        patchelf \
          --set-interpreter "${pkgs.stdenv.cc.bintools.dynamicLinker}" \
          --set-rpath "${lib.makeLibraryPath [pkgs.stdenv.cc.cc.lib pkgs.glibc]}" \
          "$out/nacl_loader"
        runHook postInstall
      '';

      meta = with lib; {
        description = "NaCl runtime files needed to run official Unvanquished NaCl gamelogic";
        sourceProvenance = [sourceTypes.binaryNativeCode];
        platforms = ["x86_64-linux"];
      };
    };
  in {
    supported = true;
    inherit package;
    installSnippet = lib.concatLines (
      [
        "mkdir -p \"$out/bin\""
      ]
      ++ map (file: "cp \"${package}/${file}\" \"$out/bin/${file}\"") runtimeFiles
    );
  }
