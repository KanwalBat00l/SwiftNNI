# KairosSNNI — Edge Node & Toy Deployment Guide

## 1. Slurm Allocations

### Edge Node (24 cores, 64GB — Phase 1 Target)
```bash
salloc \
  --job-name=Edge_Node \
  --nodes=2 \
  --ntasks-per-node=1 \
  --cpus-per-task=24 \
  --mem=64G \
  --time=00:10:00 \
  --partition=genoa
# NO --exclusive flag
```




### Toy Node (8 cores, 16GB — Development/Testing)
```bash
salloc \
  --job-name=Kairos_Toy \
  --nodes=1 \
  --ntasks-per-node=1 \
  --cpus-per-task=8 \
  --mem=16G \
  --time=00:10:00 \
  --partition=staging
# NO --exclusive flag
```

### GPU Config (A100)
```bash
salloc \
  --partition=gpu_a100 \
  --gpus=1 \
  --cpus-per-task=16 \
  --mem=64G \
  --time=00:00:30
```

---

## 2. Build & Run

```bash
# 1. Compile Server and Client
make all

# 2. Run the server with toy settings in the background
./Kairos_server -c toy_config.cfg -p toy_profile.cfg &

# 3. Execute the workload script
chmod +x toy_run.sh
./toy_run.sh
```

### Edge Node (production config)
```bash
./Kairos_server -c config.cfg -p profile.cfg &
```


---

## 3. Edge Server Specifications (Target Profiles)

| Feature | Dell PowerEdge XR8000 | HPE Edgeline EL8000 | NVIDIA MGX (Edge Config) |
|---------|----------------------|---------------------|--------------------------|
| Typical CPU | 1x Intel Xeon Scalable (4th/5th Gen) | 1x Intel Xeon (Ice Lake/Sapphire Rapids) | NVIDIA Grace (72-core) or Xeon 6 |
| Cores | Up to 32 physical cores | 16 to 32 cores per blade | 64 to 72 cores |
| RAM | Up to 512 GB DDR5 | Up to 768 GB DDR4/DDR5 | 128 GB to 480 GB (Unified) |
| Use Case | Ruggedized AI, 5G vRAN | Telco Edge, Video Analytics | Generative AI at the Edge |

---

## 4. Toy Simulation (2-node, genoa)
```bash
salloc \
  --job-name=Kairos_Toy_sim \
  --nodes=2 \
  --ntasks-per-node=1 \
  --cpus-per-task=8 \
  --mem=16G \
  --time=00:20:00 \
  --partition=genoa
```
