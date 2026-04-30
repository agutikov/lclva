# syntax=docker/dockerfile:1.7
# whisper.cpp server image — upstream publishes only the `main` example
# image, never a server tag, and there is no `.devops/server.Dockerfile`
# to build from. We build the `whisper-server` binary ourselves.
#
# This deviates from the M1.B "no custom Dockerfiles" guideline; see
# plans/milestones/m1_llm_memory.md §B Status note for the reasoning.

ARG WHISPER_REF=v1.8.4

FROM debian:bookworm-slim AS build
ARG WHISPER_REF
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates git build-essential cmake \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone --depth 1 --branch "${WHISPER_REF}" \
        https://github.com/ggml-org/whisper.cpp.git .
RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
 && cmake --build build -j --target whisper-server

FROM debian:bookworm-slim AS runtime
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates curl libgomp1 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/bin/whisper-server /usr/local/bin/whisper-server
EXPOSE 8082
ENTRYPOINT ["/usr/local/bin/whisper-server"]
CMD ["--host", "0.0.0.0", "--port", "8082"]
