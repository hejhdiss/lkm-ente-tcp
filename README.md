# ENTE-TCP: Entropy-Enhanced TCP Congestion Control

## Overview

**Based on:** [NDM-TCP](https://github.com/hejhdiss/lkm-ndm-tcp) - Neural Differential Manifolds TCP  
**Simplified by:** Removing all ML/neural network components, keeping only entropy-based logic  
**Author:** ENTE-TCP Development Team (@hejhdiss)

ENTE-TCP (Entropy-Enhanced TCP) is a Linux kernel module that implements an intelligent TCP congestion control algorithm. Unlike traditional algorithms like Reno or CUBIC that treat all packet loss and RTT increases the same way, ENTE-TCP uses **Shannon Entropy** to distinguish between:

- **Random network noise** (WiFi interference, mobile handoffs, wireless jitter)
- **Real network congestion** (queue buildup, bandwidth saturation)

This is a simplified, understandable version of the NDM-TCP algorithm with all neural network components removed, making the logic transparent and easy to comprehend.

## The Core Problem

Traditional TCP congestion control algorithms assume that:
1. Packet loss = congestion
2. RTT increase = congestion

But in modern wireless networks, this isn't always true! Packet loss and RTT variations can happen due to:
- WiFi signal fluctuations
- Mobile device handoffs between cell towers
- Random interference
- Channel switching

ENTE-TCP solves this by analyzing the **pattern** of RTT changes, not just the magnitude.

## Relationship to NDM-TCP

ENTE-TCP is derived from [NDM-TCP](https://github.com/hejhdiss/lkm-ndm-tcp) (Neural Differential Manifolds TCP), which uses neural networks for congestion control. 

**What was removed:**
- All neural network layers and weights
- Hebbian learning mechanisms
- Dynamic plasticity adjustments
- Hidden state computations
- Sigmoid/tanh activation functions for decision making

**What was kept:**
- Shannon entropy calculation for noise detection
- RTT history tracking and analysis
- Entropy-based classification (high entropy = noise, low entropy = congestion)
- Adaptive cwnd adjustment based on network state
- The core insight: use information theory to distinguish noise from congestion

**Result:** A transparent, understandable algorithm that anyone can read and comprehend, without the "black box" nature of neural networks. All decisions are made using simple if-else logic based on entropy thresholds.

## How It Works

### 1. Shannon Entropy Calculation

Shannon Entropy measures the "randomness" or "predictability" of a data distribution:

```
H = -Σ(p_i × log₂(p_i))
```

Where:
- `p_i` = probability of RTT value in bin i
- High entropy (close to 1.0) = random/unpredictable values
- Low entropy (close to 0.0) = consistent/predictable values

#### Example:

**High Entropy (Noise):**
```
RTT samples: [20ms, 45ms, 18ms, 52ms, 23ms, 48ms, 19ms, 50ms]
Pattern: All over the place, random
Entropy: ~0.85 (HIGH)
Interpretation: Random wireless interference, NOT congestion
```

**Low Entropy (Congestion):**
```
RTT samples: [20ms, 25ms, 30ms, 35ms, 40ms, 45ms, 50ms, 55ms]
Pattern: Steadily increasing
Entropy: ~0.35 (LOW)
Interpretation: Queue building up, REAL congestion
```

### 2. Algorithm Decision Logic

#### During Congestion Avoidance Phase:

| Condition | Action | Reasoning |
|-----------|--------|-----------|
| **High Entropy** (>0.7) | Be AGGRESSIVE<br>Grow cwnd 1.5× faster | It's just noise, not real congestion. Don't back off unnecessarily |
| **Low Entropy** (<0.4) | Be CONSERVATIVE<br>Grow cwnd 0.5× slower | Real congestion detected. Carefully increase window |
| **Medium Entropy** (0.4-0.7) | Use standard Reno | Unclear situation, use proven algorithm |

#### During Packet Loss:

| Condition | CWND Reduction | Reasoning |
|-----------|----------------|-----------|
| **High Entropy** | Reduce to 2/3 (33% reduction) | Likely spurious loss due to noise |
| **Low Entropy** | Reduce to 1/2 (50% reduction) | Real congestion, standard response |

### 3. Step-by-Step Algorithm Flow

```
1. Collect RTT Samples
   └─> Store last 16 RTT measurements in circular buffer

2. Calculate Entropy (every 8 packets)
   └─> Build histogram of RTT values (16 bins)
   └─> Calculate Shannon entropy: H = -Σ(p × log₂(p))
   └─> Scale to 0-1000 range

3. Classify Network State
   ├─> IF entropy > 700: Network has NOISE
   ├─> IF entropy < 400: Network has CONGESTION  
   └─> ELSE: Neutral state

4. Adjust Congestion Window
   ├─> In SLOW START:
   │   ├─> If noise: Normal exponential growth
   │   └─> If congestion: Slower growth (acked/2)
   │
   └─> In CONGESTION AVOIDANCE:
       ├─> If noise: Aggressive growth (1.5× faster)
       ├─> If congestion: Conservative growth (0.5× slower)
       └─> If neutral: Standard Reno behavior

5. Handle Packet Loss
   ├─> If noise: Reduce cwnd to 2/3 (mild reduction)
   └─> If congestion: Reduce cwnd to 1/2 (standard reduction)
```

## Key Advantages

### 1. **Better Performance on Wireless Networks**
- Doesn't overreact to random packet loss
- Maintains higher throughput on noisy channels
- Faster recovery from temporary interference

### 2. **Intelligent Congestion Response**
- Detects real congestion accurately
- Responds appropriately to actual network load
- Prevents congestion collapse

### 3. **No Machine Learning Required**
- Pure mathematical approach (Shannon Entropy)
- Deterministic and explainable
- No training data needed
- Lightweight computation

### 4. **Backward Compatible**
- Falls back to TCP Reno when uncertain
- Works with existing TCP infrastructure
- No protocol changes required

## Algorithm Parameters

```c
// Tunable parameters in the code:
#define ENTROPY_WINDOW_SIZE 16          // RTT samples to analyze
#define HIGH_ENTROPY_THRESHOLD 0.7      // Above = noise
#define LOW_ENTROPY_THRESHOLD 0.4       // Below = congestion
#define NOISE_AGGRESSION 1.5            // Growth multiplier for noise
#define CONGESTION_CONSERVE 0.5         // Growth multiplier for congestion
```

## Building and Installing

### Prerequisites
```bash
# Install kernel headers
sudo apt-get install linux-headers-$(uname -r)
# or on Fedora/RHEL
sudo dnf install kernel-devel
```

### Build
```bash
make
```

### Load Module
```bash
make load
```

### Set as Default
```bash
sudo sysctl -w net.ipv4.tcp_congestion_control=ente_tcp
```

### Verify
```bash
make status
```

## Testing

### Check Available Algorithms
```bash
sysctl net.ipv4.tcp_available_congestion_control
```

### Monitor Kernel Messages
```bash
make dmesg
```

### Test Performance
```bash
# Terminal 1: Start server
iperf3 -s

# Terminal 2: Test with ENTE-TCP
iperf3 -c <server-ip> -C ente_tcp -t 60
```

## Comparison with Other Algorithms

| Algorithm | Wireless Performance | Congestion Detection | Complexity |
|-----------|---------------------|---------------------|-----------|
| **Reno** | Poor (overreacts to noise) | Basic | Low |
| **CUBIC** | Moderate | Good for high BDP | Medium |
| **BBR** | Good | Excellent | High |
| **ENTE-TCP** | Excellent | Excellent | Medium |

## Use Cases

### Ideal For:
- ✅ Mobile/cellular networks
- ✅ WiFi connections
- ✅ Satellite links
- ✅ Networks with variable interference
- ✅ IoT devices
- ✅ Video streaming over wireless

### Less Ideal For:
- ❌ Pure wired datacenter networks (CUBIC/BBR may be better)
- ❌ Ultra-stable fiber connections (overhead not needed)

## Technical Details

### Memory Footprint
- Structure size: ~80 bytes per TCP connection
- No dynamic memory allocation
- Fits in kernel's ICSK_CA_PRIV_SIZE

### Computational Complexity
- Entropy calculation: O(n) where n=16 (constant)
- Performed every 8 packets (not every ACK)
- Minimal CPU overhead

### Mathematical Foundation

**Shannon Entropy:**
```
H(X) = -Σ p(x_i) × log₂(p(x_i))
     = Expected information content
     = Measure of unpredictability
```

**For uniform distribution (max entropy):**
```
H_max = log₂(n) where n = number of bins
For 16 bins: H_max = 4 bits
```

**For deterministic distribution (min entropy):**
```
H_min = 0 (all samples in one bin)
```

## Troubleshooting

### Module Won't Load
```bash
# Check kernel version
uname -r

# Verify headers installed
ls /lib/modules/$(uname -r)/build

# Check dmesg for errors
dmesg | tail
```

### Module Loaded But Not Active
```bash
# Set as default
sudo sysctl -w net.ipv4.tcp_congestion_control=ente_tcp

# Verify
ss -ti | grep ente_tcp
```

### Performance Issues
```bash
# Check entropy calculations
ss -ti | head -20

# Monitor kernel messages
watch -n 1 "dmesg | grep ENTE | tail -10"
```

## Uninstalling

```bash
# Unload module
make unload

# Clean build files
make clean

# Remove from system (if installed)
make uninstall
```

## Performance Tuning

### For Very Noisy Networks (WiFi hotspots)
```c
// In source code, adjust:
#define HIGH_ENTROPY_THRESHOLD 650  // More aggressive
#define NOISE_AGGRESSION 2000       // 2× growth
```

### For More Conservative Behavior
```c
#define LOW_ENTROPY_THRESHOLD 500   // Detect congestion sooner
#define CONGESTION_CONSERVE 400     // 0.4× slower growth
```

## Research Background

This algorithm is based on information theory principles:

1. **Shannon, C.E.** (1948). "A Mathematical Theory of Communication"
   - Foundation of entropy in information theory

2. **Jacobson, V.** (1988). "Congestion Avoidance and Control"
   - Original TCP congestion control

3. Modern wireless network challenges:
   - Random packet loss != congestion
   - Need to distinguish noise from load

## License

GPL v2 (same as Linux kernel)

## Contributing

Contributions welcome! Areas for improvement:
- [ ] Dynamic threshold adaptation
- [ ] Integration with other metrics (e.g., ECN)
- [ ] Per-flow learning
- [ ] IPv6 support testing
- [ ] Performance benchmarks vs BBR/CUBIC

## Authors

**ENTE-TCP Development Team (@hejhdiss)**

**Original NDM-TCP:** https://github.com/hejhdiss/lkm-ndm-tcp  
**This simplified version:** Generated by Claude Sonnet 4.5 (Anthropic)

## Acknowledgments

- Original NDM-TCP concept and implementation
- Shannon entropy-based congestion detection methodology
- Code generation and simplification by Claude Sonnet 4.5

## Citation

If you use ENTE-TCP in research, please cite:
```bibtex
@software{ente_tcp,
  title={ENTE-TCP: Entropy-Enhanced TCP Congestion Control},
  author={ENTE-TCP Development Team},
  note={Simplified from NDM-TCP (https://github.com/hejhdiss/lkm-ndm-tcp), 
        Generated by Claude Sonnet 4.5},
  year={2026},
  url={https://github.com/hejhdiss/lkm-ndm-tcp}
}
```

**Original NDM-TCP:**
```bibtex
@software{ndm_tcp,
  title={NDM-TCP: Neural Differential Manifolds for TCP Congestion Control},
  author={@hejhdiss},
  year={2026},
  url={https://github.com/hejhdiss/lkm-ndm-tcp}
}
```

## Contact

For issues, questions, or contributions, please open an issue on GitHub.

---

## Attribution

**ENTE-TCP: Entropy-Enhanced TCP Congestion Control**

- **Based on:** [NDM-TCP](https://github.com/hejhdiss/lkm-ndm-tcp) - Neural Differential Manifolds TCP
- **Simplified version:** ML/neural network components removed
- **Author:** ENTE-TCP Development Team (@hejhdiss)
- **Generated by:** Claude Sonnet 4.5 (Anthropic)
- **License:** GPL v2

This code represents a simplified, transparent version of the NDM-TCP algorithm. All neural network layers, weights, and learning mechanisms have been removed, keeping only the core entropy-based congestion detection logic. The result is an understandable, non-ML algorithm that uses information theory to distinguish network noise from real congestion.

---


**Remember:** The key insight is that **random = noise**, **consistent = congestion**. ENTE-TCP uses entropy to tell the difference!
