# MIGRATED TO <a href="https://github.com/jerryq0101/rt_tracer">ORIGINAL REPO</a>

This is a low overhead kernel module for quickly identifying worst case scheduler latency, response time, and IRQ handling delays on PREEMPT / PREEMPT_RT Linux.

This is specifically designed for and evaluted on Raspberry Pi 4 (SMP) running Buildroot's PREEMPT Linux.
- Buildroot 2025.11-git
- Any Raspberry Pi 4 compatible Linux kernel with the PREEMPT or PREEMPT_RT model.
    - My OS: Linux buildroot 6.12.41-v8 #1 SMP PREEMPT Thu Nov 20 17:18:47 UTC 2025 aarch64 GNU/Linux

Note: Despite the name, the current evaluation was performed on a PREEMPT (low latency) kernel rather than PREEMPT_RT. As a result, some wording in repo/writeup.pdf may refer to PREEMPT_RT semantics imprecisely.

The module is kernel agnostic with respect to scheduling model. It observes scheduler and interrupt tracepoints that are present across Linux configurations, and reports timing relationships between events (e.g., wakeup → run, run → sleep).

As a result, it can be used on PREEMPT, PREEMPT_RT, and non-RT kernels to diagnose scheduling behavior. Kernel configuration affects absolute latency values and tail behavior, but does not change the semantics of the metrics being collected. Full evaluation on PREEMPT_RT is left as future work. 

Additionally, the IRQ handler response time metric is defined to align with PREEMPT_RT semantics, where IRQ handlers execute in threaded context.

## Motivation

Debugging real time scheduling issues on Linux often requires collecting large kernel traces and manually inspecting them with tools such as trace-cmd or kernelshark. This process is time consuming and makes it difficult to quickly identify which task or event is responsible in a scheduling induced case of delay.

This project aims to provide a lightweight alternative: track worst case latency metrics per PID directly in the kernel, along with a best effort scheduler trace for the worst observed case. This aims to have minimal interference to the running workload.

## Features

- Tracks per pid worst case metrics: Schedule in latency, Voluntary sleep response time (wake -> first voluntary sleep), control loop relief time (wake -> timer sleep), IRQ threaded handler execution time
- Records best effort scheduler traces for worst case metrics (need to add with trace and have SLO for the specific metric)
- Allows configuration of a SLO for each metric and records number of violations of SLO

## Design highlights
- CAS loops used for max/min updates to avoid global locks
- Trace collection protected by per-metric spinlocks
- Best-effort trace semantics: correctness of numeric metrics prioritized over complete trace capture

## Installation

This project is designed to run on a Raspberry Pi 4 with a PREEMPT_RT enabled Linux kernel. I used PREEMPT in my evaluations, however, installation and functionality remain the same.

### 1. Build a PREEMPT_RT kernel for Raspberry Pi 4

1. Install Buildroot and select an existing Raspberry Pi 4 configuration in menuconfig
2. Enable a PREEMPT_RT kernel variant in the Buildroot kernel configuration (linux-menuconfig).
3. Apply the kernel patch provided in this repository:
   - Patch location: `this-repo/jerry/`
   - Add the patch according to Buildroot instructions (in 2025 Dec, I moved the `jerry` folder into `buildroot/board/` and ran `make`)
4. Build the full system image using Buildroot (`make`).

Once completed, flash the generated image onto an SD card and boot it on the Raspberry Pi 4. The system should now be running Linux with PREEMPT_RT enabled.


> Note: If you already have a PREEMPT_RT kernel running on a Pi 4, you
> may skip the Buildroot steps and proceed directly to module installation.

### 2. Build and load the kernel module

