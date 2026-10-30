# Development HTTP benchmark results

These are development measurements, not a production ranking. The workstation
was also compiling during the run, and current tgpp was unoptimized while the
archived baseline and tgbot-cpp were Release builds. Each row is the median of
three independent processes making 128 requests, including initial connection
setup, against one HTTP/1.1 loopback simulator with a 50 ms response delay.

| Client | Concurrency | Requests/s | p95 latency (ms) | Observed peak threads |
| --- | ---: | ---: | ---: | ---: |
| tgpp-baseline / sync | 4 | 76.8 | 55.9 | unmeasured |
| tgpp / sync | 128 | 1,497.4 | 65.0 | 129 |
| tgpp / async | 128 | 1,389.7 | 86.1 | 8 |
| tgbot-cpp / sync | 128 | 1,526.5 | 74.5 | 129 |

The async configuration achieved approximately 18.1x the throughput of the
archived four-worker configuration on this simulated I/O workload. This primarily
demonstrates increased network concurrency with bounded application threads.
At equal concurrency, results vary by configuration and build mode; these samples
do not establish that tgpp outperforms the other libraries. All measured calls
succeeded. The async transport's separate reserved polling slot is excluded from
the requested general-transfer concurrency, so the compared clients have equal
capacity for the measured calls.

The async client used eight observed process threads at concurrency 1 through
128; current synchronous tgpp and tgbot-cpp reached 129 at concurrency 128.
Thread counts include runtime/library threads and are specific to this host.
Peak RSS/CPU samples are retained in the raw data, but build differences and
process-launch RSS baselines make memory/CPU rankings unsuitable here.

TGBM at `7456033b00651440870987d313b6ebeed30f5f2d` could not be configured
with automatic dependency fetching disabled: required upstream dependency targets
and a configuration helper were unavailable. Its default dependency bootstrap
would download Boost and was intentionally not used on this host.
`egorpugin/tgbot` at `000fc275f8197693b0cfe2924628a38c77aaa963` could not
be built because its SW build tool was absent. Neither library was ranked.

The archived tgpp revision is `8759ad3951208b5c19b3bec38bbb327353b97de1`;
tgbot-cpp is `bff3045898dfb4a8af624e58552dab9cd3087c1f`. Full configuration
sweeps, exact compiler, source hashes, errors, build modes, and limitations are
in [the raw result file](results/development-linux.json). See [README](README.md)
for commands to repeat the experiment. A controlled all-Release run with longer
steady-state workloads remains necessary before making performance claims.
