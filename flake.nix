{
  inputs = {
    # keep-sorted start block=yes
    flake-checker = {
      url = "github:DeterminateSystems/flake-checker";
      inputs = {
        nixpkgs = {
          follows = "nixpkgs";
        };
      };
    };
    flake-parts = {
      url = "github:hercules-ci/flake-parts";
      inputs = {
        nixpkgs-lib = {
          follows = "nixpkgs";
        };
      };
    };
    nixpkgs = {
      url = "github:NixOS/nixpkgs/nixos-26.05";
    };
    pre-commit-hooks = {
      url = "github:cachix/git-hooks.nix";
      inputs = {
        nixpkgs = {
          follows = "nixpkgs";
        };
      };
    };
    systems = {
      url = "github:nix-systems/default";
    };
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs = {
        nixpkgs = {
          follows = "nixpkgs";
        };
      };
    };
    # keep-sorted end
  };

  outputs =
    {
      flake-parts,
      systems,
      flake-checker,
      ...
    }@inputs:
    flake-parts.lib.mkFlake { inherit inputs; } (
      {
        inputs,
        lib,
        ...
      }:
      {
        systems = import systems;
        imports = [
          flake-parts.flakeModules.easyOverlay
          inputs.pre-commit-hooks.flakeModule
          inputs.treefmt-nix.flakeModule
        ];

        perSystem =
          {
            system,
            pkgs,
            config,
            ...
          }:
          let
            treefmtBuild = config.treefmt.build;
          in
          {
            devShells = {
              default = pkgs.mkShell {
                PFPATH = "${
                  pkgs.buildEnv {
                    name = "zsh-comp";
                    paths = config.devShells.default.nativeBuildInputs;
                    pathsToLink = [ "/share/zsh" ];
                  }
                }/share/zsh/site-functions";
                packages = with pkgs; [
                  esphome
                  clang-tools
                ];
                inputsFrom = [
                  config.pre-commit.devShell
                  treefmtBuild.devShell
                ];
              };
            };
            pre-commit = {
              check = {
                enable = true;
              };
              settings = {
                src = ./.;
                hooks = {
                  # keep-sorted start block=yes
                  flake-checker = lib.optionalAttrs (inputs.flake-checker ? packages) {
                    enable = true;
                    package = flake-checker.packages.${system}.flake-checker;
                  };
                  staticcheck = {
                    enable = true;
                  };
                  treefmt = {
                    enable = true;
                    packageOverrides = {
                      treefmt = treefmtBuild.wrapper;
                    };
                  };
                  # keep-sorted end
                };
              };
            };
            formatter = treefmtBuild.wrapper;
            treefmt = {
              projectRootFile = "flake.nix";
              flakeCheck = false;
              programs = {
                # keep-sorted start block=yes
                clang-format = {
                  enable = true;
                };
                clang-tidy = {
                  enable = true;
                };
                deadnix = {
                  enable = true;
                };
                keep-sorted = {
                  enable = true;
                };
                nixfmt = {
                  enable = true;
                };
                statix = {
                  enable = true;
                };
                # keep-sorted end
              };
            };
          };
      }
    );
}
