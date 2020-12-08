#pragma once

#include "common/tao.hpp"
#include "containers/thread_local_tasks.hpp"
#include "ids/generator.hpp"
#include "traits/shared_function.hpp"
#include "updater/updater.hpp"

#include <boost/fiber/fiber.hpp>
#include <boost/fiber/barrier.hpp>

#include <tao/tuple/tuple.hpp>

#include <iostream>
#include <list>



template <typename D>
class base_executor;

template <typename D>
concept has_on_worker_thread = requires() {
    { std::declval<D>().on_worker_thread() };
};

template <typename D>
class base_executor
{
public:
    static inline std::array<base_executor*, 128> instances;
    static constexpr std::size_t buffer_size = 1024;

private:
    static inline base_executor** _current = &instances[0];

public:
    base_executor() noexcept
    {
        instances[0] = this; // First entry always point to the last executor created
    }

    inline base_executor* last()
    {
        return instances[0];
    }

    inline base_executor* current()
    {
        return *_current;
    }

    void start(uint8_t num_workers, bool suspend) noexcept
    {
        for (uint8_t thread_id = 1; thread_id < num_workers; ++thread_id)
        {
            _workers.push_back(std::thread(
                [this, num_workers, suspend] {
                    // Set thread algo
                    public_scheduling_algorithm<exclusive_work_stealing<0>>(num_workers, suspend);

                    // Custom behaviour, if any
                    if constexpr (!std::is_same_v<D, void> && has_on_worker_thread<D>)
                    {
                        static_cast<D&>(*this).on_worker_thread();
                    }

                    _mutex.lock();
                    // suspend main-fiber from the worker thread
                    _cv.wait(_mutex);
                    _mutex.unlock();
                })
            );
        }

        // Set thread algo
        public_scheduling_algorithm<exclusive_work_stealing<0>>(num_workers, suspend);
    }

    void stop() noexcept
    {
        // Notify, wait and join workers
        _cv.notify_all();

        for (auto& t : _workers)
        {
            t.join();
        }
    }

    template <typename C>
    constexpr void schedule(C&& callback) noexcept
    {
        get_scheduler().schedule(fu2::unique_function<void()>(std::move(callback))); // ;
    }

    void execute_tasks() noexcept
    {
        push_instance();

        for (auto ts : thread_local_storage<tasks>::get())
        {
            ts->execute();
        }

        for (auto ts : thread_local_storage<async_tasks>::get())
        {
            ts->execute();
        }

        pop_instance();
    }

    template <typename U, typename... Args>
    constexpr void update(U& updater, Args&&... args) noexcept
    {
        push_instance();

        boost::fibers::fiber([&updater, ...args{ std::forward<Args>(args) }]() mutable {
            updater.update(std::forward<Args>(args)...);
            updater.wait_update();
        }).join();

        pop_instance();
    }

    template <typename... U, typename... Args>
    constexpr void update_many(tao::tuple<Args...>&& args, U&... updaters) noexcept
    {
        push_instance();

        boost::fibers::fiber([... updaters{ &updaters }, args{ std::forward<tao::tuple<Args...>>(args) }]() mutable {
            tao::apply([&](auto&&... args) {
                ((updaters->update(std::forward<decltype(args)>(args)...)), ...);
            }, args);
            
            (updaters->wait_update(), ...);
        }).join();

        pop_instance();
    }

    template <typename U, typename... Args>
    constexpr void sync(U& updater, Args&&... args) noexcept
    {
        push_instance();
        updater.sync(std::forward<Args>(args)...);
        pop_instance();
    }

    template <template <typename...> typename S, typename... A, typename... vecs>
    constexpr uint64_t create(S<vecs...>& scheme, A&&... scheme_args) noexcept
    {
        return create_with_callback(scheme, [](auto&&... e) { return tao::tuple(e...); }, std::forward<A>(scheme_args)...);
    }

    template <template <typename...> typename S,typename... Args, typename... vecs>
    constexpr uint64_t create_with_args(S<vecs...>& scheme, Args&&... args) noexcept
    {
        return create_with_callback(scheme, [](auto&&... e) { return tao::tuple(e...); }, 
            scheme.template args<vecs>(std::forward<Args>(args)...)...
        );
    }

