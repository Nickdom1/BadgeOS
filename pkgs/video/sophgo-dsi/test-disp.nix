# Native execution of the exact header shipped in the patched kernel module.
{ pkgs }:
let
  moduleSource = import ./source.nix { inherit pkgs; };
in
pkgs.runCommand "sophgo-disp-test"
  {
    nativeBuildInputs = [
      pkgs.stdenv.cc
      pkgs.python3
    ];
  }
  ''
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 \
      -I${moduleSource}/interdrv/sophgo-dsi ${./test-disp.c} -o test-disp
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 \
      -I${moduleSource}/interdrv/sophgo-dsi ${./test-dphy.c} -o test-dphy
    mkdir -p shim/linux
    cp ${./test-iopoll.h} shim/linux/iopoll.h
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 \
      -Ishim -I${moduleSource}/interdrv/sophgo-dsi ${./test-clocks.c} -o test-clocks
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 \
      -I${moduleSource}/interdrv/sophgo-dsi ${./test-vip.c} -o test-vip
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 \
      -I${moduleSource}/interdrv/sophgo-dsi ${./test-dump.c} -o test-dump
    # MAC tests supply virtual-time polling; the kernel gate uses real iopoll.
    mkdir -p mac-shim/linux
    touch mac-shim/linux/iopoll.h
    cp ${./test-ktime.h} mac-shim/linux/ktime.h
    cp ${./test-delay.h} mac-shim/linux/delay.h
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 \
      -Imac-shim -I${moduleSource}/interdrv/sophgo-dsi ${./test-mac.c} -o test-mac
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 \
      -Ishim -I${moduleSource}/interdrv/sophgo-dsi ${./test-link.c} -o test-link
    python3 ${./extract-host.py} ${moduleSource}/interdrv/sophgo-dsi/sophgo-dsi.c
    $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 -I. \
      -I${moduleSource}/interdrv/sophgo-dsi ${./test-trace.c} -o test-trace
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 -I. ${./test-host.c} -o test-host
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 \
      -I${moduleSource}/interdrv/sophgo-dsi ${./test-graph.c} -o test-graph
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 -I. ${./test-kms.c} -o test-kms
    $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 -pthread -I. ${./test-lifetime.c} -o test-lifetime
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 -I. \
      -I${moduleSource}/interdrv/sophgo-dsi ${./test-check.c} -o test-check
    mkdir -p "$out"
    ./test-disp > "$out/result.txt"
    ./test-dphy >> "$out/result.txt"
    ./test-clocks >> "$out/result.txt"
    ./test-vip >> "$out/result.txt"
    ./test-dump >> "$out/result.txt"
    ./test-mac >> "$out/result.txt"
    ./test-link >> "$out/result.txt"
    ./test-lifetime >> "$out/result.txt"
    ./test-kms >> "$out/result.txt"
    ./test-check >> "$out/result.txt"
    ./test-trace >> "$out/result.txt"
    ./test-host >> "$out/result.txt"
    ./test-graph >> "$out/result.txt"
    cat "$out/result.txt"
  ''
