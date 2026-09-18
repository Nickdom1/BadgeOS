# Execute the patched production provider with fake MMIO, never badge access.
{ pkgs, kernel }:
pkgs.stdenv.mkDerivation {
  pname = "cv18xx-pll-test";
  version = kernel.version;
  src = kernel.src;
  patches = [ ../patches/cv18xx-pll.patch ];
  nativeBuildInputs = [ pkgs.python3 ];
  dontConfigure = true;
  buildPhase = ''
    mkdir -p shim/linux
    cp ${./test-shim.h} shim/test-shim.h
    for header in clk-provider io limits spinlock compiler bitfield iopoll bug; do
      echo '#include "test-shim.h"' > "shim/linux/$header.h"
    done
    sed -n '/^static const struct cv1800_clk_pll_limit pll_limits\[/,/^};/p' \
      drivers/clk/sophgo/clk-cv1800.c > limits.inc
    sed -n '/^static struct cv1800_clk_pll_synthesizer clk_disppll_synthesizer = {/,/^};/p' \
      drivers/clk/sophgo/clk-cv1800.c >> limits.inc
    python3 ${./extract-ccf.py}
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 -fsanitize=undefined,bounds \
      -fno-sanitize-recover=all -Ishim -I. -Iinclude -Idrivers/clk/sophgo \
      -I${./.} ${./test-pll.c} -o test-pll
    ./test-pll > result.txt
    python3 ${./extract-descriptors.py}
    $CC -std=gnu11 -Wall -Wextra -Werror -O2 -fsanitize=undefined,bounds \
      -fno-sanitize-recover=all -Ishim -I. -Iinclude -Idrivers/clk/sophgo \
      ${./test-descriptors.c} -o test-descriptors
    ./test-descriptors >> result.txt
    cat result.txt
  '';
  installPhase = ''
    mkdir -p "$out"
    cp result.txt "$out/"
  '';
}
