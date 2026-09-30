rm -rf build
cmake -S . -B build -G Ninja \
  -DPICO_BOARD=pico_w \
  -DPICOTOOL_FORCE_FETCH_FROM_GIT=ON

cmake --build build --parallel