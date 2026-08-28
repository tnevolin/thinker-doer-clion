#pragma once

#include <tuple>
#include <type_traits>
#include "robin_hood.h"

/**
 * A map that supports multiple keys by using a std::tuple as the internal key.
 * It uses robin_hood::unordered_flat_map for efficient storage.
 */
template <typename Value, typename... Keys>
class MultiKeyMap {
private:
    using KeyType = std::tuple<Keys...>;

    struct HashTuple {
        size_t operator()(const KeyType& tuple) const {
            return HashHelper<sizeof...(Keys) - 1, Keys...>::hash(tuple);
        }

    private:
        template <int Index, typename... T>
        struct HashHelper {
            static size_t hash(const std::tuple<T...>& tuple) {
                size_t h = HashHelper<Index - 1, T...>::hash(tuple);
                const auto& val = std::get<Index>(tuple);
                return h ^ (robin_hood::hash<std::decay_t<decltype(val)>>{}(val) + 0x9e3779b9 + (h << 6) + (h >> 2));
            }
        };

        template <typename... T>
        struct HashHelper<-1, T...> {
            static size_t hash(const std::tuple<T...>&) {
                return 0;
            }
        };
    };

    using MapType = robin_hood::unordered_flat_map<KeyType, Value, HashTuple>;

public:
    using iterator = typename MapType::iterator;
    using const_iterator = typename MapType::const_iterator;

    void clear() {
        m_map.clear();
    }

    void emplace(Keys... keys, const Value& value) {
        m_map.emplace(std::make_tuple(keys...), value);
    }

    bool contains(Keys... keys) const {
        return m_map.find(std::make_tuple(keys...)) != m_map.end();
    }

    Value& at(Keys... keys) {
        return m_map.at(std::make_tuple(keys...));
    }

    const Value& at(Keys... keys) const {
        return m_map.at(std::make_tuple(keys...));
    }

    Value& operator()(Keys... keys) {
        return m_map[std::make_tuple(keys...)];
    }

    iterator find(Keys... keys) {
        return m_map.find(std::make_tuple(keys...));
    }

    const_iterator find(Keys... keys) const {
        return m_map.find(std::make_tuple(keys...));
    }

    iterator end() {
        return m_map.end();
    }

    const_iterator end() const {
        return m_map.end();
    }

private:
    MapType m_map;
};


