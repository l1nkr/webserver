#include "threadpool.h"

template <typename T>
threadpool<T>::threadpool(connection_pool *connPool, int thread_number, int max_requests) {
    if (thread_number <= 0 || max_requests <= 0) {
        throw std::exception();
    }

    m_stop = false;
    m_threads = NULL;
    m_connPool = connPool;
    m_thread_number = thread_number;
    m_max_requests = max_requests;
    m_threads = new pthread_t[m_thread_number];

    if (!m_threads) {
        throw std::exception();
    }

    for (int i = 0; i < thread_number; ++i) {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])) {//主线程与子线程分离，子线程结束后，自动释放资源
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T *request) {
    m_queuelocker.lock();
    if ( m_queueRequset.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_queueRequset.push_back(request);//加入请求队列
    m_queuelocker.unlock();
    m_queuesem.post();//发送信号
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg) {
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run() {
    while (!m_stop) {
        m_queuesem.wait();
        m_queuelocker.lock();

        if (m_queueRequset.empty()) {
            m_queuelocker.unlock();
            continue;
        }

        T *request = m_queueRequset.front();//取出请求
        m_queueRequset.pop_front();
        m_queuelocker.unlock();

        if (!request) {
            continue;
        }
        connectionRAII mysqlcon(&request->mysql, m_connPool);//从连接池中获取一条连接
        
        request->process();
    }
}