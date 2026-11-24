// A map from u32 keys that walks its entries in the order of the SGI / libstdc++ 4.0 __gnu_cxx::hash_map<unsigned long, V>
// the original builds use: buckets sized from __stl_prime_list (100 asked for -> 193), a key's bucket is key % buckets,
// a new entry goes to the FRONT of its bucket chain, and a grow (when the entry count would pass the bucket count) moves
// the old chains front first, each entry to the front of its new bucket. Erase unlinks the entry; buckets never shrink.
// The order matters where the original iterates the map and the result depends on it (the rigid body simulator's
// collision pass resolves pairs sector by sector).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sc4 {

namespace sgi_hash_detail {
// __stl_next_prime: the first prime of the list not below n (the last one when none is).
inline std::size_t NextSize(std::size_t n) {
    static constexpr std::uint32_t kPrimes[] = {53u,       97u,       193u,       389u,       769u,       1543u,       3079u,
                                                6151u,     12289u,    24593u,     49157u,     98317u,     196613u,     393241u,
                                                786433u,   1572869u,  3145739u,   6291469u,   12582917u,  25165843u,   50331653u,
                                                100663319u, 201326611u, 402653189u, 805306457u, 1610612741u, 3221225473u, 4294967291u};
    for (std::uint32_t prime : kPrimes)
        if (prime >= n) return prime;
    return kPrimes[sizeof kPrimes / sizeof kPrimes[0] - 1];
}
}  // namespace sgi_hash_detail

template <class V>
class SgiHashMap {
public:
    struct Node {
        std::uint32_t key = 0;
        V value{};
        Node* next = nullptr;
    };

    explicit SgiHashMap(std::size_t hint = 100) : buckets_(sgi_hash_detail::NextSize(hint), nullptr) {}
    ~SgiHashMap() { Clear(); }
    SgiHashMap(const SgiHashMap&) = delete;
    SgiHashMap& operator=(const SgiHashMap&) = delete;

    std::size_t Size() const { return size_; }
    std::size_t BucketCount() const { return buckets_.size(); }

    V* Find(std::uint32_t key) {
        for (Node* node = buckets_[key % buckets_.size()]; node; node = node->next)
            if (node->key == key) return &node->value;
        return nullptr;
    }

    // hash_map::operator[] (find_or_insert): grows first, then finds or inserts a default value at the bucket front.
    V& operator[](std::uint32_t key) { return *Insert(key, V{}).first; }

    // insert_unique: the existing entry (false) or the new one (true).
    std::pair<V*, bool> Insert(std::uint32_t key, V value) {
        Resize(size_ + 1);
        Node*& head = buckets_[key % buckets_.size()];
        for (Node* node = head; node; node = node->next)
            if (node->key == key) return {&node->value, false};
        auto* node = new Node{key, std::move(value), head};
        head = node;
        ++size_;
        return {&node->value, true};
    }

    bool Erase(std::uint32_t key) {
        Node** link = &buckets_[key % buckets_.size()];
        for (Node* node = *link; node; link = &node->next, node = node->next) {
            if (node->key != key) continue;
            *link = node->next;
            delete node;
            --size_;
            return true;
        }
        return false;
    }

    void Clear() {
        for (Node*& head : buckets_) {
            while (head) {
                Node* next = head->next;
                delete head;
                head = next;
            }
        }
        size_ = 0;
    }

    // begin() .. end(): bucket by bucket, each chain front first.
    template <class F>
    void ForEach(F&& f) {
        for (Node* head : buckets_)
            for (Node* node = head; node; node = node->next) f(node->key, node->value);
    }

private:
    void Resize(std::size_t hint) {
        const std::size_t old = buckets_.size();
        if (hint <= old) return;
        const std::size_t count = sgi_hash_detail::NextSize(hint);
        if (count <= old) return;
        std::vector<Node*> grown(count, nullptr);
        for (Node*& head : buckets_) {
            while (Node* node = head) {
                head = node->next;
                Node*& target = grown[node->key % count];
                node->next = target;
                target = node;
            }
        }
        buckets_.swap(grown);
    }

    std::vector<Node*> buckets_;
    std::size_t size_ = 0;
};

}  // namespace sc4