1. Build the kernel module (`.ko`) as part of the Buildroot build process.

   Create a new package folder `jerry_rt_module` in `buildroot/package/` and populate with the necessary `.mk`, `Makefile`, `Config.in` files, and reference the `jerry_rt_module/Config.in`'s name from the `package/Config.in` to register it in menuconfig. (Reference Buildroot's process). My module files are all here in the repo (jerry_rt_module.mk, Makefile, rt_main.c, trace_ring_buffer.c/h, Config.in)
   
   Do `make` after the package is setup in buildroot. The module is compiled against the same kernel source tree used to build the kernel, ensuring ABI compatibility.

   After the first `make`, to recompile the module, just do `make jerry_rt_module-rebuild` at `buildroot/`. Don't forget to enable the compilation of it in `menuconfig`.

2. Copy the resulting module .ko binary to the Pi 4 (e.g. via USB storage or SCP).
    
    This is in the `/buildroot/output/build/[module]-ver/` folder.

    For me, at `/buildroot/output/build/jerry_rt_module-1.0/jerry_rt_module.ko`

3. Load the module on the Pi:
    ```bash
    insmod jerry_rt_module.ko
    ```

### 3. IRQ demo module

If you would like to test out tracking IRQ handlers and have a pushup button, you may install this module. This is installed in a similar fashion as above (`repo/irq_button_demo` is the module folder). 

Loading the module connects GPIO 17 pin on the Pi 4 to hard, soft IRQ handler functions defined in the module. (i.e. Signalling GPIO 17 triggers those functions).

> Note: While kernel modules can also be built against installed kernel headers
> on the target system, this project builds the module directly within
> Buildroot, which is the recommended approach for embedded systems.

## Usage

Remark that the module provides per PID real time scheduling diagnostics, including:
- Max/min latency and response metrics
- SLO (upper bound) violation counting
- Worst case trace capture for debugging scheduling anomalies

All interaction is done via sysfs after loading the kernel module.

### Loading the module
```bash
insmod jerry_rt_module.ko
```
Replace `jerry_rt_module.ko` with the full path to the module binary.

After this step, the module is loaded but no PIDs are tracked yet.

### Track a PID

You can track a PID with or without trace capture.

<b>Track without trace</b>
```
echo "123" > /sys/kernel/jerry_rt_module/add_pid
```

<b>Track with trace enabled</b>
```
echo "123" > /sys/kernel/jerry_rt_module/add_pid
```

Tracking with trace enables worst-case scheduler trace collection when SLO violations occur.

### Configure SLO Bounds (Violations + Trace)

After tracking a PID, you can configure upper bounds (SLOs) for specific metrics. Violations are counted whenever the metric exceeds the configured bound. 

<b>Example: Latency bound (nanoseconds)</b>
```bash
echo "123 5000" > /sys/kernel/jerry_rt_module/set_slo_latency_bound
```
Format:
```bash
echo "[pid] [bound_in_ns]" > /sys/kernel/jerry_rt_module/set_slo_XXXXX_bound
```

<b>Other supported SLOs</b>
- `set_slo_response_bound`
- `set_slo_response_relief_bound`
- `set_slo_irq_handling_bound`

<i>Note: If tracing was enabled when adding the PID, the trace desired metric's corresponding SLO must be set for a violation trace to be collected. </i>

### Stop Tracking

<b>Remove a single PID</b>
```bash
echo "123" > /sys/kernel/jerry_rt_module/del_pid
```
This:
- Stops tracking the PID
- Prints all accumulated statistics and traces to the kernel log (can view using dmesg)

<br></br>
<b>Remove the module (stop all tracking)</b>
```bash
rmmod jerry_rt_module.ko
```
This:
- Stops tracking all PIDs
- Prints statistics for all tracked PIDs
- Unloads the module


<br/>

---

<br/>
<b>Automatic cleanup on process exit</b>
If a tracked process exits normally, the module automatically emits its final statistics using the `sched_process_exit` tracepoint.

<br/>

---

### Complete Example
```bash
# Start tracking PID 22543 with tracing
echo "22543 trace" > /sys/kernel/jerry_rt_module/add_pid

# Set SLO bounds (ns)
echo "22543 2" > /sys/kernel/jerry_rt_module/set_slo_latency_bound
echo "22543 2" > /sys/kernel/jerry_rt_module/set_slo_response_bound
echo "22543 2" > /sys/kernel/jerry_rt_module/set_slo_response_relief_bound

# Let workload run...

# Stop tracking and emit statistics
echo "22543" > /sys/kernel/jerry_rt_module/del_pid
```


