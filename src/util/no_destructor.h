#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace rocketdb {
template <typename InstanceType>
class NoDestructor {
public:
    template<typename ... ConstructorArgTypes>
    explicit NoDestructor(ConstructorArgTypes&&... constructor_args) {
        static_assert(sizeof(instance_storage_) >= sizeof(InstanceType), 
                      "instance_storage_ is not large enough to hold the instance");
        static_assert(std::is_standard_layout_v<NoDestructor<InstanceType>>);
        static_assert(offsetof(NoDestructor, instance_storage) % alignof(InstanceType) == 0,
                     "instance_storage_ does not meet the instance's slignment requirement");
        static_assert(alignof(NoDestructor<InstanceType>) % alignof(InstanceType) == 0,
                     "instance_storage_ does not meet the instance's slignment requirement");

        // Construct in place, using reserved memory
        new (instance_storage_)
            InstanceType(std::forward<ConstructorArgTypes>(constructor_args)...);
    }

    ~NoDestructor() = default;

    NoDestructor(const NoDestructor&) = delete;
    NoDestructor& operator=(const NoDestructor&) = delete;

    InstanceType* get() {
        return reinterpret_cast<InstanceType*>(&instance_storage_);
    }

private:
    // Alignas ensure the allocated memory address is the required size
    alignas(InstanceType) char instance_storage_[sizeof(InstanceType)];
};

}