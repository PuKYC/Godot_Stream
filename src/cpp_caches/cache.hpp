/**
 * \file
 * \brief Generic cache implementation (value-semantic version)
 */
#ifndef CACHE_HPP
#define CACHE_HPP

#include "cache_policy.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace caches
{
/**
 * \brief Fixed sized cache that can be used with different policy types (e.g. LRU, FIFO, LFU)
 * \tparam Key Type of a key (should be hashable)
 * \tparam Value Type of a value stored in the cache
 * \tparam Policy Type of a policy to be used with the cache
 * \tparam HashMap Type of a hashmap to use for cache operations.
 *         Must have interface compatible with std::unordered_map<Key, Value>
 */
template <typename Key, typename Value, template <typename> class Policy = NoCachePolicy,
          typename HashMap = std::unordered_map<Key, Value>>
class fixed_sized_cache
{
  public:
    using map_type = HashMap;
    using value_type = Value;
    using iterator = typename map_type::iterator;
    using const_iterator = typename map_type::const_iterator;
    using operation_guard = std::lock_guard<std::mutex>;
    using on_erase_cb = std::function<void(const Key &key, const value_type &value)>;

    /**
     * \brief Fixed sized cache constructor
     * \param[in] max_size Maximum size of the cache (zero is forced to 1)
     * \param[in] policy Cache policy to use (e.g. LRUCachePolicy<Key>)
     * \param[in] on_erase Function called when an element is erased from the cache
     */
    explicit fixed_sized_cache(
        size_t max_size, const Policy<Key> policy = Policy<Key>{},
        on_erase_cb on_erase = [](const Key &, const value_type &) {})
        : cache_policy{policy}, max_cache_size{max_size == 0 ? 1 : max_size},
          on_erase_callback{on_erase}
    {
    }

    ~fixed_sized_cache() noexcept
    {
        Clear();
    }

    /**
     * \brief Put element into the cache
     * \param[in] key Key value to use
     * \param[in] value Value to assign to the given key
     */
    void Put(const Key &key, const Value &value) noexcept
    {
        operation_guard lock{safe_op};
        auto elem_it = FindElem(key);

        if (elem_it == cache_items_map.end())
        {
            // add new element to the cache
            if (cache_items_map.size() + 1 > max_cache_size)
            {
                auto disp_candidate_key = cache_policy.ReplCandidate();
                Erase(disp_candidate_key);
            }

            Insert(key, value);
        }
        else
        {
            // update previous value
            Update(key, value);
        }
    }

    /**
     * \brief Try to get an element by the given key from the cache
     * \param[in] key Get element by key
     * \return Pair of (value, success_flag). If success_flag is false,
     *         value is default-constructed and should not be used.
     */
    std::pair<value_type, bool> TryGet(const Key &key) const noexcept
    {
        operation_guard lock{safe_op};
        const auto result = GetInternal(key);
        return std::make_pair(result.second ? result.first->second : value_type{},
                              result.second);
    }

    /**
     * \brief Get element from the cache if present
     * \param[in] key Get element by key
     * \return Copy of the value stored by the specified key in the cache,
     *         or default-constructed value if the key is not found.
     */
    value_type Get(const Key &key) const noexcept
    {
        operation_guard lock{safe_op};
        auto elem = GetInternal(key);
        if (elem.second)
        {
            return elem.first->second;
        }
        else
        {
            return value_type{};
        }
    }

    /**
     * \brief Check whether the given key is presented in the cache
     * \param[in] key Element key to check
     * \retval true Element is presented in the case
     * \retval false Element is not presented in the case
     */
    bool Cached(const Key &key) const noexcept
    {
        operation_guard lock{safe_op};
        return FindElem(key) != cache_items_map.cend();
    }

    /**
     * \brief Get number of elements in cache
     * \return Number of elements currently stored in the cache
     */
    std::size_t Size() const
    {
        operation_guard lock{safe_op};
        return cache_items_map.size();
    }

    /**
     * Remove an element specified by key
     * \param[in] key Key parameter
     * \retval true if an element specified by key was found and deleted
     * \retval false if an element is not present in a cache
     */
    bool Remove(const Key &key)
    {
        operation_guard lock{safe_op};
        auto elem = FindElem(key);

        if (elem == cache_items_map.end())
        {
            return false;
        }

        Erase(elem);
        return true;
    }

  protected:
    void Clear()
    {
        operation_guard lock{safe_op};
        std::for_each(begin(), end(),
                      [&](const std::pair<const Key, value_type> &el)
                      { cache_policy.Erase(el.first); });
        cache_items_map.clear();
    }

    const_iterator begin() const noexcept
    {
        return cache_items_map.cbegin();
    }

    const_iterator end() const noexcept
    {
        return cache_items_map.cend();
    }

  protected:
    void Insert(const Key &key, const Value &value)
    {
        cache_policy.Insert(key);
        cache_items_map.emplace(key, value);
    }

    void Erase(const_iterator elem)
    {
        cache_policy.Erase(elem->first);
        on_erase_callback(elem->first, elem->second);
        cache_items_map.erase(elem);
    }

    void Erase(const Key &key)
    {
        auto elem_it = FindElem(key);
        if (elem_it != cache_items_map.end())
            Erase(elem_it);
    }

    void Update(const Key &key, const Value &value)
    {
        cache_policy.Touch(key);
        cache_items_map[key] = value;
    }

    const_iterator FindElem(const Key &key) const
    {
        return cache_items_map.find(key);
    }

    std::pair<const_iterator, bool> GetInternal(const Key &key) const noexcept
    {
        auto elem_it = FindElem(key);

        if (elem_it != end())
        {
            cache_policy.Touch(key);
            return {elem_it, true};
        }

        return {elem_it, false};
    }

  private:
    map_type cache_items_map;
    mutable Policy<Key> cache_policy;
    mutable std::mutex safe_op;
    std::size_t max_cache_size;
    on_erase_cb on_erase_callback;
};
} // namespace caches

#endif // CACHE_HPP