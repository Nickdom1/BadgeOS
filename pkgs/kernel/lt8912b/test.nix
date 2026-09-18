# Execute the patched bridge functions, never hardware or a duplicate model.
{ pkgs, kernel }:
let
  moduleSource = import ../../video/sophgo-dsi/source.nix { inherit pkgs; };
in
pkgs.stdenv.mkDerivation {
  pname = "lt8912b-test";
  version = kernel.version;
  src = kernel.src;
  patches = [ ../patches/lt8912b-lifecycle.patch ];
  nativeBuildInputs = [ pkgs.python3 ];
  dontConfigure = true;
  buildPhase = ''
    python3 ${./extract-lifetime.py}
    $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O2 -fsanitize=undefined,bounds \
      -fno-sanitize-recover=all -I. ${./test-lifetime.c} -o test-lifetime
    python3 ${./extract.py} ${./golden-registers.json}
    $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 -fsanitize=undefined,bounds \
      -fno-sanitize-recover=all -I. -Iinclude ${./test.c} -o test-bridge
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 -I. ${./test-remove.c} -o test-remove
    python3 ${../../video/sophgo-dsi/extract-chain.py}
    python3 ${../../video/sophgo-dsi/extract-host.py} ${moduleSource}/interdrv/sophgo-dsi/sophgo-dsi.c
    mkdir -p shim/linux
    cp ${../../video/sophgo-dsi/test-iopoll.h} shim/linux/iopoll.h
    cp ${../../video/sophgo-dsi/test-ktime.h} shim/linux/ktime.h
    cp ${../../video/sophgo-dsi/test-delay.h} shim/linux/delay.h
    $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers -Wno-maybe-uninitialized -O2 \
      -I. -Ishim -Iinclude -I${moduleSource}/interdrv/sophgo-dsi -I${./.} -I${../../video/sophgo-dsi} \
      ${../../video/sophgo-dsi/test-pipeline.c} ${../../video/sophgo-dsi/test-pipeline-bridge.c} \
      ${../../video/sophgo-dsi/test-chain.c} -o test-pipeline
    ./test-pipeline | tee result.txt
    ./test-bridge | tee -a result.txt
    ./test-remove | tee -a result.txt
    ./test-lifetime lifecycle | tee -a result.txt
  '';
  installPhase = ''
    mkdir -p "$out"
    cp result.txt "$out/"
  '';
}