---
### Example Output

```bash
jerry_rt_module: PID=22543, Name=background_bloc, minLat=3519, maxLat=25482,
minResponseTimeVoluntarySleepAllTypes=1017797, maxResponseTimeVoluntarySleepAllTypes=1059093, 
minResponseTimeVoluntarySleepReliefBased=9223372036854775807, maxResponseTimeVoluntarySleepReliefBased=0
  LatencyBound=2, Violations=19434
MAX TRACE LEN: 3
WORST LATENCY TRACE: 
[     0 us] Event: sched_wakeup, pid: 22543, wake_cpu: 1
[     3 us] Event: sched_switch, preemption: 0, voluntary: 1, prev_pid: 22544 (priority: 49), next_pid: 0 (priority: 120), on_cpu: 2, 
[233048.368766] [    25 us] Event: sched_switch, preemption: 0, voluntary: 0, prev_pid: 0 (priority: 120), next_pid: 22543 (priority: 69), on_cpu: 1, 
  ResponseBound=2, Violations=18267
MAX TRACE LEN: 3
WORST RESPONSE TIME TRACE: 
[     0 us] Event: sched_wakeup, pid: 22543, wake_cpu: 1
[ =    7 us] Event: sched_switch, preemption: 0, voluntary: 0, prev_pid: 0 (priority: 120), next_pid: 22543 (priority: 69), on_cpu: 1, 
[  1059 us] Event: sched_switch, preemption: 0, voluntary: 1, prev_pid: 22543 (priority: 69), next_pid: 0 (priority: 120), on_cpu: 1, 
  ResponseReliefBound=2, Violations=0
MAX TRACE LEN: 0
WORST RESPONSE RELIEF TIME TRACE: 
```

Traces help identify why a worst-case occurred (CPU, preemption, competing tasks, etc.).

---

### Control Plane Precision

The module intentionally does not strictly synchronize userspace control commands with in flight tracepoint execution.

This means the following boundaries are not precisely defined:
- When tracking exactly begins after add_pid
- When SLO bounds exactly take effect
- When tracking exactly stops after del_pid

Why this happens
- Tracepoint handlers execute asynchronously on other CPUs
- Control plane updates (echo commands) are not synchronized against them
- Writes become visible eventually, but not at a deterministic instant

Practical impact
- An initial latency cycle may be missed after add_pid
- A small number of events may still be counted after del_pid
- SLO checks may briefly use the old or new bound

This behavior is intentional.

The design prioritizes minimal runtime interference over precise control plane boundaries. A future version may tighten this window (e.g., using xchg on PID addition), at the cost of slightly higher overhead.


## Conclusion and Further reading

This module is intended as a practical, low-overhead tool for quickly assessing real time scheduling behavior on PREEMPT_RT Linux systems. Rather than replacing full tracing frameworks, it complements them by providing immediate visibility into worst case behavior with minimal disruption to the running workload.

For readers interested in deeper details, including:
- design rationale and tradeoffs
- concurrency and synchronization guarantees
- trace correctness semantics
- overhead evaluation and RT impact analysis
- space/memory considerations

A full technical writeup is available: `repo/writeup.pdf`

That document expands on the internal mechanisms, explains known limitations, and motivates the design choices made to balance observability with real-time safety.

`case_studies` and `sanity_check_workloads` are workloads used for the full writeup.

## Future Work

- Evaluations on a PREEMPT_RT kernel
- Potential races that I haven't caught probably exist
- To allow the use of tgids rather than pids, as there isn’t support for threads spawned under a pid yet.
- To allow for easier IRQ tracing setup without doing a kernel patch.
- Finding a way to use less memory when running the module for more strict embedded environments.

Contributions, feedback, and discussion are welcome (jerryq[zero one zero one with no spaces]@[jee mail like the popular one].com)
