import pandas as pd
import sys
import csv

def load_profile(profile_path):
    """Parses profile.cfg into a dictionary: model_batch -> (pre_ms, inf_ms)"""
    profile_data = {}
    try:
        with open(profile_path, 'r') as f:
            lines = [line for line in f if line.strip() and not line.startswith('#')]
            reader = csv.DictReader(lines)
            for row in reader:
                key = f"{row['model']}_{row['batch']}"
                profile_data[key] = (int(row['pre_ms']), int(row['inf_ms']))
    except Exception as e:
        print(f"Error reading profile config: {e}")
        sys.exit(1)
    return profile_data

def run_sequential_baseline(log_file, profile_path):
    profile = load_profile(profile_path)
    
    try:
        df = pd.read_csv(log_file, sep=';')
    except Exception as e:
        print(f"Error reading log: {e}")
        return

    # Filter for 'r' jobs and sort by arrival to maintain original order
    df_arrival = df[df['Type'] == 'r'].sort_values('Arrival_TS').copy()
    
    if df_arrival.empty:
        print("No 'r' jobs found in log.")
        return

    sim_results = []
    # System starts at the arrival time of the first job
    current_system_clock = df_arrival['Arrival_TS'].min()
    
    for _, job in df_arrival.iterrows():
        model_key = job['Model_Batch']
        pre_ms, inf_ms = profile.get(model_key, (0, 0))
        total_exec_ms = pre_ms + inf_ms
        
        # Start Time: System is either free at Arrival or busy until previous Finish
        start_ts = max(job['Arrival_TS'], current_system_clock)
        
        # Waiting Time: Time from Arrival to Start
        wait_ms = start_ts - job['Arrival_TS']
        
        # Finish Time: Start + Preprocessing + Inference
        finish_ts = start_ts + total_exec_ms
        
        # Total Delay: From Arrival to Completion
        delay_ms = finish_ts - job['Arrival_TS']
        
        sim_results.append({
            'Wait_S': wait_ms / 1000.0,
            'Delay_S': delay_ms / 1000.0,
            'Finish_TS': finish_ts,
            'Arrival_TS': job['Arrival_TS']
        })
        
        # Move system clock to the end of this job
        current_system_clock = finish_ts

    sim_df = pd.DataFrame(sim_results)

    # --- KPI CALCULATIONS ---
    total_jobs = len(sim_df)
    avg_wait_s = sim_df['Wait_S'].mean()
    avg_delay_s = sim_df['Delay_S'].mean()
    
    # Makespan: Total wall-clock time from first arrival to last completion
    makespan_s = (sim_df['Finish_TS'].max() - sim_df['Arrival_TS'].min()) / 1000.0
    
    # Throughput: Jobs per second
    throughput = total_jobs / makespan_s if makespan_s > 0 else 0

    # --- OUTPUT ---
    print("\n" + "="*65)
    print("SEQUENTIAL BASELINE ANALYSIS (NO SCHEDULER)")
    print("="*65)
    print(f"Total Jobs Analyzed:    {total_jobs}")
    print(f"Makespan:               {makespan_s:.2f} seconds ({makespan_s/60:.2f} minutes)")
    print(f"Throughput:             {throughput:.3f} jobs/sec")
    print("-" * 65)
    print(f"Average Waiting Time:   {avg_wait_s:.2f} seconds")
    print(f"Average Input Delay D:  {avg_delay_s:.2f} seconds")
    print("="*65 + "\n")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 baseline_final.py <logfile.csv> <profile.cfg>")
    else:
        run_sequential_baseline(sys.argv[1], sys.argv[2])