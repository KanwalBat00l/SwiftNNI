import pandas as pd
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
import struct
import os
import zlib

# ==========================================
# CONFIG PARSER
# ==========================================
def parse_profile_cfg(path):
    profiles = {}
    if not os.path.exists(path): return profiles
    with open(path, 'r') as f:
        for line in f:
            if line.startswith('#') or not line.strip(): continue
            parts = [p.strip() for p in line.split(',')]
            if len(parts) >= 9:
                key = f"{parts[0]}_{parts[1]}"
                profiles[key] = {'threads': float(parts[4]), 'inf_mem': float(parts[8])}
    return profiles

# ==========================================
# MANUAL EVALUATION METRIC (Replacing Sklearn)
# ==========================================
def calculate_r2(y_true, y_pred):
    y_true = y_true.flatten()
    y_pred = y_pred.flatten()
    ss_res = np.sum((y_true - y_pred) ** 2)
    ss_tot = np.sum((y_true - np.mean(y_true)) ** 2)
    return 1 - (ss_res / (ss_tot + 1e-8))

# ==========================================
# MODEL DEFINITION (C-19: 64-32-1 architecture)
# ==========================================
class DQNSchedulerModel(nn.Module):
    def __init__(self, input_dim):
        super(DQNSchedulerModel, self).__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, 64),   # C-19: was 16
            nn.ReLU(),
            nn.Linear(64, 32),          # C-19: was 8
            nn.ReLU(),
            nn.Linear(32, 1)            # C-19: was 8->1
        )
    def forward(self, x): return self.net(x)

# ==========================================
# FNV-1a HASH (C-20: architecture hash)
# ==========================================
def fnv1a_hash(data: bytes) -> int:
    h = 0x811c9dc5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h

