# Build with debug info (add to CMakeLists.txt or build flags):
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..

# Run your app under perf:
perf record -g ./your_app

# Analyze results:
perf report

# Or flame graph (visual):
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg

With no incoming signal
79% of CPU is in rade_acq_detect_pilots

