import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

def generate_gantt(csv_file):
    # Load the log
    df = pd.read_csv(csv_file, sep=';')
    
    # Calculate Metrics
    tardiness_mask = df['SLO_Diff'] < 0
    total_tardiness_ms = abs(df.loc[tardiness_mask, 'SLO_Diff'].sum())
    avg_wait_ms = df['Wait_Time'].mean()
    success_rate = (df['SLO_Met'].sum() / len(df)) * 100

    # Normalize time to start at 0
    start_min = df['Arrival_TS'].min()
    df['Arrival'] = (df['Arrival_TS'] - start_min) / 1000
    df['Start'] = (df['Start_TS'] - start_min) / 1000
    df['Finish'] = (df['Finish_TS'] - start_min) / 1000
    
    fig, ax = plt.subplots(figsize=(14, 8))
    
    # Professional Color Palette
    colors = {'hinet': '#3498db', 'simc1': '#2ecc71', 'simc2': '#f1c40f', 'alexnet': '#e74c3c'}
    
    for i, row in df.iterrows():
        model = row['Model_Batch'].split('_')[0]
        
        # 1. Draw Wait Time (The 'Gap' or 'Starvation')
        ax.barh(i, row['Start'] - row['Arrival'], left=row['Arrival'], 
                color='#ecf0f1', edgecolor='#bdc3c7', hatch='///', alpha=0.6)
        
        # 2. Draw Execution Time
        color = colors.get(model, '#95a5a6')
        ax.barh(i, row['Finish'] - row['Start'], left=row['Start'], 
                color=color, edgecolor='black', linewidth=1)
        
        # 3. Label each bar
        ax.text(row['Arrival'], i, f" {row['Model_Batch']}", va='center', fontsize=10, fontweight='bold')
        
        # 4. Mark SLO Violations with a red line and 'X'
        deadline = row['Arrival'] + (row['Requested_SLO'] / 1000)
        ax.vlines(deadline, i-0.4, i+0.4, colors='red', linestyles='dashed', alpha=0.5)
        if row['SLO_Met'] == 0:
            ax.scatter(deadline, i, color='red', marker='x', s=120, zorder=5)

    # Formatting
    ax.set_xlabel('Time Since First Arrival (Seconds)', fontsize=12)
    ax.set_title(f'Multi-Tenant Inference Scheduling: {len(df)} Requests\n'
                 f'Success Rate: {success_rate:.1f}% | Avg Wait: {avg_wait_ms:.0f}ms | '
                 f'Total Tardiness: {total_tardiness_ms/1000:.2f}s', fontsize=14)
    ax.invert_yaxis()
    ax.grid(axis='x', linestyle='--', alpha=0.3)
    
    # Legend
    wait_patch = mpatches.Patch(facecolor='#ecf0f1', edgecolor='#bdc3c7', hatch='///', label='Wait/Pre-processing')
    slo_line = mpatches.Patch(color='red', label='SLO Deadline (Missed if bar passes line)')
    ax.legend(handles=[wait_patch, slo_line], loc='lower right')
    
    plt.tight_layout()
    plt.savefig('logs/execution_gantt.png', dpi=300)
    print(f"Gantt chart saved to logs/execution_gantt.png")

if __name__ == "__main__":
    generate_gantt('logs/toy_scheduler_log.csv')