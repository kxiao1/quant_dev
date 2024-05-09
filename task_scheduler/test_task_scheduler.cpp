/*
Compile with
g20 -pthread task_scheduler.h test_task_scheduler.cpp -o ../bin/task_scheduler
*/

#include "task_scheduler.h"
using namespace std::chrono_literals;

int main() {
    auto start = ns::system_clock::now();
    TaskScheduler TS(start);
    std::this_thread::sleep_until(start + 100ms);
    int t1 = TS.scheduleTask(start + 700ms, 40ms);  // schedule at 100 for 700
    std::this_thread::sleep_until(start + 200ms);
    int t2 = TS.scheduleTask(start + 600ms, 40ms);  // schedule at 200 for 600
    std::this_thread::sleep_until(start + 300ms);
    int t3 = TS.scheduleTask(start + 500ms, 40ms);  // schedule at 300 for 500

    std::this_thread::sleep_until(start + 360ms);
    int t4 = TS.scheduleRepeated(start + 450ms, 500ms, 10ms);  // every 500ms

    // schedule at 400 for 405, should execute immediately
    std::this_thread::sleep_until(start + 400ms);
    int t5 = TS.scheduleTask(start + 405ms, 10ms);
    // uncomment the next line to avoid the race condition
    // std::this_thread::sleep_until(start + 415ms);
    std::this_thread::sleep_until(start + 405ms);
    bool ok1 = TS.deleteScheduled(t5);  // should fail because it has executed

    // schedule at 950 for 900 (in the past!), theoretically might come before
    // repeated task 4 but OS seems to prioritize the waiting thread. t6 is also
    // a long running job to test delay of t4
    std::this_thread::sleep_until(start + 950ms);
    int t6 = TS.scheduleTask(start + 900ms, 1000ms);

    std::this_thread::sleep_until(start + 2500ms);
    int t7 = TS.scheduleTask(start + 3000ms, 10ms);
    bool ok2 = TS.deleteScheduled(t4);  // we should skip to t7 after deletion

    int t8 = TS.scheduleTask(start + 3500ms, 10000000ms);
    int ok3 = TS.deleteScheduled(t8);   // schedule and immediately delete
    int ok4 = TS.deleteScheduled(999);  // try to delete non-existent task_id

    // this task should fail because the task scheduler has exited
    std::this_thread::sleep_until(start + 4500ms);
    int t9 = TS.scheduleTask(start + 5000ms, 10ms);
    std::cout << "Tasks created:\n";
    for (int id : {t1, t2, t3, t4, t5, t6, t7, t8, t9}) {
        std::cout << id << " ";
    }
    std::cout << std::endl;
    std::cout << "Tasks deleted successfully?\n"
              << ok1 << " " << ok2 << " " << ok3 << " " << ok4 << std::endl;
}