#ifndef MVD_TOPK_HEAP_HPP
#define MVD_TOPK_HEAP_HPP
#include <cstddef>
#include <utility>
#include <vector>

namespace mvd {
// The caller has proved value is preferable to the maximum at the root.
// Repair just that path instead of pop_heap's descent followed by push_heap.
template <typename T, typename Less>
void replaceHeapRoot(std::vector<T> &heap, T value, const Less &less) {
  size_t parent = 0;
  for (size_t child = 1; child < heap.size(); child = 2 * parent + 1) {
    if (child + 1 < heap.size() && less(heap[child], heap[child + 1])) ++child;
    if (!less(value, heap[child])) break;
    heap[parent] = std::move(heap[child]);
    parent = child;
  }
  heap[parent] = std::move(value);
}
}  // namespace mvd
#endif
