/*
A resource pool (or object pool) allows expensive resources to be recycled after
use, instead of repeatedly destroyed and recreated. The pool hands out resources
to clients in the form of unique_ptr's (shared_ptr's also possible with small
caveats -- see below), and when the client is done with the resource, its
ownership is transferred back to the pool. The unused resources are maintained
in a queue of pointers, either unique or smart depending on the implementation.

The main language features leveraged here are:
1) A custom deleter in std::unique_ptr (we use the less common template<class T,
class Deleter = ...>). This allows the resource to be moved instead of deleted
when it is about to leave the client scope.

2a) std::weak_ptr. This is a baby version of shared_ptr that does not own the
object but can check if the object has not expired (i.e. there is at least a
shared_ptr pointing to it) and promote itself to a shared_ptr if so. In our
case, we include a weak_ptr to the pool when we hand out the resource, so the
resource can be put back to the right place afterwards. We also detect if the
pool is still alive before doing so. The caveat is that we need a priori a
shared_ptr to the pool (this is unnecessary if we use the pimpl idiom.)

2b) "class ownClass: public std::enable_shared_from_this<ownClass>" is an
example of the "Curiously recurring tmeplate pattern" or CRTP (see
https://en.wikipedia.org/wiki/Curiously_recurring_template_pattern). In other
words, inheriting from class template std:enabled_shared_from_this allows you to
inherit from yourself! This pattern is used to "strengthen" a class, in this
case allowing the class to dispense std::weak_ptr's via "this->weak_from_this"
(similarly, std::shared_ptr's can be generated with "this->shared_from_this").

3) Concepts. The pool must be able to construct more resources when all existing
resources have been allocated. Therefore, if no allocator is provided to the
pool when it is created (i.e. with the default constructor), the resource type
must be default-constructible. Furthermore, we enforce the pool type in the
deleter templace so that `recycle` can be called.

4) Friend class. The deleter is a friend of the pool so it can call `recycle`.
However, clients should not need to call `recycle` directly.

Key differences between unique_ptr and shared_ptr implementations:
1) std::move(...) vs assignment (=). In a pool of shared_ptr's, resources are
removed from the list and immediately assigned to. However, for instances like
this, we need std::move or similar mechanisms when working with unique_ptrs.

2) When the resource is returned to the pool, the unique_ptr method moves
ownership naturally. With a shared_ptr we either have to move it also (somewhat
unnaturally) or reset the pointer. If the custom deleter does not explicitly
give up ownership, it might not get destroyed when going out of scope in the
special case where the object implicitly maintains a pointer to the deleter
itself. (see the diagram in the repo on which this file is based:
https://github.com/steinwurf/recycle/blob/master/src/recycle/shared_pool.hpp#L317)

3) The deleter is specified as part of the unique_ptr's type, but does not
appear in the shared_ptr's type.

We can pass resources between threads or between processes. Some examples of
expensive resources:
- file descriptors/ sockets
    fd's can be shared between threads (and processed with some effort):
    https://stackoverflow.com/questions/49130292/passing-file-descriptors-from-the-main-process-to-its-threads
    https://copyconstruct.medium.com/seamless-file-descriptor-transfer-between-processes-with-pidfd-and-pidfd-getfd-816afcd19ed4
- database connections
    (https://www.cockroachlabs.com/blog/what-is-connection-pooling/, see also
    https://github.com/brettwooldridge/HikariCP etc)
- threads (e.g.Java's Executors.newCachedThreadPool())

And just for reference:
https://stackoverflow.com/questions/200469/what-is-the-difference-between-a-process-and-a-thread/19518207#19518207
*/

#include <cassert>  // can't get rid of the heritage
#include <functional>
#include <iostream>
#include <memory>
#include <queue>  // defaults to using a deque
#include <type_traits>

template <typename RType>
class ResourcePool;

template <typename RType, typename PType>
    requires std::is_base_of_v<ResourcePool<RType>, PType>
class RDeleter {
   public:
    // PPtr is a weak pointer back to the pool, r_ptr is only used for typing
    using PPtr = std::weak_ptr<PType>;
    RDeleter(const PPtr& p_ptr, const RType* r_ptr) : pool_ptr(p_ptr) {
        // there must be a client using the pool via a shared_ptr
        assert(!p_ptr.expired());
    }

    // recycle or destroy the resource upon leaving the scope (no 'finally')
    void operator()(RType* resource_ptr) {
        if (auto pool_shared_ptr = pool_ptr.lock()) {
            std::cout << "Recycling resource" << std::endl;
            pool_shared_ptr->recycle(std::unique_ptr<RType>(resource_ptr));
        } else {
            std::cout << "Deleting resource" << std::endl;
            delete resource_ptr;  // delete memory normally otherwise
        }
    }

   private:
    PPtr pool_ptr;
};

template <typename RType>  // Resource Type
class ResourcePool : public std::enable_shared_from_this<ResourcePool<RType>> {
   public:
    using DType = RDeleter<RType, ResourcePool>;  // deleter type
    using RPtr = std::unique_ptr<RType, DType>;  // resource pointer for clients
    using AType = std::function<RType*()>;       // allocator type

    // Allow default constructor only if we can default construct each resource
    template <typename defaultable = RType>
        requires std::is_default_constructible_v<defaultable>
    ResourcePool() {}
    // Non-default: construct each resource using the provided allocator
    ResourcePool(AType&& a) : allocate(std::move(a)) {}
    // Explicitly define destructor (Rule of 3)
    ~ResourcePool() {
        std::cout << "Destroying pool" << std::endl;
        free_all_unused();
    }
    // Delete copy constructor (Rule of 3)
    ResourcePool(const ResourcePool& other) = delete;
    // Delete copy assignment (Rule of 3)
    ResourcePool& operator=(const ResourcePool& other) = delete;
    // Define move constructor
    ResourcePool(ResourcePool&& other)
        : allocate(std::move(other.allocate)), pool(std::move(other.pool)) {
        std::cout << "Calling pool's move constructor" << std::endl;
    }
    // Define move assignment
    ResourcePool& operator=(ResourcePool&& other) {
        std::cout << "Calling pool's move assignment" << std::endl;
        allocate = std::move(other.allocate);
        pool = std::move(other.pool);
        return *this;
    }
    friend class RDeleter<RType, ResourcePool<RType>>;

    RPtr request() {
        auto p_ptr = this->weak_from_this();
        // in either case, we transfer ownership of the naked resource pointer
        if (pool.empty()) {
            auto r_ptr = allocate();
            return RPtr(r_ptr, RDeleter(p_ptr, r_ptr));
        } else {
            auto r_ptr = pool.front();
            pool.pop();
            return RPtr(r_ptr, RDeleter(p_ptr, r_ptr));
        }
    }

    size_t get_num_unused() { return pool.size(); }

    void free_all_unused() {
        while (!pool.empty()) {
            delete pool.front();
            pool.pop();
        };
    }

   private:
    // allocate should be ownership free (make_unique calls new anyway)
    AType allocate = []() { return new RType(); };

    void recycle(std::unique_ptr<RType>&& resource_ptr) {
        pool.push(resource_ptr.release());
    }

    // pool is a FIFO container of pointers to resources
    // alternatively we could use unique_ptr's but this saves space
    std::queue<RType*> pool;
};