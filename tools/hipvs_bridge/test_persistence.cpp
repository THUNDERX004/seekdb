#include "seekdb_cuvs_bridge.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  const long count = 10000;
  const long dimension = 32;
  std::vector<float> vectors(count * dimension);
  for (long row = 0; row < count; ++row) {
    for (long column = 0; column < dimension; ++column) {
      vectors[row * dimension + column] =
          std::sin(float(row * 31 + column * 17));
    }
  }
  void *index = nullptr;
  if (std::strcmp(argv[1], "write") == 0) {
    index = seekdb_cuvs_build(vectors.data(), count, dimension);
    if (!index)
      return 3;
    uint64_t actual_count = 0;
    uint64_t graph_degree = 0;
    uint64_t graph_bytes = 0;
    if (seekdb_cuvs_graph_info(index, &actual_count, &graph_degree,
                               &graph_bytes) != 0 ||
        actual_count != static_cast<uint64_t>(count) || graph_bytes == 0)
      return 3;
    std::vector<unsigned char> graph(static_cast<size_t>(graph_bytes));
    if (seekdb_cuvs_copy_graph(index, graph.data(), graph_bytes) != 0)
      return 3;
    FILE *file = std::fopen(argv[2], "wb");
    if (!file)
      return 3;
    const uint64_t header[2] = {graph_degree, graph_bytes};
    const bool wrote =
        std::fwrite(header, sizeof(header), 1, file) == 1 &&
        std::fwrite(graph.data(), 1, graph.size(), file) == graph.size();
    std::fclose(file);
    if (!wrote)
      return 3;
  } else {
    FILE *file = std::fopen(argv[2], "rb");
    if (!file)
      return 4;
    uint64_t header[2] = {0, 0};
    if (std::fread(header, sizeof(header), 1, file) != 1) {
      std::fclose(file);
      return 4;
    }
    std::vector<unsigned char> graph(static_cast<size_t>(header[1]));
    const bool read =
        std::fread(graph.data(), 1, graph.size(), file) == graph.size();
    std::fclose(file);
    if (!read)
      return 4;
    void *invalid = seekdb_cuvs_deserialize_from_graph(
        graph.data(), header[1], header[0], vectors.data(), count - 1,
        dimension);
    if (invalid) {
      seekdb_cuvs_free(invalid);
      return 6;
    }
    invalid = seekdb_cuvs_deserialize_from_graph(graph.data(), header[1] + 1,
                                                 header[0], vectors.data(),
                                                 count, dimension);
    if (invalid) {
      seekdb_cuvs_free(invalid);
      return 7;
    }
    index = seekdb_cuvs_deserialize_from_graph(
        graph.data(), header[1], header[0], vectors.data(), count, dimension);
    if (!index)
      return 4;
  }
  unsigned int top_id = 0;
  float top_distance = 0.0f;
  const long topk_values[] = {10, 100, 10};
  for (long topk : topk_values) {
    std::vector<unsigned int> ids(topk);
    std::vector<float> distances(topk);
    int result = seekdb_cuvs_search(index, vectors.data(), 1, topk, ids.data(),
                                    distances.data());
    if (result != 0 || ids[0] != 0 || std::fabs(distances[0]) > 0.0001f) {
      seekdb_cuvs_free(index);
      return 5;
    }
    top_id = ids[0];
    top_distance = distances[0];
  }
  seekdb_cuvs_free(index);
  std::printf("%s: n=%ld top1=%u distance=%g\n", argv[1], count, top_id,
              top_distance);
  return 0;
}
