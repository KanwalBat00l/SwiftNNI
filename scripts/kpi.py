import pandas as pd
import sys
import random

# Define exactly what counts as a 'Large' job
LARGE_MODELS = [
    'alexnet_2', 'alexnet_4', 'alexnet_8',
    'simc2_4', 'simc2_8',
    'hinet_8', 'hinet_16'
]

TARGET_N = 500

def calculate_nni_metrics(file_path):
    try:
        df = pd.read_csv(file_path, sep=';')
    except Exception as e:
        print(f"Error: {e}")
        return

    # 1. Cleanup: Only 'r' type jobs, exclude timeouts (-2)
    # Treat -1 as success (verified inference)
    df_clean = df[(df['Type'] == 'r') & (df['Exit_Code'] != -2)].copy()
    
    if df_clean.empty:
        print("No successful jobs found.")
        return

    # 2. Normalize Count to TARGET_N
    actual_count = len(df_clean)
    
    if actual_count > TARGET_N:
        # If we have more than 500, take the first 500 to keep the arrival window tight
        df_clean = df_clean.head(TARGET_N)
        norm_factor = 1.0
    else:
        # If we have fewer (e.g. 492), we will scale the makespan
        norm_factor = TARGET_N / actual_count

    current_n = len(df_clean)

    # 3. Basic Timings
    # Raw Makespan = Last Finish - First Arrival
    raw_makespan_ms = df_clean['Finish_TS'].max() - df_clean['Arrival_TS'].min()
    
    # Normalized Makespan (Scaled to 500 jobs)
    norm_makespan_s = (raw_makespan_ms * norm_factor) / 1000.0
    
    # Normalized Throughput
    throughput = TARGET_N / norm_makespan_s
    
    # AWT is an average, so scaling isn't required for the value itself
    avg_awt = df_clean['Wait_Time'].mean()

    # 4. Identify Large Jobs
    df_clean['Is_Large'] = df_clean['Model_Batch'].isin(LARGE_MODELS)
    df_large = df_clean[df_clean['Is_Large']]
    num_large = len(df_large)

    # --- Scenario 1: Status Quo (100% Type 'r') ---
    avg_d_baseline = (df_clean['Finish_TS'] - df_clean['Arrival_TS']).mean()

    # --- Scenario 2: Optimization (25% of Large Jobs as Type 'a') ---
    if num_large > 0:
        large_indices = df_large.index.tolist()
        num_to_convert = max(1, int(num_large * 0.25))
        a_indices = set(random.sample(large_indices, k=num_to_convert))
        
        delay_vals_mixed = []
        for idx, row in df_clean.iterrows():
            if idx in a_indices:
                # Type 'a' ignores wait time
                delay_vals_mixed.append(row['Finish_TS'] - row['Start_TS'])
            else:
                delay_vals_mixed.append(row['Finish_TS'] - row['Arrival_TS'])
        
        avg_d_optimized = sum(delay_vals_mixed) / len(delay_vals_mixed)
    else:
        avg_d_optimized = avg_d_baseline

    # --- Final Output ---
    print("\n" + "="*65)
    print(f"FILE: {file_path}")
    print(f"Normalization: Scaled {actual_count} jobs to {TARGET_N} job baseline")
    print(f"Elephants: {num_large} Large Jobs detected")
    print("-" * 65)
    print(f"Normalized Makespan:          {norm_makespan_s:.2f} seconds")
    print(f"Normalized Throughput:        {throughput:.3f} jobs/sec")
    print(f"Average Waiting Time (AWT):   {avg_awt:.2f} ms")
    print("-" * 65)
    print(f"AVG Delay D (Baseline):       {avg_d_baseline:.2f} ms")
    print(f"AVG Delay D (25% Large as 'a'): {avg_d_optimized:.2f} ms")
    print(f"System Improvement (D):       {avg_d_baseline - avg_d_optimized:.2f} ms")
    
    if num_large > 0:
        print(f"Avg Wait of Large Jobs:       {df_large['Wait_Time'].mean():.2f} ms")
    print("="*65 + "\n")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 kpi_v6_normalized.py <logfile.csv>")
    else:
        calculate_nni_metrics(sys.argv[1])