{
  self,
  system,
  nixpkgs,
  flake-utils,
}: let
  pkgs = import nixpkgs {
    inherit system;
  };

  common = import ./common.nix {
    inherit pkgs;
  };

  packages = import ./engine.nix {
    inherit self pkgs;
  };
in {
  inherit packages;

  apps = {
    client = flake-utils.lib.mkApp {
      drv = packages.client;
      name = "daemon";
    };

    server = flake-utils.lib.mkApp {
      drv = packages.server;
      name = "daemonded";
    };

    tty-client = flake-utils.lib.mkApp {
      drv = packages.tty-client;
      name = "daemon-tty";
    };

    default = self.apps.${system}.client;
  };

  devShells.default = pkgs.mkShell {
    inputsFrom = [packages.client];
    nativeBuildInputs = common.commonNativeBuildInputs;
    buildInputs = common.commonBuildInputs;
  };
}
