#include "../src/topk_heap.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>

int main() {
  std::mt19937 rng(20261112);
  size_t replacements = 0;
  for (size_t size : {1,2,3,4,7,10,31,32,63,64,127,256}) {
    for (size_t trial = 0; trial < 40; ++trial) {
      using Key = std::pair<int64_t,int64_t>;
      std::multiset<Key> oracle;
      std::vector<Key> heap;
      const auto less = std::less<Key>{};
      for (size_t i = 0; i < size; ++i) {
        Key value{int64_t(rng()%31), int64_t(rng()%101)};
        heap.push_back(value);
        oracle.insert(value);
      }
      std::make_heap(heap.begin(),heap.end(),less);
      for (size_t i = 0; i < 2000; ++i) {
        Key value{int64_t(rng()%31)-int64_t(i/100), int64_t(rng()%101)};
        if (value >= heap.front()) continue;
        oracle.erase(std::prev(oracle.end()));
        oracle.insert(value);
        mvd::replaceHeapRoot(heap,value,less);
        ++replacements;
        if (!std::is_heap(heap.begin(),heap.end(),less) ||
            std::multiset<Key>(heap.begin(),heap.end()) != oracle) {
          throw std::runtime_error("root replacement changed heap membership or ordering");
        }
      }
      std::sort_heap(heap.begin(),heap.end(),less);
      if (!std::equal(heap.begin(),heap.end(),oracle.begin(),oracle.end())) {
        throw std::runtime_error("final heap order differs from independent multiset");
      }
    }
  }
  std::cout << "PASS " << replacements << " independently checked TopK root replacements\n";
}
