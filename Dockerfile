FROM ubuntu:24.04 AS build

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       ca-certificates g++ cmake make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM ubuntu:24.04

RUN mkdir -p /app /data
COPY --from=build /src/build/minivectordb /app/minivectordb

VOLUME ["/data"]
WORKDIR /app
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir=/data"]
