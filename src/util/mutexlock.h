#pragma once

#include "../port/port.h"
#include "../port/thread_annotations.h"
#include "../port/port_stdcxx.h"

namespace rocketdb {

class MutexLock {
    public:
        explicit MutexLock(port::Mutex* mu) EXCLUSIVE_LOCK_FUNCTION(mu) : mu_(mu) {
            this->mu_->Lock();
        }

        ~MutexLock() UNLOCK_FUNCTION() { this->mu_->Unlock(); }

        MutexLock(const MutexLock&) = delete;
        MutexLock& operator=(const MutexLock&) = delete;

    private:
        port::Mutex* const mu_;
};

}