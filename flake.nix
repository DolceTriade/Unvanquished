{
  description = "Nix flake for building Unvanquished client, server, and tty-client targets";

  inputs = {
    flake-utils.url = "github:numtide/flake-utils";
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = {
    self,
    flake-utils,
    nixpkgs,
  }:
    flake-utils.lib.eachDefaultSystem (
      system:
        import ./nix {
          inherit self system nixpkgs flake-utils;
        }
    );
}
