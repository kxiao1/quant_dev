/*
    The goal is to create a single-threaded task scheduler that executes tasks
   sent to it by a client (in our case, from the main function). The task
   scheduler orders tasks by their desired execution time, delaying in the case
   of long-running tasks or interruption by the addition of new tasks. When no
   tasks are due to be executed, the scheduler sleeps until a new task is added
   that should be executed before the earliest existing task. In particular, the
   scheduler does not wake up every 100ms or so to check its state.
*/

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace ns = std::chrono;  // similar to Python's datetime class
std::string printTime() {
    auto now = ns::system_clock::now();
    // auto curr_t = ns::system_clock::to_time_t(now); // only gets seconds
    // std::cout << std::put_time(std::localtime(&curr_t), *) << std::endl;
    return std::to_string(
               ns::duration_cast<ns::milliseconds>(now.time_since_epoch())
                   .count() %
               100000) +
           "ms\t";
}

struct Task {
    int task_id;
    ns::system_clock::time_point start_time;
    ns::milliseconds running_time;
    // ns::milliseconds repeat_interval{0}; // we now store in the unordered map

    void run() {
        // Simulate a possibly long-running function
        std::this_thread::sleep_for(running_time);
        std::cout << printTime() << "Finished task " << task_id << std::endl;
    }
    bool operator<(const Task& other) const {
        return this->start_time > other.start_time;  // small t = high priority
    }
};

class TaskScheduler {
   public:
    // https://en.cppreference.com/w/cpp/language/pointer#Pointers_to_members
    // https://stackoverflow.com/questions/10673585/start-thread-with-member-function
    TaskScheduler(ns::system_clock::time_point s)
        : start(s), event_loop_thread(&TaskScheduler::runEventLoop, this) {}

    ~TaskScheduler() {
        std::cout << printTime() << "Ending the event loop" << std::endl;
        event_loop_thread.join();
        std::cout << printTime() << "Destroying the task scheduler"
                  << std::endl;
    }

    // Returns task_id of a job to be run once at start_time
    int scheduleTask(ns::system_clock::time_point start_time,
                     ns::milliseconds running_time) {
        std::scoped_lock lck(q_mutex);
        if (!get_event_loop_running()) {
            return -1;
        }
        std::cout << printTime() << "Adding task " << next_task_id
                  << " (single) to the queue" << std::endl;

        // aggregate initialization allows us to specify first few fields only
        // https://softwareengineering.stackexchange.com/questions/262463/should-we-add-constructors-to-structs
        taskq.emplace(next_task_id, start_time, running_time);
        q_cvar.notify_one();  // proactively notify and let event loop check
        return next_task_id++;
    }

    // Returns task_id which remains the same each time the same task is run
    int scheduleRepeated(ns::system_clock::time_point start_time,
                         ns::milliseconds repeat_interval,
                         ns::milliseconds running_time) {
        std::scoped_lock lck(q_mutex);
        if (!get_event_loop_running()) {
            return -1;
        }
        std::cout << printTime() << "Adding task " << next_task_id
                  << " (repeated) to the queue" << std::endl;
        taskq.emplace(next_task_id, start_time, running_time);
        repeated_tasks.insert_or_assign(next_task_id, repeat_interval);
        q_cvar.notify_one();  // proactively notify and let event loop check
        return next_task_id++;
    }

    bool deleteScheduled(int task_id) {
        std::scoped_lock lck(q_mutex);
        if (!get_event_loop_running()) {
            return false;
        }
        bool ok = false;
        if (repeated_tasks.find(task_id) != repeated_tasks.end()) {
            // To delete a repeated task, we will first stop the repetition and
            // and then cancel the upcoming iteration if it's not running
            repeated_tasks.erase(task_id);
            ok = true;
        }
        if ((!ok) && executed_tasks.find(task_id) != executed_tasks.end()) {
            // However, if a single task has run, report an error
            std::cout << printTime() << "ERROR: task " << task_id
                      << " has been executed" << std::endl;
            return false;
        }

        // Here we find and cancel the next iteration of single or repeated task
        {
            // a std::priority_queue restricts access to underlying data, so we
            // have to search through it one by one (we don't face the same
            // issue if we call std::make_heap, std::heap_pop, std::heap_push
            // directly on a vector without overlaying an adapter class)
            std::vector<Task> tasks;
            tasks.reserve(taskq.size());  // we want to set capacity not size
            while ((!taskq.empty()) && taskq.top().task_id != task_id) {
                tasks.push_back(std::move(taskq.top()));
                taskq.pop();
            }
            if ((!taskq.empty()) && taskq.top().task_id == task_id) {
                taskq.pop();  // this is the one we want to delete
                ok = true;
            }
            // in either case, put the rest back
            for (const auto& task : tasks) {
                taskq.push(std::move(task));
            }
        }
        if (!ok) {
            std::cout << printTime() << "ERROR: task " << task_id
                      << " not found" << std::endl;
            return false;
        }
        std::cout << printTime() << "Deleting task " << task_id << std::endl;
        q_cvar.notify_one();  // proactively notify and let event loop check
        return true;
    }