    template <template <typename...> typename S, typename C, typename... A, typename... vecs>
    constexpr uint64_t create_with_callback(S<vecs...>& scheme, C&& callback, A&&... scheme_args) noexcept
    {
        uint64_t id = id_generator<S<vecs...>>().next();
        return create_with_callback(id, scheme, std::move(callback), std::forward<A>(scheme_args)...);
    }

    //template <template <typename...> typename S, typename C, typename... A, typename... vecs>
    //__declspec(noinline) constexpr uint64_t create_with_callback(uint64_t id, S<vecs...>& scheme, C&& callback, A&&... scheme_args)
    //{
    //    static_assert(sizeof...(vecs) == sizeof...(scheme_args), "Incomplete scheme creation");

    //    schedule([id, callback{ std::move(callback) }, ...scheme_args{ std::move(scheme_args) }, &scheme] () {
    //        auto entities = callback(tao::apply([&](auto&&... args) {
    //            auto component = scheme_args.comp.alloc(id, std::forward<decay_t<decltype(args)>>(args)...);
    //            component->base()->scheme_information(scheme);
    //            return component;
    //        }, scheme_args.args)...);

    //        tao::apply([](auto&&... entities) {
    //            (..., entities->base()->scheme_created());
    //        }, std::move(entities));
    //    });

    //    return id;
    //}

    template <template <typename...> typename S, typename C, typename... A, typename... vecs>
    constexpr uint64_t create_with_callback(uint64_t id, S<vecs...>& scheme, C&& callback, A&&... scheme_args) noexcept
        requires (... && !std::is_lvalue_reference<A>::value)
    {
        static_assert(sizeof...(vecs) == sizeof...(scheme_args), "Incomplete scheme creation");

        schedule([
            this,
            id,
            &scheme,
            callback = std::move(callback),
            ... scheme_args = std::forward<A>(scheme_args)
        ] () mutable {
            // Create entities by using each allocator and arguments
            // Call callback now too
            auto entities = tao::apply(std::move(callback), tao::forward_as_tuple(create(id, scheme, std::move(scheme_args)) ...));

            // Notify of complete scheme creation
            tao::apply([](auto&&... entities) {
                (..., entities->base()->scheme_created());
            }, std::move(entities));
        });

        return id;
    }

    template <template <typename...> typename S, typename P, typename C, typename... A, typename... vecs>
    constexpr void create_with_precondition(S<vecs...>& scheme, P&& precond, C&& callback, A&&... scheme_args) noexcept
        requires (... && !std::is_lvalue_reference<A>::value)
    {
        static_assert(sizeof...(vecs) == sizeof...(scheme_args), "Incomplete scheme creation");

        schedule([
            this,
            &scheme,
            precond = std::move(precond),
            callback = std::move(callback),
            ... scheme_args = std::forward<A>(scheme_args)
        ] () mutable {
            auto main_entity = precond();
            if (main_entity)
            {
                tao::apply(std::move(callback), scheme.search(main_entity->id()));
                return;
            }
            
            uint64_t id = id_generator<S<vecs...>>().next();
            // Create entities by using each allocator and arguments
            // Call callback now too
            auto entities = tao::apply(std::move(callback), tao::forward_as_tuple(create(id, scheme, std::move(scheme_args)) ...));

            // Notify of complete scheme creation
            tao::apply([](auto&&... entities) {
                (..., entities->base()->scheme_created());
            }, std::move(entities));
        });
    }

private:
    template <template <typename...> typename S, typename... vecs, typename T>
    constexpr auto create(uint64_t id, S<vecs...>& scheme, T&& scheme_args) noexcept
    {
        // Create by invoking with arguments
        auto entity = tao::apply([&scheme_args, &id](auto&&... args) {
            return scheme_args.comp.alloc(id, std::forward<std::decay_t<decltype(args)>>(args)...);
        }, scheme_args.args);

        // Notify of creation
        entity->base()->scheme_information(scheme);

        return entity;
    }

    inline constexpr void push_instance()
    {
        ++_current;
        *_current = this;
    }

    inline constexpr void pop_instance()
    {
        --_current;
    }

protected:
    inline tasks& get_scheduler() noexcept
    {
        thread_local tasks ts;
        return ts;
    }

    template <typename T>
    inline generator<T>& id_generator() noexcept
    {
        thread_local generator<T> gen;
        return gen;
    }

private:
    std::vector<std::thread> _workers;
    boost::fibers::mutex _mutex;
    boost::fibers::condition_variable_any _cv;
};


using executor = base_executor<void>;

