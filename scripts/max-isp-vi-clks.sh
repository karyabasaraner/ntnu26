#!/bin/bash
#
# Set ISP and VI clocks to maximum and lock them.
#

set -euo pipefail

run_root() {
  if [[ "${EUID}" -eq 0 ]]; then
    "$@"
  else
    sudo "$@"
  fi
}

write_root() {
  local value="$1"
  local path="$2"
  if [[ "${EUID}" -eq 0 ]]; then
    printf '%s\n' "${value}" > "${path}"
  else
    printf '%s\n' "${value}" | sudo tee "${path}" >/dev/null
  fi
}

run_root nvpmodel -m 0
echo "Ran nvpmodel"
run_root jetson_clocks
echo "Ran jetson_clocks"

MAX_VI_RATE=$(run_root cat /sys/kernel/debug/bpmp/debug/clk/vi/max_rate)
MAX_ISP_RATE=$(run_root cat /sys/kernel/debug/bpmp/debug/clk/isp/max_rate)
MAX_NVCSI_RATE=$(run_root cat /sys/kernel/debug/bpmp/debug/clk/nvcsi/max_rate)
MAX_VIC_RATE=$(run_root cat /sys/kernel/debug/bpmp/debug/clk/vic/max_rate)
MAX_EMC_RATE=$(run_root cat /sys/kernel/debug/bpmp/debug/clk/emc/max_rate)

write_root "${MAX_VI_RATE}" /sys/kernel/debug/bpmp/debug/clk/vi/rate
write_root "${MAX_ISP_RATE}" /sys/kernel/debug/bpmp/debug/clk/isp/rate
write_root "${MAX_NVCSI_RATE}" /sys/kernel/debug/bpmp/debug/clk/nvcsi/rate
write_root "${MAX_VIC_RATE}" /sys/kernel/debug/bpmp/debug/clk/vic/rate
write_root "${MAX_EMC_RATE}" /sys/kernel/debug/bpmp/debug/clk/emc/rate

write_root 1 /sys/kernel/debug/bpmp/debug/clk/vi/mrq_rate_locked
write_root 1 /sys/kernel/debug/bpmp/debug/clk/isp/mrq_rate_locked
write_root 1 /sys/kernel/debug/bpmp/debug/clk/nvcsi/mrq_rate_locked
write_root 1 /sys/kernel/debug/bpmp/debug/clk/vic/mrq_rate_locked
write_root 1 /sys/kernel/debug/bpmp/debug/clk/emc/mrq_rate_locked

echo "Maximum VI clock set : ${MAX_VI_RATE}"
echo "Maximum ISP clock set : ${MAX_ISP_RATE}"
echo "Maximum NVCSI clock set : ${MAX_NVCSI_RATE}"
echo "Maximum VIC clock set : ${MAX_VIC_RATE}"
echo "Maximum EMC clock set : ${MAX_EMC_RATE}"
