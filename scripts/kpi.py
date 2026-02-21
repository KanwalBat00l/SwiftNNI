import pandas as pd
import sys
import random

# Define exactly what counts as a 'Large' job for your paper
LARGE_MODELS = [
    'alexnet_2', 'alexnet_4', 'alexnet_8',
    'simc2_4', 'simc2_8',
    'hinet_8', 'hinet_16'
]

def calculate_nni_metrics(file_path):
    try:
        df = pd.read_csv(file_path, sep=';')
    except Exception as e:
        print(f"Error: {e}")
        return

    # 1. Cleanup: Only 'r' type jobs, exclude timeouts (-2)
    df_clean = df[(df['Type'] == 'r') & (df['Exit_Code'] != -2)].copy()
    if df_clean.empty:
        print("No successful jobs found.")
        return

    total_jobs = len(df_clean)
    
    # Identify the 'Elephants'
    df_clean['Is_Large'] = df_clean['Model_Batch'].isin(LARGE_MODELS)
    df_large = df_clean[df_clean['Is_Large']]
    num_large = len(df_large)

    # --- Metrics ---
    makespan_sec = (df_clean['Finish_TS'].max() - df_clean['Arrival_TS'].min()) / 1000.0
    throughput = total_jobs / makespan_sec
    avg_awt = df_clean['Wait_Time'].mean()

    # --- Scenario 1: Status Quo (100% Type 'r') ---
    # Delay D = Finish - Arrival
    df_clean['D_r'] = df_clean['Finish_TS'] - df_clean['Arrival_TS']
    avg_d_baseline = df_clean['D_r'].mean()

    # --- Scenario 2: Optimization (25% of Large Jobs are Type 'a') ---
    if num_large > 0:
        # Sample 25% of the large jobs
        large_indices = df_large.index.tolist()
        num_to_convert = max(1, int(num_large * 0.25))
        a_indices = set(random.sample(large_indices, k=num_to_convert))
        
        delay_vals_mixed = []
        for idx, row in df_clean.iterrows():
            if idx in a_indices:
                # Type 'a' (Advanced): D = Finish - Start (Wait time is 0 for the user)
                delay_vals_mixed.append(row['Finish_TS'] - row['Start_TS'])
            else:
                # Type 'r' (Regular): D = Finish - Arrival (Includes wait)
                delay_vals_mixed.append(row['Finish_TS'] - row['Arrival_TS'])
        
        avg_d_optimized = sum(delay_vals_mixed) / len(delay_vals_mixed)
    else:
        avg_d_optimized = avg_d_baseline

    print("\n" + "="*65)
    print(f"FILE: {file_path}")
    print(f"Workload: {total_jobs} total jobs | {num_large} Large Jobs (Elephants)")
    print("-" * 65)
    print(f"Average Waiting Time (AWT):   {avg_awt:.2f} ms")
    print(f"Throughput:                   {throughput:.3f} jobs/sec")
    print("-" * 65)
    print(f"AVG Delay D (Baseline):       {avg_d_baseline:.2f} ms")
    print(f"AVG Delay D (25% Large as 'a'): {avg_d_optimized:.2f} ms")
    
    # Calculate improvement specifically for the system average
    improvement = avg_d_baseline - avg_d_optimized
    print(f"System-wide Improvement:      {improvement:.2f} ms")
    
    if num_large > 0:
        # Extra Metric: How much did those specific large jobs improve?
        large_wait_avg = df_large['Wait_Time'].mean()
        print(f"Average Wait of a Large Job:  {large_wait_avg:.2f} ms")
    print("="*65 + "\n")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 kpi_v5_final.py <logfile.csv>")
    else:
        calculate_nni_metrics(sys.argv[1])