# ==========================================
# MAIN PIPELINE
# ==========================================
def main():
    LOG_FILE = "logs/scheduler_log.csv"
    CFG_FILE = "profile.cfg"
    MODEL_PTH = "models/dqn_model.pth"
    WEIGHTS_BIN = "models/dqn_weights.bin"

    # FU-12: Minimum training samples threshold
    MIN_TRAIN_SAMPLES = 50000

    metadata = parse_profile_cfg(CFG_FILE)
    if not os.path.exists(LOG_FILE):
        print(f"[Error] {LOG_FILE} missing!")
        return

    # Load and Preprocess
    df = pd.read_csv(LOG_FILE, sep=';')
    df = df[df['Exit_Code'] == 0]

    # FU-12: Check minimum sample count before training
    if len(df) < MIN_TRAIN_SAMPLES:
        print(f"[FU-12] Insufficient training data: {len(df)} samples < {MIN_TRAIN_SAMPLES} required.")
        print(f"[FU-12] Waiting for more scheduling data. Collect at least {MIN_TRAIN_SAMPLES - len(df)} more jobs.")
        return

    df['Req_Threads'] = df['Model_Batch'].map(lambda x: metadata.get(x, {'threads':0})['threads'])
    df['Req_Mem'] = df['Model_Batch'].map(lambda x: metadata.get(x, {'inf_mem':0})['inf_mem'])
    df['Slack_Sec'] = (df['Requested_SLO'] - df['Total_Wait_Time']) / 1000.0

    # FU-8: Multi-objective DQN reward — R = (w1*R_SLO) + (w2*R_TAT) + (w3*R_Energy)
    w1_slo = 10.0      # SLO compliance weight
    w2_tat = 1.0        # Turnaround time efficiency weight
    w3_energy = 0.1     # Energy efficiency weight

    # Try to load weights from config file
    cfg_candidates = ['config.cfg', '../config.cfg']
    for cfg_path in cfg_candidates:
        if os.path.exists(cfg_path):
            with open(cfg_path, 'r') as cf:
                for line in cf:
                    line = line.strip()
                    if line.startswith('DQN_REWARD_W1_SLO='): w1_slo = float(line.split('=')[1])
                    elif line.startswith('DQN_REWARD_W2_TAT='): w2_tat = float(line.split('=')[1])
                    elif line.startswith('DQN_REWARD_W3_ENERGY='): w3_energy = float(line.split('=')[1])
            break

    print(f"[FU-8] Multi-objective weights: w1_slo={w1_slo}, w2_tat={w2_tat}, w3_energy={w3_energy}")

    def get_reward(row):
        # R_SLO: SLO compliance component
        r_slo = 1.0 if row['SLO_Met'] == 1 else -0.5
        if row['SLO_Diff'] < 0:
            r_slo -= abs(row['SLO_Diff']) / 1000.0  # Proportional penalty for violation

        # R_TAT: Turnaround time efficiency (lower TAT = better)
        actual_tat = row.get('Actual_TAT', 0)
        requested_slo = row.get('Requested_SLO', 1)
        r_tat = max(0.0, 1.0 - (actual_tat / max(requested_slo, 1))) if requested_slo > 0 else 0.0

        # R_Energy: Energy efficiency (lower energy = better, normalized)
        energy = row.get('Energy_uJ', 0)
        r_energy = max(0.0, 1.0 - (energy / 1e9)) if energy > 0 else 0.5  # Normalize to ~1GJ

        return (w1_slo * r_slo) + (w2_tat * r_tat) + (w3_energy * r_energy)

    df['Reward'] = df.apply(get_reward, axis=1)

    # 5 Features: [Slack, CPU, Mem, Threads, InfMem]
    X = df[['Slack_Sec', 'Norm_CPU' if 'Norm_CPU' in df else 'CPU_Load',
            'Norm_Mem' if 'Norm_Mem' in df else 'Mem_GB',
            'Req_Threads', 'Req_Mem']].values.astype(np.float32)
    y = df['Reward'].values.astype(np.float32).reshape(-1, 1)

    # FU-12: Temporal validation split (80/20) — most recent 20% for test
    # Time-series split preserves chronological order: train on past, test on future
    split = int(len(X) * 0.8)
    X_train, X_test = X[:split], X[split:]
    y_train, y_test = y[:split], y[split:]
    print(f"[FU-12] Temporal split: {len(X_train)} train / {len(X_test)} test (chronological order preserved)")

    model = DQNSchedulerModel(input_dim=5)
    if os.path.exists(MODEL_PTH):
        model.load_state_dict(torch.load(MODEL_PTH))
        print("[AI] Loaded existing model for retraining.")

    # FU-2: Imitation Learning Bootstrap — pre-train on EDF traces
    # If EDF log exists, bootstrap the DQN by imitating EDF scheduling decisions
    EDF_LOG = "logs/EDF_scheduler_log.csv"
    edf_candidates = [EDF_LOG, "logs/EDF_toy_scheduler_log.csv", "../logs/EDF_scheduler_log.csv"]
    edf_path = None
    for candidate in edf_candidates:
        if os.path.exists(candidate):
            edf_path = candidate
            break

    if edf_path and not os.path.exists(MODEL_PTH):
        print(f"[FU-2] Imitation Learning: Bootstrapping DQN on EDF traces from {edf_path}")
        edf_df = pd.read_csv(edf_path, sep=';')
        edf_df = edf_df[edf_df['Exit_Code'] == 0]

        if len(edf_df) > 0:
            edf_df['Req_Threads'] = edf_df['Model_Batch'].map(lambda x: metadata.get(x, {'threads':0})['threads'])
            edf_df['Req_Mem'] = edf_df['Model_Batch'].map(lambda x: metadata.get(x, {'inf_mem':0})['inf_mem'])
            edf_df['Slack_Sec'] = (edf_df['Requested_SLO'] - edf_df['Total_Wait_Time']) / 1000.0

            # EDF reward: jobs are ordered by deadline — successful EDF orderings get high reward
            edf_df['IL_Reward'] = edf_df.apply(get_reward, axis=1)

            X_edf = edf_df[['Slack_Sec', 'Norm_CPU' if 'Norm_CPU' in edf_df else 'CPU_Load',
                            'Norm_Mem' if 'Norm_Mem' in edf_df else 'Mem_GB',
                            'Req_Threads', 'Req_Mem']].values.astype(np.float32)
            y_edf = edf_df['IL_Reward'].values.astype(np.float32).reshape(-1, 1)

            il_optimizer = optim.Adam(model.parameters(), lr=0.01)
            il_criterion = nn.MSELoss()
            il_epochs = min(1000, max(500, len(edf_df)))  # FU-2: 500-1000 episodes

            print(f"[FU-2] Imitation Learning: {len(X_edf)} EDF samples, {il_epochs} epochs")
            for epoch in range(il_epochs):
                model.train()
                il_optimizer.zero_grad()
                preds = model(torch.tensor(X_edf))
                loss = il_criterion(preds, torch.tensor(y_edf))
                loss.backward()
                il_optimizer.step()
                if epoch % 100 == 0:
                    print(f"  [IL] Epoch {epoch}/{il_epochs} | Loss: {loss.item():.4f}")
            print(f"[FU-2] Imitation Learning complete. Final IL loss: {loss.item():.4f}")
        else:
            print("[FU-2] EDF log found but no successful jobs — skipping IL bootstrap.")
    elif os.path.exists(MODEL_PTH):
        print("[FU-2] Existing model found — skipping IL bootstrap (retraining mode).")

    optimizer = optim.Adam(model.parameters(), lr=0.005)
    criterion = nn.MSELoss()

    print("[AI] Training...")
    for epoch in range(101):
        model.train()
        optimizer.zero_grad()
        preds = model(torch.tensor(X_train))
        loss = criterion(preds, torch.tensor(y_train))
        loss.backward()
        optimizer.step()
        if epoch % 20 == 0: print(f" Epoch {epoch} | Loss: {loss.item():.4f}")

    # Evaluation
    model.eval()
    with torch.no_grad():
        test_preds = model(torch.tensor(X_test)).numpy()
        r2 = calculate_r2(y_test, test_preds)
        print(f"\n[Evaluation] Model R^2 Score: {r2:.4f}")

        # FU-12: Alignment accuracy — fraction of test samples where predicted rank
        # order matches ground truth rank order (pairwise concordance)
        n_test = len(y_test)
        concordant = 0
        total_pairs = 0
        for i in range(n_test):
            for j in range(i + 1, min(i + 50, n_test)):  # Sample nearby pairs for efficiency
                true_order = y_test[i] > y_test[j]
                pred_order = test_preds[i] > test_preds[j]
                if true_order == pred_order:
                    concordant += 1
                total_pairs += 1
        alignment_acc = concordant / max(1, total_pairs)
        print(f"[FU-12] Alignment accuracy: {alignment_acc:.4f} ({concordant}/{total_pairs} concordant pairs)")
        if alignment_acc < 0.90:
            print(f"[FU-12] WARNING: Alignment accuracy {alignment_acc:.2%} < 90% target. Consider more training data.")

    if not os.path.exists("models"): os.makedirs("models")
    torch.save(model.state_dict(), MODEL_PTH)

    # Export Binary Weights for C++ with DQN2 header (C-20)
    # Format: [4B magic "DQN2"][4B input][4B h1][4B h2][4B output]
    #         [4B arch_hash][4B crc32][raw float32 weights]
    INPUT_DIM, HIDDEN1, HIDDEN2, OUTPUT_DIM = 5, 64, 32, 1

    # Collect raw weight bytes
    weight_bytes = b""
    for param in model.parameters():
        weight_bytes += param.data.numpy().astype('float32').tobytes()

    # Architecture hash (FNV-1a of "5-64-32-1")
    arch_str = f"{INPUT_DIM}-{HIDDEN1}-{HIDDEN2}-{OUTPUT_DIM}"
    arch_hash = fnv1a_hash(arch_str.encode('ascii'))

    # CRC32 of raw weights
    crc32_val = zlib.crc32(weight_bytes) & 0xFFFFFFFF

    with open(WEIGHTS_BIN, "wb") as f:
        # DQN2 Header (28 bytes)
        f.write(b"DQN2")
        f.write(struct.pack('<IIII', INPUT_DIM, HIDDEN1, HIDDEN2, OUTPUT_DIM))
        f.write(struct.pack('<I', arch_hash))
        f.write(struct.pack('<I', crc32_val))
        # Raw weights
        f.write(weight_bytes)

    total_params = sum(p.numel() for p in model.parameters())
    total_size = 28 + len(weight_bytes)
    print(f"[Export] Saved DQN2 to {WEIGHTS_BIN}")
    print(f"  Architecture: {arch_str}")
    print(f"  Parameters: {total_params}")
    print(f"  File size: {total_size} bytes (28B header + {len(weight_bytes)}B weights)")
    print(f"  CRC32: 0x{crc32_val:08X}")

if __name__ == "__main__":
    main()
