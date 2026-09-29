#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

// String-keyed map that remembers insertion order (like a Python dict): iteration, keys and JSON
// output follow the order keys were first added. Small maps are scanned linearly; past kIndexAt
// entries a hash index is built. Storage is a deque, so references to existing entries stay valid
// when new keys are added (callers hold `Value&` across evaluations that may insert).
template <class V>
class OrderedMapT {
public:
    using value_type = std::pair<std::string, V>;
    // One heap node per entry: addresses stay put when more keys are added.
    using Store = std::vector<std::unique_ptr<value_type>>;

    template <class Base, class Ref, class Ptr>
    class Iter {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = OrderedMapT::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = Ptr;
        using reference = Ref;
        Iter() = default;
        explicit Iter(Base b) : it_(b) {}
        Ref operator*() const { return **it_; }
        Ptr operator->() const { return it_->get(); }
        Iter& operator++() { ++it_; return *this; }
        Iter operator++(int) { Iter t = *this; ++it_; return t; }
        Iter operator+(long n) const { return Iter(it_ + n); }
        Iter operator-(long n) const { return Iter(it_ - n); }
        long operator-(const Iter& o) const { return static_cast<long>(it_ - o.it_); }
        bool operator==(const Iter& o) const { return it_ == o.it_; }
        bool operator!=(const Iter& o) const { return it_ != o.it_; }
        Base base() const { return it_; }
    private:
        Base it_{};
    };
    using iterator = Iter<typename Store::iterator, value_type&, value_type*>;
    using const_iterator = Iter<typename Store::const_iterator, const value_type&, const value_type*>;

    OrderedMapT() = default;
    OrderedMapT(const OrderedMapT& o) { for (const auto& e : o.items_) append(e->first, e->second); }
    OrderedMapT& operator=(const OrderedMapT& o) {
        if (this != &o) { clear(); for (const auto& e : o.items_) append(e->first, e->second); }
        return *this;
    }
    OrderedMapT(OrderedMapT&&) noexcept = default;
    OrderedMapT& operator=(OrderedMapT&&) noexcept = default;
    // From an unordered source (a module's variables): order is whatever the source gives.
    explicit OrderedMapT(const std::unordered_map<std::string, V>& src) {
        for (const auto& kv : src) append(kv.first, kv.second);
    }

    iterator begin() { return iterator(items_.begin()); }
    iterator end() { return iterator(items_.end()); }
    const_iterator begin() const { return const_iterator(items_.begin()); }
    const_iterator end() const { return const_iterator(items_.end()); }
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    void clear() { items_.clear(); index_.clear(); indexed_ = false; }
    void reserve(size_t) {}

    iterator find(const std::string& key) {
        long i = locate(key);
        return i < 0 ? end() : begin() + i;
    }
    const_iterator find(const std::string& key) const {
        long i = locate(key);
        return i < 0 ? end() : begin() + i;
    }
    size_t count(const std::string& key) const { return locate(key) < 0 ? 0 : 1; }

    V& operator[](const std::string& key) {
        long i = locate(key);
        if (i >= 0) return items_[static_cast<size_t>(i)]->second;
        return append(key, V()).second;
    }
    V& at(const std::string& key) {
        long i = locate(key);
        if (i < 0) throw std::out_of_range("OrderedMap::at");
        return items_[static_cast<size_t>(i)]->second;
    }
    const V& at(const std::string& key) const {
        long i = locate(key);
        if (i < 0) throw std::out_of_range("OrderedMap::at");
        return items_[static_cast<size_t>(i)]->second;
    }
    std::pair<iterator, bool> insert(const value_type& kv) {
        long i = locate(kv.first);
        if (i >= 0) return {begin() + i, false};
        append(kv.first, kv.second);
        return {end() - 1, true};
    }
    template <class... A>
    std::pair<iterator, bool> emplace(const std::string& key, A&&... a) {
        long i = locate(key);
        if (i >= 0) return {begin() + i, false};
        append(key, V(std::forward<A>(a)...));
        return {end() - 1, true};
    }
    size_t erase(const std::string& key) {
        long i = locate(key);
        if (i < 0) return 0;
        items_.erase(items_.begin() + i);
        rebuild();
        return 1;
    }
    iterator erase(iterator it) {
        size_t pos = static_cast<size_t>(it - begin());
        items_.erase(it.base());
        rebuild();
        return begin() + static_cast<long>(pos);
    }

private:
    static constexpr size_t kIndexAt = 12;

    long locate(const std::string& key) const {
        if (indexed_) {
            auto it = index_.find(key);
            return it == index_.end() ? -1 : static_cast<long>(it->second);
        }
        for (size_t i = 0; i < items_.size(); i++) {
            if (items_[i]->first == key) return static_cast<long>(i);
        }
        return -1;
    }
    value_type& append(const std::string& key, V v) {
        items_.push_back(std::make_unique<value_type>(key, std::move(v)));
        if (indexed_) {
            index_.emplace(key, items_.size() - 1);
        } else if (items_.size() > kIndexAt) {
            rebuild();
        }
        return *items_.back();
    }
    void rebuild() {
        index_.clear();
        indexed_ = items_.size() > kIndexAt;
        if (!indexed_) return;
        for (size_t i = 0; i < items_.size(); i++) index_.emplace(items_[i]->first, i);
    }

    Store items_;
    std::unordered_map<std::string, size_t> index_;
    bool indexed_ = false;
};
