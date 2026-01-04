class JobQueue {
  std::list<Job> queue;
  std::mutex mtx;

public:
  void enqueue(Job j) {
      std::lock_guard<std::mutex> lk(mtx);
      queue.push_back(j);
  }

  // This is the core SJF+Aging Dequeue logic
  Job dequeueBest(FileManager& fm, ServerContext& ctx) {
      std::lock_guard<std::mutex> lk(mtx);
      auto best_it = queue.end();
      double best_score = 1e18; // Smaller is better

      for (auto it = queue.begin(); it != queue.end(); ++it) {
          // 1. Check if ANY unique file for this model is READY
          std::string model_key = it->model + "_" + std::to_string(it->batch);
          std::string file = fm.getReadyFile(model_key);
          
          if (file.empty()) continue; // Head-of-Line Avoidance

          // 2. SJF + Aging Calculation
          double est_time = ctx.profiles[model_key].avg_exec_time;
          long wait_time = nowMs() - it->arrival_ts;
          double score = est_time - (wait_time * ctx.cfg.aging_factor);

          if (score < best_score) {
              best_score = score;
              best_it = it;
              best_it->assigned_file = file; // Attach the unique file to the job
          }
      }

      if (best_it != queue.end()) {
          Job selected = *best_it;
          queue.erase(best_it);
          return selected;
      }
      return {}; // No runnable job found
  }
};