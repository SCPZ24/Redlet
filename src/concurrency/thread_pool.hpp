#pragma once

#include "../config.hpp"

#include "task.hpp"

#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <queue>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>


class ThreadPool{
private:
    std::queue<Task> tasks;

    std::mutex mtx;
    std::condition_variable cv;

    std::vector<std::thread> workers;

    bool stopping = false;


    void worker_loop(){
        while(true){
            Task task;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this]() -> bool{
                    return !tasks.empty() || stopping;
                });

                if(stopping && tasks.empty()){
                    return;
                }

                task = std::move(tasks.front());
                tasks.pop();
            }

            try{
                task();
            }catch(...){
                std::cerr<<"ThreadPool may not destroyed properly"<<std::endl;
            }
        }
    }

public:
    ThreadPool(std::size_t num_workers = DEFAULT_WORKER_COUNT){
        workers.reserve(num_workers);
        try{
            for(std::size_t i = 0; i < num_workers; ++i){
                workers.emplace_back([this]{
                    worker_loop();
                });
            }
        }catch(...){
            {
                std::lock_guard<std::mutex> lock(mtx);
                stopping = true;
            }
            cv.notify_all();
            for(auto& worker : workers){
                worker.join();
            }
            std::cerr<<"ThreadPool launch failed"<<std::endl;
            throw;
        }
    }


    void submit(Task task){
        {
            std::lock_guard<std::mutex> lock(mtx);
            tasks.push(std::move(task));
        }
        cv.notify_one();
    }

    
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    ~ThreadPool(){
        {
            std::lock_guard<std::mutex> lock(mtx);
            stopping = true;
        }
        cv.notify_all();
        for(auto& worker : workers){
            worker.join();
        }
    }
};
