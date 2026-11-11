FROM gcc:14.3.0-bookworm@sha256:5e927c284bf55a7dc796262e311a0703344f62f41f5621eb56843111b1d37e15 AS build

WORKDIR /src
COPY src ./src
RUN mkdir -p /out \
    && g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -pthread \
       -static-libstdc++ -static-libgcc src/minivectordb.cpp -o /out/minivectordb

FROM debian:bookworm-slim@sha256:734a72b3d6142a71993b4bc580ca01d8ca687f77a2c422d96a961d3376b179bc

RUN mkdir -p /app /data
COPY --from=build /out/minivectordb /app/minivectordb

VOLUME ["/data"]
WORKDIR /app
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir=/data"]
