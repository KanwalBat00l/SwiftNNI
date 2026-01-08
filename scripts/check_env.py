import sys
import os

def check_env():
    print("--- SAPPIS AI Environment Check ---")
    print(f"Python Version: {sys.version}")
    
    dependencies = ["torch", "pandas", "numpy", "sklearn"]
    missing = []

    for lib in dependencies:
        try:
            m = __import__(lib)
            # Handle sklearn naming quirk
            version = m.__version__ if lib != "sklearn" else __import__("sklearn").__version__
            print(f"[OK] {lib.ljust(10)} | Version: {version}")
        except ImportError:
            print(f"[!!] {lib.ljust(10)} | NOT FOUND")
            missing.append(lib)

    print("\n--- Hardware Check ---")
    try:
        import torch
        print(f"PyTorch CPU support: OK")
        print(f"PyTorch CUDA (GPU): {'Available' if torch.cuda.is_available() else 'Not Available (Not needed for this project)'}")
        
        # Simple smoke test: Create a tiny neural network
        test_net = torch.nn.Linear(5, 1)
        test_input = torch.randn(1, 5)
        output = test_net(test_input)
        print(f"Neural Inference Test: OK (Output: {output.item():.4f})")
    except Exception as e:
        print(f"Neural Test: FAILED ({e})")

    if missing:
        print(f"\n[ACTION REQUIRED] Please install missing packages: pip install {' '.join(missing)}")
    else:
        print("\n[SUCCESS] Environment is ready for DQNScheduler training.")

if __name__ == "__main__":
    check_env()