#pragma once
// Bounded-queue log destination (console mirror, transcript file). Pushing never blocks: a stalled
// destination (a console in QuickEdit "Select" mode, a disk or antivirus hiccup) must never propagate
// back to the thread that logged. See docs/congelacion_consola.md for the freeze this fixes.
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// One destination of the transcript, fed through a bounded queue and written by its own thread. Pushing
// never blocks: while the destination stalls (a console in QuickEdit selection, a disk or antivirus
// hiccup) chunks beyond the cap are dropped and counted, and the count is written once it moves again.
// Before this, the pump wrote the console mirror inline: a PowerShell window left in "Select" mode
// stopped the pump, the 8 KB pipe filled, and the game thread froze inside its next log line.
class TranscriptSink {
public:
    using WriteFn = std::function<void(const char*, size_t)>;
    TranscriptSink(std::string name, size_t capBytes, WriteFn write)
        : m_name(std::move(name)), m_cap(capBytes), m_write(std::move(write)), m_thread([this] { Run(); }) {}
    ~TranscriptSink() { Close(std::chrono::milliseconds(0)); }

    void Push(const char* data, size_t size) {
        if (size == 0) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) return;
        if (m_queuedBytes + size > m_cap) {
            m_droppedBytes += size;
            return;
        }
        m_queue.emplace_back(data, size);
        m_queuedBytes += size;
        m_cv.notify_one();
    }

    // Drains what is queued, then stops. Returns false when the writer is still stuck inside the
    // destination after `timeout`; the thread is then left detached so process exit is never blocked.
    bool Close(std::chrono::milliseconds timeout) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) return m_finished;
            m_closed = true;
        }
        m_cv.notify_all();
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_doneCv.wait_for(lock, timeout, [this] { return m_finished; })) {
            lock.unlock();
            if (m_thread.joinable()) m_thread.detach();
            return false;
        }
        lock.unlock();
        if (m_thread.joinable()) m_thread.join();
        return true;
    }

private:
    void Run() {
        for (;;) {
            std::string chunk;
            size_t dropped = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return !m_queue.empty() || m_closed; });
                if (m_queue.empty()) break;
                chunk = std::move(m_queue.front());
                m_queue.pop_front();
                m_queuedBytes -= chunk.size();
                dropped = m_droppedBytes;
                m_droppedBytes = 0;
            }
            if (dropped != 0) {
                const std::string note = "[runtime] transcript: dropped " + std::to_string(dropped) +
                                         " bytes while the " + m_name + " was stalled\n";
                m_write(note.data(), note.size());
            }
            m_write(chunk.data(), chunk.size());
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_finished = true;
        m_doneCv.notify_all();
    }

    std::string m_name;
    size_t m_cap;
    WriteFn m_write;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_doneCv;
    std::deque<std::string> m_queue;
    size_t m_queuedBytes = 0;
    size_t m_droppedBytes = 0;
    bool m_closed = false;
    bool m_finished = false;
    std::thread m_thread;
};

