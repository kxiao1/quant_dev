/*
Compile with
    g20 resource_pool.h test_resource_pool.cpp -o ../bin/resource_pool;
*/
#include "resource_pool.h"

class DefaultableResource {
   public:
    DefaultableResource() {
        std::cout << "Allocating defaultable resource" << std::endl;
    }
    ~DefaultableResource() {
        std::cout << "Destroying defaultable resource" << std::endl;
    }
};

class NonDefaultableResource {
   public:
    NonDefaultableResource() = delete;
    NonDefaultableResource(size_t s) : size(s) {
        std::cout << "Allocating resource of size " << size << std::endl;
    };
    ~NonDefaultableResource() {
        std::cout << "Destroying resource of size " << size << std::endl;
    }
    void use() { std::cout << "Using resource of size " << size << std::endl; }

   private:
    size_t size;
};

int main() {
    // shared_ptr is needed for the weak_ptr to detect that the pool is alive.
    // If the actual pool is hidden in pimpl, then we don't need to make_shared
    // because the exposed class already maintains a shared_ptr to the pool.
    auto pool0 = std::make_shared<ResourcePool<DefaultableResource>>();
    {
        auto user0(pool0->request());
        // pool.recycle(user0); // inaccessible!
    }
    auto pool0_obj = std::move(*pool0);       // test move constructor
    pool0.reset();                            // pool0 gets deleted here
    assert(pool0_obj.get_num_unused() == 1);  // but its contents are safe

    auto get_pool = [&]() { return ResourcePool<DefaultableResource>(); };
    auto pool1 = new ResourcePool<DefaultableResource>();
    *pool1 = get_pool();  // test move assignment
    // make a dummy shared_ptr here to prevent memory leaks
    auto pool1_ptr = std::shared_ptr<ResourcePool<DefaultableResource>>(pool1);

    auto allocator = [&]() { return new NonDefaultableResource(5); };
    auto pool2 =
        std::make_shared<ResourcePool<NonDefaultableResource>>(allocator);
    {
        {
            auto user1(pool2->request());
            user1->use();
        }
        {
            auto user2(pool2->request());  // test no extra allocation
            user2->use();
        }
    }
    assert(pool2->get_num_unused() == 1);

    // !this should throw because we did not pass an allocator
    // ResourcePool<NonDefaultableResource> pool3;

    std::cout << "\nNow let's delete rather than recycle:" << std::endl;
    // Create temporary pool scope to test delete behavior when pool is gone
    auto get_from_temp_pool = [&]() {
        auto temp_pool = std::move(pool2);
        return temp_pool->request();  // pool2 gets destroyed after this
    };
    auto user3(get_from_temp_pool());
    // Only pool0 and pool1 are left. Both are destroyed as we exit the scope.

    // Another way of achieving the same effect
    auto pool3 = std::make_shared<ResourcePool<DefaultableResource>>();
    auto user4(pool3->request());
    pool3.reset();  // explicitly destroy the pool

    // A 3rd hacky way: we need a dummy deleter because otherwise the stack and
    // shared_ptr will both try to delete the object (see next comment block)
    pool3 = std::shared_ptr<ResourcePool<DefaultableResource>>(
        &pool0_obj, [](ResourcePool<DefaultableResource>* obj) {});
    auto user5(pool3->request());
    pool3.reset();

    // !this will throw at runtime: both the ptr and stack delete the resource
    // pool3 = std::shared_ptr<ResourcePool<DefaultableResource>>(&pool0_obj);

    std::cout << "\nCleanup:" << std::endl;  // users 3-5, pools 0-1

    /* (unsafe stuff!) */
    // this only works when you call it from a shared_ptr
    // auto pool3_ptr = pool3_obj.shared_from_this();
    // pool3_ptr.reset(); // reset without deleting the underlying object
}
