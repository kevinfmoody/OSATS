FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j \
 && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim
RUN useradd -r -s /usr/sbin/nologin osats
COPY --from=build /src/build/osats-server /usr/local/bin/osats-server
COPY --from=build /src/build/osats-cli /usr/local/bin/osats-cli
COPY --from=build /src/build/osats-client /usr/local/bin/osats-client
USER osats
EXPOSE 4400 4401
ENTRYPOINT ["osats-server"]
CMD ["--port", "4400", "--http-port", "4401", "--verbose"]
