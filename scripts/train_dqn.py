import pandas as pd
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
import os

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
# MODEL DEFINITION
# ==========================================
class DQNSchedulerModel(nn.Module):
    def __init__(self, input_dim):
        super(DQNSchedulerModel, self).__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, 16),
            nn.ReLU(),
            nn.Linear(16, 8),
            nn.ReLU(),
            nn.Linear(8, 1)
        )
    def forward(self, x): return self.net(x)

# ==========================================
# MAIN PIPELINE
# ==========================================
def main():
    LOG_FILE = "logs/scheduler_log.csv"
    CFG_FILE = "profile.cfg"
    MODEL_PTH = "models/dqn_model.pth"
    WEIGHTS_BIN = "models/dqn_weights.bin"

    metadata = parse_profile_cfg(CFG_FILE)
    if not os.path.exists(LOG_FILE):
        print(f"[Error] {LOG_FILE} missing!")
        return

    # Load and Preprocess
    df = pd.read_csv(LOG_FILE, sep=';')
    df = df[df['Exit_Code'] == 0]

    df['Req_Threads'] = df['Model_Batch'].map(lambda x: metadata.get(x, {'threads':0})['threads'])
    df['Req_Mem'] = df['Model_Batch'].map(lambda x: metadata.get(x, {'inf_mem':0})['inf_mem'])
    df['Slack_Sec'] = (df['Requested_SLO'] - df['Wait_Time']) / 1000.0

    def get_reward(row):
        base = 10.0 if row['SLO_Met'] == 1 else -5.0
        penalty = abs(row['SLO_Diff']) / 100.0 if row['SLO_Diff'] < 0 else 0
        return base - penalty

    df['Reward'] = df.apply(get_reward, axis=1)

    # 5 Features: [Slack, CPU, Mem, Threads, InfMem]
    X = df[['Slack_Sec', 'Norm_CPU' if 'Norm_CPU' in df else 'CPU_Load', 
            'Norm_Mem' if 'Norm_Mem' in df else 'Mem_GB', 
            'Req_Threads', 'Req_Mem']].values.astype(np.float32)
    y = df['Reward'].values.astype(np.float32).reshape(-1, 1)

    # Manual Train/Test Split (80/20)
    indices = np.arange(len(X))
    np.random.shuffle(indices)
    split = int(len(X) * 0.8)
    X_train, X_test = X[indices[:split]], X[indices[split:]]
    y_train, y_test = y[indices[:split]], y[indices[split:]]

    model = DQNSchedulerModel(input_dim=5)
    if os.path.exists(MODEL_PTH):
        model.load_state_dict(torch.load(MODEL_PTH))
        print("[AI] Loaded existing model for retraining.")

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

    if not os.path.exists("models"): os.makedirs("models")
    torch.save(model.state_dict(), MODEL_PTH)

    # Export Binary Weights for C++
    with open(WEIGHTS_BIN, "wb") as f:
        for param in model.parameters():
            f.write(param.data.numpy().astype('float32').tobytes())
    print(f"[Export] Saved to {WEIGHTS_BIN}")

if __name__ == "__main__":
    main()