   private:
    void runEventLoop() {
        // for simplicity, suppose the task scheduler runs for a limited time
        auto last_time = start + MAX_DURATION;
        auto next_time{last_time};  // must initialize before declaring lambda
        auto new_earliest = [&]() {
            // ignore the case of start_time > next_time, extra wakeup anyway
            return (!taskq.empty()) && taskq.top().start_time < next_time;
        };

        // we can manually lock and unlock a unique_lock but not a scoped_lock
        std::unique_lock lck(q_mutex);

        event_loop_running = true;  // this is the first place we need to lock
        while (true) {
            // We have the lock here so the queue state is accurate
            // Always update the queue state after reacquiring a lock: a task
            // could have been run, added or deleted
            std::cout << printTime() << "Updating queue state" << std::endl;
            next_time = taskq.empty()
                            ? last_time
                            : std::min(last_time, taskq.top().start_time);

            bool timeout = !q_cvar.wait_until(lck, next_time, new_earliest);

            // There are 4 possible reasons why we stopped waiting:
            // 1) A new task was added (either single or repeated)
            // 2) An existing task was deleted
            // 3) We timed out because we are due to run a task
            // 4) We timed out because we exceeded the max duration

            if (timeout && next_time == last_time) {  // case 4
                std::cout << printTime() << "Shutting down event loop"
                          << std::endl;
                event_loop_running = false;
                return;  // we waited until timeout to shut down the scheduler
            }

            // We have the lock here so the queue state is accurate
            // If taskq is empty (this is only possible in case 2), then we have
            // nothing else to check and just need to update the queue state
            // before going back to sleep
            if (taskq.empty()) {
                continue;
            }

            // The queue is not empty and we need to execute the earliest task
            // if necessary. If not just update the queue state before sleeping
            Task t = taskq.top();

            // Cases 1) and 3) might require us to execute a task:
            // 1) its scheduled time is not too far away so we don't bother
            // waiting (this could be case 1 adding a task for very soon)
            // 3) its scheduled time was reached as indicated by the timeout,

            if (timeout ||
                t.start_time < ns::system_clock::now() + MIN_DURATION) {
                taskq.pop();
                executed_tasks.insert(t.task_id);  // in case the delete comes
                lck.unlock();
                std::cout << printTime() << "Running task " << t.task_id
                          << std::endl;
                t.run();  // run while unlocked
                // Without manual locking we give up lck even if we did nothing
                lck.lock();
                if (repeated_tasks.find(t.task_id) != repeated_tasks.end()) {
                    std::cout << printTime() << "Adding repeated task "
                              << t.task_id << " back to the queue" << std::endl;
                    taskq.emplace(t.task_id,
                                  t.start_time + repeated_tasks.at(t.task_id),
                                  t.running_time);
                }
            }
        }
    }

    bool get_event_loop_running() {
        if (!event_loop_running) {
            std::cout << printTime() << "ERROR: Event loop not running"
                      << std::endl;
        }
        return event_loop_running;
    }

    // synchronize the start time between the threads
    ns::system_clock::time_point start;
    // if the new task starts within MIN_DURATION from now, don't add to queue
    ns::milliseconds MIN_DURATION{20};
    // MAX_DURATION for which the task scheduler is allowed to run
    ns::seconds MAX_DURATION{4};

    int next_task_id{1};
    std::unordered_set<int> executed_tasks;
    std::unordered_map<int, ns::milliseconds> repeated_tasks;

    std::thread event_loop_thread;
    bool event_loop_running = false;

    std::priority_queue<Task> taskq;  // defaults to vector
    std::mutex q_mutex;
    std::condition_variable q_cvar;
};